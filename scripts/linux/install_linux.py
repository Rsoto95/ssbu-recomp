#!/usr/bin/env python3
"""Build the native Linux game from your own packages or dump: the Flatpak's installer, and a command line.

    python scripts/linux/install_linux.py --base game.nsz --update update.nsz --keys prod.keys
    python scripts/linux/install_linux.py --base game.nsp --update update.nsp --keys prod.keys --dlc dlc1.nsz --dlc dlc2.nsz
    python scripts/linux/install_linux.py --dump <a folder with exefs/ and romfs/>
    python scripts/linux/install_linux.py --check-tools
    python scripts/linux/install_linux.py --dry-run --base ... --update ... --keys ...

The base and the update are the 13.0.4 or 13.0.5 packages you own: .nsz, .nsp or .xci. A .nsz is decompressed into
<storage>/nsz_cache first (about as large as the original file; removed after the dump unless --keep-cache). The dump is
about 17 GB; the build about 8 GB. Everything stays on this PC.

The game itself is never in this repository or in the Flatpak: the game library is generated from your dump by
scripts/setup.py --target linux. The first install takes 1.5-2.5 hours and can be stopped and continued.

DLC: only the add-on content ids in the file names are read (the content is already in the game update); they are written
to <build>/config/dlc.txt, which the game reads (runtime/nnmisc.c, nn::aoc). Nothing is decrypted or copied.

--progress-json adds lines starting with "##ssbu " and a JSON object, for the launcher window.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SETUP_STEPS = ['deps', 'stage', 'convert', 'modules', 'tables', 'lift', 'sdk', 'compile', 'nro', 'link', 'check']
BUILD_NEED_GB = 8.0
DUMP_NEED_GB = 17.0
SHADER_CACHE_GB = 4.0
PROGRESS = False
LOG = None
LOG_LOCK = threading.Lock()


def say(text=''):
    text = str(text)
    with LOG_LOCK:
        print(text, flush=True)
        if LOG:
            LOG.write(text + '\n')
            LOG.flush()


def emit(event, **kw):
    """A machine-readable line for scripts/linux/launcher.py (not written to install.log)."""
    if PROGRESS:
        with LOG_LOCK:
            print('##ssbu ' + json.dumps(dict(event=event, **kw), sort_keys=True), flush=True)


def free_gb(path):
    p = os.path.abspath(path)
    while p and not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    try:
        return shutil.disk_usage(p).free / 2.0 ** 30
    except OSError:
        return None


def default_storage():
    if os.environ.get('SSBU_DATA_DIR'):
        return os.environ['SSBU_DATA_DIR']
    if os.environ.get('FLATPAK_ID'):    # Flatpak's XDG_DATA_HOME is its own ~/.var/app, which an uninstall deletes
        return os.path.expanduser('~/.local/share/ssbu-recomp')
    return os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ssbu-recomp')


def tool_root(name, probe):
    """imgui / suyu: the Flatpak puts them in /app/share/ssbu, a developer's tree has them in deps/."""
    app = os.path.join('/app', 'share', 'ssbu', name)
    cands = [os.environ.get(name.upper() + '_ROOT'), app, os.path.join(REPO, 'deps', name)]
    for c in cands:
        if c and os.path.exists(os.path.join(c, probe)):
            return os.path.abspath(c)
    return None


def tool_args():
    out = []
    imgui = tool_root('imgui', os.path.join('imgui.h'))
    suyu = tool_root('suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))
    if imgui:
        out += ['--imgui', imgui]
    if suyu:
        out += ['--suyu', suyu]
    return out


def build_env(storage):
    env = dict(os.environ)
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUNBUFFERED'] = '1'
    tmp = os.path.join(storage, 'tmp')
    os.makedirs(tmp, exist_ok=True)
    env['TMPDIR'] = tmp
    # the Flatpak's clang: copied from org.freedesktop.Sdk.Extension.llvm20; a developer's tree uses the system clang.
    clang_dir = '/app/llvm20/bin'
    if os.path.isfile(os.path.join(clang_dir, 'clang')):
        env['PATH'] = clang_dir + os.pathsep + env.get('PATH', '')
    for name in ('imgui', 'suyu'):
        root = tool_root(name, 'imgui.h' if name == 'imgui' else
                         os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))
        if root:
            env[name.upper() + '_ROOT'] = root
    return env


