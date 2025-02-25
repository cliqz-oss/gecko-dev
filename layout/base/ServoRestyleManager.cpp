/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ServoRestyleManager.h"

#include "mozilla/DocumentStyleRootIterator.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoStyleSet.h"
#include "mozilla/Unused.h"
#include "mozilla/ViewportFrame.h"
#include "mozilla/dom/ChildIterator.h"
#include "mozilla/dom/ElementInlines.h"
#include "nsBlockFrame.h"
#include "nsBulletFrame.h"
#include "nsPlaceholderFrame.h"
#include "nsContentUtils.h"
#include "nsCSSFrameConstructor.h"
#include "nsPrintfCString.h"
#include "nsRefreshDriver.h"
#include "nsStyleChangeList.h"

using namespace mozilla::dom;

namespace mozilla {

#ifdef DEBUG
static bool
IsAnonBox(const nsIFrame& aFrame)
{
  return aFrame.StyleContext()->IsAnonBox();
}

static const nsIFrame*
FirstContinuationOrPartOfIBSplit(const nsIFrame* aFrame)
{
  if (!aFrame) {
    return nullptr;
  }

  return nsLayoutUtils::FirstContinuationOrIBSplitSibling(aFrame);
}

static const nsIFrame*
ExpectedOwnerForChild(const nsIFrame& aFrame)
{
  if (IsAnonBox(aFrame)) {
    return aFrame.GetParent()->IsViewportFrame() ? nullptr : aFrame.GetParent();
  }

  if (aFrame.IsBulletFrame()) {
    return aFrame.GetParent();
  }

  const nsIFrame* parent = FirstContinuationOrPartOfIBSplit(aFrame.GetParent());

  if (aFrame.IsTableFrame()) {
    MOZ_ASSERT(parent->IsTableWrapperFrame());
    parent = FirstContinuationOrPartOfIBSplit(parent->GetParent());
  }

  // We've handled already anon boxes and bullet frames, so now we're looking at
  // a frame of a DOM element or pseudo. Hop through anon and line-boxes
  // generated by our DOM parent, and go find the owner frame for it.
  while (parent && (IsAnonBox(*parent) || parent->IsLineFrame())) {
    auto* pseudo = parent->StyleContext()->GetPseudo();
    if (pseudo == nsCSSAnonBoxes::tableWrapper) {
      const nsIFrame* tableFrame = parent->PrincipalChildList().FirstChild();
      MOZ_ASSERT(tableFrame->IsTableFrame());
      // Handle :-moz-table and :-moz-inline-table.
      parent = IsAnonBox(*tableFrame) ? parent->GetParent() : tableFrame;
    } else {
      parent = parent->GetParent();
    }
    parent = FirstContinuationOrPartOfIBSplit(parent);
  }

  return parent;
}

void
ServoRestyleState::AssertOwner(const ServoRestyleState& aParent) const
{
  MOZ_ASSERT(mOwner);
  MOZ_ASSERT(!mOwner->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW));
  MOZ_ASSERT(ExpectedOwnerForChild(*mOwner) == aParent.mOwner);
}

nsChangeHint
ServoRestyleState::ChangesHandledFor(const nsIFrame& aFrame) const
{
  if (!mOwner) {
    MOZ_ASSERT(!mChangesHandled);
    return mChangesHandled;
  }

  MOZ_ASSERT(mOwner == ExpectedOwnerForChild(aFrame),
             "Missed some frame in the hierarchy?");
  return mChangesHandled;
}
#endif

ServoRestyleManager::ServoRestyleManager(nsPresContext* aPresContext)
  : RestyleManager(StyleBackendType::Servo, aPresContext)
  , mReentrantChanges(nullptr)
{
}

void
ServoRestyleManager::PostRestyleEvent(Element* aElement,
                                      nsRestyleHint aRestyleHint,
                                      nsChangeHint aMinChangeHint)
{
  MOZ_ASSERT(!(aMinChangeHint & nsChangeHint_NeutralChange),
             "Didn't expect explicit change hints to be neutral!");
  if (MOZ_UNLIKELY(IsDisconnected()) ||
      MOZ_UNLIKELY(PresContext()->PresShell()->IsDestroying())) {
    return;
  }

  // We allow posting restyles from within change hint handling, but not from
  // within the restyle algorithm itself.
  MOZ_ASSERT(!ServoStyleSet::IsInServoTraversal());

  if (aRestyleHint == 0 && !aMinChangeHint) {
    return; // Nothing to do.
  }

  // Processing change hints sometimes causes new change hints to be generated,
  // and very occasionally, additional restyle hints. We collect the change
  // hints manually to avoid re-traversing the DOM to find them.
  if (mReentrantChanges && !aRestyleHint) {
    mReentrantChanges->AppendElement(ReentrantChange { aElement, aMinChangeHint });
    return;
  }

  if (aRestyleHint & ~eRestyle_AllHintsWithAnimations) {
    mHaveNonAnimationRestyles = true;
  }

  if (aRestyleHint & eRestyle_LaterSiblings) {
    aRestyleHint &= ~eRestyle_LaterSiblings;

    nsRestyleHint siblingHint = eRestyle_Subtree;
    Element* current = aElement->GetNextElementSibling();
    while (current) {
      Servo_NoteExplicitHints(current, siblingHint, nsChangeHint(0));
      current = current->GetNextElementSibling();
    }
  }

  if (aRestyleHint || aMinChangeHint) {
    Servo_NoteExplicitHints(aElement, aRestyleHint, aMinChangeHint);
  }
}

