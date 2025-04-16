import asyncio
import unittest


class TestFuture(unittest.TestCase):
    def test_future(self):
        async def set_after(fut, delay, value):
            await asyncio.sleep(delay)
            fut.set_result(value)

        async def main():
            loop = asyncio.get_running_loop()
            fut = loop.create_future()
            loop.create_task(
                set_after(fut, .1, '... world'))

            with self.assertRaises(TypeError):
                freeze(fut)

            await fut

        asyncio.run(main())
