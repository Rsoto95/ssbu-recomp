"""Linux runtime publication and launcher recovery, without network access or game files.

    python3 tests/linux_runtime_update_test.py
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from linux import prepare_current_runtime as runtime

spec = importlib.util.spec_from_file_location('launcher_test', ROOT / 'scripts/run.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux runtime publication')
class RuntimeUpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='linux update test ')
        self.addCleanup(self.tmp.cleanup)
        self.build = Path(self.tmp.name)
        (self.build / 'work').mkdir()
        (self.build / 'config').mkdir()
        self.gini = str(self.build / 'config/graphics.ini')
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def record(self):
        exe = self.build / 'work/ssbu_native'
        exe.write_bytes(b'installed binary')
        lib = self.build / 'work/ssbu_game_eh_o1.so'
        lib.write_bytes(b'game library')
        record = dict(commit='abc', source_identity='sources', executable_sha256=runtime.sha256(exe),
                      game_library_stat=[lib.stat().st_size, lib.stat().st_mtime_ns], game_identity='game', game_identity_version=1)
        runtime.atomic_json(runtime.manifest_path(self.build), record)
        return record

    def status(self):
        with patch.object(runtime, 'commit_id', return_value='abc'), patch.object(runtime, 'source_identity', return_value='sources'):
            return runtime.status(self.build)

    def test_matching_manifest_skips_build(self):
        self.record()
        self.assertTrue(self.status()[0])

    def test_pending_build_retries_even_when_manifest_matches(self):
        self.record()
        (self.build / 'ssbu_native.runtime.pending.json').write_text('{}')
        self.assertFalse(self.status()[0])

    def test_replaced_executable_is_detected(self):
        self.record()
        (self.build / 'work/ssbu_native').write_bytes(b'other binary')
        self.assertFalse(self.status()[0])

    def test_missing_executable_is_detected(self):
        self.record()
        (self.build / 'work/ssbu_native').unlink()
        self.assertFalse(self.status()[0])

    def test_manual_pull_detected_by_commit(self):
        self.record()
        with patch.object(runtime, 'commit_id', return_value='new'), patch.object(runtime, 'source_identity', return_value='sources'):
            self.assertFalse(runtime.status(self.build)[0])

    def test_local_source_edits_detected(self):
        self.record()
        with patch.object(runtime, 'commit_id', return_value='abc'), patch.object(runtime, 'source_identity', return_value='edited'):
            self.assertFalse(runtime.status(self.build)[0])

    def test_changed_game_library_detected(self):
        self.record()
        (self.build / 'work/ssbu_game_eh_o1.so').write_bytes(b'relinked game library')
        self.assertFalse(self.status()[0])

    def test_missing_manifest_requests_adoption(self):
        self.assertFalse(self.status()[0])

    def test_abi_changes_require_setup(self):
        self.record()
        with patch.object(runtime, 'game_identity', return_value='changed'):
            self.assertTrue(runtime.needs_game_setup(self.build))

    def test_failed_atomic_copy_preserves_executable(self):
        self.record()
        exe = self.build / 'work/ssbu_native'
        candidate = self.build / 'candidate'
        candidate.write_bytes(b'new')
        with patch.object(runtime.shutil, 'copy2', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                runtime.atomic_copy(candidate, exe)
        self.assertEqual(exe.read_bytes(), b'installed binary')
        self.assertEqual(list(exe.parent.glob('ssbu_native.*')), [])

    def test_manual_pull_and_offline_start_reconcile_without_fetch(self):
        with patch.object(launcher, 'linux_source_update', return_value=False), patch.object(runtime, 'status', return_value=(False, 'old binary')), patch.object(launcher, 'linux_rebuild', return_value=True) as rebuild:
            self.assertFalse(launcher.linux_update(self.gini))
        rebuild.assert_called_once_with(self.gini)

    def test_failed_build_is_retried_next_launch(self):
        with patch.object(launcher, 'linux_source_update', return_value=False), patch.object(runtime, 'status', return_value=(False, 'pending')), patch.object(launcher, 'linux_rebuild', return_value=False) as rebuild:
            self.assertFalse(launcher.linux_update(self.gini))
            self.assertFalse(launcher.linux_update(self.gini))
        self.assertEqual(rebuild.call_count, 2)

    def test_source_pull_restarts_before_importing_builder(self):
        with patch.object(launcher, 'linux_source_update', return_value=True), patch.object(launcher, 'linux_rebuild') as rebuild:
            self.assertTrue(launcher.linux_update(self.gini))
        rebuild.assert_not_called()

    def test_off_policy_does_not_update_or_build(self):
        Path(self.gini).write_text('auto_update = off\n')
        with patch.object(launcher, 'linux_source_update') as fetch, patch.object(launcher, 'linux_rebuild') as rebuild:
            self.assertFalse(launcher.linux_update(self.gini))
        fetch.assert_not_called()
        rebuild.assert_not_called()

    def test_ask_policy_does_not_fetch_without_explicit_update(self):
        Path(self.gini).write_text('auto_update = ask\n')
        with patch.dict(os.environ, {}, clear=True), patch.object(launcher, 'UPDATE_ASKED', False, create=True), patch.object(launcher.subprocess, 'run') as run:
            self.assertFalse(launcher.linux_source_update(self.gini))
        run.assert_not_called()

    def test_failed_rebuild_returns_failure_and_keeps_custom_build(self):
        with patch.object(runtime, 'needs_game_setup', return_value=False), patch.object(launcher.subprocess, 'run', return_value=types.SimpleNamespace(returncode=1)) as run:
            self.assertFalse(launcher.linux_rebuild(self.gini))
        cmd = run.call_args.args[0]
        self.assertEqual(cmd[cmd.index('--build') + 1], str(self.build))
        self.assertIn('prepare_current_runtime.py', cmd[2])

    def test_game_changes_select_full_setup_in_custom_build(self):
        (self.build / 'state.json').write_text(json.dumps({'dump': {'exefs': '/my exefs', 'romfs': '/my romfs'}}))
        with patch.object(runtime, 'needs_game_setup', return_value=True), patch.object(launcher.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0)) as run:
            self.assertTrue(launcher.linux_rebuild(self.gini))
        cmd = run.call_args.args[0]
        self.assertIn('--from', cmd)
        self.assertEqual(cmd[cmd.index('--from') + 1], 'tables')
        self.assertEqual(cmd[cmd.index('--build') + 1], str(self.build))
        self.assertEqual(cmd[cmd.index('--romfs') + 1], '/my romfs')

    def prepare_fixture(self):
        self.record()
        (self.build / 'state.json').write_text(json.dumps({'dump': {'romfs': '/my romfs'}}))
        candidate = self.build / 'cache/linux-runtime/ssbu_native/ssbu_native'
        candidate.parent.mkdir(parents=True)
        candidate.write_bytes(b'verified candidate')
        setup = types.SimpleNamespace(STAGE_PLAN=[], copy_if_changed=lambda *args: None)
        patches = [patch.object(runtime, 'setup_module', return_value=setup),
                   patch.object(runtime, 'commit_id', return_value='abc'),
                   patch.object(runtime, 'source_identity', return_value='sources'),
                   patch.object(runtime, 'game_identity', return_value='game'),
                   patch.object(runtime.subprocess, 'check_output', return_value='test compiler')]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def test_compile_failure_keeps_installed_binary_and_pending_retry(self):
        self.prepare_fixture()
        with patch.object(runtime.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['build.py'])):
            with self.assertRaises(subprocess.CalledProcessError):
                runtime.prepare(self.build, 'ssbu_native', 4, 'test-clang')
        self.assertEqual((self.build / 'work/ssbu_native').read_bytes(), b'installed binary')
        self.assertTrue((self.build / 'ssbu_native.runtime.pending.json').exists())

    def test_selftest_failure_keeps_installed_binary(self):
        self.prepare_fixture()
        with patch.object(runtime.subprocess, 'run', side_effect=[None, subprocess.CalledProcessError(1, ['candidate'])]):
            with self.assertRaises(subprocess.CalledProcessError):
                runtime.prepare(self.build, 'ssbu_native', 4, 'test-clang')
        self.assertEqual((self.build / 'work/ssbu_native').read_bytes(), b'installed binary')
        self.assertTrue((self.build / 'ssbu_native.runtime.pending.json').exists())

    def test_validated_candidate_is_published_and_previous_retained(self):
        self.prepare_fixture()
        with patch.object(runtime.subprocess, 'run'):
            runtime.prepare(self.build, 'ssbu_native', 4, 'test-clang')
        self.assertEqual((self.build / 'work/ssbu_native').read_bytes(), b'verified candidate')
        self.assertEqual((self.build / 'work/ssbu_native_previous').read_bytes(), b'installed binary')
        self.assertFalse((self.build / 'ssbu_native.runtime.pending.json').exists())
        self.assertTrue(self.status()[0])

    def test_second_publisher_refuses_active_build_lock(self):
        import fcntl
        self.prepare_fixture()
        with open(self.build / 'linux-runtime.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError, 'another Linux runtime build'):
                runtime.prepare(self.build, 'ssbu_native', 4, 'test-clang')
        self.assertEqual((self.build / 'work/ssbu_native').read_bytes(), b'installed binary')
        self.assertFalse((self.build / 'ssbu_native.runtime.pending.json').exists())


if __name__ == '__main__':
    unittest.main()