void
ServoRestyleManager::PostRestyleEventForCSSRuleChanges()
{
  mRestyleForCSSRuleChanges = true;
  mPresContext->PresShell()->EnsureStyleFlush();
}

/* static */ void
ServoRestyleManager::PostRestyleEventForAnimations(
  Element* aElement,
  CSSPseudoElementType aPseudoType,
  nsRestyleHint aRestyleHint)
{
  Element* elementToRestyle =
    EffectCompositor::GetElementToRestyle(aElement, aPseudoType);

  if (!elementToRestyle) {
    // FIXME: Bug 1371107: When reframing happens,
    // EffectCompositor::mElementsToRestyle still has unbinded old pseudo
    // element. We should drop it.
    return;
  }

  Servo_NoteExplicitHints(elementToRestyle, aRestyleHint, nsChangeHint(0));
}

void
ServoRestyleManager::RebuildAllStyleData(nsChangeHint aExtraHint,
                                         nsRestyleHint aRestyleHint)
{
   // NOTE(emilio): GeckoRestlyeManager does a sync style flush, which seems not
   // to be needed in my testing.
  PostRebuildAllStyleDataEvent(aExtraHint, aRestyleHint);
}

void
ServoRestyleManager::PostRebuildAllStyleDataEvent(nsChangeHint aExtraHint,
                                                  nsRestyleHint aRestyleHint)
{
  StyleSet()->ClearDataAndMarkDeviceDirty();

  DocumentStyleRootIterator iter(mPresContext->Document());
  while (Element* root = iter.GetNextStyleRoot()) {
    PostRestyleEvent(root, aRestyleHint, aExtraHint);
  }

  // TODO(emilio, bz): Extensions can add/remove stylesheets that can affect
  // non-inheriting anon boxes. It's not clear if we want to support that, but
  // if we do, we need to re-selector-match them here.
}

/* static */ void
ServoRestyleManager::ClearServoDataFromSubtree(Element* aElement)
{
  if (!aElement->HasServoData()) {
    MOZ_ASSERT(!aElement->HasDirtyDescendantsForServo());
    MOZ_ASSERT(!aElement->HasAnimationOnlyDirtyDescendantsForServo());
    return;
  }

  StyleChildrenIterator it(aElement);
  for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
    if (n->IsElement()) {
      ClearServoDataFromSubtree(n->AsElement());
    }
  }

  aElement->ClearServoData();
  aElement->UnsetHasDirtyDescendantsForServo();
  aElement->UnsetHasAnimationOnlyDirtyDescendantsForServo();
}

/* static */ void
ServoRestyleManager::ClearRestyleStateFromSubtree(Element* aElement)
{
  if (aElement->HasDirtyDescendantsForServo() ||
      aElement->HasAnimationOnlyDirtyDescendantsForServo()) {
    StyleChildrenIterator it(aElement);
    for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
      if (n->IsElement()) {
        ClearRestyleStateFromSubtree(n->AsElement());
      }
    }
  }

  Unused << Servo_TakeChangeHint(aElement);
  aElement->UnsetHasDirtyDescendantsForServo();
  aElement->UnsetHasAnimationOnlyDirtyDescendantsForServo();
  aElement->UnsetFlags(NODE_DESCENDANTS_NEED_FRAMES);
}

/**
 * This struct takes care of encapsulating some common state that text nodes may
 * need to track during the post-traversal.
 *
 * This is currently used to properly compute change hints when the parent
 * element of this node is a display: contents node, and also to avoid computing
 * the style for text children more than once per element.
 */
struct ServoRestyleManager::TextPostTraversalState
{
public:
  TextPostTraversalState(nsStyleContext& aParentContext,
                         bool aDisplayContentsParentStyleChanged,
                         ServoRestyleState& aParentRestyleState)
    : mParentContext(aParentContext)
    , mParentRestyleState(aParentRestyleState)
    , mStyle(nullptr)
    , mShouldPostHints(aDisplayContentsParentStyleChanged)
    , mShouldComputeHints(aDisplayContentsParentStyleChanged)
    , mComputedHint(nsChangeHint_Empty)
  {}

  nsStyleChangeList& ChangeList() { return mParentRestyleState.ChangeList(); }

  nsStyleContext& ComputeStyle(nsIContent* aTextNode)
  {
    if (!mStyle) {
      mStyle = mParentRestyleState.StyleSet().ResolveStyleForText(
        aTextNode, &mParentContext);
    }
    MOZ_ASSERT(mStyle);
    return *mStyle;
  }

