/*[clinic input]
preserve
[clinic start generated code]*/

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
/*[clinic end generated code: output=580876fead975241 input=a9049054013a1b77]*/
