import importlib
import pkgutil
import csv
import types
from collections import Counter
from immutable import check_freezable
import inspect
import os

results = []

MAX_MOD_DEPTH = 20
MAX_CLASS_DEPTH = 10
FREEZABLE_CHECK = {1: "VALID_BUILTIN", 2: "VALID_EXPLICIT", 3: "VALID_IMPLICIT", -1: "INVALID_NOT_FREEZABLE", -2: "INVALID_C_EXTENSIONS", -3: "ERROR"}
FLAG_HEADER = ["nb", "nb_index", "nb_int", "nb_float", "sq", "sq_item", "mp", "mp_subscript", "am", "am_await", "am_aiter", "am_anext", "am_send", "bf", "bf_getbuffer", "tp_getattr", "tp_setattr", "tp_methods", "tp_setattro_default", "tp_setattro", "tp_getattro_default", "tp_getattro", "tp_getset", "tp_getsets_full", "tp_getsets_weakref_only", "tp_getsets_dict_only", "tp_str", "tp_str_default", "tp_repr", "tp_repr_default", "tp_richcompare", "tp_richcompare_default", "tp_hash", "tp_hash_default"]

stats = []

STAT_HEADER = ["mod_name", "type_ctn", "verdict_valid_builtin", "verdict_valid_explicit", "verdict_valid_implicit", "verdict_invalid_not_freezable", "verdict_invalid_c_extensions", "verdict_error", "get_src_err", "no_file_err", "get_attr_err", "max_recursion", "import_mod_err"]
STAT_MOD_NAME = 0
STAT_TYPE_CTN = 1
STAT_VERDICT_INDEX = {"VALID_BUILTIN": 2, "VALID_EXPLICIT": 3, "VALID_IMPLICIT": 4, "INVALID_NOT_FREEZABLE": 5, "INVALID_C_EXTENSIONS": 6, "ERROR": 7}
STAT_GET_SRC_ERR = 8
STAT_NO_FILE_ERR = 9
STAT_GET_ATTR_ERR = 10
STAT_MAX_RECURSION = 11
STAT_IMPORT_MOD_ERR = 12
STAT_SIZE = 12

def get_source(ty):
    global stats

    try:
        if inspect.isclass(ty):
            file = inspect.getfile(ty)
            if file:
                return file
            else:
                stats[STAT_NO_FILE_ERR] += 1
    except Exception as e:
        stats[STAT_GET_SRC_ERR] += 1
    return "Unknown"

def check_freezable_and_parse(ty):
    res = check_freezable(ty)

    if res in FREEZABLE_CHECK:
        verdict = FREEZABLE_CHECK[res]
        flags = 0
        reason = ""
    else:
        if res < 0:
            verdict = "INVALID_C_EXTENSIONS"
            res *= -1
        else:
            verdict = "VALID_IMPLICIT"
        flags = res >> 16
        reason = res & 0xffff

    return (verdict, flags, reason)

def parse_flags(flags):
    flags_list = [0] * len(FLAG_HEADER)
    for index in range(len(FLAG_HEADER)):
        flags_list[index] = (flags >> index) & 0x1

    return flags_list

def get_ty_name_with_super(ty):
    type_name = ""
    for idx, super in enumerate(reversed(ty.__mro__)):
        # Skip object
        if idx == 0:
            continue
        if idx == 1:
            type_name = f"{super.__qualname__}"
        else:
            type_name = f"{super.__qualname__}({type_name})"

    return ty.__module__ + "." + type_name

def walk_type(mod_name, ty):
    stats[STAT_TYPE_CTN] += 1
    source = get_source(ty)

    for idx, super in enumerate(reversed(ty.__mro__)):
        # Skip object
        if idx == 0:
            continue

        verdict_ty = super
        verdict_super = super == ty

        (verdict, flags, reason) = check_freezable_and_parse(verdict_ty)

        if not verdict.startswith("VALID"):
            break

    data = [mod_name, get_ty_name_with_super(ty), not verdict_super, get_ty_name_with_super(verdict_ty), verdict, reason, source]
    data += parse_flags(flags)
    results.append(data)
    stats[STAT_VERDICT_INDEX[verdict]] += 1

def walk_module(mod_name, mod):
    global stats

    for attr_name in dir(mod):
        try:
            attr = getattr(mod, attr_name)
        except Exception as e:
            stats[STAT_GET_ATTR_ERR] += 1
            continue

        if isinstance(attr, type):
            walk_type(mod_name, attr)

def recurse_modules(parent_name, mod, depth=0):
    global stats

    if len(parent_name) == 0:
        mod_name = mod.__name__
    else:
        mod_name = parent_name + mod.__name__ + "."
    if depth > MAX_MOD_DEPTH:
        stats[STAT_MAX_RECURSION] += 1
        return

    walk_module(mod_name, mod)

    if not hasattr(mod, '__path__'):
        return
    for _, name, _ in pkgutil.iter_modules(mod.__path__, mod.__name__ + "."):
        try:
            child = importlib.import_module(name)
        except Exception as e:
            stats[STAT_IMPORT_MOD_ERR] += 1
            continue
        recurse_modules(mod_name, child, depth + 1)

def walki_talky(mod_name):
    global stats

    root_mod = importlib.import_module(mod_name)
    stats = [0] * STAT_SIZE
    stats[STAT_MOD_NAME] = mod_name

    recurse_modules("", root_mod)

    try:
        os.mkdir("output")
    except Exception as e:
        pass

    with open(f"./output/{mod_name}_types.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["module", "type", "super_failed", "type_verdict", "verdict", "reason", "source"] + FLAG_HEADER)
        writer.writerows(results)

    with open(f"./output/{mod_name}_stats.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(STAT_HEADER)
        writer.writerow(stats)

def main():
    walki_talky("csv")

main()

