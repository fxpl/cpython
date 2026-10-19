#ifndef Py_INTERNAL_MUTEXOBJECT_H
#define Py_INTERNAL_MUTEXOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "exports.h"

PyAPI_DATA(PyTypeObject) _PyMutex_Type;

typedef struct _PyMutexObject _PyMutexObject;
#define _PyMutexObject_CAST(op) _Py_CAST(_PyMutexObject*, op)
#define _PyMutexObject_Check(x) Py_IS_TYPE((x), &_PyMutex_Type)


typedef uintptr_t _PyPyrona_owner_id_t;

#ifdef _Py_PYRONA_INTERPRETER_SHARING
// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the mutex.
#define _Py_PYRONA_RELEASED_OWNER_ID ((_PyPyrona_owner_id_t)0xff00ff00ff00ff00LL)
#else
#define _Py_PYRONA_RELEASED_OWNER_ID _Py_UNOWNED_TID
#endif


PyAPI_FUNC(_PyPyrona_owner_id_t) _PyPyrona_ThisOwnerId(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_MUTEXOBJECT_H */