  void ComputeHintIfNeeded(nsIContent* aContent,
                           nsIFrame* aTextFrame,
                           nsStyleContext& aNewContext)
  {
    MOZ_ASSERT(aTextFrame);
    MOZ_ASSERT(aNewContext.GetPseudo() == nsCSSAnonBoxes::mozText);

    if (MOZ_LIKELY(!mShouldPostHints)) {
      return;
    }

    nsStyleContext* oldContext = aTextFrame->StyleContext();
    MOZ_ASSERT(oldContext->GetPseudo() == nsCSSAnonBoxes::mozText);

    // We rely on the fact that all the text children for the same element share
    // style to avoid recomputing style differences for all of them.
    //
    // TODO(emilio): The above may not be true for ::first-{line,letter}, but
    // we'll cross that bridge when we support those in stylo.
    if (mShouldComputeHints) {
      mShouldComputeHints = false;
      uint32_t equalStructs, samePointerStructs;
      mComputedHint =
        oldContext->CalcStyleDifference(&aNewContext,
                                        &equalStructs,
                                        &samePointerStructs);
      mComputedHint = NS_RemoveSubsumedHints(
        mComputedHint, mParentRestyleState.ChangesHandledFor(*aTextFrame));
    }

    if (mComputedHint) {
      mParentRestyleState.ChangeList().AppendChange(
        aTextFrame, aContent, mComputedHint);
    }
  }

private:
  nsStyleContext& mParentContext;
  ServoRestyleState& mParentRestyleState;
  RefPtr<nsStyleContext> mStyle;
  bool mShouldPostHints;
  bool mShouldComputeHints;
  nsChangeHint mComputedHint;
};

// Find the first-letter frame for the given element, if any.  Returns null to
// indicate there isn't one.
static nsIFrame*
FindFirstLetterFrameForElement(const Element* aElement)
{
  nsIFrame* frame = aElement->GetPrimaryFrame();
  if (!frame) {
    return nullptr;
  }
  // The first-letter frame will always be inside the content insertion frame,
  // which will always be a block if we have a first-letter frame at all.
  frame = frame->GetContentInsertionFrame();
  if (!frame) {
    // We're a leaf; certainly no first-letter frame.
    return nullptr;
  }

  if (!frame->IsFrameOfType(nsIFrame::eBlockFrame)) {
    return nullptr;
  }

  return static_cast<nsBlockFrame*>(frame)->GetFirstLetter();
}

static void
UpdateBackdropIfNeeded(nsIFrame* aFrame,
                       ServoStyleSet& aStyleSet,
                       nsStyleChangeList& aChangeList)
{
  const nsStyleDisplay* display = aFrame->StyleContext()->StyleDisplay();
  if (display->mTopLayer != NS_STYLE_TOP_LAYER_TOP) {
    return;
  }

  // Elements in the top layer are guaranteed to have absolute or fixed
  // position per https://fullscreen.spec.whatwg.org/#new-stacking-layer.
  MOZ_ASSERT(display->IsAbsolutelyPositionedStyle());

  nsIFrame* backdropPlaceholder =
    aFrame->GetChildList(nsIFrame::kBackdropList).FirstChild();
  if (!backdropPlaceholder) {
    return;
  }

  MOZ_ASSERT(backdropPlaceholder->IsPlaceholderFrame());
  nsIFrame* backdropFrame =
    nsPlaceholderFrame::GetRealFrameForPlaceholder(backdropPlaceholder);
  MOZ_ASSERT(backdropFrame->IsBackdropFrame());
  MOZ_ASSERT(backdropFrame->StyleContext()->GetPseudoType() ==
             CSSPseudoElementType::backdrop);

  RefPtr<nsStyleContext> newContext =
    aStyleSet.ResolvePseudoElementStyle(aFrame->GetContent()->AsElement(),
                                        CSSPseudoElementType::backdrop,
                                        aFrame->StyleContext(),
                                        /* aPseudoElement = */ nullptr);

  // NOTE(emilio): We can't use the changes handled for the owner of the
  // backdrop frame, since it's out of flow, and parented to the viewport frame.
  MOZ_ASSERT(backdropFrame->GetParent()->IsViewportFrame());
  ServoRestyleState state(aStyleSet, aChangeList);
  aFrame->UpdateStyleOfOwnedChildFrame(backdropFrame, newContext, state);
}

static void
UpdateFirstLetterIfNeeded(nsIFrame* aFrame, ServoRestyleState& aRestyleState)
{
  if (!aFrame->HasFirstLetterChild()) {
    return;
  }

  // We need to find the block the first-letter is associated with so we can
  // find the right element for the first-letter's style resolution.  Might as
  // well just delegate the whole thing to that block.
  nsIFrame* block = aFrame;
  while (!block->IsFrameOfType(nsIFrame::eBlockFrame)) {
    block = block->GetParent();
  }
  static_cast<nsBlockFrame*>(block->FirstContinuation())->
    UpdateFirstLetterStyle(aRestyleState);
}

static void
UpdateFramePseudoElementStyles(nsIFrame* aFrame,
                               ServoRestyleState& aRestyleState)
{
  // first-letter needs to be updated before first-line, because first-line can
  // change the style of the first-letter.
  UpdateFirstLetterIfNeeded(aFrame, aRestyleState);

  if (aFrame->IsFrameOfType(nsIFrame::eBlockFrame)) {
    static_cast<nsBlockFrame*>(aFrame)->UpdatePseudoElementStyles(aRestyleState);
  }

  UpdateBackdropIfNeeded(
    aFrame, aRestyleState.StyleSet(), aRestyleState.ChangeList());
}

