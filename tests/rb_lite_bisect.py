"""Rollback re-run cost and look, offline (2026-11-05): which re-run skips are clean.

The re-run passes of a rollback can skip presentation work (runtime/rollback/debug/ring_simulation.c "lite" groups,
SSBU_RB_SIM_LITE; runtime/rollback/model/runahead.c "kick" skips, SSBU_RB_SIM_SKIP; SSBU_RB_NET_LASTLITE for the last re-run
pass). scripts/run.py turned all of them off on 2026-10-28 ("the blue / black fighters"). This script runs one scripted fight
per configuration with the forced-rollback simulation (SSBU_RB_SIM: a rollback every N real passes, k = 1..K), no network,
and reports for each: the re-run pass mean (ms), the rollbacks done, exactness against the first configuration's hash
(tests/rb_sim_cmp.py) and screenshots of fixed fight frames (to compare the fighters' colours by eye).

    python tests/rb_lite_bisect.py [name=SKIP,LITE,LASTLITE ...]      (default: the bisect set below)
Output: build_1305/logs/rblite/<name>.log, <name>.hash.txt, <name>/fight_*.bmp, and a table at the end.
No game content in this file."""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BUILD = os.path.join(REPO, 'build_1305')
OUT = os.path.join(BUILD, 'logs', 'rblite')
PY = os.path.join(REPO, 'deps', 'python', 'python.exe')
SEQ = os.path.join(HERE, 'seq_completo_1305.txt')
SHOTS = '600,900,1200,1500,1800,2100'
SECS = int(os.environ.get('RBLITE_SECS', '150'))

DEFAULT = [('ref', '0,0,0'), ('off', '0,0,0'), ('all', '49,127,1')] + \
          [('lite%d' % b, '0,%d,1' % b) for b in (1, 2, 4, 8, 16, 32, 64)] + \
          [('kick%d' % b, '%d,0,1' % b) for b in (1, 16, 32)]


def ini(path):
    with open(path, 'w') as f:
        f.write('renderer = vulkan\nwindow = windowed 1280x720\nvsync = off\nrollback = off\nonline_menu = off\nmenu = off\n')


def run(name, skip, lite, last, ref_hash):
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    log, hashf, r1 = os.path.join(OUT, name + '.log'), os.path.join(OUT, name + '.hash.txt'), os.path.join(OUT, name + '.r1.log')
    for f in (log, hashf, r1):
        if os.path.exists(f):
            os.remove(f)
    gini = os.path.join(OUT, 'graphics.ini')
    ini(gini)
    env = ['SSBU_ONLINE=off', 'SSBU_GRAPHICS_CONFIG=' + gini, 'SSBU_AUDIO_MUTE=1', 'SSBU_SKIP_MOVIES=1', 'SSBU_FIXED_SEED=1',
           'SSBU_DETERMINISTIC=1', 'SSBU_FIGHT_HASH=' + hashf, 'SSBU_R1_LOG=' + r1, 'SSBU_SHOT_FIGHT=' + SHOTS,
           'SSBU_SHOT_DIR=' + d, 'SSBU_LS_X_WW=1', 'SSBU_RB_AUDIO_OFF=1', 'SSBU_LATENCY=low',
           'SSBU_RB_SIM_SKIP=%d' % skip, 'SSBU_RB_SIM_LITE=%d' % lite, 'SSBU_RB_NET_LASTLITE=%d' % last]
    env += os.environ.get('RBLITE_EXTRA', '').split()     # more NAME=value settings for every run (e.g. SSBU_RB_SIM_PAGES=1)
    if name != 'ref':
        env.append('SSBU_RB_SIM=10,7')      # a rollback every 10 real passes, k = 1..7 in turn
    cmd = [PY, os.path.join(REPO, 'scripts', 'run.py'), '--build', BUILD, '--userdata', os.path.join(BUILD, 'test_saves', 'a'),
           '--seq', SEQ, '--seconds', str(SECS), '--log', log] + sum((['--env', e] for e in env), [])
    subprocess.run(cmd, cwd=REPO, timeout=SECS + 300)
    text = open(log, errors='replace').read() if os.path.exists(log) else ''
    rtext = open(r1, errors='replace').read() if os.path.exists(r1) else text
    m = re.findall(r'RB SIM passes .*?re-run (\d+) mean ([\d.]+)', rtext + text)
    rerun = '%s ms (%s)' % (m[-1][1], m[-1][0]) if m else '-'
    rb = re.findall(r'RB SIM: (\d+) rollbacks', rtext + text)
    exact = '-'
    if name != 'ref' and ref_hash and os.path.exists(hashf):
        # the pass counters after "P" (P <n> <kind>) differ between a run with rollbacks and one without: left out; the
        # re-run check (every re-run replays the timeline exactly) is the one that matters here (the real timeline of a
        # run with forced rollbacks drifts from the reference one after ~245 frames even with no skip at all)
        def norm(src):
            dst = src + '.n'
            with open(src, errors='replace') as f, open(dst, 'w') as o:
                for ln in f:
                    o.write(re.sub(r'\bP \d+ \d+ ', 'P ', ln))
            return dst
        r = subprocess.run([PY, os.path.join(HERE, 'rb_sim_cmp.py'), norm(hashf), r1 if os.path.exists(r1) else log, norm(ref_hash)],
                           capture_output=True, text=True)
        tail = (r.stdout.strip().splitlines() or ["?"])[-1]
        m2 = re.search(r're-run runs \d+ \(frames \d+\): ([^;]+)', tail)
        exact = 're-runs ' + (m2.group(1) if m2 else tail[:60])
    shots = len([f for f in os.listdir(d) if f.endswith('.bmp')])
    trap = 'TRAP' if 'GUEST TRAP' in text or 'HOST EXCEPTION' in text else ''
    return '%-8s skip %-3d lite %-3d last %d | re-run %-14s | rollbacks %-5s | shots %d | %s %s' % (
        name, skip, lite, last, rerun, rb[-1] if rb else '-', shots, exact, trap)


def main():
    os.makedirs(OUT, exist_ok=True)
    sets = [(a.split('=')[0], a.split('=')[1]) for a in sys.argv[1:]] or DEFAULT
    ref_hash = os.path.join(OUT, 'ref.hash.txt')
    rows = []
    for name, v in sets:
        s, l, la = (int(x) for x in v.split(','))
        rows.append(run(name, s, l, la, ref_hash if os.path.exists(ref_hash) else None))
        print(rows[-1], flush=True)
    print('\n'.join(rows))


if __name__ == '__main__':
    main()
