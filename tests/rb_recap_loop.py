"""Rollback re-capture modes, offline (2026-11-05): what saving no state after a 1-frame rollback costs and gains.

runtime/rollback/controller/rollback_controller.c SSBU_RB_NET_RECAP: mode 5 saves the state of the re-run frame of every
1-frame rollback; mode 6 does not (the next real pass saves the following frame anyway; a later wrong guess at exactly the
next frame then loads one frame older and runs 2 frames). This script plays one scripted fight per mode in the rollback
loop test (SSBU_RB_NET=loop,<late>: no network, the other player is a bot whose pads arrive <late> frames late, so the
rollbacks are real wrong guesses) with the same seeds, and reports per mode: the rollbacks by length (k), the rollback
parts (load, re-run passes, re-run captures in ms per rollback) and whether the finalized frames' checksums
(SSBU_RB_NET_LOG) are the same in every mode (the same fight).

    python tests/rb_recap_loop.py [late=4] [delay=2] [modes=5,6]
Output: build_1305/logs/rbrecap/m<mode>.log, m<mode>.r1.log, m<mode>.hlog.txt, and a table at the end.
No game content in this file."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BUILD = os.path.join(REPO, 'build_1305')
OUT = os.path.join(BUILD, 'logs', 'rbrecap')
PY = os.path.join(REPO, 'deps', 'python', 'python.exe')
SEQ = os.path.join(HERE, 'seq_completo_1305.txt')
SECS = int(os.environ.get('RBRECAP_SECS', '150'))


def ini(path):
    with open(path, 'w') as f:
        f.write('renderer = vulkan\nwindow = windowed 1280x720\nvsync = off\nrollback = off\nonline_menu = off\nmenu = off\n')


def run(mode, late, delay):
    name = 'm%d' % mode
    log, r1, hlog = (os.path.join(OUT, name + x) for x in ('.log', '.r1.log', '.hlog.txt'))
    for f in (log, r1, hlog):
        if os.path.exists(f):
            os.remove(f)
    gini = os.path.join(OUT, 'graphics.ini')
    ini(gini)
    env = ['SSBU_ONLINE=off', 'SSBU_GRAPHICS_CONFIG=' + gini, 'SSBU_AUDIO_MUTE=1', 'SSBU_SKIP_MOVIES=1', 'SSBU_FIXED_SEED=1',
           'SSBU_DETERMINISTIC=1', 'SSBU_R1_LOG=' + r1, 'SSBU_LS_X_WW=1', 'SSBU_RB_AUDIO_OFF=1', 'SSBU_LATENCY=low',
           'SSBU_RB_SIM_SKIP=49', 'SSBU_RB_SIM_LITE=123', 'SSBU_RB_NET_LASTLITE=1',
           'SSBU_RB_NET=loop,%d' % late, 'SSBU_RB_NET_DELAY=%d' % delay, 'SSBU_RB_NET_BOT=7', 'SSBU_RB_NET_LOOP_SEED=7',
           'SSBU_RB_NET_LOG=' + hlog, 'SSBU_RB_NET_RECAP=%d' % mode]
    cmd = [PY, os.path.join(REPO, 'scripts', 'run.py'), '--build', BUILD, '--userdata', os.path.join(BUILD, 'test_saves', 'a'),
           '--seq', SEQ, '--seconds', str(SECS), '--log', log] + sum((['--env', e] for e in env), [])
    subprocess.run(cmd, cwd=REPO, timeout=SECS + 300)
    text = ''
    for f in (r1, log):
        if os.path.exists(f):
            text += open(f, errors='replace').read()
    ks = re.findall(r'RB NET report: (\d+) real frames, (\d+) rollbacks, (\d+) frames run again \(k: ([^)]*)\)', text)
    parts = re.findall(r'rollback parts \(ms per rollback\): before the load ([\d.]+).*?load ([\d.]+), re-run passes ([\d.]+) '
                       r'\(([\d.]+) per rollback, ([\d.]+) each\), re-run captures ([\d.]+) \(([\d.]+) per rollback', text)
    trap = 'TRAP' if 'GUEST TRAP' in text or 'HOST EXCEPTION' in text else ''
    frames = {}
    if os.path.exists(hlog):
        for ln in open(hlog, errors='replace'):
            m = re.match(r'f (\d+) h (\S+)', ln)
            if m:
                frames[int(m.group(1))] = m.group(2)
    return name, ks[-1] if ks else None, parts[-1] if parts else None, frames, trap


def main():
    kv = dict(a.split('=', 1) for a in sys.argv[1:])
    late, delay = int(kv.get('late', 4)), int(kv.get('delay', 2))
    modes = [int(x) for x in kv.get('modes', '5,6').split(',')]
    os.makedirs(OUT, exist_ok=True)
    res = [run(m, late, delay) for m in modes]
    base = res[0][3]
    for name, ks, parts, frames, trap in res:
        same = len(set(base) & set(frames))
        diff = sum(1 for f in set(base) & set(frames) if base[f] != frames[f])
        line = '%s | ' % name
        line += ('real %s rollbacks %s again %s (k: %s)' % ks) if ks else 'no report'
        if parts:
            line += ' | load %s, re-run %s (%s each), recap %s (%s per rb) | ms per rollback total %.3f' % (
                parts[1], parts[2], parts[4], parts[5], parts[6], float(parts[0]) + float(parts[1]) + float(parts[2]) + float(parts[5]))
        line += ' | frames compared %d, different %d %s' % (same, diff, trap)
        print(line, flush=True)


if __name__ == '__main__':
    main()
