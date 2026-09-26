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
import time
import unittest

DRIVER = Path(sys.argv.pop(1)).resolve()
FIXTURE = r'''
import json, os, pathlib, socket, sys, time, fcntl
role = pathlib.Path(sys.argv[0]).name
record = pathlib.Path(os.environ['HOLDER_TEST_RECORD']) / role
record.write_text(json.dumps({'pid': os.getpid(), 'cwd': os.getcwd(),
    'env': {k: os.environ.get(k) for k in ['GSETTINGS_SCHEMA_DIR', 'GIO_MODULE_DIR',
    'GDK_PIXBUF_MODULE_FILE', 'GTK_PATH', 'XDG_DATA_DIRS', 'ENCHANT_CONFIG_DIR', 'DICPATH']}}))
(record.parent / (role + '.' + str(os.getpid()))).write_text('started')
if role == 'holder-desktop':
    sys.exit(23)
if os.environ.get('HOLDER_TEST_EXIT'):
    sys.exit(int(os.environ['HOLDER_TEST_EXIT']))
if os.environ.get('HOLDER_TEST_RACE'):
    lock = open(record.parent / 'daemon.lock', 'w')
    try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError: sys.exit(2)
    deadline = time.monotonic() + 3
    while len(list(record.parent.glob('holderd.*'))) < 2 and time.monotonic() < deadline:
        time.sleep(.01)
    time.sleep(1)
if os.environ.get('HOLDER_TEST_SILENT'):
    time.sleep(60)
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
        for key in ('HOLDER_TEST_EXIT', 'HOLDER_TEST_RACE', 'HOLDER_TEST_SILENT',
                    'HOLDER_TEST_ALERT_HELPER', 'HOLDER_TEST_STARTUP_MS'):
            self.env.pop(key, None)
        for key in ('GSETTINGS_SCHEMA_DIR', 'GIO_MODULE_DIR', 'GDK_PIXBUF_MODULE_FILE',
                    'GTK_PATH', 'XDG_DATA_DIRS', 'ENCHANT_CONFIG_DIR', 'DICPATH'):
            self.env.pop(key, None)

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
        self.assertEqual(self.record('holderd')['cwd'], str(self.root))
        self.assertTrue(all(value is None for value in self.record('holderd')['env'].values()))
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

    def test_incompatible_service_is_reported_without_spawning(self):
        self.layout()
        self.sock.listen()
        def serve():
            self.sock.settimeout(5)
            conn, _ = self.sock.accept()
            with conn:
                conn.settimeout(2)
                conn.recv(1024)
                conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnope')
        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 6)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn("expected ping response", error)
        self.assertFalse(list(self.records.iterdir()))

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
        self.env['HOLDER_TEST_EXIT'] = '7'
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

    def test_concurrent_cold_launches_share_winner(self):
        self.layout()
        self.env['HOLDER_TEST_RACE'] = '1'
        self.env['HOLDER_TEST_STARTUP_MS'] = '5000'
        processes = []
        try:
            for _ in range(2):
                p = subprocess.Popen([str(self.launcher)], cwd=self.home, env=self.env,
                                     pass_fds=(self.sock.fileno(),), start_new_session=True,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                processes.append(p)
            for p in processes:
                self.assertEqual(p.wait(timeout=7), 23)
            self.assertEqual(len(list(self.records.glob('holderd.*'))), 2)
            self.assertEqual(len(list(self.records.glob('holder-desktop.*'))), 2)
        finally:
            for p in processes:
                try: os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait(timeout=3)

    def test_readiness_timeout_does_not_launch_desktop(self):
        self.layout()
        self.env['HOLDER_TEST_SILENT'] = '1'
        self.env['HOLDER_TEST_STARTUP_MS'] = '1000'
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('within 1 seconds', error)
        self.assertFalse((self.records / 'holder-desktop').exists())

    def test_lock_exit_without_winner_reports_exit_and_timeout(self):
        self.layout()
        self.env['HOLDER_TEST_EXIT'] = '2'
        self.env['HOLDER_TEST_STARTUP_MS'] = '3000'
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('exit code 2', error)
        self.assertIn('within 3 seconds', error)

    def test_unwritable_log_does_not_prevent_launch(self):
        self.layout()
        (self.home / 'Library').write_text('blocks log directory')
        _, code, error = self.run_launcher()
        self.assertEqual(code, 23, error)

    def test_log_rotation_and_build_identity(self):
        self.layout()
        log = self.home / 'Library/Logs/Holder/launcher.log'
        log.parent.mkdir(parents=True)
        log.write_text('x' * (256 * 1024))
        _, code, error = self.run_launcher()
        self.assertEqual(code, 23, error)
        self.assertTrue(log.with_name('launcher.log.1').exists())
        self.assertLess(log.stat().st_size, 256 * 1024)
        self.assertRegex(log.read_text(), r'\d{4}-\d{2}-\d{2}T.*Holder launcher \d+\.\d+\.\d+')
        self.assertIn('Backend ready after', log.read_text())

    def test_alert_arguments_are_passed_directly(self):
        self.base = self.base / 'quoted "name" and back\\slash'
        self.layout()
        (self.bin / 'holderd').unlink()
        helper = self.base / "fake alert helper"
        args_file = self.records / 'alert-args'
        helper.write_text('#!' + sys.executable + '\nimport json, sys\nfrom pathlib import Path\n' +
                          'Path(' + repr(str(args_file)) + ').write_text(json.dumps(sys.argv[1:]))\n')
        helper.chmod(0o755)
        self.env['HOLDER_TEST_ALERT_HELPER'] = str(helper)
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        args = json.loads(args_file.read_text())
        self.assertEqual(args[0], '-e')
        message = 'Holder backend was not found:\n\n' + str(self.bin / 'holderd')
        quoted = message.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
        self.assertEqual(args[1], 'display alert "Holder" message "' + quoted + '"')
        self.assertEqual(len(args), 2)

    def test_alert_failure_keeps_stderr_and_log(self):
        self.layout()
        (self.bin / 'holderd').unlink()
        self.env['HOLDER_TEST_ALERT_HELPER'] = str(self.base / 'missing-helper')
        _, code, error = self.run_launcher()
        self.assertEqual(code, 1)
        self.assertIn('Holder backend was not found', error)
        log = (self.home / 'Library/Logs/Holder/launcher.log').read_text()
        self.assertIn('Could not display error alert', log)

if __name__ == '__main__':
    unittest.main()
