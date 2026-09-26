"""Native WinHTTP and actual-entrypoint tests; private ports, no GUI or real Holder."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

PROBE, LAUNCHER, CHILD = [Path(sys.argv.pop(1)).resolve() for _ in range(3)]
PONG = b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong"


class Server:
    def __init__(self, chunks=(), start_when=None):
        self.chunks = chunks
        self.start_when = start_when
        self.stop = threading.Event()
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.port = self.socket.getsockname()[1]
        self.requests = []
        self.workers = []
        self.errors = []
        if start_when is None:
            self.socket.listen()
        self.socket.settimeout(.1)
        self.thread = threading.Thread(target=self.serve)

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        if self.start_when is not None:
            while not self.start_when.exists():
                if self.stop.wait(.01):
                    return
            self.socket.listen()
        while not self.stop.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            worker = threading.Thread(target=self.respond, args=(connection,))
            self.workers.append(worker)
            worker.start()

    def respond(self, connection):
        with connection:
            connection.settimeout(1)
            request = b""
            try:
                while b"\r\n\r\n" not in request:
                    data = connection.recv(4096)
                    if not data:
                        return
                    request += data
                    if len(request) > 8192:
                        self.errors.append("oversized request")
                        return
                self.requests.append(request)
                for delay, data in self.chunks:
                    if self.stop.wait(delay):
                        return
                    connection.sendall(data)
            except OSError:
                # Cancellation, early rejection, and deliberate peer closure
                # are normal test outcomes.
                pass

    def __exit__(self, *args):
        self.stop.set()
        self.thread.join(2)
        self.socket.close()
        for worker in self.workers:
            worker.join(2)
        if self.thread.is_alive() or any(w.is_alive() for w in self.workers):
            raise AssertionError("fixture thread did not stop")
        if self.errors:
            raise AssertionError(self.errors)


class ProbeTests(unittest.TestCase):
    def probe(self, port, expected, timeout=500, repetitions=1):
        result = subprocess.run([PROBE, str(port), str(timeout), str(repetitions)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = [line.split() for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), repetitions)
        for outcome, elapsed in rows:
            self.assertEqual(outcome, expected, result.stdout)
            self.assertLess(int(elapsed), timeout + 250, result.stdout)

    def response(self, data, expected="incompatible"):
        with Server([(0, data)]) as server:
            self.probe(server.port, expected)
            self.assertTrue(server.requests)
            self.assertTrue(server.requests[0].startswith(b"GET /ping HTTP/1.1\r\n"))

    def test_valid_content_length(self):
        self.response(PONG, "healthy")

    def test_close_framing(self):
        self.response(b"HTTP/1.0 200 OK\r\n\r\npong", "healthy")

    def test_chunked_framing(self):
        self.response(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      b"2\r\npo\r\n2\r\nng\r\n0\r\n\r\n", "healthy")

    def test_fragmented_headers_and_body(self):
        with Server([(.002, bytes([byte])) for byte in PONG]) as server:
            self.probe(server.port, "healthy", timeout=1500)

    def test_wrong_bodies(self):
        for body in (b"", b"ping", b"pon", b"pong\n", b"pong-extra", b"PONG", b"po\x00g"):
            with self.subTest(body=body):
                self.response(b"HTTP/1.1 200 OK\r\nContent-Length: " +
                              str(len(body)).encode() + b"\r\n\r\n" + body)

    def test_non_200(self):
        for status in (b"204 No Content", b"404 Not Found", b"503 Unavailable"):
            with self.subTest(status=status):
                self.response(b"HTTP/1.1 " + status + b"\r\nContent-Length: 0\r\n\r\n")

    def test_redirect_is_not_followed(self):
        with Server([(0, PONG)]) as target:
            self.response(b"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" +
                          str(target.port).encode() + b"/ping\r\nContent-Length: 0\r\n\r\n")
            self.assertEqual(target.requests, [])

    def test_malformed_status(self):
        self.response(b"HTTP/1.1 not-a-status\r\n\r\n")

    def test_oversized_headers(self):
        self.response(b"HTTP/1.1 200 OK\r\nX-Large: " + b"a" * 16384 + b"\r\n\r\npong")

    def test_oversized_body(self):
        self.response(b"HTTP/1.1 200 OK\r\nContent-Length: 16384\r\n\r\n" + b"x" * 16384)

    def test_refused_connection(self):
        # Reserve the port without listening, so no other service can claim it.
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            self.probe(reserved.getsockname()[1], "unavailable")

    def test_immediate_close(self):
        with Server() as server:
            self.probe(server.port, "incompatible")

    def test_silent_peer_and_cancellation_cleanup(self):
        with Server([(5, b"")]) as server:
            self.probe(server.port, "unavailable", timeout=50, repetitions=40)

    def test_trickling_headers_obey_total_deadline(self):
        with Server([(.06, bytes([byte])) for byte in PONG]) as server:
            self.probe(server.port, "unavailable", timeout=200)

    def test_trickling_body_obeys_total_deadline(self):
        chunks = [(0, b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n")]
        chunks += [(.15, bytes([byte])) for byte in b"pong"]
        with Server(chunks) as server:
            self.probe(server.port, "unavailable", timeout=200)

    def test_zero_budget(self):
        with Server([(0, PONG)]) as server:
            self.probe(server.port, "unavailable", timeout=0)
            self.assertEqual(server.requests, [])

    def test_success_and_rejection_cleanup(self):
        for response, expected in ((PONG, "healthy"),
                                   (PONG.replace(b"pong", b"nope"), "incompatible")):
            with self.subTest(expected=expected), Server([(0, response)]) as server:
                self.probe(server.port, expected, repetitions=80)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="holder windows ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "bin").mkdir()
        shutil.copy2(LAUNCHER, self.root / "Holder.exe")
        for name in ("holderd.exe", "holder-desktop.exe"):
            shutil.copy2(CHILD, self.root / "bin" / name)
        self.env = dict(os.environ, LOCALAPPDATA=str(self.root / "local"), TEMP=str(self.root))
        self.backend = self.root / "holderd.exe.started"
        self.desktop = self.root / "holder-desktop.exe.started"

    def launch(self, port, success):
        result = subprocess.run([self.root / "Holder.exe", str(port)], cwd=self.root.parent,
                                env=self.env, capture_output=True, text=True, timeout=12)
        self.assertEqual(result.returncode, 0 if success else 1, result.stdout + result.stderr)
        if success:
            deadline = time.monotonic() + 2
            while not self.desktop.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(self.desktop.exists())
        else:
            self.assertFalse(self.desktop.exists())
            self.assertIn("expected Holder ping response", result.stderr)
            self.assertIn("expected Holder ping response",
                          (self.root / "local" / "holder" / "launcher.log").read_text())

    def test_healthy_reuse(self):
        with Server([(0, PONG)]) as server:
            self.launch(server.port, True)
        self.assertFalse(self.backend.exists())

    def test_collision_starts_neither_child(self):
        with Server([(0, PONG.replace(b"pong", b"nope"))]) as server:
            self.launch(server.port, False)
        self.assertFalse(self.backend.exists())

    def test_absent_backend_is_started(self):
        with Server([(0, PONG)], start_when=self.backend) as server:
            self.launch(server.port, True)
        self.assertTrue(self.backend.exists())

    def test_collision_during_readiness_stops_desktop(self):
        with Server([(0, PONG.replace(b"pong", b"nope"))], start_when=self.backend) as server:
            self.launch(server.port, False)
        self.assertTrue(self.backend.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
