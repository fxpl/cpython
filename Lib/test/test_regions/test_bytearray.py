import unittest

from regions import Region


class BytearraySubclass(bytearray):
    pass


class BytesFormatter:
    def __bytes__(self):
        return b"x"


class TestRegionBytearray(unittest.TestCase):
    def make_region_bytearray(self, value=b"abc"):
        r = Region()
        r.b = bytearray(value)
        self.assertTrue(r.owns(r.b))
        return r

    def assert_neutral(self, operation, value=b"abc"):
        r = self.make_region_bytearray(value)
        base_lrc = r._lrc
        operation(r.b)
        self.assertEqual(r._lrc, base_lrc)

    def test_sequence_and_mapping_neutral_slots(self):
        operations = [
            len,
            lambda b: b + bytearray(b"d"),
            lambda b: b * 2,
            lambda b: b[0],
            lambda b: b[:2],
            lambda b: b.__contains__(ord("a")),
            lambda b: b.__setitem__(0, ord("z")),
            lambda b: b.__setitem__(slice(1, 2), b"yy"),
            lambda b: b.__delitem__(slice(1, 2)),
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_neutral(operation)

    def test_inplace_concat_returns_tracked_self(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        result = r.b.__iadd__(b"d")
        self.assertIs(result, r.b)
        self.assertEqual(r._lrc, base_lrc + 1)
        result = None
        self.assertEqual(r._lrc, base_lrc)

        r = self.make_region_bytearray()
        base_lrc = r._lrc
        with self.assertRaises(BufferError):
            r.b.__iadd__(r.b)
        self.assertEqual(r._lrc, base_lrc)

    def test_inplace_repeat_returns_tracked_self(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        result = r.b.__imul__(2)
        self.assertIs(result, r.b)
        self.assertEqual(r._lrc, base_lrc + 1)
        result = None
        self.assertEqual(r._lrc, base_lrc)

        r = self.make_region_bytearray()
        base_lrc = r._lrc
        result = r.b.__imul__(1)
        self.assertIs(result, r.b)
        self.assertEqual(r._lrc, base_lrc + 1)
        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_repr_str_compare_and_mod_are_neutral(self):
        operations = [
            repr,
            str,
            lambda b: b == b"abc",
            lambda b: b != b"abc",
            lambda b: b"%s" % b"x",
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_neutral(operation)

    def test_mod_notifies_formatter_type_use(self):
        r = self.make_region_bytearray(b"%s")
        r.formatter = BytesFormatter()
        base_lrc = r._lrc
        r.b % r.formatter
        self.assertEqual(r._lrc, base_lrc)
        self.assertTrue(r.is_dirty)

    def test_constructor_init_and_resize_paths_are_neutral(self):
        self.assert_neutral(lambda b: b.__init__([65, 66, 67]))
        self.assert_neutral(lambda b: b.__init__(b"xyz"))
        self.assert_neutral(lambda b: b.__init__("xyz", "ascii"))
        self.assert_neutral(lambda b: b.resize(5))
        self.assert_neutral(lambda b: b.clear())

    def test_search_prefix_suffix_and_predicate_methods_are_neutral(self):
        operations = [
            lambda b: b.find(b"b"),
            lambda b: b.count(b"b"),
            lambda b: b.index(b"b"),
            lambda b: b.rfind(b"b"),
            lambda b: b.rindex(b"b"),
            lambda b: b.startswith(b"a"),
            lambda b: b.endswith(b"c"),
            lambda b: b.isalnum(),
            lambda b: b.isalpha(),
            lambda b: b.isascii(),
            lambda b: b.isdigit(),
            lambda b: b.islower(),
            lambda b: b.isspace(),
            lambda b: b.istitle(),
            lambda b: b.isupper(),
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_neutral(operation)

    def test_copying_transform_methods_are_neutral(self):
        operations = [
            lambda b: b.copy(),
            lambda b: b.removeprefix(b"a"),
            lambda b: b.removesuffix(b"c"),
            lambda b: b.translate(bytes.maketrans(b"abc", b"ABC")),
            lambda b: bytearray.maketrans(b"a", b"b"),
            lambda b: b.replace(b"b", b"x"),
            lambda b: b.split(b"b"),
            lambda b: b.rsplit(b"b"),
            lambda b: b.partition(b"b"),
            lambda b: b.rpartition(b"b"),
            lambda b: b.splitlines(),
            lambda b: b.strip(),
            lambda b: b.lstrip(),
            lambda b: b.rstrip(),
            lambda b: b.capitalize(),
            lambda b: b.center(5),
            lambda b: b.expandtabs(),
            lambda b: b.swapcase(),
            lambda b: b.title(),
            lambda b: b.upper(),
            lambda b: b.lower(),
            lambda b: b.zfill(5),
            lambda b: b.decode("ascii"),
            lambda b: b.join([b"x", b"y"]),
            lambda b: b.ljust(5),
            lambda b: b.rjust(5),
            lambda b: b.hex(),
            lambda b: b.__reduce__(),
            lambda b: b.__reduce_ex__(4),
            lambda b: b.__sizeof__(),
            lambda b: b.__alloc__(),
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_neutral(operation)

    def test_mutating_methods_are_neutral(self):
        operations = [
            lambda b: b.reverse(),
            lambda b: b.insert(1, ord("x")),
            lambda b: b.append(ord("x")),
            lambda b: b.pop(),
            lambda b: b.remove(ord("b")),
        ]
        for operation in operations:
            with self.subTest(operation=operation):
                self.assert_neutral(operation)

    def test_self_referential_slice_assignment_is_neutral(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        r.b[1:2] = r.b
        self.assertEqual(r._lrc, base_lrc)

    def test_extend_is_lrc_neutral_and_notifies_iterable_type_use(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        r.b.extend([120, 121])
        self.assertEqual(r._lrc, base_lrc)
        self.assertTrue(r.is_dirty)

    def test_iterator_lrc_creation_yield_exhaustion_and_abandonment(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        it = iter(r.b)
        self.assertEqual(r._lrc, base_lrc + 1)
        value = next(it)
        self.assertEqual(r._lrc, base_lrc + 1)
        value = None
        self.assertEqual(r._lrc, base_lrc + 1)
        list(it)
        self.assertEqual(r._lrc, base_lrc)

        it = iter(r.b)
        self.assertEqual(r._lrc, base_lrc + 1)
        it = None
        self.assertEqual(r._lrc, base_lrc)

    def test_iterator_methods_are_neutral(self):
        r = self.make_region_bytearray()
        it = iter(r.b)
        base_lrc = r._lrc
        it.__length_hint__()
        it.__setstate__(1)
        self.assertEqual(r._lrc, base_lrc)

    def test_iterator_self_iter_returns_tracked_self(self):
        r = self.make_region_bytearray()
        it = iter(r.b)
        base_lrc = r._lrc
        result = iter(it)
        self.assertIs(result, it)
        self.assertEqual(r._lrc, base_lrc)
        result = None
        self.assertEqual(r._lrc, base_lrc)

        r.it = iter(r.b)
        base_lrc = r._lrc
        result = iter(r.it)
        self.assertIs(result, r.it)
        self.assertEqual(r._lrc, base_lrc + 1)
        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_buffer_get_and_release_are_neutral(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        view = memoryview(r.b)
        self.assertEqual(r._lrc, base_lrc)
        view.release()
        self.assertEqual(r._lrc, base_lrc)

    def test_fromhex_exact_and_subclass_paths_are_neutral(self):
        r = Region()
        r.hex_string = "61 62 63"
        base_lrc = r._lrc
        bytearray.fromhex(r.hex_string)
        self.assertEqual(r._lrc, base_lrc)

        r = Region()
        r.hex_string = "61 62 63"
        base_lrc = r._lrc
        BytearraySubclass.fromhex(r.hex_string)
        self.assertEqual(r._lrc, base_lrc)
        self.assertTrue(r.is_dirty)

    def test_failure_paths_clean_stack_references(self):
        r = self.make_region_bytearray()
        base_lrc = r._lrc
        with self.assertRaises(TypeError):
            r.b.extend([1, object()])
        self.assertEqual(r._lrc, base_lrc)
        with self.assertRaises(TypeError):
            r.b.__init__([1, object()])
        self.assertEqual(r._lrc, base_lrc)


if __name__ == "__main__":
    unittest.main()
