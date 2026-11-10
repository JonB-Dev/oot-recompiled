#!/usr/bin/env python3
"""Local-only worker for the native Mac builder. No shell interpolation of user input."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys

ROM_SHA1 = 'ad69c91157f6705e8ab06c79fe08aad47bb57ba7'
FORMULAE = ['cmake', 'ninja', 'llvm', 'lld', 'make', 'git', 'sdl2', 'freetype', 'python']


def verify_rom(path):
    if not path.is_file():
        raise ValueError('Choose a readable ROM file first.')
    digest = hashlib.sha1()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != ROM_SHA1:
        raise ValueError('This ROM does not match unmodified USA (NTSC-U) 1.0 in .z64 format. No build was started.')


@contextmanager
def build_lock(root):
    (root / 'build').mkdir(exist_ok=True)
    with (root / 'build/gui-builder.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('A builder is already working in this source folder.')
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def build_plan(root, rom, tracing):
    steps = [('Preparing source dependencies', ['git', 'submodule', 'update', '--init', '--recursive']),
             ('Building and testing the Mac app', ['bash', str(root / 'tools/build_macos.sh'), str(rom)])]
    if tracing:
        steps.append(('Building and testing fancy lighting', ['bash', str(root / 'tools/build_macos_rt_probe.sh')]))
    return steps


def check(root, tracing):
    problems = []
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        problems.append('An Apple Silicon Mac is required.')
    if shutil.disk_usage(root).free < 30 * 1024**3:
        problems.append('At least 30 GiB of free disk space is required before building.')
    if sys.version_info < (3, 10):
        problems.append('Python 3.10 or newer is required. Install build tools, then reopen this builder.')
    try:
        developer = subprocess.check_output(['xcode-select', '-p'], text=True).strip()
        if not developer.endswith('.app/Contents/Developer'):
            problems.append('Open full Xcode, complete its setup, then select it in Xcode → Settings → Locations → Command Line Tools.')
        if subprocess.run(['xcrun', '--sdk', 'macosx', 'metal', '--version'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            problems.append('Install Metal Toolchain in Xcode → Settings → Components.')
    except (OSError, subprocess.CalledProcessError):
        problems.append('Install and open Xcode to finish its first-run setup.')
    if not shutil.which('brew'):
        problems.append('Homebrew is required. Open the Homebrew setup link below.')
    else:
        required = [name for name in FORMULAE if name not in ('make', 'python')]
        if not shutil.which('make'): required.append('make')
        if sys.version_info < (3, 10): required.append('python')
        missing = [name for name in required if subprocess.run(['brew', 'list', '--versions', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode]
        if missing:
            problems.append('Click Install build tools. Missing: ' + ', '.join(missing))
    if tracing and not (root / 'build/metal-converter-local/usr/local/bin/metal-shaderconverter').is_file():
        problems.append('Select Apple’s Metal Shader Converter 4.0 beta 3 package, or turn off fancy lighting.')
    return problems


def run_command(command, **kwargs):
    # Give each stage its own process group so Cancel also stops compilers and helpers.
    child = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        code = child.wait()
        if code:
            raise subprocess.CalledProcessError(code, command)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()


def cancel(signum, frame):
    raise KeyboardInterrupt('Build cancelled. Completed downloads can be reused on retry.')


def source_folder(root):
    return root if (root / '.git').exists() else root / 'build/source'


def prepare_source(root):
    source = source_folder(root)
    if source == root:
        return source
    if not source.exists():
        print('STEP: Downloading the experiment source for this ZIP download', flush=True)
        run_command(['git', 'clone', '--branch', 'codex/macos-experiment', '--single-branch',
                     'https://github.com/boscacci/oot-recompiled.git', str(source)])
    if not (source / '.git').exists():
        raise ValueError('The source download is incomplete. Inspect build/source before retrying.')
    origin = subprocess.check_output(['git', '-C', str(source), 'remote', 'get-url', 'origin'], text=True).strip()
    if origin != 'https://github.com/boscacci/oot-recompiled.git':
        raise ValueError('The build/source checkout has an unexpected origin; it was not used.')
    return source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['check', 'build', 'install-tools', 'converter'])
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--package', type=Path)
    parser.add_argument('--accept-license', action='store_true')
    parser.add_argument('--tracing', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    signal.signal(signal.SIGTERM, cancel)
    os.environ['PYTHON_BIN'] = sys.executable
    os.environ['PYTHONUNBUFFERED'] = '1'
    try:
        if args.action == 'check':
            problems = check(root, args.tracing)
            print('\n'.join(problems) if problems else 'Ready. Choose your ROM, then click Build app.', flush=True)
            return bool(problems)
        with build_lock(root):
            if args.action == 'install-tools':
                if not shutil.which('brew'):
                    raise ValueError('Install Homebrew first using the setup link.')
                run_command(['brew', 'install', *FORMULAE])
                print('Build tools installed. Reopen the builder if Python was updated.', flush=True)
            elif args.action == 'converter':
                if not args.accept_license or not args.package or not args.package.is_file():
                    raise ValueError('Select the converter package and accept its license first.')
                run_command(['bash', str(root / 'tools/setup_metal_shader_converter.sh'), '--accept-license', str(args.package.resolve())])
            else:
                verify_rom(args.rom or Path('/nonexistent-rom'))
                problems = check(root, args.tracing)
                if problems:
                    raise ValueError('\n'.join(problems))
                source = prepare_source(root)
                os.environ['METAL_SHADERCONVERTER'] = str(root / 'build/metal-converter-local/usr/local/bin/metal-shaderconverter')
                revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
                print('Source commit: ' + revision, flush=True)
                for title, command in build_plan(source, args.rom.resolve(), args.tracing):
                    print('\nSTEP: ' + title, flush=True)
                    run_command(command, cwd=source)
                app = source / ('build/rt-compile-probe' if args.tracing else 'build-cmake') / 'OoTRecompiled.app'
                if not (app / 'Contents/MacOS/OoTRecompiled').is_file():
                    raise ValueError('The build finished without producing an app.')
                print('SUCCESS: App built and native tests passed. Click Open app.', flush=True)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        print('\nSTOPPED: ' + str(error), flush=True)
        return 1

if __name__ == '__main__':
    sys.exit(main())
