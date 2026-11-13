"""Build and atomically install the Linux runtime used by normal play.

    deps/venv/bin/python scripts/linux/prepare_current_runtime.py [--jobs 4]

Uses the local checkout, without fetching updates. Existing generated game code is reused;
use setup.py when the lifter or game-library interface needs rebuilding. --exe can retain
an explicitly named experimental runtime instead of updating the normal ssbu_native.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def setup_module():
    spec = importlib.util.spec_from_file_location('setup_linux_runtime', ROOT / 'scripts/setup.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def commit_id():
    result = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else 'local'


def manifest_path(build, exe='ssbu_native'):
    return Path(build) / (exe + '.runtime.json')


def generated_headers(build):
    # Use repository headers when available; copy the remaining headers from the installed build.
    return sorted(
        path for path in (Path(build) / 'work').iterdir()
        if path.is_file()
        and path.suffix in ('.h', '.inc')
        and not (ROOT / 'runtime' / path.name).exists()
    )


def source_identity(build):
    """Hash the sources, build scripts and generated headers used to compile the runtime."""
    setup = setup_module()
    digest = hashlib.sha256(setup.tree_digest().encode())
    build_scripts = [ROOT / 'scripts/linux/prepare_current_runtime.py', ROOT / 'scripts/setup.py']
    for path in build_scripts + generated_headers(build):
        digest.update(path.name.encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def game_identity():
    # These files can change generated code or its shared data layout, so changes require setup.
    files = sorted((ROOT / 'tools/lifter').rglob('*.py'))
    files = [path for path in files if path.name != 'build.py']
    files += sorted((ROOT / 'fighter_sdk').rglob('*.h'))
    files += [ROOT / 'runtime' / name for name in ('runtime.h', 'game_data.h', 'fpexact.h', 'neon.h')]
    digest = hashlib.sha256()
    for path in files:
        if path.exists():
            digest.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def read_manifest(build, exe='ssbu_native'):
    try:
        data = json.loads(manifest_path(build, exe).read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def status(build, exe='ssbu_native'):
    """Return whether the installed runtime matches local sources, and explain why."""
    build = Path(build)
    manifest = read_manifest(build, exe)
    try:
        if (build / (exe + '.runtime.pending.json')).exists():
            return False, 'an earlier runtime build did not finish'
        if not manifest:
            return False, 'no installed runtime manifest'
        if manifest['commit'] != commit_id() or manifest['source_identity'] != source_identity(build):
            return False, 'runtime sources or generated headers changed'
        if manifest['executable_sha256'] != sha256(build / 'work' / exe):
            return False, 'installed executable changed or is missing'
        library_stat = (build / 'work/ssbu_game_eh_o1.so').stat()
        if manifest['game_library_stat'] != [library_stat.st_size, library_stat.st_mtime_ns]:
            return False, 'generated game library changed'
        return True, 'installed runtime matches the local checkout'
    except (OSError, KeyError):
        return False, 'installed runtime is incomplete'


def needs_game_setup(build):
    """Check whether known game-code inputs changed since the last recorded build."""
    manifest = read_manifest(build)
    return bool(
        manifest.get('game_identity_version') == 1
        and manifest.get('game_identity') != game_identity()
    )


def atomic_json(path, data):
    """Replace a JSON file only after its complete contents have been written."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(data, stream, indent=2)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def atomic_copy(source, target):
    """Copy beside the destination, then rename so readers cannot see a partial file."""
    fd, temporary = tempfile.mkstemp(prefix=target.name + '.', dir=target.parent)
    os.close(fd)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def stage_candidate_sources(build, installed_work, candidate_work):
    """Copy current sources and generated headers into the separate build directory."""
    setup = setup_module()
    candidate_work.mkdir(parents=True, exist_ok=True)
    staged_files = []

    for source, target, extension in setup.STAGE_PLAN:
        for directory, folders, files in os.walk(ROOT / source):
            folders[:] = [name for name in folders if name != '__pycache__']
            for name in files:
                if extension and not name.endswith(extension):
                    continue
                source_path = Path(directory) / name
                destination = candidate_work / target / source_path.relative_to(ROOT / source)
                setup.copy_if_changed(str(source_path), str(destination))
                staged_files.append(str(destination.relative_to(candidate_work)))

    # Remove files deleted from the repository so the cache cannot keep compiling them.
    staged_manifest = candidate_work / 'staged-files.json'
    if staged_manifest.exists():
        removed_files = set(json.loads(staged_manifest.read_text())) - set(staged_files)
        for name in removed_files:
            stale = candidate_work / name
            if stale.resolve().is_relative_to(candidate_work.resolve()) and stale.is_file():
                stale.unlink()
    atomic_json(staged_manifest, staged_files)

    for header in generated_headers(build):
        setup.copy_if_changed(str(header), str(candidate_work / header.name))

    # The candidate links to the installed libraries; it does not rebuild them.
    for name in ('ssbu_game_eh_o1.so', 'ssbu_glslang.so'):
        link = candidate_work / name
        if link.is_symlink() and link.resolve() != installed_work / name:
            link.unlink()
        if not link.exists():
            link.symlink_to(installed_work / name)


