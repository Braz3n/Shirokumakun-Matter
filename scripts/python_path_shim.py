import os
import sys


class PythonPath:
    """Context manager that temporarily adds a subdirectory to sys.path.

    Used by Matter's codegen_paths.py to import py_matter_idl without a
    Pigweed venv.  Normally provided by scripts/setup/python_path.py in the
    upstream connectedhomeip tree, which is absent from the NCS fork.
    """

    def __init__(self, *subpath, relative_to=None):
        if relative_to is not None:
            base = os.path.dirname(os.path.abspath(relative_to))
        else:
            base = os.path.dirname(os.path.abspath(__file__))
        self._path = os.path.join(base, *subpath)

    def __enter__(self):
        if self._path not in sys.path:
            sys.path.insert(0, self._path)
        return self

    def __exit__(self, *args):
        try:
            sys.path.remove(self._path)
        except ValueError:
            pass