bool
ServoRestyleManager::ProcessPostTraversal(Element* aElement,
                                          nsStyleContext* aParentContext,
                                          ServoRestyleState& aRestyleState)
{
  nsIFrame* styleFrame = nsLayoutUtils::GetStyleFrame(aElement);

  // NOTE(emilio): This is needed because for table frames the bit is set on the
  // table wrapper (which is the primary frame), not on the table itself.
  const bool isOutOfFlow =
    aElement->GetPrimaryFrame() &&
    aElement->GetPrimaryFrame()->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW);

  // Grab the change hint from Servo.
  nsChangeHint changeHint = Servo_TakeChangeHint(aElement);

  // We should really fix the weird primary frame mapping for image maps
  // (bug 135040)...
  if (styleFrame && styleFrame->GetContent() != aElement) {
    MOZ_ASSERT(styleFrame->IsImageFrame());
    styleFrame = nullptr;
  }

  // Handle lazy frame construction by posting a reconstruct for any lazily-
  // constructed roots.
  if (aElement->HasFlag(NODE_NEEDS_FRAME)) {
    changeHint |= nsChangeHint_ReconstructFrame;
    MOZ_ASSERT(!styleFrame);
  }

  if (styleFrame && !isOutOfFlow) {
    changeHint = NS_RemoveSubsumedHints(
      changeHint, aRestyleState.ChangesHandledFor(*styleFrame));
  }

  // Although we shouldn't generate non-ReconstructFrame hints for elements with
  // no frames, we can still get them here if they were explicitly posted by
  // PostRestyleEvent, such as a RepaintFrame hint when a :link changes to be
  // :visited.  Skip processing these hints if there is no frame.
  if ((styleFrame || (changeHint & nsChangeHint_ReconstructFrame)) && changeHint) {
    aRestyleState.ChangeList().AppendChange(styleFrame, aElement, changeHint);
  }

  // If our change hint is reconstruct, we delegate to the frame constructor,
  // which consumes the new style and expects the old style to be on the frame.
  //
  // XXXbholley: We should teach the frame constructor how to clear the dirty
  // descendants bit to avoid the traversal here.
  if (changeHint & nsChangeHint_ReconstructFrame) {
    ClearRestyleStateFromSubtree(aElement);
    return true;
  }

  // TODO(emilio): We could avoid some refcount traffic here, specially in the
  // ServoComputedValues case, which uses atomic refcounting.
  //
  // Hold the old style context alive, because it could become a dangling
  // pointer during the replacement. In practice it's not a huge deal, but
  // better not playing with dangling pointers if not needed.
  RefPtr<ServoStyleContext> oldStyleContext =
    styleFrame ? styleFrame->StyleContext()->AsServo() : nullptr;

  UndisplayedNode* displayContentsNode = nullptr;
  // FIXME(emilio, bug 1303605): This can be simpler for Servo.
  // Note that we intentionally don't check for display: none content.
  if (!oldStyleContext) {
    displayContentsNode =
      PresContext()->FrameConstructor()->GetDisplayContentsNodeFor(aElement);
    if (displayContentsNode) {
      oldStyleContext = displayContentsNode->mStyle->AsServo();
    }
  }

  RefPtr<ServoComputedValues> computedValues =
    aRestyleState.StyleSet().ResolveServoStyle(aElement);

  // Note that we rely in the fact that we don't cascade pseudo-element styles
  // separately right now (that is, if a pseudo style changes, the normal style
  // changes too).
  //
  // Otherwise we should probably encode that information somehow to avoid
  // expensive checks in the common case.
  //
  // Also, we're going to need to check for pseudos of display: contents
  // elements, though that is buggy right now even in non-stylo mode, see
  // bug 1251799.
  const bool recreateContext = oldStyleContext &&
    oldStyleContext->ComputedValues() != computedValues;

  Maybe<ServoRestyleState> thisFrameRestyleState;
  if (styleFrame) {
    auto type = isOutOfFlow
      ? ServoRestyleState::Type::OutOfFlow
      : ServoRestyleState::Type::InFlow;

    thisFrameRestyleState.emplace(*styleFrame, aRestyleState, changeHint, type);
  }

  // We can't really assume as used changes from display: contents elements (or
  // other elements without frames).
  ServoRestyleState& childrenRestyleState =
    thisFrameRestyleState ? *thisFrameRestyleState : aRestyleState;

  RefPtr<ServoStyleContext> newContext = nullptr;
  if (recreateContext) {
    MOZ_ASSERT(styleFrame || displayContentsNode);

    auto pseudo = aElement->GetPseudoElementType();
    nsIAtom* pseudoTag = pseudo == CSSPseudoElementType::NotPseudo
      ? nullptr : nsCSSPseudoElements::GetPseudoAtom(pseudo);

    newContext = aRestyleState.StyleSet().GetContext(
      computedValues.forget(), aParentContext, pseudoTag, pseudo, aElement);

    newContext->ResolveSameStructsAs(PresContext(), oldStyleContext);

    // We want to walk all the continuations here, even the ones with different
    // styles.  In practice, the only reason we get continuations with different
    // styles here is ::first-line (::first-letter never affects element
    // styles).  But in that case, newContext is the right context for the
    // _later_ continuations anyway (the ones not affected by ::first-line), not
    // the earlier ones, so there is no point stopping right at the point when
    // we'd actually be setting the right style context.
    //
    // This does mean that we may be setting the wrong style context on our
    // initial continuations; ::first-line fixes that up after the fact.
    for (nsIFrame* f = styleFrame; f; f = f->GetNextContinuation()) {
      f->SetStyleContext(newContext);
    }

    if (MOZ_UNLIKELY(displayContentsNode)) {
      MOZ_ASSERT(!styleFrame);
      displayContentsNode->mStyle = newContext;
    }

    if (styleFrame) {
      styleFrame->UpdateStyleOfOwnedAnonBoxes(childrenRestyleState);
    }

    if (!aElement->GetParent()) {
      // This is the root.  Update styles on the viewport as needed.
      ViewportFrame* viewport =
        do_QueryFrame(mPresContext->PresShell()->GetRootFrame());
      if (viewport) {
        // NB: The root restyle state, not the one for our children!
        viewport->UpdateStyle(aRestyleState);
      }
    }

    // Some changes to animations don't affect the computed style and yet still
    // require the layer to be updated. For example, pausing an animation via
    // the Web Animations API won't affect an element's style but still
    // requires to update the animation on the layer.
    //
    // We can sometimes reach this when the animated style is being removed.
    // Since AddLayerChangesForAnimation checks if |styleFrame| has a transform
    // style or not, we need to call it *after* setting |newContext| to
    // |styleFrame| to ensure the animated transform has been removed first.
    AddLayerChangesForAnimation(
      styleFrame, aElement, aRestyleState.ChangeList());
  }

  const bool descendantsNeedFrames =
    aElement->HasFlag(NODE_DESCENDANTS_NEED_FRAMES);
  const bool traverseElementChildren =
    aElement->HasDirtyDescendantsForServo() ||
    aElement->HasAnimationOnlyDirtyDescendantsForServo() ||
    descendantsNeedFrames;
  const bool traverseTextChildren = recreateContext || descendantsNeedFrames;
  bool recreatedAnyContext = recreateContext;
  if (traverseElementChildren || traverseTextChildren) {
    nsStyleContext* upToDateContext =
      recreateContext ? newContext : oldStyleContext;

    StyleChildrenIterator it(aElement);
    TextPostTraversalState textState(*upToDateContext,
                                     displayContentsNode && recreateContext,
                                     childrenRestyleState);
    for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
      if (traverseElementChildren && n->IsElement()) {
        recreatedAnyContext |= ProcessPostTraversal(
          n->AsElement(), upToDateContext, childrenRestyleState);
      } else if (traverseTextChildren && n->IsNodeOfType(nsINode::eTEXT)) {
        recreatedAnyContext |= ProcessPostTraversalForText(n, textState);
      }
    }
  }

  // We want to update frame pseudo-element styles after we've traversed our
  // kids, because some of those updates (::first-line/::first-letter) need to
  // modify the styles of the kids, and the child traversal above would just
  // clobber those modifications.
  if (recreateContext && styleFrame) {
    UpdateFramePseudoElementStyles(styleFrame, childrenRestyleState);
  }

  aElement->UnsetHasDirtyDescendantsForServo();
  aElement->UnsetHasAnimationOnlyDirtyDescendantsForServo();
  aElement->UnsetFlags(NODE_DESCENDANTS_NEED_FRAMES);
  return recreatedAnyContext;
}

