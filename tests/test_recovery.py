#!/usr/bin/env python3
"""Black-box crash/restart regressions; run with a freshly built RMDB binary.

All database files are created below a temporary directory. Tests hold an
advisory lock because the teaching server uses the fixed TCP port 8765.
"""
import argparse
import fcntl
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest


BINARY = None


class Server:
    def __init__(self, root):
        self.root = Path(root)
        self.proc = None
        self.output = None
        self.clients = []

    def start(self):
        self.output = (self.root / "server.log").open("ab")
        self.proc = subprocess.Popen([BINARY, "db"], cwd=self.root,
                                     stdout=self.output, stderr=self.output)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise AssertionError((self.root / "server.log").read_text(errors="replace"))
            try:
                return self.connect()
            except OSError:
                time.sleep(0.03)
        raise AssertionError("RMDB did not begin listening within 15 seconds")

    def connect(self):
        client = socket.create_connection(("127.0.0.1", 8765), timeout=3)
        client.settimeout(5)
        self.clients.append(client)
        return client

    def stop(self, crash=False):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        for client in self.clients:
            client.close()
        self.clients.clear()
        if self.output:
            self.output.close()
        self.proc = None


def query(client, sql):
    client.sendall(sql.encode() + b"\0")
    result = b""
    while b"\0" not in result:
        chunk = client.recv(8192)
        if not chunk:
            raise AssertionError(f"Server closed the connection for {sql!r}")
        result += chunk
    return result.split(b"\0", 1)[0].decode()


def rows(client, sql="select * from t;"):
    result = query(client, sql)
    if "Error" in result or result == "abort\n":
        raise AssertionError(result)
    values = []
    for line in result.splitlines():
        if re.match(r"^\|\s*-?\d", line):
            values.append(tuple(int(item.strip()) for item in line.strip("|").split("|")))
    return sorted(values)


