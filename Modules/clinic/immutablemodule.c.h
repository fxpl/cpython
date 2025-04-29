/*[clinic input]
preserve
[clinic start generated code]*/

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#  include "pycore_gc.h"            // PyGC_Head
#  include "pycore_runtime.h"       // _Py_ID()
#endif


PyDoc_STRVAR(immutable_register_freezable__doc__,
"register_freezable($module, obj, /)\n"
"--\n"
"\n"
"Register a type as freezable.");

#define IMMUTABLE_REGISTER_FREEZABLE_METHODDEF    \
    {"register_freezable", (PyCFunction)immutable_register_freezable, METH_O, immutable_register_freezable__doc__},

PyDoc_STRVAR(immutable_freeze__doc__,
"freeze($module, obj, /)\n"
"--\n"
"\n"
"Freeze an object and its graph.");

#define IMMUTABLE_FREEZE_METHODDEF    \
    {"freeze", (PyCFunction)immutable_freeze, METH_O, immutable_freeze__doc__},

PyDoc_STRVAR(immutable_isfrozen__doc__,
"isfrozen($module, obj, /)\n"
"--\n"
"\n"
"Check if an object is frozen.");

#define IMMUTABLE_ISFROZEN_METHODDEF    \
    {"isfrozen", (PyCFunction)immutable_isfrozen, METH_O, immutable_isfrozen__doc__},

PyDoc_STRVAR(immutable_isfreezable__doc__,
"isfreezable($module, obj, /)\n"
"--\n"
"\n"
"Check if an object can be frozen.");

#define IMMUTABLE_ISFREEZABLE_METHODDEF    \
    {"isfreezable", (PyCFunction)immutable_isfreezable, METH_O, immutable_isfreezable__doc__},
/*[clinic end generated code: output=b26b154bf5fbe7c9 input=a9049054013a1b77]*/
