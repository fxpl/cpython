#ifndef Py_ATOMIC_H
#define Py_ATOMIC_H
#ifdef __cplusplus
extern "C" {
#endif

// TODO(Immutable): Hack as some refcount.h build needs this, and it wasn't on. Investigate later.
//#ifndef Py_LIMITED_API
#  define Py_CPYTHON_ATOMIC_H
#  include "cpython/pyatomic.h"
#  undef Py_CPYTHON_ATOMIC_H
//#endif

#ifdef __cplusplus
}
#endif
#endif  /* !Py_ATOMIC_H */