bool
ServoRestyleManager::ProcessPostTraversalForText(
    nsIContent* aTextNode,
    TextPostTraversalState& aPostTraversalState)
{
  // Handle lazy frame construction.
  if (aTextNode->HasFlag(NODE_NEEDS_FRAME)) {
    aPostTraversalState.ChangeList().AppendChange(
      nullptr, aTextNode, nsChangeHint_ReconstructFrame);
    return true;
  }

  // Handle restyle.
  nsIFrame* primaryFrame = aTextNode->GetPrimaryFrame();
  if (!primaryFrame) {
    return false;
  }

  nsStyleContext& newContext = aPostTraversalState.ComputeStyle(aTextNode);
  aPostTraversalState.ComputeHintIfNeeded(aTextNode, primaryFrame, newContext);

  // We want to walk all the continuations here, even the ones with different
  // styles.  In practice, the only reasons we get continuations with different
  // styles are ::first-line and ::first-letter.  But in those cases,
  // newContext is the right context for the _later_ continuations anyway (the
  // ones not affected by ::first-line/::first-letter), not the earlier ones,
  // so there is no point stopping right at the point when we'd actually be
  // setting the right style context.
  //
  // This does mean that we may be setting the wrong style context on our
  // initial continuations; ::first-line/::first-letter fix that up after the
  // fact.
  for (nsIFrame* f = primaryFrame; f; f = f->GetNextContinuation()) {
    f->SetStyleContext(&newContext);
  }

  return true;
}

void
ServoRestyleManager::ClearSnapshots()
{
  for (auto iter = mSnapshots.Iter(); !iter.Done(); iter.Next()) {
    iter.Key()->UnsetFlags(ELEMENT_HAS_SNAPSHOT | ELEMENT_HANDLED_SNAPSHOT);
    iter.Remove();
  }
}

