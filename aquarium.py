import importlib
import pkgutil
import csv
import types
from collections import Counter
from immutable import check_freezable
import inspect

MAX_DEPTH = 20
results = []
error_log = []

FREEZABLE_CHECK = {1: "VALID_BUILTIN", 2: "VALID_EXPLICIT", 3: "VALID_IMPLICIT", -1: "INVALID_NOT_FREEZABLE", -2: "INVALID_C_EXTENSIONS", -3: "ERROR"}

def get_source(ty):
    try:
        if inspect.isclass(ty):
            file = inspect.getfile(ty)
            if file:
                return file
    except Exception as e:
        pass
    return "Unknown"

def check_freezable_and_parse(ty):
    res = check_freezable(ty)

    if res in FREEZABLE_CHECK:
        verdict = FREEZABLE_CHECK[res]
        flags = ""
        reason = ""
    else:
        if res < 0:
            verdict = "INVALID_C_EXTENSIONS"
            res *= -1
        else:
            verdict = "VALID_IMPLICIT"
        flags = bin(res >> 16)
        reason = res & 0xffff

    return (verdict, flags, reason)


def walk_module(mod):
    for attr_name in dir(mod):
        try:
            attr = getattr(mod, attr_name)
        except Exception as e:
            error_log.append(("getattr", mod.__name__, attr_name, repr(e)))
            continue
        if isinstance(attr, type):
            (verdict, flags, reason) = check_freezable_and_parse(attr)
            source = get_source(attr)
            results.append((mod.__name__, attr.__module__, attr.__qualname__, verdict, flags, reason, source))

def recurse_modules(mod, depth=0):
    if depth > MAX_DEPTH:
        error_log.append(("max recursion", mod.__name__, "", ""))
        return
    walk_module(mod)
    if not hasattr(mod, '__path__'):
        return
    for _, name, _ in pkgutil.iter_modules(mod.__path__, mod.__name__ + "."):
        try:
            child = importlib.import_module(name)
        except Exception as e:
            error_log.append(("import", name, "", repr(e)))
            continue
        recurse_modules(child, depth + 1)

def main():
    root_mod = importlib.import_module("csv")
    recurse_modules(root_mod)
    with open("output.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["parent_module", "type_module", "type_name", "verdict", "flags", "reason", "source"])
        writer.writerows(results)
    # Print error stats
    error_types = Counter(err[0] for err in error_log)
    for etype, count in error_types.items():
        print(f"{etype} errors: {count}")

main()

