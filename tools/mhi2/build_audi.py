#!/usr/bin/env python3
"""Build QEMU, bridge libraries and Audi A3 media in a new output directory."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from lzo_library import load_lzo2

SCRIPTS = Path(__file__).resolve().parent
REPO = SCRIPTS.parents[1]


def run(command, **kwargs):
    print('+ ' + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def build_dependencies(cache):
    """Fetch the pinned C build dependencies without Meson's shallow clones."""
    for name in ('keycodemapdb', 'berkeley-softfloat-3', 'berkeley-testfloat-3'):
        wrap = configparser.ConfigParser()
        wrap.read(REPO/'subprojects'/(name+'.wrap'))
        spec = wrap['wrap-git']
        destination = REPO/'subprojects'/name
        vendored = destination.exists() and not (destination/'.git').exists()
        vendored_source = destination
        if vendored:
            destination = cache/name
            cache.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            run(['git', 'clone', spec['url'], destination])
            run(['git', '-C', destination, 'checkout', '--detach', spec['revision']])
        shallow = subprocess.check_output(['git', '-C', str(destination),
                                           'rev-parse', '--is-shallow-repository'], text=True).strip()
        if shallow == 'true':
            run(['git', '-C', destination, 'fetch', '--unshallow'])
        revision = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != spec['revision']:
            raise ValueError('Build dependency revision mismatch: '+str(destination))
        if spec.get('patch_directory'):
            overlay = REPO/'subprojects/packagefiles'/spec['patch_directory']
            for source in overlay.rglob('*'):
                if source.is_file():
                    target = destination/source.relative_to(overlay)
                    if not target.exists():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(source, target)
        if vendored:
            # This private branch includes dependency snapshots. Verify every
            # tracked snapshot file against the pinned full-history checkout,
            # including QEMU's build overlays, without changing source files.
            tracked = subprocess.check_output(
                ['git', '-C', str(REPO), 'ls-files', '-z', '--',
                 str(vendored_source.relative_to(REPO))]).decode().split('\0')
            if not any(tracked):
                raise ValueError('Untracked dependency directory: '+str(vendored_source))
            for filename in filter(None, tracked):
                original = REPO/filename
                relative = original.relative_to(vendored_source)
                if relative.name == '.meson-subproject-wrap-hash.txt':
                    continue
                expected = destination/relative
                if not expected.is_file() or original.read_bytes() != expected.read_bytes():
                    raise ValueError('Vendored dependency differs from pinned source: '+filename)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--bootstrap', type=Path, required=True, help='Extracted bootstrap-v1 directory')
    ap.add_argument('--output', type=Path, required=True, help='New build directory outside the source checkout')
    ap.add_argument('--arm-prefix', default='arm-none-eabi-', help='Toolchain path/prefix ending in arm-none-eabi-')
    ap.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    args = ap.parse_args()
    args.archive = args.archive.resolve(strict=True)
    args.bootstrap = args.bootstrap.resolve(strict=True)
    args.output = args.output.resolve()
    if args.output.exists():
        ap.error('Output already exists; use a new directory')
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    for command in ('cc', 'c++', 'make', 'ninja', 'pkg-config', 'git',
                    args.arm_prefix+'gcc', args.arm_prefix+'ld', args.arm_prefix+'strip'):
        if not shutil.which(command):
            ap.error('Missing tool: ' + command)
    sevenzip = shutil.which('7zz') or shutil.which('7z')
    if not sevenzip:
        ap.error('Install 7-Zip')
    try:
        load_lzo2()
    except OSError as exc:
        ap.error(str(exc))
    import cryptography  # noqa: F401
    import PySide6  # noqa: F401
    manifest = json.loads((args.bootstrap/'bootstrap.json').read_text())
    if manifest.get('version') != 1 or set(manifest['files']) != {'nor.bin', 'iram.bin', 'emmc-template.raw'}:
        ap.error('Unexpected bootstrap version or file list')
    for name, expected in manifest['files'].items():
        with (args.bootstrap/name).open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                ap.error('Bootstrap checksum mismatch: '+name)
    args.output.mkdir(parents=True)
    firmware = args.output/'firmware'
    run([sevenzip, 'x', '-y', '-o'+str(firmware), args.archive], stdout=subprocess.DEVNULL)
    metadata = (firmware/'metainfo2.txt').read_text()
    if 'MHI2_ER_AU37x_P5089' not in metadata:
        ap.error('Archive is not the supported Audi A3 P5089 update')
    build_dependencies(args.output/'dependencies')
    build = args.output/'qemu'
    build.mkdir()
    run([REPO/'configure', '--target-list=arm-softmmu', '--disable-docs',
         '--disable-sdl', '--disable-gtk', '--disable-fuse', '--disable-werror',
         '--disable-rust',
         '--python='+sys.executable], cwd=build)
    run(['ninja', '-C', build, '-j', args.jobs, 'qemu-system-arm'])
    bridge = args.output/'bridge'
    env = dict(os.environ, MHI2_BUILD_LIBRARIES_ONLY='1', MHI2_BUILD_DIR=str(bridge),
               MHI2_ARM_CC=args.arm_prefix+'gcc', MHI2_ARM_LD=args.arm_prefix+'ld')
    run(['bash', SCRIPTS/'glforward/build_local.sh'], env=env)
    media = args.output/'media'
    base, main_ifs, ui = media/'base', media/'main-ifs', media/'ui'
    run([sys.executable, SCRIPTS/'prepare.py', '--extracted', firmware,
         '--base-nor', args.bootstrap/'nor.bin', '--base-iram', args.bootstrap/'iram.bin',
         '--base-emmc', args.bootstrap/'emmc-template.raw', '--output', base])
    run([sys.executable, SCRIPTS/'extract_main_ifs.py',
         firmware/'MMX2/mifs-stage2/70/default/mifs-stage2.img', main_ifs])
    run([sys.executable, SCRIPTS/'prepare_ui.py', '--base', base, '--main-ifs', main_ifs,
         '--app', firmware/'MMX2/app/70/default/app.img', '--formatted-disk',
         args.bootstrap/'emmc-template.raw', '--bridge-dir', bridge, '--output', ui])
    launcher = args.output/'run.sh'
    launcher.write_text('#!/usr/bin/env bash\nset -euo pipefail\n'
        'here=$(cd "$(dirname "$0")" && pwd)\n'
        'export QEMU_SYSTEM_ARM="$here/qemu/qemu-system-arm"\n'
        'export MHI2_GLHOST="$here/bridge/mhi2-glhost"\n'
        'export PATH='+shlex.quote(str(Path(sys.executable).parent))+':"$PATH"\n'
        'exec bash '+shlex.quote(str(SCRIPTS/'run_audi_ui.sh'))+' --media "$here/media/ui" "$@"\n')
    launcher.chmod(0o755)
    with args.archive.open('rb') as stream:
        archive_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = {'source_commit': subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
              'archive_sha256': archive_hash, 'bootstrap': manifest,
              'python': sys.version, 'arm_compiler': subprocess.check_output([args.arm_prefix+'gcc', '--version'], text=True).splitlines()[0]}
    (args.output/'build-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Ready: bash '+shlex.quote(str(launcher)))


if __name__ == '__main__':
    main()
