#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Fail the build if actual compiler dependencies of core escape its allowlist."""
import argparse
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='cc')
parser.add_argument('--cflags', default='-std=c99 -DGRAPHICS_API_OPENGL_21',
                    help="the flags core is compiled with, so conditional includes are seen as the build sees them")
parser.add_argument('--raylib', required=True)
parser.add_argument('--probe', type=pathlib.Path, help='Additional translation unit for checking the guard')
args = parser.parse_args()
raylib = pathlib.Path(args.raylib).resolve()
allowed = [(root / 'core').resolve(), raylib, pathlib.Path('/usr/include'), pathlib.Path('/usr/lib'), pathlib.Path('/usr/local/include'), pathlib.Path('/usr/local/lib')]
files = sorted((root / 'core').glob('*.c')) + sorted((root / 'core').glob('*.h'))
if args.probe:
    files.append(args.probe.resolve())
for source in files:
    result = subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) + ['-I' + str(raylib), '-x', 'c', '-M', '-MT', 'core_dependency', str(source)], capture_output=True, text=True)
    if result.returncode:
        sys.stderr.write(result.stderr)
        sys.exit(result.returncode)
    dependencies = shlex.split(result.stdout.replace('\\\n', ' ').split(':', 1)[1])
    for dependency in dependencies:
        path = pathlib.Path(dependency).resolve()
        if args.probe and path == args.probe.resolve():
            continue
        if not any(path == directory or directory in path.parents for directory in allowed):
            name = source.relative_to(root) if root in source.parents else source
            sys.exit(f'Forbidden core dependency: {name} -> {path}')
print('Core dependency boundary: PASS')
