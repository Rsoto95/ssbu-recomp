#!/usr/bin/env python3
"""Small, game-free checks of scripts/linux/install_linux.py (paths, DLC ids, dry run). No dump or keys are needed."""
import contextlib
import io
import json
import os
import sys
import tempfile
import threading
import time
from unittest import mock

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts', 'linux'))
import install_linux as d  # noqa: E402


def test_dlc_rows():
    rows = d.dlc_rows([
        '/tmp/My DLC [01006A800016F041][v0].nsz',
        '/tmp/Other [01006A800016E000].nsp',
        '/tmp/Not DLC [01006A800016E000].nsp',
    ])
    assert rows == {0x41: 'My DLC'}, rows


def test_write_dlc():
    with tempfile.TemporaryDirectory() as tmp:
        d.write_dlc(tmp, ['/tmp/My DLC [01006A800016F041][v0].nsz'])
        text = open(os.path.join(tmp, 'config', 'dlc.txt'), encoding='utf-8').read()
        assert '0x041' in text and 'My DLC' in text, text
        d.write_dlc(tmp, [])
        text = open(os.path.join(tmp, 'config', 'dlc.txt'), encoding='utf-8').read()
        assert '0x041' not in text and 'My DLC' not in text, text


def test_dump_check():
    with tempfile.TemporaryDirectory() as tmp:
        assert d.check_dump(tmp)
        for rel in (('exefs', 'main'), ('exefs', 'sdk'), ('romfs', 'data.arc')):
            p = os.path.join(tmp, *rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').close()
        assert d.check_dump(tmp) is None


def test_tail_log():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'compile.log')
        open(path, 'wb').close()
        stop = threading.Event()
        d.PROGRESS = True
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            t = threading.Thread(target=d.tail_log, args=(path, stop, 'compile'), daemon=True)
            t.start()
            with open(path, 'w', encoding='utf-8') as f:
                f.write('sources: 1162   to compile: 1162   jobs: 8\n')
                f.write('  10/1162   30s elapsed   20s compiler time\n')
                f.write('  c_03f7.c         78s  helpers not inlined\n')
                f.write('compiled: 1162   failed: 0   elapsed: 90s\n')
            time.sleep(1.0)
            with open(path, 'w', encoding='utf-8') as f:       # a retry truncated the log
                f.write('new run\n')
            time.sleep(1.0)
            stop.set()
            t.join(timeout=2)
        d.PROGRESS = False
        text = out.getvalue()
        assert 'sources: 1162' in text, text
        assert '"total": 1162' in text, text
        assert '"done": 10' in text, text
        assert '"last": "c_03f7.c"' in text, text
        assert '"done": 1162' in text, text
        assert 'new run' in text, text


