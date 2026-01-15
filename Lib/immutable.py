"""User-facing immutability helpers.

This module re-exports the public API from the internal C extension
`_immutable`, keeping the programmer-facing surface in Python.
"""

from __future__ import annotations

import _immutable as _c

register_freezable = _c.register_freezable
freeze = _c.freeze
isfrozen = _c.isfrozen
NotFreezable = getattr(_c, "NotFreezable", None)
NotFreezableError = _c.NotFreezableError

__all__ = [
    "register_freezable",
    "freeze",
    "isfrozen",
    "NotFreezable",
    "NotFreezableError",
]

__version__ = getattr(_c, "__version__", "1.0")