ServoElementSnapshot&
ServoRestyleManager::SnapshotFor(Element* aElement)
{
  MOZ_ASSERT(!mInStyleRefresh);

  // NOTE(emilio): We can handle snapshots from a one-off restyle of those that
  // we do to restyle stuff for reconstruction, for example.
  //
  // It seems to be the case that we always flush in between that happens and
  // the next attribute change, so we can assert that we haven't handled the
  // snapshot here yet. If this assertion didn't hold, we'd need to unset that
  // flag from here too.
  //
  // Can't wait to make ProcessPendingRestyles the only entry-point for styling,
  // so this becomes much easier to reason about. Today is not that day though.
  MOZ_ASSERT(aElement->HasServoData());
  MOZ_ASSERT(!aElement->HasFlag(ELEMENT_HANDLED_SNAPSHOT));

  ServoElementSnapshot* snapshot = mSnapshots.LookupOrAdd(aElement, aElement);
  aElement->SetFlags(ELEMENT_HAS_SNAPSHOT);

  nsIPresShell* presShell = mPresContext->PresShell();
  presShell->EnsureStyleFlush();

  return *snapshot;
}

/* static */ nsIFrame*
ServoRestyleManager::FrameForPseudoElement(const Element* aElement,
                                           nsIAtom* aPseudoTagOrNull)
{
  if (!aPseudoTagOrNull) {
    return nsLayoutUtils::GetStyleFrame(aElement);
  }

  if (aPseudoTagOrNull == nsCSSPseudoElements::before) {
    Element* pseudoElement = nsLayoutUtils::GetBeforePseudo(aElement);
    return pseudoElement ? nsLayoutUtils::GetStyleFrame(pseudoElement) : nullptr;
  }

  if (aPseudoTagOrNull == nsCSSPseudoElements::after) {
    Element* pseudoElement = nsLayoutUtils::GetAfterPseudo(aElement);
    return pseudoElement ? nsLayoutUtils::GetStyleFrame(pseudoElement) : nullptr;
  }

  if (aPseudoTagOrNull == nsCSSPseudoElements::firstLetter) {
    return FindFirstLetterFrameForElement(aElement);
  }

  if (aPseudoTagOrNull == nsCSSPseudoElements::firstLine) {
    // TODO(emilio, bz): Figure out the best way to diff these styles.
    return nullptr;
  }

  MOZ_CRASH("Unkown pseudo-element given to "
            "ServoRestyleManager::FrameForPseudoElement");
  return nullptr;
}

void
ServoRestyleManager::DoProcessPendingRestyles(TraversalRestyleBehavior
                                                aRestyleBehavior)
{
  MOZ_ASSERT(PresContext()->Document(), "No document?  Pshaw!");
  MOZ_ASSERT(!nsContentUtils::IsSafeToRunScript(), "Missing a script blocker!");
  MOZ_ASSERT(!mInStyleRefresh, "Reentrant call?");

  if (MOZ_UNLIKELY(!PresContext()->PresShell()->DidInitialize())) {
    // PresShell::FlushPendingNotifications doesn't early-return in the case
    // where the PreShell hasn't yet been initialized (and therefore we haven't
    // yet done the initial style traversal of the DOM tree). We should arguably
    // fix up the callers and assert against this case, but we just detect and
    // handle it for now.
    return;
  }

  // Create a AnimationsWithDestroyedFrame during restyling process to
  // stop animations and transitions on elements that have no frame at the end
  // of the restyling process.
  AnimationsWithDestroyedFrame animationsWithDestroyedFrame(this);

  ServoStyleSet* styleSet = StyleSet();
  nsIDocument* doc = PresContext()->Document();
  bool animationOnly = aRestyleBehavior ==
                         TraversalRestyleBehavior::ForAnimationOnly;

  // Ensure the refresh driver is active during traversal to avoid mutating
  // mActiveTimer and mMostRecentRefresh time.
  PresContext()->RefreshDriver()->MostRecentRefresh();


  // Perform the Servo traversal, and the post-traversal if required. We do this
  // in a loop because certain rare paths in the frame constructor (like
  // uninstalling XBL bindings) can trigger additional style validations.
  mInStyleRefresh = true;
  if (mHaveNonAnimationRestyles && !animationOnly) {
    ++mAnimationGeneration;
  }

  TraversalRestyleBehavior restyleBehavior = mRestyleForCSSRuleChanges
    ? TraversalRestyleBehavior::ForCSSRuleChanges
    : TraversalRestyleBehavior::Normal;
  while (animationOnly ? styleSet->StyleDocumentForAnimationOnly()
                       : styleSet->StyleDocument(restyleBehavior)) {
    if (!animationOnly) {
      ClearSnapshots();
    }

    // Recreate style contexts, and queue up change hints (which also handle
    // lazy frame construction).
    nsStyleChangeList currentChanges(StyleBackendType::Servo);
    DocumentStyleRootIterator iter(doc);
    bool anyStyleChanged = false;
    while (Element* root = iter.GetNextStyleRoot()) {
      ServoRestyleState state(*styleSet, currentChanges);
      anyStyleChanged |= ProcessPostTraversal(root, nullptr, state);
    }

    // Process the change hints.
    //
    // Unfortunately, the frame constructor can generate new change hints while
    // processing existing ones. We redirect those into a secondary queue and
    // iterate until there's nothing left.
    ReentrantChangeList newChanges;
    mReentrantChanges = &newChanges;
    while (!currentChanges.IsEmpty()) {
      ProcessRestyledFrames(currentChanges);
      MOZ_ASSERT(currentChanges.IsEmpty());
      for (ReentrantChange& change: newChanges)  {
        if (!(change.mHint & nsChangeHint_ReconstructFrame) &&
            !change.mContent->GetPrimaryFrame()) {
          // SVG Elements post change hints without ensuring that the primary
          // frame will be there after that (see bug 1366142).
          //
          // Just ignore those, since we can't really process them.
          continue;
        }
        currentChanges.AppendChange(change.mContent->GetPrimaryFrame(),
                                    change.mContent, change.mHint);
      }
      newChanges.Clear();
    }
    mReentrantChanges = nullptr;


    if (anyStyleChanged) {
      // Maybe no styles changed when:
      //
      //  * Only explicit change hints were posted in the first place.
      //  * When an attribute or state change in the content happens not to need
      //    a restyle after all.
      //
      // In any case, we don't need to increment the restyle generation in that
      // case.
      IncrementRestyleGeneration();
    }
  }

  FlushOverflowChangedTracker();

  if (!animationOnly) {
    ClearSnapshots();
    styleSet->AssertTreeIsClean();
    mHaveNonAnimationRestyles = false;
  }
  mRestyleForCSSRuleChanges = false;
  mInStyleRefresh = false;

  // Now that everything has settled, see if we have enough free rule nodes in
  // the tree to warrant sweeping them.
  styleSet->MaybeGCRuleTree();

  // Note: We are in the scope of |animationsWithDestroyedFrame|, so
  //       |mAnimationsWithDestroyedFrame| is still valid.
  MOZ_ASSERT(mAnimationsWithDestroyedFrame);
  mAnimationsWithDestroyedFrame->StopAnimationsForElementsWithoutFrames();
}

