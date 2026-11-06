#!/usr/bin/env python3
"""Two-instance Local Wireless rollback test on the native Linux build.

    deps/venv/bin/python tests/linux_pair.py <tag> [options]

Starts the host, waits for the lobby/room sequence, starts the join, waits for both games, then
compares the per-frame checksums with tests/rbn_cmp.py. The logs and save copies are kept under
tests/logs/linux_pair_<tag>/.

The menu automation uses tests/seq_lwr_host_linux.txt / seq_lwr_join_linux.txt (the Windows
sequences with @movie replaced by the Linux title draw-count wait). The netcode checksum source
is the fighter frame line, which is enabled by SSBU_FIGHT_HASH; that is set by default.

No game content in this file. SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, 'tests', 'logs')


def tail(path, n=300000):
    try:
        with open(path, errors='replace') as f:
            return f.read()[-n:]
    except OSError:
        return ''


def launch(args, side, data, seq, online, name):
    out = args.out
    game_log = os.path.join(out, side + '.log')
    rbn = os.path.join(out, side + '.rbn')
    fhash = os.path.join(out, side + '.fhash')
    env = [
        'SSBU_ONLINE=' + online,
        'SSBU_ONLINE_LOOPBACK=1',
        'SSBU_ONLINE_STUN=off',
        'SSBU_ONLINE_UPNP=off',
        'SSBU_RB_NET=net',
        'SSBU_RB_AUDIO_OFF=1',
        'SSBU_RB_NET_DETAIL=1',
        'SSBU_RB_NET_THREAD_EXCLUDE=' + ('1' if args.exclude else '0'),
        'SSBU_RB_NET_LOG=' + rbn,
        'SSBU_FIGHT_HASH=' + fhash,
        'SSBU_VK_WATCHDOG=' + str(args.watchdog),
        'SSBU_ONLINE_NAME=LinuxPair-' + side,
    ]
    if args.bots:
        bot = args.bots[0 if side == 'host' else 1]
        if bot:
            env.append('SSBU_RB_NET_BOT=' + bot)
    env += args.env
    cmd = [sys.executable, os.path.join(ROOT, 'scripts', 'run.py'), '--linux',
           '--build', args.build, '--userdata', data, '--log', game_log,
           '--seconds', str(args.seconds)]
    if seq:
        cmd += ['--seq', seq]
    for e in env:
        cmd += ['--env', e]
    run_log = open(os.path.join(out, side + '.run'), 'wb')
    print('%s: %s' % (side, ' '.join(cmd)), flush=True)
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=run_log, stderr=subprocess.STDOUT)
    return p, run_log


def copy_saves(args):
    for side, src in (('host', args.host_data), ('join', args.join_data)):
        dst = os.path.join(args.out, side + '_userdata')
        if not os.path.isdir(src):
            sys.exit('%s userdata does not exist: %s' % (side, src))
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
        print('%s save copy: %s -> %s' % (side, src, dst), flush=True)


def f_lines(path):
    try:
        with open(path, errors='replace') as f:
            return sum(1 for line in f if line.startswith('f '))
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tag', help='name for tests/logs/linux_pair_<tag>')
    ap.add_argument('--build', default='build-linux-1305')
    ap.add_argument('--host-data', default='/tmp/opencode/ud_host9')
    ap.add_argument('--join-data', default='/tmp/opencode/ud_join9')
    ap.add_argument('--host-seq', default=os.path.join(ROOT, 'tests', 'seq_lwr_host_linux.txt'))
    ap.add_argument('--join-seq', default=os.path.join(ROOT, 'tests', 'seq_lwr_join_linux.txt'))
    ap.add_argument('--wait', type=float, default=60.0, help='seconds between the host and the join start')
    ap.add_argument('--seconds', type=int, default=600, help='game seconds; both games stop after this')
    ap.add_argument('--watchdog', type=int, default=5, help='SSBU_VK_WATCHDOG seconds')
    ap.add_argument('--port', type=int, default=39520)
    ap.add_argument('--bots', default='7,11', help='host,join SSBU_RB_NET_BOT seeds; empty = no bot')
    ap.add_argument('--no-bots', action='store_true')
    ap.add_argument('--exclude', action='store_true', help='keep the Linux network exclusion (default: full rewind)')
    ap.add_argument('--manual', action='store_true', help='no menu sequence: start both games and wait for you to create/join the room; then wait for the match')
    ap.add_argument('--out', help='run directory (default: tests/logs/linux_pair_<tag>)')
    ap.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    args = ap.parse_args()
    args.out = os.path.abspath(args.out or os.path.join(LOGS, 'linux_pair_' + args.tag))
    if args.no_bots:
        args.bots = ',,'
    else:
        args.bots = (args.bots.split(',') + ['', ''])[:2]
    os.makedirs(args.out, exist_ok=True)
    copy_saves(args)
    hlogp = os.path.join(args.out, 'host.log')
    jlogp = os.path.join(args.out, 'join.log')
    host_seq = None if args.manual else args.host_seq
    join_seq = None if args.manual else args.join_seq
    if args.manual:
        print('manual mode: create the Local Wireless room in the host window, then join it from the join window; '
              'the script waits for the match', flush=True)
    host = join = None
    hlog = jlog = None
    try:
        host, hlog = launch(args, 'host', os.path.join(args.out, 'host_userdata'), host_seq,
                            'host:%d' % args.port, 'Host')
        print('host started; waiting %.0f s before the join' % args.wait, flush=True)
        time.sleep(args.wait)
        join, jlog = launch(args, 'join', os.path.join(args.out, 'join_userdata'), join_seq,
                            'join:127.0.0.1:%d' % args.port, 'Join')
        print('join started; waiting for the games (up to %d s)' % (args.seconds + 180), flush=True)
        deadline = time.time() + args.seconds + 180
        while time.time() < deadline:
            if host.poll() is not None and join.poll() is not None:
                break
            if host.poll() is None and join.poll() is None and 'match 1 over' in tail(hlogp) and 'match 1 over' in tail(jlogp):
                print('match 1 over; waiting 5 s before stopping the games', flush=True)
                time.sleep(5)
                break
            time.sleep(5)
    finally:
        for p in (host, join):
            if p is not None and p.poll() is None:
                print('terminating a game that is still running', flush=True)
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        for f in (hlog, jlog):
            if f:
                f.close()
    hrbn, jrbn = os.path.join(args.out, 'host.rbn'), os.path.join(args.out, 'join.rbn')
    crashed = []
    summary = []
    for side, path in (('host', hlogp), ('join', jlogp)):
        text = tail(path)
        for line in text.splitlines():
            if any(k in line for k in ('RB NET: match start', '[rbnet] after the start barrier',
                                       'RB NET report: ', 'FAULT RECOVERY: ', 'more than 4096 faults',
                                       'HOST SIGNAL', 'std::terminate', 'RING load of frame',
                                       'NETWORK: ', 'checksums compared')):
                summary.append('%s: %s' % (side, line.strip()))
        if 'HOST SIGNAL' in text or 'std::terminate' in text:
            crashed.append(side)
    print('\n== summary ==')
    for line in summary[-120:]:
        print(line)
    hf, jf = f_lines(hrbn), f_lines(jrbn)
    print('\n%s: %d f lines\n%s: %d f lines' % (hrbn, hf, jrbn, jf))
    rc = 0
    if crashed:
        print('CRASH: %s' % ', '.join(crashed))
        rc = 3
    if hf and jf:
        print('\n== checksum comparison ==')
        rc = max(rc, subprocess.run([sys.executable, os.path.join(ROOT, 'tests', 'rbn_cmp.py'), hrbn, jrbn],
                                    cwd=ROOT).returncode)
    else:
        print('NO CHECKSUM FRAMES: SSBU_FIGHT_HASH/SSBU_RB_NET_LOG did not produce f lines')
        rc = max(rc, 2)
    print('\nrun directory: %s' % args.out)
    return rc


if __name__ == '__main__':
    sys.exit(main())
