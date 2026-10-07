#!/usr/bin/env python3
"""Build QEMU, bridge libraries and selected MHI2 media in a new directory.

This filename remains compatible with existing Audi callers. New users can use
build_firmware.py with --firmware audi-a3, vw or porsche.
"""
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
from firmware_profile import PROFILES, common_metadata

SCRIPTS = Path(__file__).resolve().parent
REPO = SCRIPTS.parents[1]


def run(command, **kwargs):
    print('+ ' + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def build_dependencies(cache):
    """Materialize only the pinned dependencies needed by arm-softmmu.

    Source ZIPs have no Git index and do not contain submodules. Fetch each
    dependency independently, then copy its pinned tree and QEMU's Meson overlay.
    Existing source files must match; never silently overwrite local edits.
    """
    cache.mkdir(parents=True, exist_ok=True)
    for name in ('keycodemapdb', 'berkeley-softfloat-3', 'berkeley-testfloat-3'):
        wrap = configparser.ConfigParser()
        wrap.read(REPO/'subprojects'/(name+'.wrap'))
        spec = wrap['wrap-git']
        checkout = cache/name
        if not checkout.exists():
            run(['git', 'init', checkout])
            run(['git', '-C', checkout, 'remote', 'add', 'origin', spec['url']])
            run(['git', '-C', checkout, 'fetch', '--depth=1', 'origin', spec['revision']])
            run(['git', '-C', checkout, 'checkout', '--detach', 'FETCH_HEAD'])
        revision = subprocess.check_output(
            ['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != spec['revision']:
            raise ValueError('Build dependency revision mismatch: '+str(checkout))
        if subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain']):
            raise ValueError('Build dependency cache has local changes: '+str(checkout))
        files = subprocess.check_output(
            ['git', '-C', str(checkout), 'ls-files', '-z']).decode().split('\0')
        sources = {relative: checkout/relative for relative in filter(None, files)}
        if spec.get('patch_directory'):
            overlay = REPO/'subprojects/packagefiles'/spec['patch_directory']
            sources.update({str(p.relative_to(overlay)): p
                            for p in overlay.rglob('*') if p.is_file()})
        destination = REPO/'subprojects'/name
        for relative, source in sources.items():
            target = destination/relative
            if target.exists() and target.read_bytes() != source.read_bytes():
                raise ValueError('Dependency differs from pinned source: '+str(target))
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copy2(source, target)


def source_revision():
    """Read provenance without mistaking a ZIP's enclosing repo for QEMU."""
    if (REPO/'.git').exists():
        return subprocess.check_output(
            ['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    archive = SCRIPTS/'source-version.txt'
    value = archive.read_text().strip() if archive.is_file() else ''
    return value if len(value) == 40 and all(c in '0123456789abcdef' for c in value) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--firmware', choices=PROFILES, default='audi-a3',
                    help='Firmware profile (legacy default: audi-a3)')
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
    for command in ('cc', 'c++', 'make', 'ninja', 'pkg-config', 'git', 'ffmpeg',
                    args.arm_prefix+'gcc', args.arm_prefix+'ld', args.arm_prefix+'strip'):
        if not shutil.which(command):
            ap.error('Missing tool: ' + command)
    libraries = ('egl', 'glesv2', 'libavcodec', 'libavutil', 'libswscale', 'libusb-1.0')
    if subprocess.run(['pkg-config', '--exists', *libraries]).returncode:
        ap.error('Missing development libraries: pkg-config must find ' + ' '.join(libraries))
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
    metadata = common_metadata(firmware/'metainfo2.txt')
    expected = PROFILES[args.firmware]['train']
    if metadata.get('release') != expected:
        ap.error(f'Archive train {metadata.get("release")!r} does not match '
                 f'{args.firmware}: expected {expected}')
    build_dependencies(args.output/'dependencies')
    build = args.output/'qemu'
    build.mkdir()
    run([REPO/'configure', '--target-list=arm-softmmu', '--disable-docs',
         '--disable-sdl', '--disable-gtk', '--disable-fuse', '--disable-werror',
         '--disable-rust', '--enable-libusb',
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
        'exec bash '+shlex.quote(str(SCRIPTS/'run_ui.sh'))+
        ' --firmware '+shlex.quote(args.firmware)+' --media "$here/media/ui" "$@"\n')
    launcher.chmod(0o755)
    with args.archive.open('rb') as stream:
        archive_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = {'source_commit': source_revision(),
              'firmware_profile': args.firmware, 'firmware_train': expected,
              'archive_sha256': archive_hash, 'bootstrap': manifest,
              'python': sys.version, 'arm_compiler': subprocess.check_output([args.arm_prefix+'gcc', '--version'], text=True).splitlines()[0]}
    (args.output/'build-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Ready: bash '+shlex.quote(str(launcher)))


if __name__ == '__main__':
    main()
