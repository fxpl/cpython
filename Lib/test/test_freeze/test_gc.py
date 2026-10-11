from gc import collect
import unittest
from immutable import deep_freeze

class GCInteropTest(unittest.TestCase):
  def test_collect(self):
    # Make an object
    a = {}
    # Change generation
    collect()
    # Freeze it
    deep_freeze(a)
    # f
    collect()
