"""Exercise TCP message framing and orderly shutdown with live transactions."""
import signal
import socket
import struct
import sys
import time
import unittest

from sql_harness import Server

BINARY = sys.argv.pop(1) if len(sys.argv) > 1 else "build/bin/rmdb"


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.server = Server(BINARY)
        self.addCleanup(self.server.close)
        self.client = self.server.start()

    def test_fragmented_request_waits_for_terminator(self):
        self.client.sendall(b"create ta")
        self.client.settimeout(.08)
        with self.assertRaises(socket.timeout):
            self.client.recv(4096)
        self.client.settimeout(5)
        self.client.sendall(b"ble t (id int);\0")
        self.assertEqual(self.client.recv(4096), b"\0")
        self.server.query(self.client, "insert into t values (1);")
        self.assertEqual(self.server.rows(self.client, "select * from t;"), [("1",)])

    def test_coalesced_requests_each_receive_a_response(self):
        self.client.sendall(b"create table t (id int);\0insert into t values (1);\0")
        data = bytearray()
        while data.count(0) < 2:
            data.extend(self.client.recv(4096))
        self.assertEqual(data, b"\0\0")
        self.assertEqual(self.server.rows(self.client, "select * from t;"), [("1",)])

    def test_select_lock_conflict_has_a_terminated_abort_response(self):
        self.server.query(self.client, "create table t (id int);")
        self.server.query(self.client, "insert into t values (1);")
        self.server.query(self.client, "begin;")
        self.server.query(self.client, "update t set id=2;")
        reader = self.server.connect()
        reader.settimeout(1)
        self.assertEqual(self.server.query(reader, "select * from t;", error=True), "abort\n")
        self.server.query(self.client, "rollback;")
        self.assertEqual(self.server.rows(reader, "select * from t;"), [("1",)])

    def test_graceful_shutdown_aborts_active_transaction_before_exit(self):
        self.server.query(self.client, "create table t (id int);")
        self.server.query(self.client, "begin;")
        self.server.query(self.client, "insert into t values (9);")
        observer = self.server.connect()
        self.server.query(observer, "show tables;")
        self.server.process.send_signal(signal.SIGINT)
        self.assertEqual(self.server.process.wait(timeout=8), 0)
        data = (self.server.root / "db" / "db.log").read_bytes()
        offset = 0
        transactions = {}
        while offset < len(data):
            kind, lsn, length, tid, previous = struct.unpack_from("=iiIii", data, offset)
            transactions[tid] = kind
            offset += length
        # LogType::commit = 4, LogType::ABORT = 5.
        self.assertTrue(all(kind in (4, 5) for kind in transactions.values()), transactions)
        self.server.stop()
        self.client = self.server.start()
        self.assertEqual(self.server.rows(self.client, "select * from t;"), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
