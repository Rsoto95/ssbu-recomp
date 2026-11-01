#!/usr/bin/env python3
"""Start the built game (or run a scripted match) with the paths scripts/setup.py recorded.

    python scripts/run.py --userdata <folder for saves>                    play (window, sound)
    python scripts/run.py --userdata <folder> --seq tests/seq_battle_play.txt --seconds 240 --exe ssbu_match.exe
                                                                           scripted match: the input sequence is played,
                                                                           the window stays off-screen and muted when the exe
                                                                           is not named ssbu_native.exe / ssbu_user.exe

--linux runs the native Linux build (scripts/setup.py --target linux): build folder build-linux, exe ssbu_native. On Linux
it is the default when build-linux exists and build (a Windows cross build) does not; --userdata then defaults to
$XDG_DATA_HOME/ssbu-recomp/userdata (~/.local/share/ssbu-recomp/userdata).

--mac runs the native macOS build (scripts/setup.py --target macos): build folder build-macos, exe ssbu_native. On a Mac it is
the default when build-macos exists; --userdata then defaults to ~/Library/Application Support/ssbu-recomp/userdata, and
--play writes the game log to build-macos/logs/game_<time>.log (the last 10 are kept).

--userdata must be a folder you can lose: the game writes its save data there (use a copy when testing).
Options go to the game as environment variables (listed in BUILDING.md); --env NAME=VALUE adds one.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_location  # noqa: E402


def ini_on(path, key):
    """`key = on` (or 1 / yes / true) in a graphics.ini; False when the file or the key is missing."""
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                k, sep, v = line.partition('=')
                if sep and k.strip().lower() == key:
                    return v.split('#')[0].split(';')[0].strip().lower() in ('on', '1', 'yes', 'true')
    except OSError:
        pass
    return False


def ini_get(path, key):
    """The value of `key` in a graphics.ini (lower case, comments cut), or None when the file or the key is missing."""
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                k, sep, v = line.partition('=')
                if sep and k.strip().lower() == key:
                    return v.split('#')[0].split(';')[0].strip().lower()
    except OSError:
        pass
    return None


# (fork Rsoto95/ssbu-recomp, 2026-10-28) "Play SSBU.bat" plays as the group plays, with nothing to set up: online play
# in the "same network" mode over the Radmin network (the first game started is the lobby by itself, no program to run;
# the game's Local Wireless rooms: 1-on-1 rooms play with rollback) with rollback on and the settings the tests of
# 2026-10-28 used. In config\graphics.ini, rollback = off turns all of it off and group_lobby = off keeps the rest but
# connects as F1 > Online says; an --env given to run.py wins over all of them.
PLAY_LOBBY = 'lan'
PLAY_DEFAULTS = {
    # (the input delay: graphics.ini input_delay, F1 > Gameplay, default 3; each match uses the larger of the two players')
    'SSBU_LATENCY': 'low',
    'SSBU_LS_X_WW': '1',             # the ring tracks the writes to the big extras ranges with GetWriteWatch
    'SSBU_RB_AUDIO_OFF': '1',        # the audio threads held in a fight (the main thread empties their queue: SSBU_RB_AUDIO_PUMP)
    'SSBU_RB_SIM_SKIP': '0',         # re-run passes do the whole presentation work (the blue / black fighters)
    'SSBU_RB_SIM_LITE': '0',
    'SSBU_RB_NET_LASTLITE': '0',
}


def auto_update(gini):
    """(fork, 2026-10-30) "Play SSBU.bat" brings the game up to date by itself: when GitHub's main branch has a newer commit
    than the one update.py last installed (build\\update_manifest.json), the installer runs as its Resume does (update.py,
    the new prebuilt runtime, the build steps that changed) and run.py starts again with the new code. True when it ran
    (the caller starts run.py again). One API call of a few hundred ms when nothing is new; no internet or a GitHub error:
    the installed game is played. Not in a git checkout (a developer's own copy: update.py by hand), not after a restart
    (SSBU_PLAY_UPDATED), and graphics.ini auto_update = off turns it off. Play SSBU.bat itself is never changed by it while it
    runs: cmd.exe reads a .bat at its byte offset, so a rewritten launcher would run broken lines."""
    if os.environ.get('SSBU_PLAY_UPDATED') or ini_get(gini, 'auto_update') in ('off', '0', 'no', 'false'):
        return False
    if os.path.exists(os.path.join(REPO, '.git')):
        return False
    import update
    have = (update.read_manifest() or {}).get('commit')
    try:
        want = update.http('https://api.github.com/repos/%s/commits/%s' % (update.DEFAULT_REPO, update.DEFAULT_BRANCH),
                           'application/vnd.github.sha', timeout=8).decode().strip()
    except Exception as e:                            # no internet, GitHub down or its rate limit: play what is installed
        print('Update check skipped (%s): playing the installed version.' % e)
        return False
    # the code may be new already ("Update only" in the installer window runs update.py alone) while the prebuilt runtime
    # it names was never downloaded and installed: install.py keeps the package in deps\ once it used it
    pending = None
    try:
        with open(os.path.join(REPO, 'scripts', 'install.py'), encoding='utf-8') as f:
            m = re.search(r"RUNTIME_RELEASE = \('([^']+)'", f.read())
        if m and not os.path.isfile(os.path.join(REPO, 'deps', m.group(1).rsplit('/', 1)[-1])):
            pending = m.group(1).rsplit('/', 1)[-1]
    except OSError:
        pass
    if not re.fullmatch(r'[0-9a-f]{40}', want) or (want == have and not pending):
        return False
    print('=' * 78)
    if want != have:
        print('A new version of the game is on GitHub (%s): installing it before the game starts.' % want[:7])
    else:
        print('The new game program (%s) is not installed yet: installing it before the game starts.' % pending)
    print('This usually takes under a minute (longer when the game itself has to be built again). Do not close this window.')
    print('=' * 78, flush=True)
    rc = subprocess.run([sys.executable, '-u', os.path.join(REPO, 'scripts', 'install.py'), '--no-gui', '--no-shortcuts'],
                        cwd=REPO).returncode
    if rc and runtime_only_update(os.path.join(os.path.dirname(os.path.dirname(gini)), 'work')):
        print('\nUpdate installed (the game program only).\n')
    elif rc:
        print('\nThe update did not finish (code %d). The game starts anyway; "Install SSBU.bat" > Resume finishes it.\n' % rc)
    else:
        print('\nUpdate installed.\n')
    return True


def update_watch_wanted(gini):
    """(fork, 2026-11-01) the same conditions as auto_update, minus the restart guard: a player's installed copy, not a git checkout."""
    return not os.path.exists(os.path.join(REPO, '.git')) and ini_get(gini, 'auto_update') not in ('off', '0', 'no', 'false')


def update_watch(flag):
    """(fork, 2026-11-01) While the game runs: GitHub's main branch is asked every 10 minutes (one API call); a commit newer
    than the installed one (build\\update_manifest.json) writes `flag` (its first 7 characters). The game reads that file
    (menu.cpp update_check): a message, and F1 > "Update and restart", which starts this launcher again in a new window
    (SSBU_UPDATE_CMD) and closes the game; auto_update then installs it before the game opens again."""
    import threading
    import update

    def watch():
        have = (update.read_manifest() or {}).get('commit')
        while True:
            time.sleep(600)
            try:
                want = update.http('https://api.github.com/repos/%s/commits/%s' % (update.DEFAULT_REPO, update.DEFAULT_BRANCH),
                                   'application/vnd.github.sha', timeout=8).decode().strip()
            except Exception:                         # no internet, GitHub down or its rate limit: next time
                continue
            if re.fullmatch(r'[0-9a-f]{40}', want) and want != have:
                try:
                    with open(flag, 'w') as f:
                        f.write(want[:7])
                    print('A new version of the game is on GitHub (%s): F1 > "Update and restart" installs it.' % want[:7], flush=True)
                except OSError:
                    pass
                return
    threading.Thread(target=watch, daemon=True).start()


def wait_for_old_game():
    """(fork, 2026-11-01) started by the game's "Update and restart" (SSBU_UPDATE_WAIT_PID): the old game is let close before
    the update replaces its files (at most 60 s)."""
    pid = os.environ.pop('SSBU_UPDATE_WAIT_PID', '')
    if os.name != 'nt' or not pid.isdigit():
        return
    import ctypes
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x00100000, False, int(pid))   # SYNCHRONIZE
    if h:
        print('Waiting for the game to close...', flush=True)
        k.WaitForSingleObject(h, 60000)
        k.CloseHandle(h)


