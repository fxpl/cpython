# import unittest
from immutable import freeze

class QName:
    def foo(self):
        return QName
    def bar(self):
        return QName

freeze(QName)