void
ServoRestyleManager::ProcessPendingRestyles()
{
  DoProcessPendingRestyles(TraversalRestyleBehavior::Normal);
}

void
ServoRestyleManager::UpdateOnlyAnimationStyles()
{
  // Bug 1365855: We also need to implement this for SMIL.
  bool doCSS = PresContext()->EffectCompositor()->HasPendingStyleUpdates();
  if (!doCSS) {
    return;
  }

  DoProcessPendingRestyles(TraversalRestyleBehavior::ForAnimationOnly);
}

void
ServoRestyleManager::RestyleForInsertOrChange(nsINode* aContainer,
                                              nsIContent* aChild)
{
  //
  // XXXbholley: We need the Gecko logic here to correctly restyle for things
  // like :empty and positional selectors (though we may not need to post
  // restyle events as agressively as the Gecko path does).
  //
  // Bug 1297899 tracks this work.
  //
}

void
ServoRestyleManager::RestyleForAppend(nsIContent* aContainer,
                                      nsIContent* aFirstNewContent)
{
  //
  // XXXbholley: We need the Gecko logic here to correctly restyle for things
  // like :empty and positional selectors (though we may not need to post
  // restyle events as agressively as the Gecko path does).
  //
  // Bug 1297899 tracks this work.
  //
}

void
ServoRestyleManager::ContentRemoved(nsINode* aContainer,
                                    nsIContent* aOldChild,
                                    nsIContent* aFollowingSibling)
{
  NS_WARNING("stylo: ServoRestyleManager::ContentRemoved not implemented");
}

void
ServoRestyleManager::ContentStateChanged(nsIContent* aContent,
                                         EventStates aChangedBits)
{
  MOZ_ASSERT(!mInStyleRefresh);

  if (!aContent->IsElement()) {
    return;
  }

  Element* aElement = aContent->AsElement();
  nsChangeHint changeHint;
  nsRestyleHint restyleHint;

  if (!aElement->HasServoData()) {
    return;
  }

  // NOTE: restyleHint here is effectively always 0, since that's what
  // ServoStyleSet::HasStateDependentStyle returns. Servo computes on
  // ProcessPendingRestyles using the ElementSnapshot, but in theory could
  // compute it sequentially easily.
  //
  // Determine what's the best way to do it, and how much work do we save
  // processing the restyle hint early (i.e., computing the style hint here
  // sequentially, potentially saving the snapshot), vs lazily (snapshot
  // approach).
  //
  // If we take the sequential approach we need to specialize Servo's restyle
  // hints system a bit more, and mesure whether we save something storing the
  // restyle hint in the table and deferring the dirtiness setting until
  // ProcessPendingRestyles (that's a requirement if we store snapshots though),
  // vs processing the restyle hint in-place, dirtying the nodes on
  // PostRestyleEvent.
  //
  // If we definitely take the snapshot approach, we should take rid of
  // HasStateDependentStyle, etc (though right now they're no-ops).
  ContentStateChangedInternal(aElement, aChangedBits, &changeHint,
                              &restyleHint);

  // Don't bother taking a snapshot if no rules depend on these state bits.
  //
  // We always take a snapshot for the LTR/RTL event states, since Servo doesn't
  // track those bits in the same way, and we know that :dir() rules are always
  // present in UA style sheets.
  if (!aChangedBits.HasAtLeastOneOfStates(DIRECTION_STATES) &&
      !StyleSet()->HasStateDependency(*aElement, aChangedBits)) {
    return;
  }

  ServoElementSnapshot& snapshot = SnapshotFor(aElement);
  EventStates previousState = aElement->StyleState() ^ aChangedBits;
  snapshot.AddState(previousState);

  if (Element* parent = aElement->GetFlattenedTreeParentElementForStyle()) {
    parent->NoteDirtyDescendantsForServo();
  }

  if (restyleHint || changeHint) {
    Servo_NoteExplicitHints(aElement, restyleHint, changeHint);
  }
}

