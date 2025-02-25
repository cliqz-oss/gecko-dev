Running Linters Locally
=======================

You can run all the various linters in the tree using the ``mach lint`` command. Simply pass in the
directory or file you wish to lint (defaults to current working directory):

.. parsed-literal::

    ./mach lint path/to/files

Multiple paths are allowed:

.. parsed-literal::

    ./mach lint path/to/foo.js path/to/bar.py path/to/dir

``Mozlint`` will automatically determine which types of files exist, and which linters need to be run
against them. For example, if the directory contains both JavaScript and Python files then mozlint
will automatically run both ESLint and Flake8 against those files respectively.

To restrict which linters are invoked manually, pass in ``-l/--linter``:

.. parsed-literal::

    ./mach lint -l eslint path/to/files

Finally, ``mozlint`` can lint the files touched by outgoing revisions or the working directory using
the ``-o/--outgoing`` and ``-w/--workdir`` arguments respectively. These work both with mercurial and
git. In the case of ``--outgoing``, the default remote repository the changes would be pushed to is
used as the comparison. If desired, a remote can be specified manually:

.. parsed-literal::

    ./mach lint --workdir
    ./mach lint --outgoing
    ./mach lint --outgoing origin/master
