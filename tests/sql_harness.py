"""Run the real RMDB server in a disposable directory; never touch user databases."""
import fcntl
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


class Server:
    def __init__(self, binary):
        self.binary = str(Path(binary).resolve())
        self.temp = tempfile.TemporaryDirectory(prefix="rmdb-regression-")
        self.root = Path(self.temp.name)
        self.process = None
        self.clients = []
        self.lock = open("/tmp/rmdb-test-port-8765.lock", "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX)

    def start(self):
        self.log = open(self.root / "server.log", "ab")
        self.process = subprocess.Popen([self.binary, "db"], cwd=self.root,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError((self.root / "server.log").read_text(errors="replace"))
            try:
                client = self.connect()
                return client
            except (ConnectionRefusedError, OSError):
                time.sleep(.03)
        raise AssertionError("RMDB did not start within 12 seconds")

    def connect(self):
        client = socket.create_connection(("127.0.0.1", 8765), timeout=5)
        client.settimeout(5)
        self.clients.append(client)
        return client

    def query(self, client, sql, *, error=False):
        client.sendall(sql.encode() + b"\0")
        result = bytearray()
        while b"\0" not in result:
            chunk = client.recv(65536)
            if not chunk:
                raise AssertionError("RMDB closed connection during " + sql)
            result.extend(chunk)
        text = result.split(b"\0", 1)[0].decode()
        failed = "Error:" in text or text.strip() in ("abort", "failure")
        if error:
            if not failed:
                raise AssertionError(f"Expected SQL failure: {sql}\n{text}")
        elif failed:
            raise AssertionError(f"Unexpected SQL failure: {sql}\n{text}")
        return text

    def rows(self, client, sql):
        # The grading file preserves full BIGINT/DATETIME values; network output
        # deliberately truncates display columns to 16 characters.
        output = self.root / "db" / "output.txt"
        offset = output.stat().st_size if output.exists() else 0
        self.query(client, sql)
        with output.open() as stream:
            stream.seek(offset)
            lines = stream.read().splitlines()
        return [tuple(part.strip() for part in line.split("|")[1:-1])
                for line in lines if line.startswith("|")][1:]

    def stop(self, crash=False):
        for client in self.clients:
            client.close()
        self.clients.clear()
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if crash else signal.SIGINT)
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if hasattr(self, "log"):
            self.log.close()

    def close(self):
        self.stop()
        self.temp.cleanup()
        self.lock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
