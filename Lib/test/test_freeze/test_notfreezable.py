import unittest

class Mock(notfreezable):
    def __init__(self, name):
        self.name = name


class TestNotFreezable(unittest.TestCase):
    def test_not_freezable(self):
        # Test that the NotFreezable class raises an exception when trying to freeze
        mock = Mock("test")
        with self.assertRaises(TypeError):
            freeze(mock)