def run_stream(argv, env, progress=None, cwd=None):
    """Run a child, print its output as it comes, and return its exit code. \\r progress lines become normal lines."""
    say('$ ' + ' '.join(argv))
    p = subprocess.Popen(argv, cwd=cwd or REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    buf = b''
    while True:
        chunk = p.stdout.read(4096)
        if not chunk:
            break
        buf += chunk.replace(b'\r', b'\n')
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            text = line.decode('utf-8', 'replace').rstrip()
            if text:
                say(text)
                if progress:
                    progress(text)
    if buf.strip():
        text = buf.decode('utf-8', 'replace').rstrip()
        say(text)
        if progress:
            progress(text)
    return p.wait()


def tail_log(log_path, stop, step):
    """Stream a step's own log (compile.log / nro.log) while setup.py writes it: the compilers print there, not to
    our stdout. Every new line goes to the launcher's log view, with counts and a heartbeat so a slow file never
    looks like a freeze."""
    offset = 0
    try:
        offset = os.path.getsize(log_path)
    except OSError:
        pass
    buf = ''
    done = total = 0
    elapsed = 0
    last = None
    last_seconds = None
    started = time.time()
    last_beat = time.time()
    while not stop.is_set():
        try:
            size = os.path.getsize(log_path)
            if size < offset:                          # truncated or rotated: follow the new file
                offset = 0
                buf = ''
            if size > offset:
                with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
                    f.seek(offset)
                    data = f.read()
                offset = size
                buf += data
                while '\n' in buf:
                    line, buf = buf.split('\n', 1)
                    if line.startswith('$ '):
                        continue
                    if line.strip():
                        say(line)
                    m = re.match(r'^sources:\s+(\d+)\s+to compile:\s+(\d+)', line)
                    if m:
                        total, done = int(m.group(2)), 0
                        started = time.time()
                        emit('compile', step=step, done=0, total=total, elapsed=0)
                        last_beat = time.time()
                        continue
                    m = re.match(r'^\s*(\d+)/(\d+)\s+(\d+)s elapsed', line)
                    if m:
                        done, total, elapsed = int(m.group(1)), int(m.group(2)), int(m.group(3))
                        emit('compile', step=step, done=done, total=total, elapsed=elapsed)
                        last_beat = time.time()
                        continue
                    m = re.match(r'^\s+(\S+)\s+(\d+)s\s+', line)
                    if m:
                        last, last_seconds = m.group(1), int(m.group(2))
                        emit('compile', step=step, done=done, total=total,
                             elapsed=int(time.time() - started), last=last, last_seconds=last_seconds)
                        last_beat = time.time()
                        continue
                    m = re.match(r'^compiled:\s+(\d+)\s+failed:\s+(\d+)', line)
                    if m:
                        done = int(m.group(1))
                        total = total or done
                        emit('status', text=line.strip())
                        emit('compile', step=step, done=done, total=total, elapsed=int(time.time() - started))
                        last_beat = time.time()
                    elif line.startswith('FAILED'):
                        emit('status', text=line.strip()[:200])
        except OSError:
            pass
        if time.time() - last_beat >= 8:
            emit('compile', step=step, done=done, total=total, elapsed=int(time.time() - started),
                 heartbeat=True, last=last, last_seconds=last_seconds)
            last_beat = time.time()
        stop.wait(0.4)


def dlc_rows(paths):
    """{index: label} from the add-on content ids [01006A800016Fxxx] in the file names. Only names are read."""
    app = 0x01006A800016E000
    names = []
    for p in paths:
        if os.path.isdir(p):
            names += sorted(os.listdir(p))
        else:
            names.append(os.path.basename(p))
    rows = {}
    for name in names:
        for m in re.finditer(r'\[([0-9A-Fa-f]{16})\]', name):
            index = int(m.group(1), 16) - app - 0x1000
            if 0 < index < 0x1000:
                rows[index] = ' '.join(re.sub(r'\[[^\]]*\]', ' ', os.path.splitext(name)[0]).split())
    return rows


def write_dlc(build, paths):
    """config/dlc.txt beside the build's work folder: what runtime/nnmisc.c nn::aoc reads. Written even when empty, so
    removing a DLC from the list removes it from the game."""
    out = os.path.join(build, 'config', 'dlc.txt')
    rows = dlc_rows(paths)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('# add-on contents owned (index = add-on content id - application id - 0x1000); written by install_linux.py\n')
        for index in sorted(rows):
            f.write('0x%03x   # %s\n' % (index, rows[index]))
    if rows:
        say('DLC: %d add-on content(s) listed in %s (the content is part of the game update)' % (len(rows), out))
    else:
        say('DLC: none listed (%s is empty: nothing is unlocked)' % out)
    return rows


def dump_progress(line):
    m = re.search(r'(\d+(?:\.\d+)?)%', line)
    if m:
        emit('step', step='dump', pct=float(m.group(1)))
    elif 'decompress' in line.lower():
        emit('status', text=line.strip()[:200])


def setup_progress(line):
    m = re.match(r'\[(\d+)/11\] (\w+)', line)
    if m:
        emit('step', step='setup', n=int(m.group(1)), of=11, name=m.group(2))


def read_install(storage):
    try:
        with open(os.path.join(storage, 'install.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def fail(text):
    say('FAILED: ' + text)
    emit('error', text=text)
    return 1


def check_dump(path):
    for rel in (('exefs', 'main'), ('exefs', 'sdk'), ('romfs', 'data.arc')):
        if not os.path.isfile(os.path.join(path, *rel)):
            return '%s is not a dump folder: %s missing' % (path, os.path.join(*rel))
    return None


def main(argv=None):
    global PROGRESS, LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', help='the base game: .nsz, .nsp or .xci (v0)')
    ap.add_argument('--update', help='the update 13.0.4 or 13.0.5: .nsz, .nsp or .xci')
    ap.add_argument('--keys', help='prod.keys (not needed with --dump; found in ~/.switch when omitted)')
    ap.add_argument('--dlc', action='append', default=[], help='a DLC package or a folder of them (repeatable); only names are read')
    ap.add_argument('--firmware', help='optional: your firmware folder or its extracted sysdata/fonts')
    ap.add_argument('--dump', help='use an existing dump folder (exefs/ and romfs/ inside) instead of packages')
    ap.add_argument('--storage', help='where dump/, build-linux/, userdata/ and nsz_cache/ go (default: %s)' % default_storage())
    ap.add_argument('--jobs', type=int, default=0, help='compiler jobs (default: setup.py decides, up to 24)')
    ap.add_argument('--no-inline', action='store_true', help='a shorter build, a slower game (setup.py --no-inline)')
    ap.add_argument('--keep-cache', action='store_true', help='keep the decompressed .nsp files (large)')
    ap.add_argument('--progress-json', action='store_true', help='also print "##ssbu {json}" lines for the launcher')
    ap.add_argument('--check-tools', action='store_true', help='check pkg-config / clang / the Python packages, no dump')
    ap.add_argument('--dry-run', action='store_true', help='print the plan, change nothing')
    a = ap.parse_args(argv)
    PROGRESS = a.progress_json

    storage = os.path.abspath(a.storage or default_storage())
    if not a.dry_run:
        os.makedirs(storage, exist_ok=True)
        LOG = open(os.path.join(storage, 'install.log'), 'w', encoding='utf-8', errors='replace')
    say('OpenSmash Ultimate: Linux install')
    say('storage: %s' % storage)
    build = os.path.join(storage, 'build-linux')
    userdata = os.path.join(storage, 'userdata')

    if a.check_tools:
        env = build_env(storage)
        cmd = [sys.executable, os.path.join(REPO, 'scripts', 'setup.py'), '--target', 'linux', '--tools-only',
               '--build', build] + tool_args() + (['--jobs', str(a.jobs)] if a.jobs else [])
        return 0 if run_stream(cmd, env) == 0 else 1

    dump = os.path.abspath(a.dump) if a.dump else os.path.join(storage, 'dump')
    if a.dump:
        why = check_dump(dump)
        if why:
            return fail(why)
    else:
        if not (a.base and a.update):
            return fail('choose the base game and the update (or --dump for a dump folder you already made)')
        for label, p in (('base game', a.base), ('update', a.update)):
            if not os.path.isfile(p):
                return fail('%s not found: %s' % (label, p))
        if a.keys and not os.path.isfile(a.keys):
            return fail('prod.keys not found: %s' % a.keys)
        if not a.keys and not os.path.isfile(os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')):
            return fail('package files are encrypted: choose your prod.keys')

    # space: dump + the .nsz decompression cache while it runs; the build after it; the shader cache on first play
    need = 0.0
    if not a.dump:
        need += DUMP_NEED_GB
        try:
            need += 1.25 * sum(os.path.getsize(p) for p in (a.base, a.update) if p.lower().endswith('.nsz')) / 2.0 ** 30
        except OSError:
            pass
    if not os.path.exists(os.path.join(build, 'state.json')):
        need += BUILD_NEED_GB
    if not os.path.isdir(os.path.join(build, 'cache')):
        need += SHADER_CACHE_GB
    free = free_gb(storage)
    if free is not None:
        say('disk: %.0f GB free, about %.0f GB needed (%s)' % (free, need, 'the first build' if need else 'nothing new'))
        if free < need and not a.dry_run:
            return fail('not enough free space in %s: %.0f GB free, about %.0f GB needed. Free some space or choose another storage folder.'
                        % (storage, free, need))

    if a.dry_run:
        say('would %s' % ('use the dump %s' % dump if a.dump else 'unpack %s + %s into %s' % (a.base, a.update, dump)))
        say('would build with: %s --target linux --build %s' % (os.path.join(REPO, 'scripts', 'setup.py'), build))
        rows = dlc_rows(a.dlc)
        if rows:
            say('DLC: ' + ', '.join('0x%03x' % i for i in sorted(rows)))
        return 0

    # ---- game files
    if a.dump:
        emit('step', step='dump', pct=100.0)
        emit('status', text='Using the dump folder %s' % dump)
    else:
        emit('step', step='dump', pct=0.0)
        emit('status', text='Unpacking the game files (about %d GB)...' % DUMP_NEED_GB)
        env = build_env(storage)
        env['SSBU_TMP'] = os.path.join(storage, 'nsz_cache')
        if a.keys:
            env['SSBU_PROD_KEYS'] = os.path.abspath(a.keys)
            title = os.path.join(os.path.dirname(os.path.abspath(a.keys)), 'title.keys')
            if os.path.isfile(title):
                env['SSBU_TITLE_KEYS'] = title
        part = dump + '.part'
        shutil.rmtree(part, ignore_errors=True)
        cmd = [sys.executable, os.path.join(REPO, 'tools', 'extract', 'dump_folder.py'),
               os.path.abspath(a.base), os.path.abspath(a.update), part]
        if a.firmware:
            cmd += ['--firmware', os.path.abspath(a.firmware)]
        if run_stream(cmd, env, progress=dump_progress) != 0:
            shutil.rmtree(part, ignore_errors=True)
            return fail('the game files could not be unpacked. Check that prod.keys belongs to the same console as the '
                        'game files, and that the base game and the update are the same game.')
        if os.path.isdir(dump):
            say('replacing the old dump in %s' % dump)
            shutil.rmtree(dump)
        os.replace(part, dump)
        if not a.keep_cache:
            shutil.rmtree(os.path.join(storage, 'nsz_cache'), ignore_errors=True)
        emit('step', step='dump', pct=100.0)
        emit('status', text='Game files ready.')

    # ---- build
    emit('step', step='build', pct=0.0)
    emit('status', text='Building the game (1.5-2.5 hours the first time; it can be stopped and continued)...')
    stop = threading.Event()
    logs = os.path.join(build, 'logs')
    tails = [threading.Thread(target=tail_log, args=(os.path.join(logs, 'compile.log'), stop, 'compile'), daemon=True),
             threading.Thread(target=tail_log, args=(os.path.join(logs, 'nro.log'), stop, 'nro'), daemon=True)]
    for tail in tails:
        tail.start()
    cmd = [sys.executable, os.path.join(REPO, 'scripts', 'setup.py'), '--target', 'linux', '--dump', dump,
           '--build', build] + tool_args() + (['--jobs', str(a.jobs)] if a.jobs else []) + (['--no-inline'] if a.no_inline else [])
    rc = run_stream(cmd, build_env(storage), progress=setup_progress)
    stop.set()
    for tail in tails:
        tail.join(timeout=2)
    if rc != 0:
        return fail('the build stopped (code %d). The next Install continues where it stopped; the log is %s.'
                    % (rc, os.path.join(storage, 'install.log')))

    rows = write_dlc(build, a.dlc)
    version = None
    try:
        with open(os.path.join(build, 'state.json'), encoding='utf-8') as f:
            version = json.load(f).get('game_version')
    except (OSError, ValueError):
        pass
    commit = None
    try:
        commit = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() or None
    except OSError:
        pass
    info = {'format': 1, 'storage': storage, 'dump': dump, 'build': build, 'userdata': userdata,
            'game_version': version, 'source_commit': commit, 'dlc': [os.path.abspath(p) for p in a.dlc],
            'built': time.strftime('%Y-%m-%d %H:%M:%S')}
    with open(os.path.join(storage, 'install.json'), 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=1)
    emit('done', build=build, dump=dump, userdata=userdata, game_version=version, dlc=sorted(rows))
    say('Installed: %s' % os.path.join(build, 'work', 'ssbu_native'))
    say('Start it with: %s --linux --build %s --play --userdata %s' % (
        os.path.join(REPO, 'scripts', 'run.py'), build, userdata))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