def build_environment(build, exe, jobs, clang, commit):
    """Use explicit tool paths first, then the paths saved by the last successful build."""
    tools = read_manifest(build, exe).get('tools', {})
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(('RECOMP_', 'SSBU_'))
    }
    env.update(
        RECOMP_TARGET='linux',
        RECOMP_CLANG=clang,
        RECOMP_GEN=str(build / 'work/gen_eh_o2'),
        RECOMP_EXE=exe,
        RECOMP_DLL='ssbu_game_eh_o1.so',
        RECOMP_VERSION=commit[:7],
        RECOMP_REUSE_GAME_DLL='1',
        RECOMP_HOST_JOBS=str(jobs),
    )
    dependency_defaults = {
        'IMGUI_ROOT': ROOT / 'deps/imgui',
        'VULKAN_HEADERS': ROOT / 'deps/Vulkan-Headers',
        'SUYU_ROOT': ROOT / 'deps/suyu',
    }
    for variable, default in dependency_defaults.items():
        env[variable] = os.environ.get(variable) or tools.get(variable) or str(default)

    pkg_config_paths = (
        str(Path.home() / '.local/lib/pkgconfig'),
        env.get('PKG_CONFIG_PATH') or tools.get('PKG_CONFIG_PATH', ''),
    )
    env['PKG_CONFIG_PATH'] = os.pathsep.join(
        dict.fromkeys(path for path in pkg_config_paths if path)
    )
    return env


def refresh_compiler_cache(candidate_work, exe, clang, source_digest, env):
    """Discard objects when compiler settings or build inputs change."""
    build_variable_prefixes = ('RECOMP_', 'IMGUI_', 'VULKAN_', 'SUYU_', 'PKG_CONFIG_')
    config = {
        'clang': clang,
        'source_identity': source_digest,
        'compiler': subprocess.check_output([clang, '--version'], text=True),
        'environment': {
            key: value for key, value in env.items()
            if key.startswith(build_variable_prefixes)
        },
    }
    config_path = candidate_work / 'compiler-config.json'
    if not config_path.exists() or json.loads(config_path.read_text()) != config:
        shutil.rmtree(candidate_work / 'obj_host', ignore_errors=True)
    atomic_json(config_path, config)

    # build.py checks timestamps, but the version stamp is a compiler definition.
    # Refresh boot.obj even when boot.c itself has not changed.
    (candidate_work / 'obj_host' / exe / 'boot.obj').unlink(missing_ok=True)


def validate_candidate(build, installed_work, candidate, env):
    """Run the exception self-test with scratch saves and online play disabled."""
    state = json.loads((build / 'state.json').read_text())
    main_elf = build / 'binaries' / state.get('game_version', '13.0.4') / 'main.elf'
    scratch = build / 'selftest_userdata/runtime-update'
    scratch.mkdir(parents=True, exist_ok=True)
    test_env = dict(
        env,
        SSBU_ONLINE='off',
        SSBU_CXXRT_SELFTEST='1',
        SSBU_USERDATA=str(scratch),
        SSBU_ROMFS=state['dump']['romfs'],
    )
    subprocess.run(
        [str(candidate), str(main_elf), '5'],
        cwd=installed_work,
        env=test_env,
        check=True,
        timeout=120,
    )