def runtime_only_update(work):
    """(fork, 2026-10-30) The installer stopped (a tester's copy had no extracted dump left and the game files had moved: its
    "Find your game files" step cannot pass), but a new prebuilt runtime changes only the exe and its DLLs, which need no
    game files. The package install.py names is downloaded and checked (its SHA-256; it fits this copy of the project; its
    game-data interface is the one of the game DLL built here, build\\work\\game_data.h) and its .exe / .dll files are copied
    into build\\work, the old exe kept as ssbu_native_previous.exe. True when done; on any doubt False (nothing changed)."""
    import hashlib
    import zipfile
    import setup
    import update
    try:
        with open(os.path.join(REPO, 'scripts', 'install.py'), encoding='utf-8') as f:
            m = re.search(r"RUNTIME_RELEASE = \('([^']+)',\s*'([0-9a-f]{64})'\)", f.read())
        if not m or not os.path.isfile(os.path.join(work, 'ssbu_game_eh_o1.dll')):
            return False
        url, sha = m.group(1), m.group(2)
        dst = os.path.join(REPO, 'deps', url.rsplit('/', 1)[-1])
        if not (os.path.isfile(dst) and hashlib.sha256(open(dst, 'rb').read()).hexdigest() == sha):
            print('Downloading the game program only: %s' % url, flush=True)
            data = update.http(url, timeout=300)
            if hashlib.sha256(data).hexdigest() != sha:
                print('The download does not match its SHA-256: not used.')
                return False
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst + '.part', 'wb') as f:
                f.write(data)
            os.replace(dst + '.part', dst)
        man = setup.read_manifest(dst)
        why = setup.runtime_mismatch(man) if man else 'not a runtime package'
        if why:
            print('The game program cannot be installed alone: %s' % why)
            return False
        with open(os.path.join(work, 'game_data.h'), encoding='utf-8', errors='replace') as f:
            built = re.search(r'#define GAME_DATA_VERSION (\d+)', f.read())
        if not built or int(built.group(1)) != man.get('game_data_version'):
            print('The game program cannot be installed alone: the game here was built for another game-data interface '
                  '(%s, the package %s): "Install SSBU.bat" > Resume is needed.' % (built.group(1) if built else '?', man.get('game_data_version')))
            return False
        files = {n: v for n, v in man['files'].items() if n.lower().endswith(('.exe', '.dll'))}
        exe = os.path.join(work, 'ssbu_native.exe')
        with zipfile.ZipFile(dst) as z:
            for n, v in files.items():
                data = z.read(n)
                if hashlib.sha256(data).hexdigest() != v['sha256']:
                    print('%s in the package does not match its manifest: not installed.' % n)
                    return False
            if os.path.isfile(exe):
                shutil.copy2(exe, os.path.join(work, 'ssbu_native_previous.exe'))
            for n in files:
                with open(os.path.join(work, n + '.new'), 'wb') as f:
                    f.write(z.read(n))
                os.replace(os.path.join(work, n + '.new'), os.path.join(work, n))
        print('Installed %s from %s (the previous exe is ssbu_native_previous.exe).' % (', '.join(sorted(files)), os.path.basename(dst)))
        return True
    except Exception as e:                            # a download, a locked file, a bad zip: the installed game is played
        print('The game program could not be installed alone (%s).' % e)
        return False


