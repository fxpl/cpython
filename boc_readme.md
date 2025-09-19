## Building a BOC-Compatible Python

1. Clone the Pyrona repository and check out the `boc` branch.

2. Run the configure script. There are various flags you can pass. I usually compile with debug
   information and sanitisers enabled, and with optimisations disabled. This is useful for debugging,
   but not for performance measurements. For example:

   ```bash
   ./configure --with-pydebug --with-assertions --with-address-sanitizer --with-undefined-behavior-sanitizer CFLAGS='-O0 -g' CXXFLAGS='-O0 -g' BOOT_CFLAGS='-O0 -g'
   ```

3. Build Python with `make`. Use the `-j` flag to specify the number of parallel jobs:

   ```bash
   make -j8
   ```

4. Run Python with:

   ```bash
   ./python
   ```

## Summary of the BOC API

This section assumes a basic familiarity with behaviour-oriented concurrency, in particular
*when-blocks* and *cowns*.

The BOC API is implemented in `concurrent.boc`:

```python
import concurrent.boc
```

A cown is represented by the `Cown` type and is initialised with a default value:

```python
c1 = Cown(42)
```

A when-block is created with the `@when` decorator. The decorated function defines the when-block,
and it is scheduled as soon as the decorator runs (i.e. when the function is defined).
This means that if you create a when-block at the module level, it will be scheduled during module
initialisation. However, it is also perfectly valid to create when-blocks inside other functions.

```python
@when(c1)
def mul2(c1: Cown):
    # A cown’s value is accessed through its `val` attribute.
    # The `val` attribute is only accessible inside a when-block.
    c1.val *= 2
    return c1.val
```

For convenience, a cown is automatically created with the same name as the decorated function,
holding the return value (or exception) of the when-block:

```python
@when(c1, mul2)
def _(x, y):
    assert x.val == y.val == 84
```

### An Escape Hatch from When-Blocks

In BOC, the general principle is that cowns should only be accessed inside when-blocks. This means
that a cown’s value cannot easily be transferred from a when-block back to the main thread. However,
there are situations where this is necessary.

For such cases, the API provides the functions `block_on_cown()` and `block_on_cowns()`. These
functions take one or more cowns and block the current thread until the value(s) of the cown(s)
become available.

```python
x = boc.block_on_cown(c1, timeout=10)
assert x == 84
```

This API is somewhat at odds with the principles of BOC programming, and it also introduces the risk
of deadlocks (you'll have to verify this). Because of that, we should emphasise to users that these
functions should be used sparingly and only when absolutely necessary.
