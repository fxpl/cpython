from _multiprocessing import SemLock

from .test_common import BaseNotFreezableTest

SEMAPHORE = 1
SEM_VALUE_MAX = SemLock.SEM_VALUE_MAX

class TestSemLock(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=SemLock(SEMAPHORE, 0, SEM_VALUE_MAX, "mock", True), **kwargs)
