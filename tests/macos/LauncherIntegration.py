"""Run the launcher entrypoint in private installations, with no GUI or real daemon."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

DRIVER = Path(sys.argv.pop(1)).resolve()
FIXTURE = r'''
import json, os, pathlib, socket, sys
role = pathlib.Path(sys.argv[0]).name
record = pathlib.Path(os.environ['HOLDER_TEST_RECORD']) / role
record.write_text(json.dumps({'pid': os.getpid(), 'cwd': os.getcwd(),
    'env': {k: os.environ.get(k) for k in ['GSETTINGS_SCHEMA_DIR', 'GIO_MODULE_DIR',
    'GDK_PIXBUF_MODULE_FILE', 'GTK_PATH', 'XDG_DATA_DIRS', 'ENCHANT_CONFIG_DIR', 'DICPATH']}}))
if role == 'holder-desktop':
    sys.exit(23)
if os.environ.get('HOLDER_TEST_EXIT'):
    sys.exit(7)
server = socket.socket(fileno=int(os.environ['HOLDER_TEST_SOCKET']))
server.listen()
while True:
    conn, _ = server.accept()
    with conn:
        conn.settimeout(2)
        try:
            data = b''
            while b'\r\n\r\n' not in data and len(data) < 1024:
                part = conn.recv(1024)
                if not part: break
                data += part
            conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong')
        except OSError:
            pass
'''

class LauncherIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="holder-launcher-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve() / "Zoë's 持有 apps"
        self.base.mkdir()
        self.home = self.base / 'home'
        self.home.mkdir()
        self.records = self.base / 'records'
        self.records.mkdir()
        self.sock = socket.socket()
        self.addCleanup(self.sock.close)
        # Reserve but do not listen: the first probe is refused until the fake backend starts.
        self.sock.bind(('127.0.0.1', 0))
        self.env = dict(os.environ, HOME=str(self.home),
                        HOLDER_TEST_PORT=str(self.sock.getsockname()[1]),
                        HOLDER_TEST_SOCKET=str(self.sock.fileno()),
                        HOLDER_TEST_RECORD=str(self.records))
        self.env.pop('HOLDER_TEST_EXIT', None)

    def layout(self, bundle=True):
        if bundle:
            contents = self.base / 'Renamed Holder.app' / 'Contents'
            self.launcher = contents / 'MacOS' / 'Holder'
            self.root = contents / 'Resources'
            self.bin = self.root / 'bin'
        else:
            self.root = self.base / 'development'
            self.bin = self.root / 'bin'
            self.launcher = self.bin / 'Holder'
        self.bin.mkdir(parents=True)
        self.launcher.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(DRIVER, self.launcher)
        for role in ('holderd', 'holder-desktop'):
            path = self.bin / role
            path.write_text('#!' + sys.executable + '\n' + FIXTURE)
            path.chmod(0o755)

    def run_launcher(self):
        # A backend inherits stderr and survives the launcher: a file avoids waiting
        # for pipe EOF and avoids pipe-buffer deadlocks on failure diagnostics.
        with tempfile.TemporaryFile(mode='w+t') as errors:
            process = subprocess.Popen([str(self.launcher)], cwd=self.home, env=self.env,
                                       pass_fds=(self.sock.fileno(),), start_new_session=True,
                                       stdout=subprocess.DEVNULL, stderr=errors, text=True)
            try:
                process.wait(timeout=6)
            finally:
                # Our private process group contains only this launch and its fixtures.
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.wait(timeout=3)
            errors.seek(0)
            return process.pid, process.returncode, errors.read()

    def record(self, role):
        return json.loads((self.records / role).read_text())

    def assert_environment(self, role):
        record = self.record(role)
        self.assertEqual(record['cwd'], str(self.root))
        expected = {
            'GSETTINGS_SCHEMA_DIR': 'share/glib-2.0/schemas',
            'GIO_MODULE_DIR': 'lib/gio/modules',
            'GDK_PIXBUF_MODULE_FILE': 'lib/gdk-pixbuf-2.0/2.10.0/loaders.cache',
            'GTK_PATH': 'lib/gtk-4.0', 'XDG_DATA_DIRS': 'share',
            'ENCHANT_CONFIG_DIR': 'share/enchant-2', 'DICPATH': 'share/enchant/hunspell'}
        self.assertEqual(record['env'], {k: str(self.root / v) for k, v in expected.items()})

    def test_bundle_cold_start_and_exec_handoff(self):
        self.layout()
        pid, code, error = self.run_launcher()
        self.assertEqual(code, 23, error)
        self.assertEqual(self.record('holder-desktop')['pid'], pid)
        self.assertNotEqual(self.record('holderd')['pid'], pid)
        self.assert_environment('holderd')
        self.assert_environment('holder-desktop')

    def test_developer_cold_start(self):
        self.layout(False)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 23, error)
        self.assert_environment('holder-desktop')

    def test_reuse_existing_backend(self):
        self.layout()
        self.sock.listen()
        def serve():
            self.sock.settimeout(5)
            conn, _ = self.sock.accept()
            with conn:
                conn.settimeout(2)
                conn.recv(1024)
                conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong')
        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 6)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 23, error)
        self.assertFalse((self.records / 'holderd').exists())

    def test_missing_bundle_backend_does_not_fall_back(self):
        self.layout()
        (self.bin / 'holderd').unlink()
        for role in ('holderd', 'holder-desktop'):
            shutil.copy2(self.bin / 'holder-desktop', self.launcher.parent / role)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn(str(self.bin / 'holderd'), error)
        self.assertFalse(list(self.records.iterdir()))
        self.assertIn(error.strip(), (self.home / 'Library/Logs/Holder/launcher.log').read_text())

    def test_backend_spawn_failure(self):
        self.layout()
        (self.bin / 'holderd').chmod(0o644)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('Failed to start ' + str(self.bin / 'holderd'), error)
        self.assertFalse(list(self.records.iterdir()))

    def test_backend_early_exit(self):
        self.layout()
        self.env['HOLDER_TEST_EXIT'] = '1'
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('exit code 7', error)
        self.assertFalse((self.records / 'holder-desktop').exists())

    def test_desktop_exec_failure(self):
        self.layout()
        (self.bin / 'holder-desktop').chmod(0o644)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('Failed to start ' + str(self.bin / 'holder-desktop'), error)
        self.assertTrue((self.records / 'holderd').exists())
        self.assertFalse((self.records / 'holder-desktop').exists())

if __name__ == '__main__':
    unittest.main()
