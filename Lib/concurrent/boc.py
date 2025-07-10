import inspect
import math
from inspect import Parameter
from typing import Any, Callable, NamedTuple

import _boc
from _boc import Cown

__all__ = [
    "Cown",
    "block_on_cowns",
    "InterpreterPoolInfo",
    "interpreter_pool_info",
    "when",
]


def when(*cowns: Cown):
    """Decorator for a when block.

    ```Python
    @when(cown_1, cown_2, ...)
    def foo(cown_1, cown_2, ...):
        print(cown_1.val)
        cown_1.val = cown_2.val
        print(cown_1.val)
        return 42
    ```

    Spawns a when block and schedules it immediately. This decorator is not like
    a normal decorator as it does not create a function, it rather takes the
    provided function and uses it as a when block on the given cowns. This when
    block might be run on a separate sub-interpreter, so all modules used must
    be manually imported in the when block.

    The function name (`foo` in this case) will be assigned a cown with the
    return value of the when block.
    """

    def decorator(when_block: Callable):
        sig = inspect.signature(when_block)
        n_args: int = sum(
            1
            for p in sig.parameters.values()
            if p.kind == Parameter.POSITIONAL_ONLY
            or p.kind == Parameter.POSITIONAL_OR_KEYWORD
        )

        if not (
            n_args == len(cowns)
            or n_args < len(cowns)
            and any(
                p.kind == Parameter.VAR_POSITIONAL
                for p in sig.parameters.values()
            )
        ):
            raise RuntimeError(
                f"Wrong number of arguments in when block: Expected "
                f"{len(cowns)} arguments for {len(cowns)} cowns but the "
                f"when block only takes {n_args} arguments"
            )

        return _boc.when(when_block, cowns)

    return decorator


class InterpreterPoolInfo(NamedTuple):
    """Information about the sub-interpreter pool for BOC."""

    n_live: int
    "The number of live/initialised sub-interpreters, both busy and idle."

    n_idle: int
    "The number of idle sub-interpreters, (n_live - n_busy)."

    n_max_interpreters: int
    """The maximum number of sub-interpreters that can be spawned.
    The current implementation sets this to `os.cpu_count()`.
    """

    @property
    def n_busy(self) -> int:
        "The number of currently working sub-interpreters (n_live - n_idle)."
        return self.n_live - self.n_idle


def interpreter_pool_info() -> InterpreterPoolInfo:
    """Get information about the current state of the BOC sub-interpreter pool."""
    return InterpreterPoolInfo(**_boc.interpreter_pool_info())


def block_on_cown(cown: Cown, timeout: float | None = None) -> Any:
    """Block the current thread and wait for the value of a cown."""

    return block_on_cowns(cown, timeout=timeout)[0]


def block_on_cowns(*cowns: Cown, timeout: float | None = None) -> tuple:
    """Block the current thread and wait for the values of one or multiple
    cowns.
    """

    timeout_ns = -1 if timeout is None else math.ceil(timeout * 1e9)
    return _boc.block_on_cowns(cowns, timeout_ns)
