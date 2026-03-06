"""User-facing immutability helpers.

This module re-exports the public API from the internal C extension
`_immutable`, keeping the programmer-facing surface in Python.
"""

from __future__ import annotations

import _immutable as _c

register_freezable = _c.register_freezable
freeze = _c.freeze
isfrozen = _c.isfrozen
set_freezable = _c.set_freezable
NotFreezable = getattr(_c, "NotFreezable", None)
NotFreezableError = _c.NotFreezableError
ImmutableModule = _c.ImmutableModule
FREEZABLE_YES = _c.FREEZABLE_YES
FREEZABLE_NO = _c.FREEZABLE_NO
FREEZABLE_EXPLICIT = _c.FREEZABLE_EXPLICIT
FREEZABLE_PROXY = _c.FREEZABLE_PROXY

__all__ = [
    "register_freezable",
    "freeze",
    "isfrozen",
    "set_freezable",
    "NotFreezable",
    "NotFreezableError",
    "ImmutableModule",
    "FREEZABLE_YES",
    "FREEZABLE_NO",
    "FREEZABLE_EXPLICIT",
    "FREEZABLE_PROXY",
]

__version__ = getattr(_c, "__version__", "1.0")