def log_headers(path):
    data = Path(path).read_bytes()
    offset = 0
    result = []
    while offset + 20 <= len(data):
        header = struct.unpack_from("=iiIii", data, offset)
        if header[2] < 20 or offset + header[2] > len(data):
            break
        result.append(header)
        offset += header[2]
    return result


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rmdb-recovery-")
        self.server = Server(self.temp.name)

    def tearDown(self):
        self.server.stop(crash=True)
        self.temp.cleanup()

    def seed(self):
        client = self.server.start()
        query(client, "create table t (id int, v int);")
        query(client, "insert into t values (1, 10);")
        self.assertEqual(rows(client), [(1, 10)])
        return client

    def test_transaction_and_log_ids_continue_after_restart(self):
        self.seed()
        self.server.stop()
        before = log_headers(Path(self.temp.name) / "db/db.log")
        client = self.server.start()
        query(client, "insert into t values (2, 20);")
        self.assertEqual(rows(client), [(1, 10), (2, 20)])
        self.server.stop()
        after = log_headers(Path(self.temp.name) / "db/db.log")[len(before):]
        self.assertGreater(min(h[1] for h in after), max(h[1] for h in before))
        self.assertGreater(min(h[3] for h in after), max(h[3] for h in before))

    def test_schema_boundary_reserves_lsn_after_losing_buffered_begins(self):
        client = self.seed()
        log_path = Path(self.temp.name) / "db/db.log"
        durable_size = log_path.stat().st_size
        for _ in range(5):
            other = self.server.connect()
            query(other, "begin;")
        query(client, "create table fresh (id int, v int);")
        self.server.stop(crash=True)
        # Model CREATE metadata/epoch reaching disk before its buffered BEGIN
        # and COMMIT, along with the other clients' buffered BEGIN records.
        with log_path.open("r+b") as log:
            log.truncate(durable_size)
        client = self.server.start()
        query(client, "insert into fresh values (7, 70);")
        self.server.stop(crash=True)
        client = self.server.start()
        self.assertEqual(rows(client, "select * from fresh;"), [(7, 70)])

    def test_crashed_update_after_restart_is_not_a_previous_commit(self):
        self.seed()
        self.server.stop()
        client = self.server.start()
        query(client, "begin;")
        query(client, "update t set v = 99 where id = 1;")
        self.server.stop(crash=True)
        client = self.server.start()
        self.assertEqual(rows(client), [(1, 10)])

    def test_old_loser_does_not_erase_reused_slot_on_next_recovery(self):
        client = self.server.start()
        query(client, "create table t (id int, v int);")
        query(client, "begin;")
        query(client, "insert into t values (7, 70);")
        self.server.stop(crash=True)
        client = self.server.start()
        # Use a single new transaction (ID 0 on the original implementation)
        # so its read statements cannot accidentally commit the old loser ID.
        query(client, "begin;")
        for _ in range(8):
            self.assertEqual(rows(client), [])
        query(client, "insert into t values (8, 80);")
        self.assertEqual(rows(client), [(8, 80)])
        query(client, "commit;")
        self.server.stop(crash=True)
        client = self.server.start()
        self.assertEqual(rows(client), [(8, 80)])

    def test_disconnecting_rolls_back_and_releases_locks(self):
        client = self.seed()
        query(client, "begin;")
        query(client, "update t set v = 99 where id = 1;")
        client.shutdown(socket.SHUT_RDWR)
        client.close()
        observer = self.server.connect()
        deadline = time.monotonic() + 2
        last = None
        while time.monotonic() < deadline:
            last = query(observer, "select * from t;")
            if last != "abort\n":
                break
            time.sleep(0.02)
        self.assertNotEqual(last, "abort\n", "disconnected transaction retained its table lock")
        self.assertEqual(rows(observer), [(1, 10)])

    def test_partial_log_tail_does_not_hide_future_commits(self):
        self.seed()
        self.server.stop()
        with (Path(self.temp.name) / "db/db.log").open("ab") as log:
            log.write(b"\x01\x00\x00\x00\x02\x00\x00")
        client = self.server.start()
        query(client, "insert into t values (2, 20);")
        self.assertEqual(rows(client), [(1, 10), (2, 20)])
        self.server.stop(crash=True)
        client = self.server.start()
        self.assertEqual(rows(client), [(1, 10), (2, 20)])

    def test_aborted_insert_is_removed_even_if_its_dirty_page_was_written(self):
        client = self.server.start()
        query(client, "create table t (id int, v int);")
        query(client, "begin;")
        query(client, "insert into t values (7, 70);")
        # A clean stop writes the dirty heap. Append the real ABORT record
        # layout to model a crash after durable ABORT, before undo-page flush.
        self.server.stop()
        log_path = Path(self.temp.name) / "db/db.log"
        headers = log_headers(log_path)
        last = headers[-1]
        with log_path.open("ab") as log:
            log.write(struct.pack("=iiIii", 5, last[1] + 1, 20, last[3], last[1]))
        client = self.server.start()
        self.assertEqual(rows(client), [])

    def test_sql_error_rolls_back_explicit_transaction(self):
        client = self.seed()
        query(client, "begin;")
        query(client, "update t set v = 99 where id = 1;")
        self.assertIn("Error", query(client, "insert into t values ('bad', 20);"))
        self.assertEqual(rows(client), [(1, 10)])

    def test_syntax_error_rolls_back_and_returns_an_error(self):
        client = self.seed()
        query(client, "begin;")
        query(client, "update t set v = 99 where id = 1;")
        self.assertIn("Error", query(client, "select from;"))
        self.assertEqual(rows(client), [(1, 10)])

    def test_indexed_key_shift_can_be_rolled_back(self):
        client = self.seed()
        query(client, "insert into t values (2, 20);")
        query(client, "create index t (id);")
        query(client, "begin;")
        self.assertNotIn("Error", query(client, "update t set id = id + 1;"))
        self.assertEqual(rows(client), [(2, 10), (3, 20)])
        query(client, "rollback;")
        self.assertEqual(rows(client), [(1, 10), (2, 20)])
        self.assertEqual(rows(client, "select * from t where id = 1;"), [(1, 10)])
        self.server.stop(crash=True)
        client = self.server.start()
        self.assertEqual(rows(client), [(1, 10), (2, 20)])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args, remaining = parser.parse_known_args()
    BINARY = os.path.abspath(args.binary)
    with open("/tmp/rmdb-test-port-8765.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        unittest.main(argv=[__file__] + remaining)
