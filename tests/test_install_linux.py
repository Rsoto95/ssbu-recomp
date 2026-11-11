#!/usr/bin/env python3
"""Small, game-free checks of scripts/linux/install_linux.py (paths, DLC ids, dry run). No dump or keys are needed."""
import contextlib
import io
import os
import sys
import tempfile
import threading
import time

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
            stop.set()
            t.join(timeout=2)
        d.PROGRESS = False
        text = out.getvalue()
        assert 'sources: 1162' in text, text
        assert '"total": 1162' in text, text
        assert '"done": 10' in text, text
        assert '"last": "c_03f7.c"' in text, text
        assert '"done": 1162' in text, text


def test_dry_run():
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, 'dump')
        for rel in (('exefs', 'main'), ('exefs', 'sdk'), ('romfs', 'data.arc')):
            p = os.path.join(dump, *rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').close()
        rc = d.main(['--dry-run', '--dump', dump, '--storage', os.path.join(tmp, 'storage')])
        assert rc == 0, rc


if __name__ == '__main__':
    test_dlc_rows()
    test_write_dlc()
    test_dump_check()
    test_tail_log()
    test_dry_run()
    print('test_install_linux: ok')
