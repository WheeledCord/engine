#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Build a project against an installed or vendored engine SDK.

The project says what it is in engine.project and nothing about how to build it: no rules, no
compiler flags, no paths into the engine. Everything else comes from the SDK, which is found
through pkg-config or named outright, and never lives inside the project."""
import argparse
import glob
import os
import pathlib
import shutil
import subprocess
import sys

MANIFEST = 'engine.project'
# What a manifest may say. Anything else is a mistake worth stopping for.
KEYS = {'name', 'module', 'source', 'script', 'assets', 'scenes', 'shaders'}
MODULES = {'core', 'gameplay', 'script-s7', 'script-pawn', 'entry'}


def fail(message):
    sys.exit(f'engine-build: {message}')


def read_manifest(path):
    """One key and its value per line, # starts a comment, order does not matter."""
    if not path.is_file():
        fail(f'no {MANIFEST} in {path.parent}')
    project = {'name': None, 'module': [], 'source': [], 'script': [],
               'assets': [], 'scenes': [], 'shaders': []}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        key, _, value = line.partition(' ')
        value = value.strip()
        if key not in KEYS or not value:
            fail(f'{path}:{number}: expected one of {" ".join(sorted(KEYS))} and a value')
        if key == 'name':
            project['name'] = value
        else:
            project[key].append(value)
    if not project['name']:
        fail(f'{path}: the project needs a name')
    if not project['source']:
        fail(f'{path}: the project needs at least one source')
    for module in project['module']:
        if module not in MODULES:
            fail(f'{path}: unknown module {module}, expected one of {" ".join(sorted(MODULES))}')
    return project


def pkg_config(sdk, *arguments):
    environment = dict(os.environ)
    if sdk:
        environment['PKG_CONFIG_PATH'] = str(pathlib.Path(sdk).resolve() / 'lib' / 'pkgconfig')
    result = subprocess.run(['pkg-config', *arguments, 'engine'], capture_output=True, text=True,
                            env=environment)
    if result.returncode:
        where = f'the SDK at {sdk}' if sdk else 'pkg-config'
        fail(f'{where} does not describe the engine: {result.stderr.strip()}')
    return result.stdout.split()


def find_sdk(project_dir, named):
    """Named outright, in the environment, vendored beside the project, or installed."""
    for candidate in (named, os.environ.get('ENGINE_SDK'), project_dir / 'sdk'):
        if candidate and pathlib.Path(candidate).joinpath('lib/pkgconfig/engine.pc').is_file():
            return pathlib.Path(candidate).resolve()
    if shutil.which('pkg-config') and subprocess.run(['pkg-config', '--exists', 'engine']).returncode == 0:
        return None  # installed: pkg-config knows where everything is
    fail('no SDK found. Pass --sdk, set ENGINE_SDK, vendor one at ./sdk, or install one')


def sources_of(project, project_dir):
    files = []
    for pattern in project['source']:
        matched = sorted(project_dir.glob(pattern))
        if not matched:
            fail(f'no source matches {pattern}')
        files.extend(matched)
    return files


def check_includes(compiler, cflags, sources, project_dir, sdk_includes):
    """The project may reach its own files, the SDK's headers and the system's. Nothing else: a
    project that quietly includes engine internals would be back to depending on this layout."""
    allowed = [project_dir.resolve(), *sdk_includes,
               *(pathlib.Path(p) for p in ('/usr/include', '/usr/lib', '/usr/local/include',
                                           '/usr/local/lib'))]
    for source in sources:
        result = subprocess.run([compiler, *cflags, '-M', '-MT', 'project', str(source)],
                                capture_output=True, text=True)
        if result.returncode:
            sys.stderr.write(result.stderr)
            fail(f'{source} does not compile')
        for dependency in result.stdout.replace('\\\n', ' ').split(':', 1)[1].split():
            path = pathlib.Path(dependency).resolve()
            if not any(path == directory or directory in path.parents for directory in allowed):
                fail(f'{source} includes {path}, which is outside the project and the SDK')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', nargs='?', default='.', type=pathlib.Path)
    parser.add_argument('--sdk', help='an SDK tree to build against')
    parser.add_argument('--out', help='where to put the built project (default: <project>/build)')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--run', action='store_true', help='run it once it is built')
    args = parser.parse_args()

    project_dir = args.project.resolve()
    project = read_manifest(project_dir / MANIFEST)
    sdk = find_sdk(project_dir, args.sdk)
    out = pathlib.Path(args.out).resolve() if args.out else project_dir / 'build'
    objects_dir = out / 'objects'
    objects_dir.mkdir(parents=True, exist_ok=True)

    cflags = pkg_config(sdk, '--cflags') + ['-I' + str(project_dir)]
    libs = pkg_config(sdk, '--libs', '--static')
    datadir = pkg_config(sdk, '--variable=datadir')[0]
    sdk_includes = [pathlib.Path(flag[2:]).resolve() for flag in cflags if flag.startswith('-I')]

    sources = sources_of(project, project_dir)
    if 'entry' in project['module']:
        # The standard main, which calls the project's EngineApplicationMain.
        sources.append(pathlib.Path(datadir) / 'src' / 'entry.c')
    check_includes(args.cc, cflags, [s for s in sources if project_dir in s.parents], project_dir,
                   sdk_includes)

    warnings = ['-std=c99', '-O2', '-g', '-Wall', '-Wextra']
    objects = []
    for source in sources:
        target = objects_dir / (source.stem + '.o')
        objects.append(str(target))
        command = [args.cc, *warnings, *cflags, '-c', str(source), '-o', str(target)]
        if subprocess.run(command).returncode:
            fail(f'{source} did not compile')
    binary = out / project['name']
    # One static binary: the SDK's libraries are linked in, so it needs no engine to be installed.
    if subprocess.run([args.cc, *objects, *libs, '-o', str(binary)]).returncode:
        fail('link failed')

    # What the project ships beside it, so the binary finds everything from its own directory.
    for kind in ('assets', 'scenes', 'shaders'):
        for directory in project[kind]:
            source_dir = project_dir / directory
            if not source_dir.is_dir():
                fail(f'{kind} directory {directory} is not there')
            shutil.copytree(source_dir, out / directory, dirs_exist_ok=True)
    for script in project['script']:
        source = project_dir / script
        if not source.is_file():
            fail(f'script {script} is not there')
        if source.suffix == '.pwn':
            compiled = out / (source.stem + '.amx')
            command = [f'{datadir}/pawn/pawncc', f'-i{datadir}/pawn', f'-o{compiled}', str(source)]
            if subprocess.run(command).returncode:
                fail(f'{script} did not compile')
        else:
            (out / source.name).write_bytes(source.read_bytes())
    # The engine's own runtime data travels with the binary, so it runs from anywhere.
    for directory in ('fonts', 'shaders'):
        shutil.copytree(pathlib.Path(datadir) / directory, out / 'core' / directory,
                        dirs_exist_ok=True)

    print(f'engine-build: {binary}')
    if args.run:
        sys.exit(subprocess.run([str(binary)], cwd=out).returncode)


if __name__ == '__main__':
    main()