def ensure_firewall(exe, build, gini):
    """(fork, 2026-10-30) An inbound Windows Firewall rule for the game, made once with one administrator prompt. Without it
    a friend's Windows drops the lobby's beacon and the other games' packets on the Radmin network (usually a Public network):
    their game makes a lobby of its own, the others move to it, everything is relayed through that PC (ping 150 ms, ten
    times the rollbacks; testers' logs 2026-10-29). The rule is named "SSBU native" like the one of "Instalar fix 60fps.bat";
    a refused prompt is remembered in <build>\\firewall_refused.txt and not asked again (delete the file to be asked again).
    graphics.ini firewall = off turns the check off."""
    if ini_get(gini, 'firewall') in ('off', '0', 'no', 'false'):
        return
    refused = os.path.join(build, 'firewall_refused.txt')

    def has_rule():
        try:
            out = subprocess.run(['netsh', 'advfirewall', 'firewall', 'show', 'rule', 'name=SSBU native', 'verbose'],
                                 capture_output=True, timeout=15).stdout.decode('mbcs', 'replace')
        except (OSError, subprocess.SubprocessError):
            return None                               # netsh missing or stuck: nothing is asked
        return os.path.normcase(exe) in os.path.normcase(out)

    have = has_rule()
    if have is None or have:
        return
    try:
        with open(refused, encoding='utf-8') as f:
            if os.path.normcase(f.read().strip()) == os.path.normcase(exe):
                return
    except OSError:
        pass
    print('The Windows Firewall has no rule for the game yet: without it the other players may not reach you (disconnects,\n'
          'high ping, error 2618). Windows now asks for administrator permission ONCE to add it. Press "Yes".')
    inner = ("New-NetFirewallRule -DisplayName 'SSBU native' -Direction Inbound -Program '%s' -Action Allow -Profile Any | Out-Null"
             % exe.replace("'", "''"))
    outer = ("Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile','-Command','%s'"
             % inner.replace("'", "''"))
    try:
        subprocess.run(['powershell', '-NoProfile', '-Command', outer], capture_output=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        pass
    if has_rule():
        print('Firewall rule added.')
        return
    print('The firewall rule was not added (the permission was refused). Not asked again; to be asked again delete\n  %s' % refused)
    try:
        with open(refused, 'w', encoding='utf-8') as f:
            f.write(exe)
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--userdata', help='the save folder (required, except with --play)')
    ap.add_argument('--play', action='store_true', help='what "Play SSBU.bat" does: the saves in <build>\\saves, until you quit; '
                                                        'a plain message when the game is not built yet')
    ap.add_argument('--build', default=None, help='build folder (default: the install location of build_location.txt, else <repo>/build; <repo>/build-linux with --linux)')
    ap.add_argument('--linux', action='store_true', help='the native Linux build (default build folder build-linux, exe ssbu_native, '
                                                        'saves in ~/.local/share/ssbu-recomp/userdata unless --userdata)')
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0 = play until you quit)')
    ap.add_argument('--seq', help='input sequence file (tests/seq_*.txt): lines starting with # are comments')
    ap.add_argument('--exe', help='exe name inside build/work (default ssbu_native.exe, ssbu_native with --linux); a copy of it is made if missing')
    ap.add_argument('--refresh', action='store_true', help='copy ssbu_native.exe over --exe again when the copy is older than it')
    ap.add_argument('--seed', help='SSBU_FIXED_SEED value (deterministic random numbers)')
    ap.add_argument('--log', help='write the game output to this file instead of the console')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE, may be repeated')
    ap.add_argument('--mac', action='store_true', help='the native macOS build (default build folder build-macos, exe ssbu_native, '
                                                      'saves in ~/Library/Application Support/ssbu-recomp/userdata unless --userdata)')
    a = ap.parse_args()
    wait_for_old_game()
    if a.play and os.name == 'nt' and not (a.linux or a.mac or a.build) and \
            auto_update(os.path.join(os.path.abspath(build_location.get()), 'config', 'graphics.ini')):
        # the new run.py (and the rest) runs from here on; the launcher's own arguments again
        return subprocess.run([sys.executable, os.path.abspath(__file__)] + sys.argv[1:],
                              env=dict(os.environ, SSBU_PLAY_UPDATED='1')).returncode
    if sys.platform.startswith('linux') and not a.linux and not a.build and not os.path.exists(os.path.join(REPO, 'build', 'state.json')):
        a.linux = True
    if sys.platform == 'darwin' and not a.mac and not a.linux and not a.build and os.path.exists(os.path.join(REPO, 'build-macos', 'state.json')):
        a.mac = True
    if a.mac:
        a.build = os.path.abspath(a.build or os.path.join(REPO, 'build-macos'))
        if not a.userdata:
            a.userdata = os.path.expanduser('~/Library/Application Support/ssbu-recomp/userdata')
        if not os.path.exists(os.path.join(a.build, 'state.json')):
            print('no build in %s: run scripts/setup.py --target macos first (BUILDING_MAC.md)' % a.build)
            return 1
        if a.play and not a.log:                      # the game's log to a file (as --play does on Windows), the last 10 kept
            logs = os.path.join(a.build, 'logs')
            os.makedirs(logs, exist_ok=True)
            a.log = os.path.join(logs, time.strftime('game_%Y%m%d_%H%M%S.log'))
            old = sorted(n for n in os.listdir(logs) if n.startswith('game_') and n.endswith('.log'))
            for n in old[:-9]:
                try:
                    os.remove(os.path.join(logs, n))
                except OSError:
                    pass
            print('Playing. The game log goes to %s. Close the game window to quit.' % a.log)
    elif a.linux:
        a.build = os.path.abspath(a.build or os.path.join(REPO, 'build-linux'))
        if not a.userdata:
            a.userdata = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ssbu-recomp', 'userdata')
        if not os.path.exists(os.path.join(a.build, 'state.json')):
            print('no build in %s: run scripts/setup.py --target linux first (BUILDING_LINUX.md)' % a.build)
            return 1
    else:
        a.build = os.path.abspath(a.build or build_location.get())   # the game runs in build\work: a relative --build would point elsewhere there
        if a.play:
            a.userdata = a.userdata or os.path.join(a.build, 'saves')
            if not (os.path.isfile(os.path.join(a.build, 'work', 'ssbu_native.exe')) and os.path.isfile(os.path.join(a.build, 'state.json'))):
                print('The game is not built yet (looked in %s): double-click "Install SSBU.bat" first.' % a.build)
                return 1
            gone = [f for f in ('ssbu_game_eh_o1.dll', 'SDL3.dll', 'ssbu_glslang.dll') if not os.path.isfile(os.path.join(a.build, 'work', f))]
            if gone:
                print('The game files are incomplete: %s missing in %s.\n'
                      'Usually an antivirus removed it (Windows Security > Virus & threat protection > Protection history: restore it\n'
                      'and allow it), or the installation stopped before the end. Double-click "Install SSBU.bat" and press Resume:\n'
                      'it makes the missing files again in a few minutes.' % (', '.join(gone), os.path.join(a.build, 'work')))
                return 1
            if not a.log:                             # the game's output to a file, not the console: a console that keeps
                logs = os.path.join(a.build, 'logs')  # scrolling costs frames (testers, 2026-10-20); F1 > Log shows the file
                os.makedirs(logs, exist_ok=True)
                a.log = os.path.join(logs, time.strftime('game_%Y%m%d_%H%M%S.log'))
                old = sorted(n for n in os.listdir(logs) if n.startswith('game_') and n.endswith('.log'))
                for n in old[:-9]:                    # keep the last 10 game logs
                    try:
                        os.remove(os.path.join(logs, n))
                    except OSError:
                        pass
                print('Playing. The game log goes to %s (F1 > Log shows it). This window can stay minimised.' % a.log)
        if not a.userdata:
            ap.error('--userdata is required (or --play)')
    main_exe = 'ssbu_native' if (a.linux or a.mac) else 'ssbu_native.exe'
    a.exe = a.exe or main_exe
    state_file = os.path.join(a.build, 'state.json')
    with open(state_file) as f:
        state = json.load(f)
    work = os.path.join(a.build, 'work')
    rollback = False
    gini = os.path.join(a.build, 'config', 'graphics.ini')
    if a.play and a.exe == main_exe and not (a.linux or a.mac) and ini_get(gini, 'rollback') not in ('off', '0', 'no', 'false'):   # (fork: on unless graphics.ini says off)
        # F1 > Experimental > Rollback netcode: the normal exe and game DLL with SSBU_RB_NET (since R4 the ring saves the fight on
        # the normal build: runtime/rb_learned.c); the old pair ssbu_native_rb.exe + ssbu_game_r1.dll of an older install is not used
        rollback = True
        print('Rollback netcode on (the normal build; no second compile needed)')
    exe = os.path.join(work, a.exe)
    native = os.path.join(work, main_exe)
    if not os.path.exists(exe):
        shutil.copy2(native, exe)
    elif exe != native and os.path.getmtime(exe) < os.path.getmtime(native):
        # a copy made before the last link runs the old runtime; an exe linked on purpose with other sources
        # (RECOMP_EXE) is older too, so it is only replaced when asked
        if a.refresh:
            shutil.copy2(native, exe)
            print('%s was older than %s: copied again' % (a.exe, main_exe))
        else:
            print('note: %s is older than %s and may run an older runtime (--refresh copies it again)' % (a.exe, main_exe))
    # the window's icon and the loading animation's pictures (display.c / loading_anim.c read them beside the exe): newer
    # ones after an update are copied at once (the build copies them only at link)
    for name in ('opensmash.ico', 'loading_kong.png', 'loading_kong_back.png', 'loading_arm.png'):
        logo = os.path.join(REPO, 'media', name)
        if os.path.isfile(logo) and not (a.linux or a.mac):
            dst = os.path.join(work, name)
            try:
                if not os.path.isfile(dst) or open(dst, 'rb').read() != open(logo, 'rb').read():
                    shutil.copy2(logo, dst)
            except OSError:
                pass
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    romfs = state['dump']['romfs']
    if not os.path.isdir(romfs) and os.path.isdir(os.path.join(a.build, 'dump', 'romfs')):
        romfs = os.path.join(a.build, 'dump', 'romfs')       # the folder was moved: state.json still names the old place
    env['SSBU_ROMFS'] = romfs
    env['SSBU_USERDATA'] = os.path.abspath(a.userdata)
    if not (a.linux or a.mac) and not any(os.path.isfile(os.path.join(work, n)) for n in ('libopus-0.dll', 'opus.dll')):
        # libopus decodes the game's music and most sounds (testers' log 2026-10-20: "libopus NOT FOUND", no sound): the
        # copy install.py fetched into deps\opus is used when the one beside the exe is missing
        dep = os.path.join(REPO, 'deps', 'opus', 'libopus-0.dll')
        if os.path.isfile(dep):
            env['SSBU_OPUS_DLL'] = dep
        else:
            print('WARNING: libopus-0.dll is missing (beside the game and in deps\\opus): the music and most sounds will be silent.\n'
                  'Double-click "Install SSBU.bat" and press Resume: it downloads it again (an antivirus may have removed it).')
    for sub, var in (('fonts', 'SSBU_FONTS'), ('zoneinfo', 'SSBU_TZDIR')):
        p = os.path.join(a.build, 'sysdata', sub)
        if os.path.isdir(p):
            env[var] = p
    if a.seq:
        with open(a.seq) as f:
            env['SSBU_SEQ'] = ' '.join(l.strip() for l in f if l.strip() and not l.startswith('#'))
    if a.seed:
        env['SSBU_FIXED_SEED'] = a.seed
    if rollback and 'SSBU_RB_NET' not in env:
        env['SSBU_RB_NET'] = 'net'                    # rollback over the online connection (net_relay); needs online play on
    if a.play and rollback:                           # (fork) the group's settings and lobby, see PLAY_DEFAULTS
        for k, v in PLAY_DEFAULTS.items():
            env.setdefault(k, v)
        if ini_get(gini, 'group_lobby') not in ('off', '0', 'no', 'false'):   # (graphics.ini group_lobby = off: F1 > Online's mode instead)
            env.setdefault('SSBU_ONLINE', PLAY_LOBBY)
        print('Online: %s (play in Local Wireless rooms: 1-on-1 rooms use rollback), rollback on, input delay %s' % (
            'same network (Radmin): the first game started is the lobby' if env.get('SSBU_ONLINE') == 'lan' else 'lobby %s' % env.get('SSBU_ONLINE', 'from graphics.ini'),
            env.get('SSBU_RB_NET_DELAY') or '%s (F1 > Gameplay)' % (ini_get(gini, 'input_delay') or '3')))
    if a.mac and not (env.get('VK_DRIVER_FILES') or env.get('VK_ICD_FILENAMES')):
        # MoltenVK (the Vulkan driver on Metal) named for Homebrew's Vulkan loader, as play_mac.sh does: run.py started by hand
        icd = next((p for p in ('/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json', '/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json',
                                '/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json', '/usr/local/share/vulkan/icd.d/MoltenVK_icd.json')
                    if os.path.isfile(p)), None)
        if icd:
            env['VK_DRIVER_FILES'] = icd
    if a.play and os.name == 'nt' and not (a.linux or a.mac) and a.exe == main_exe and update_watch_wanted(gini):
        flag = os.path.join(a.build, 'update_ready.txt')    # (fork) a newer version found while playing (update_watch)
        try:
            os.remove(flag)
        except OSError:
            pass
        env['SSBU_UPDATE_FLAG'] = flag
        env['SSBU_UPDATE_CMD'] = subprocess.list2cmdline([sys.executable, os.path.abspath(__file__)] + sys.argv[1:])
        env['SSBU_UPDATE_DIR'] = REPO                  # (where "Play SSBU.bat" runs it)
        update_watch(flag)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    if a.play and os.name == 'nt' and not (a.linux or a.mac) and env.get('SSBU_ONLINE', '') not in ('off', '0', 'no', 'false'):
        ensure_firewall(os.path.abspath(exe), a.build, gini)
    os.makedirs(a.userdata, exist_ok=True)
    os.makedirs(os.path.join(a.build, 'cache'), exist_ok=True)      # the runtime creates ..\cache\vk below it but not the folder itself (found 2026-10-13: with SSBU_VK_PREP=0 on a clean build nothing was saved)
    cmd = [exe, os.path.join(a.build, 'binaries', state.get('game_version', '13.0.4'), 'main.elf'), str(a.seconds)]
    t0 = time.time()
    out = open(a.log, 'wb') if a.log else None
    p = subprocess.run(cmd, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT if out else None)
    print('game exited with code %d after %.0f s' % (p.returncode, time.time() - t0))
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