def publish_candidate(
    build, exe, candidate, *, commit, source_digest, library_hash, clang, env, started,
):
    """Keep the previous executable, install the candidate, and record its build inputs."""
    installed_work = build / 'work'
    destination = installed_work / exe
    library = installed_work / 'ssbu_game_eh_o1.so'
    saved_tool_paths = ('IMGUI_ROOT', 'VULKAN_HEADERS', 'SUYU_ROOT', 'PKG_CONFIG_PATH')
    manifest = {
        'commit': commit,
        'source_identity': source_digest,
        'game_identity': game_identity(),
        'game_identity_version': 1,
        'executable': str(destination),
        'executable_sha256': sha256(candidate),
        'game_library_sha256': library_hash,
        'game_library_stat': [library.stat().st_size, library.stat().st_mtime_ns],
        'seconds': round(time.monotonic() - started, 2),
        'validation': 'cxxrt self-test passed',
        'tools': dict(clang=clang, **{key: env[key] for key in saved_tool_paths}),
    }
    if destination.exists():
        atomic_copy(destination, installed_work / (exe + '_previous'))
    atomic_copy(candidate, destination)
    atomic_json(manifest_path(build, exe), manifest)

    # Leave the pending marker in place until both the executable and manifest are installed.
    (build / (exe + '.runtime.pending.json')).unlink(missing_ok=True)
    print('Installed:', destination, flush=True)


def prepare(build, exe, jobs, clang, setup_complete=False):
    """Stage, compile, validate, then publish one runtime while holding the build lock."""
    # Import here so Windows and macOS launchers can import the module without loading fcntl.
    import fcntl

    build = Path(build).resolve()
    installed_work = build / 'work'
    library = installed_work / 'ssbu_game_eh_o1.so'
    if not library.exists():
        raise RuntimeError('generated game library missing; finish scripts/setup.py --target linux first')

    build.mkdir(parents=True, exist_ok=True)
    with open(build / 'linux-runtime.lock', 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another Linux runtime build is in progress; retry after it finishes')

        if exe == 'ssbu_native' and needs_game_setup(build) and not setup_complete:
            raise RuntimeError(
                'lifter/game interface changed: run scripts/setup.py --target linux '
                '--from tables with your build/dump options'
            )

        commit = commit_id()
        source_digest = source_identity(build)
        pending = build / (exe + '.runtime.pending.json')
        atomic_json(pending, dict(commit=commit, source_identity=source_digest))
        started = time.monotonic()

        # Compile separately so a failed build cannot overwrite the installed executable.
        candidate_work = build / 'cache/linux-runtime' / exe
        stage_candidate_sources(build, installed_work, candidate_work)
        library_hash = sha256(library)
        env = build_environment(build, exe, jobs, clang, commit)
        refresh_compiler_cache(candidate_work, exe, clang, source_digest, env)

        print('Building Linux runtime', commit[:7], 'with', jobs, 'compiler jobs', flush=True)
        subprocess.run(
            [sys.executable, '-u', 'build.py', 'link'],
            cwd=candidate_work,
            env=env,
            check=True,
        )
        if library_hash != sha256(library):
            raise RuntimeError('game library changed during runtime build; retry')
        if commit != commit_id() or source_digest != source_identity(build):
            raise RuntimeError('checkout/build inputs changed during compilation; retry')

        candidate = candidate_work / exe
        validate_candidate(build, installed_work, candidate, env)
        if (
            library_hash != sha256(library)
            or commit != commit_id()
            or source_digest != source_identity(build)
        ):
            raise RuntimeError('checkout/build inputs changed during validation; retry')

        publish_candidate(
            build, exe, candidate,
            commit=commit,
            source_digest=source_digest,
            library_hash=library_hash,
            clang=clang,
            env=env,
            started=started,
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, default=ROOT / 'build-linux')
    parser.add_argument('--exe', default='ssbu_native')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--clang', default=os.environ.get('RECOMP_CLANG'))
    parser.add_argument('--setup-complete', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--check', action='store_true', help='check installed runtime without building; exit 1 if stale')
    args = parser.parse_args(argv)
    if not sys.platform.startswith('linux'):
        parser.error('this helper targets Linux')
    saved_compiler = read_manifest(args.build, args.exe).get('tools', {}).get('clang')
    args.clang = args.clang or saved_compiler or shutil.which('clang-19') or shutil.which('clang')
    if args.jobs < 1 or not args.clang or Path(args.exe).name != args.exe or args.exe in ('.', '..'):
        parser.error('need a compiler, a positive --jobs value and a simple executable name')
    if args.check:
        current, reason = status(args.build, args.exe)
        print(reason)
        return 0 if current else 1
    try:
        prepare(args.build, args.exe, args.jobs, args.clang, args.setup_complete)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('Linux runtime not installed:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
