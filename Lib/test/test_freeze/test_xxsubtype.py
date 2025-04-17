import unittest
from test.support import import_helper

xxsubtype = import_helper.import_module('xxsubtype')

class xxsubtypelib(unittest.TestCase):

    def test_spamdict_setstate(self):
        import xxsubtype as spam
        a = spam.spamlist()

        freeze(a)

        with self.assertRaises(NotWritableError):
            a.setstate(17)

    def test_spamdict_setstate(self):
        import xxsubtype as spam
        a = spam.spamdict()

        freeze(a)

        with self.assertRaises(NotWritableError):
            a.setstate(17)
