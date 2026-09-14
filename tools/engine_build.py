#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Build a project against an installed or vendored engine SDK.

The project says what it is in engine.project and nothing about how to build it: no rules, no
compiler flags, no paths into the engine. Everything else comes from the SDK, which is found
through pkg-config or named outright, and never lives inside the project."""
import argparse
import datetime
import os
import pathlib
import shutil
import subprocess
import sys

MANIFEST = 'engine.project'
# What a manifest may say. Anything else is a mistake worth stopping for.
KEYS = {'name', 'engine', 'module', 'source', 'script', 'assets', 'scenes', 'shaders'}
MODULES = {'core', 'gameplay', 'script-s7', 'script-pawn', 'entry'}


def fail(message):
    sys.exit(f'engine-build: {message}')


def read_manifest(path):
    """One key and its value per line, # starts a comment, order does not matter."""
    if not path.is_file():
        fail(f'no {MANIFEST} in {path.parent}')
    project = {'name': None, 'engine': None, 'module': [], 'source': [], 'script': [],
               'assets': [], 'scenes': [], 'shaders': []}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        key, _, value = line.partition(' ')
        value = value.strip()
        if key not in KEYS or not value:
            fail(f'{path}:{number}: expected one of {" ".join(sorted(KEYS))} and a value')
        if key in ('name', 'engine'):
            project[key] = value
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


def warn_if_stale(sdk, revision):
    """An SDK built from an older checkout than the engine sitting beside it.

    A stale SDK does not announce itself: the headers are simply older, so the failure arrives as a
    compile error in the project's own source, naming the project's line and suggesting some other
    function. Saying so here costs one git call and turns that into one sentence."""
    if revision in (None, 'unknown'):
        return
    def is_engine(directory):
        return (directory / 'Makefile.core').is_file() and (directory / '.git').exists()

    engine = None
    starts = [pathlib.Path(sdk)] if sdk else []
    starts.append(pathlib.Path.cwd())
    for start in starts:
        for directory in (start, *start.parents):
            # The checkout is an ancestor when building inside it, and a sibling from a workspace
            # laid out as engine/ beside Games/ and Tools/, which is how projects here sit.
            for candidate in (directory, directory / 'engine'):
                if is_engine(candidate):
                    engine = candidate
                    break
            if engine:
                break
        if engine:
            break
    if not engine:
        return  # nothing to compare against; an installed SDK on its own is not evidence of staleness
    head = subprocess.run(['git', '-C', str(engine), 'rev-parse', '--short', 'HEAD'],
                          capture_output=True, text=True)
    if head.returncode != 0:
        return
    current = head.stdout.strip()
    if current and not revision.startswith(current):
        print(f'engine-build: warning: this SDK was built from {revision}, but the engine at '
              f'{engine} is now {current}. Rebuild and reinstall it '
              f'(make -f Makefile.core install PREFIX=...) if the build fails oddly.',
              file=sys.stderr)


def check_engine(project, version, revision):
    """A project may say which engine it is meant for, and is not built against another one by
    accident: an exact version or revision, or a floor with >=."""
    wanted = project['engine']
    if not wanted:
        return
    if wanted.startswith('>='):
        floor = wanted[2:].strip()
        if sorted([floor, version.split('-')[0]], key=lambda v: [int(p) for p in v.split('.')])[0] != floor:
            fail(f'this project asks for engine >= {floor}, the SDK is {version}')
        return
    if wanted not in (version, revision):
        fail(f'this project asks for engine {wanted}, the SDK is {version} ({revision})')


def write_stamp(path, project, version, revision, sdk, compiler, cflags, libs, sources):
    """What built this, beside what was built: an engine anyone can check out, and the flags and
    files that went in. A binary with no account of where it came from is the thing that makes a
    build hard to repeat."""
    described = subprocess.run([compiler, '--version'], capture_output=True, text=True)
    lines = [f'project {project["name"]}',
             f'engine {version}',
             f'revision {revision}',
             f'sdk {sdk if sdk else "installed, found through pkg-config"}',
             f'compiler {compiler} ({described.stdout.splitlines()[0] if described.stdout else "?"})',
             f'cflags {" ".join(cflags)}',
             f'libs {" ".join(libs)}',
             f'built {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}']
    lines += [f'source {s}' for s in sources]
    lines += [f'script {s}' for s in project['script']]
    path.write_text('\n'.join(lines) + '\n')


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
    version = pkg_config(sdk, '--modversion')[0]
    revision = (pkg_config(sdk, '--variable=revision') or ['unknown'])[0]
    check_engine(project, version, revision)
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

    write_stamp(out / 'engine-build.stamp', project, version, revision, sdk, args.cc, cflags, libs,
                [str(s.relative_to(project_dir)) for s in sources if project_dir in s.parents])
    warn_if_stale(sdk, revision)
    where = sdk if sdk else 'installed, via pkg-config'
    print(f'engine-build: {binary} (engine {version}, sdk: {where})')
    if args.run:
        sys.exit(subprocess.run([str(binary)], cwd=out).returncode)


if __name__ == '__main__':
    main()
