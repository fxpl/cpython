from xml.etree.ElementTree import ElementTree, Element, XMLParser
import unittest


from . import BaseNotFreezableTest, BaseObjectTest


class TestElementTree(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=ElementTree(), **kwargs)


class TestXMLParser(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=XMLParser(), **kwargs)


class TestElement(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        obj = Element("tag", {"key": "value"})
        super().__init__(*args, obj=obj, **kwargs)

    def test_set(self):
        with self.assertRaises(NotWritableError):
            self.obj.set("key", "value")

    def test_setitem(self):
        with self.assertRaises(NotWritableError):
            self.obj["key"] = "value"

    def test_delitem(self):
        with self.assertRaises(NotWritableError):
            del self.obj["key"]

    def test_clear(self):
        with self.assertRaises(NotWritableError):
            self.obj.clear()

    def test_append(self):
        with self.assertRaises(NotWritableError):
            self.obj.append(Element("child"))

    def test_insert(self):
        with self.assertRaises(NotWritableError):
            self.obj.insert(0, Element("child"))

    def test_remove(self):
        with self.assertRaises(NotWritableError):
            self.obj.remove(Element("child"))

    def test_iter(self):
        it = self.obj.iter()
        with self.assertRaises(TypeError):
            freeze(it)


if __name__ == '__main__':
    unittest.main()
