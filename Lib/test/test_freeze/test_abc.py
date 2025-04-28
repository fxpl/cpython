from abc import ABC, abstractmethod


from . import BaseObjectTest


class TestABC(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        class A(ABC):
            @abstractmethod
            def foo(self):
                pass

        class B(A):
            def foo(self):
                print("foo")

        self.A = A
        super().__init__(*args, obj=B(), **kwargs)

    def test_abstract_immutable(self):
        self.assertTrue(isimmutable(self.A))

    def test_register(self):
        class C(ABC):
            @abstractmethod
            def bar(self):
                pass

        with self.assertRaises(TypeError):
            self.A.register(C)

    def test_invalid_cache(self):
        class D(ABC):
            @abstractmethod
            def baz(self):
                pass

        class E(ABC):
            @abstractmethod
            def qux(self):
                pass

        self.assertFalse(issubclass(E, D))

        D.register(E)

        class F(D):
            def baz(self):
                pass

        x = F()
        freeze(x)
        self.assertTrue(isimmutable(D))
        with self.assertRaises(TypeError):
            # the caches are invalidated but cannot be updated
            # because the class is frozen
            isinstance(x, D)

    def test_valid_cache(self):
        class DD(ABC):
            @abstractmethod
            def baz(self):
                pass

        class EE(ABC):
            @abstractmethod
            def qux(self):
                pass

        DD.register(EE)

        self.assertTrue(issubclass(EE, DD))

        class FF(DD):
            def baz(self):
                pass

        x = FF()
        freeze(x)
        self.assertTrue(isimmutable(DD))
        self.assertTrue(isinstance(x, DD))
