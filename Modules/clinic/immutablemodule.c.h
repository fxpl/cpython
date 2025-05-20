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

PyDoc_STRVAR(immutable_check_freezable__doc__,
"check_freezable($module, obj, /)\n"
"--\n"
"\n"
"Checks if an object is freezable and returns the reason.");

#define IMMUTABLE_CHECK_FREEZABLE_METHODDEF    \
    {"check_freezable", (PyCFunction)immutable_check_freezable, METH_O, immutable_check_freezable__doc__},
/*[clinic end generated code: output=5d19ceb1c0528c0b input=a9049054013a1b77]*/
