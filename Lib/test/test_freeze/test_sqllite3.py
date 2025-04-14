from test.support import import_helper

import_helper.import_module('_sqlite3')

import sqlite3

from . import BaseNotFreezableTest


class TestConnection(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=sqlite3.connect(':memory:'), **kwargs)

    def tearDown(self):
        self.obj.close()


class TestCursor(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        self.con = sqlite3.connect(':memory:')
        super().__init__(*args, obj=self.con.cursor(), **kwargs)

    def tearDown(self):
        self.con.close()


class TestBlob(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        self.con = sqlite3.connect(':memory:')
        self.con.execute("CREATE TABLE test(blob_col blob)")
        self.con.execute("INSERT INTO test(blob_col) VALUES(zeroblob(13))")

        blob = self.con.blobopen("test", "blob_col", 1)
        super().__init__(*args, obj=blob, **kwargs)

    def tearDown(self):
        self.con.close()
