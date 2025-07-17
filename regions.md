# Write Barrier Guide

## Return `Py_NewRef()`

Getter functions often return a new reference to a contained object using `Py_NewRef(x)`/`Py_XNewRef(x)`. Regions need to know about this new reference to ensure that they stay open. The simplest way is to replace calls to `Py_NewRef()` with `PyRegion_NewRef()` which returns a new reference and also increases the local reference count of `x`'s region.

```diff
    struct MyObject {
        PyObject* field;
    }

    PyObject* PyObject_get_field(MyObject* self) {
-       return Py_NewRef(self->field);
+       return PyRegion_NewRef(self->field);
    }
```

## Moving References

## Staging References