static inline bool
AttributeInfluencesOtherPseudoClassState(const Element& aElement,
                                         const nsIAtom* aAttribute)
{
  // We must record some state for :-moz-browser-frame and
  // :-moz-table-border-nonzero.
  if (aAttribute == nsGkAtoms::mozbrowser) {
    return aElement.IsAnyOfHTMLElements(nsGkAtoms::iframe, nsGkAtoms::frame);
  }

  if (aAttribute == nsGkAtoms::border) {
    return aElement.IsHTMLElement(nsGkAtoms::table);
  }

  return false;
}

static inline bool
NeedToRecordAttrChange(const ServoStyleSet& aStyleSet,
                       const Element& aElement,
                       int32_t aNameSpaceID,
                       nsIAtom* aAttribute,
                       bool* aInfluencesOtherPseudoClassState)
{
  *aInfluencesOtherPseudoClassState =
    AttributeInfluencesOtherPseudoClassState(aElement, aAttribute);

  // If the attribute influences one of the pseudo-classes that are backed by
  // attributes, we just record it.
  if (*aInfluencesOtherPseudoClassState) {
    return true;
  }

  // We assume that id and class attributes are used in class/id selectors, and
  // thus record them.
  //
  // TODO(emilio): We keep a filter of the ids in use somewhere in the StyleSet,
  // presumably we could try to filter the old and new id, but it's not clear
  // it's worth it.
  if (aNameSpaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::id || aAttribute == nsGkAtoms::_class)) {
    return true;
  }

  // We always record lang="", even though we force a subtree restyle when it
  // changes, since it can change how its siblings match :lang(..) due to
  // selectors like :lang(..) + div.
  if (aAttribute == nsGkAtoms::lang) {
    return true;
  }

  // Otherwise, just record the attribute change if a selector in the page may
  // reference it from an attribute selector.
  return aStyleSet.MightHaveAttributeDependency(aElement, aAttribute);
}

void
ServoRestyleManager::AttributeWillChange(Element* aElement,
                                         int32_t aNameSpaceID,
                                         nsIAtom* aAttribute, int32_t aModType,
                                         const nsAttrValue* aNewValue)
{
  TakeSnapshotForAttributeChange(aElement, aNameSpaceID, aAttribute);
}

void
ServoRestyleManager::ClassAttributeWillBeChangedBySMIL(Element* aElement)
{
  TakeSnapshotForAttributeChange(aElement, kNameSpaceID_None,
                                 nsGkAtoms::_class);
}

void
ServoRestyleManager::TakeSnapshotForAttributeChange(Element* aElement,
                                                    int32_t aNameSpaceID,
                                                    nsIAtom* aAttribute)
{
  MOZ_ASSERT(!mInStyleRefresh);

  if (!aElement->HasServoData()) {
    return;
  }

  bool influencesOtherPseudoClassState;
  if (!NeedToRecordAttrChange(*StyleSet(),
                              *aElement,
                              aNameSpaceID,
                              aAttribute,
                              &influencesOtherPseudoClassState)) {
    return;
  }

  ServoElementSnapshot& snapshot = SnapshotFor(aElement);
  snapshot.AddAttrs(aElement, aNameSpaceID, aAttribute);

  if (influencesOtherPseudoClassState) {
    snapshot.AddOtherPseudoClassState(aElement);
  }

  if (Element* parent = aElement->GetFlattenedTreeParentElementForStyle()) {
    parent->NoteDirtyDescendantsForServo();
  }
}

// For some attribute changes we must restyle the whole subtree:
//
// * <td> is affected by the cellpadding on its ancestor table
// * lang="" and xml:lang="" can affect all descendants due to :lang()
//
static inline bool
AttributeChangeRequiresSubtreeRestyle(const Element& aElement, nsIAtom* aAttr)
{
  if (aAttr == nsGkAtoms::cellpadding) {
    return aElement.IsHTMLElement(nsGkAtoms::table);
  }

  return aAttr == nsGkAtoms::lang;
}

void
ServoRestyleManager::AttributeChanged(Element* aElement, int32_t aNameSpaceID,
                                      nsIAtom* aAttribute, int32_t aModType,
                                      const nsAttrValue* aOldValue)
{
  MOZ_ASSERT(!mInStyleRefresh);

  if (nsIFrame* primaryFrame = aElement->GetPrimaryFrame()) {
    primaryFrame->AttributeChanged(aNameSpaceID, aAttribute, aModType);
  }

  auto changeHint = nsChangeHint(0);
  auto restyleHint = nsRestyleHint(0);

  changeHint |= aElement->GetAttributeChangeHint(aAttribute, aModType);

  if (aAttribute == nsGkAtoms::style) {
    restyleHint |= eRestyle_StyleAttribute;
  } else if (AttributeChangeRequiresSubtreeRestyle(*aElement, aAttribute)) {
    restyleHint |= eRestyle_Subtree;
  } else if (aElement->IsAttributeMapped(aAttribute)) {
    restyleHint |= eRestyle_Self;
  }

  if (restyleHint || changeHint) {
    Servo_NoteExplicitHints(aElement, restyleHint, changeHint);
  }
}

nsresult
ServoRestyleManager::ReparentStyleContext(nsIFrame* aFrame)
{
  NS_WARNING("stylo: ServoRestyleManager::ReparentStyleContext not implemented");
  return NS_OK;
}

} // namespace mozilla
