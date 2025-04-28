from hashlib import blake2b, blake2s
import unittest


class TestHashlib(unittest.TestCase):
    def test_blake2b(self):
        h = blake2b(digest_size=32)
        h.update(b'Hello world')
        freeze(h)
        with self.assertRaises(TypeError):
            h.update(b'!')

    def test_blake2s(self):
        h = blake2s(digest_size=32)
        h.update(b'Hello world')
        freeze(h)
        with self.assertRaises(TypeError):
            h.update(b'!')
