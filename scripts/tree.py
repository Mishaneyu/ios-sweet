"""Assemble the UniXNU build tree the firmware and payload steps expect.

UniXNU's firmware driver builds with PACKAGES_PATH entries that point into the
build tree (``{tree}/package`` and ``{tree}/drivers/DarwinBoot`` in board.toml),
and the payload step runs the device's ADT script out of ``<tree>/scripts``
(cwd = the tree). Upstream assembles that tree with ``unixnu tree`` after
resolving modules from feeds; this device repository already has every source
checked out next to it, so the same layout is built directly:

    out/raphael/tree/
      package      -> unixnu/package
      drivers/<x>  -> unixnu/drivers/<x>     (symlinks, so patches apply live)
      kits/<x>     -> unixnu/kits/<x>
      scripts/     <- unixnu/scripts (minus the driver itself) + device scripts
      build/ .cache/ refer/
      .cache/firmware -> <device>/.cache/firmware
"""
import shutil
from pathlib import Path


def _link(target, link_path):
    target = Path(target).resolve()
    link_path = Path(link_path)
    link_path.parent.mkdir(parents=True, exist_ok=True)
    if link_path.is_symlink() or link_path.exists():
        link_path.unlink()
    link_path.symlink_to(target)


def assemble(work, unixnu, device):
    tree = Path(work) / 'tree'
    tree.mkdir(parents=True, exist_ok=True)

    _link(Path(unixnu) / 'package', tree / 'package')
    for folder in ('drivers', 'kits'):
        source = Path(unixnu) / folder
        if not source.is_dir():
            continue
        destination = tree / folder
        destination.mkdir(exist_ok=True)
        for child in sorted(source.iterdir()):
            _link(child, destination / child.name)

    for name in ('build', '.cache', 'refer'):
        (tree / name).mkdir(parents=True, exist_ok=True)
    firmware = Path(device) / '.cache' / 'firmware'
    firmware.mkdir(parents=True, exist_ok=True)
    _link(firmware, tree / '.cache' / 'firmware')

    scripts = tree / 'scripts'
    scripts.mkdir(exist_ok=True)
    for base in (Path(unixnu), Path(device)):
        source = base / 'scripts'
        if not source.is_dir():
            continue
        for path in sorted(source.rglob('*')):
            relative = path.relative_to(source)
            if path.is_dir() or '__pycache__' in relative.parts or relative.parts[0] == 'unixnu':
                continue
            target = scripts / relative
            if target.is_file() and target.stat().st_size == path.stat().st_size:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    return tree