def test_dry_run():
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, 'dump')
        for rel in (('exefs', 'main'), ('exefs', 'sdk'), ('romfs', 'data.arc')):
            p = os.path.join(dump, *rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').close()
        rc = d.main(['--dry-run', '--dump', dump, '--storage', os.path.join(tmp, 'storage')])
        assert rc == 0, rc


def test_flatpak_storage():
    import launcher as l  # noqa: E402  (no gi at import: main() loads GTK)
    with tempfile.TemporaryDirectory() as tmp:
        home, data, conf = (os.path.join(tmp, n) for n in ('home', 'data', 'config'))
        os.makedirs(home)
        env = {'HOME': home, 'XDG_DATA_HOME': data, 'XDG_CONFIG_HOME': conf, 'FLATPAK_ID': 'io.test.App',
               'SSBU_DATA_DIR': ''}
        with mock.patch.dict(os.environ, env):
            assert l.default_storage() == os.path.join(home, '.local', 'share', 'ssbu-recomp'), l.default_storage()
            new = os.path.join(home, '.local', 'share', 'ssbu-recomp')
            assert d.default_storage() == new, d.default_storage()
            # Flatpak's ~/.var/app folder: a build, and a save newer than the one already in the home folder
            old = os.path.join(data, 'ssbu-recomp')
            for base, content, stamp in ((old, b'flatpak', 2000), (new, b'native', 1000)):
                save = os.path.join(base, 'userdata', 'save', 'save_data')
                os.makedirs(save)
                with open(os.path.join(save, 'system_data.bin'), 'wb') as f:
                    f.write(content)
                os.utime(os.path.join(save, 'system_data.bin'), (stamp, stamp))
            os.makedirs(os.path.join(old, 'build-linux', 'work'))
            with open(os.path.join(old, 'install.json'), 'w', encoding='utf-8') as f:
                json.dump({'storage': old, 'build': os.path.join(old, 'build-linux'),
                           'userdata': os.path.join(old, 'userdata'), 'dump': '/elsewhere/dump'}, f)
            os.makedirs(os.path.dirname(l.config_path()))
            with open(l.config_path(), 'w', encoding='utf-8') as f:
                json.dump({'storage': old, 'dump': '/elsewhere/dump'}, f)
            assert l.migrate_flatpak_storage() is True
            assert os.path.isdir(os.path.join(new, 'build-linux', 'work'))

            def read(path):
                with open(path, 'rb') as f:
                    return f.read()

            save = os.path.join(new, 'userdata', 'save', 'save_data', 'system_data.bin')
            assert read(save) == b'flatpak'           # the newer save stays the active one
            kept = [n for n in os.listdir(new) if n.startswith('userdata.before-flatpak-')]
            assert len(kept) == 1, kept
            save = os.path.join(new, kept[0], 'save', 'save_data', 'system_data.bin')
            assert read(save) == b'native'            # the other one is kept, not deleted
            info = json.load(open(os.path.join(new, 'install.json')))
            assert info['storage'] == new and info['build'] == os.path.join(new, 'build-linux')
            assert info['userdata'] == os.path.join(new, 'userdata') and info['dump'] == '/elsewhere/dump', info
            assert json.load(open(l.config_path()))['storage'] == new
            assert l.migrate_flatpak_storage() is False           # a second start finds the build there and stops
        with mock.patch.dict(os.environ, dict(env, FLATPAK_ID='')):
            assert l.default_storage() == os.path.join(data, 'ssbu-recomp'), l.default_storage()
        with mock.patch.dict(os.environ, dict(env, HOME=os.path.join(tmp, 'home2'),
                                              XDG_DATA_HOME=os.path.join(tmp, 'data2'))):
            os.makedirs(os.path.join(tmp, 'home2'))
            old = os.path.join(tmp, 'data2', 'ssbu-recomp')
            os.makedirs(os.path.join(old, 'userdata'))
            with open(os.path.join(old, 'last_run.log'), 'w', encoding='utf-8') as f:
                f.write('a game is writing this')             # a session is running
            assert l.migrate_flatpak_storage() is False
            assert os.path.isdir(old)
        with mock.patch.dict(os.environ, dict(env, HOME=os.path.join(tmp, 'home3'),
                                              XDG_DATA_HOME=os.path.join(tmp, 'data3'))):
            os.makedirs(os.path.join(tmp, 'home3'))
            old = os.path.join(tmp, 'data3', 'ssbu-recomp')
            stale = os.path.join(old, 'userdata', 'save.work.123')
            os.makedirs(stale)                                # a crashed session's work folder, hours old
            os.utime(stale, (1000, 1000))
            assert l.migrate_flatpak_storage() is True        # a stale work folder does not block the move
            assert os.path.isdir(os.path.join(tmp, 'home3', '.local', 'share', 'ssbu-recomp', 'userdata'))


if __name__ == '__main__':
    test_dlc_rows()
    test_write_dlc()
    test_dump_check()
    test_tail_log()
    test_dry_run()
    test_flatpak_storage()
    print('test_install_linux: ok')
