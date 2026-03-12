"""User-facing immutability helpers.

This module re-exports the public API from the internal C extension
`_immutable`, keeping the programmer-facing surface in Python.
"""

from __future__ import annotations

import _immutable as _c

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


def freezable(cls):
    """Class decorator: mark a class as always freezable."""
    set_freezable(cls, FREEZABLE_YES)
    return cls


def unfreezable(cls):
    """Class decorator: mark a class as never freezable."""
    set_freezable(cls, FREEZABLE_NO)
    return cls


def explicitlyFreezable(cls):
    """Class decorator: mark a class as freezable only when passed directly to freeze()."""
    set_freezable(cls, FREEZABLE_EXPLICIT)
    return cls


def frozen(cls):
    """Class decorator: make a class freezable, then freeze it."""
    set_freezable(cls, FREEZABLE_YES)
    freeze(cls)
    return cls


__all__ = [
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
    "freezable",
    "unfreezable",
    "explicitlyFreezable",
    "frozen",
]

__version__ = getattr(_c, "__version__", "1.0")
