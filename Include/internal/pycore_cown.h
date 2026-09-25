#ifndef Py_INTERNAL_COWN_H
#define Py_INTERNAL_COWN_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "exports.h"

typedef struct _PyCownObject _PyCownObject;
#define _PyCownObject_CAST(op) _Py_CAST(_PyCownObject*, op)

PyAPI_DATA(PyTypeObject) _PyCown_Type;

typedef uintptr_t _PyCown_owner_id_t;

#ifdef _Py_PYRONA_INTERPRETER_SHARING
// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the cown.
#define _Py_PYRONA_RELEASED_OWNER_ID ((_PyCown_owner_id_t)0xff00ff00ff00ff00LL)
#else
#define _Py_PYRONA_RELEASED_OWNER_ID _Py_UNOWNED_TID
#endif


PyAPI_FUNC(_PyCown_owner_id_t) _PyCown_ThisOwnerId(void);

/* The interpreter currently owning the cown, or `_PyCown_ReleasedIpid()` when
 * no interpreter does. Safe to call from any interpreter. */
PyAPI_FUNC(_PyCown_owner_id_t) _PyCown_Owner(PyObject *cown);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */