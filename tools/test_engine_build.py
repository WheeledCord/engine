#!/usr/bin/env python3
"""Focused regression checks for engine-build's filesystem and compile contracts."""
import contextlib
import io
import os
import pathlib
import shutil
import tempfile
import time

import engine_build


def check(condition, message):
    if not condition:
        raise SystemExit(f'engine-build tests: FAIL: {message}')


def sync_tree_replaces_stale_files(root):
    source = root / 'source'
    output = root / 'build'
    source.mkdir()
    (source / 'current.txt').write_text('current')
    (output / 'assets').mkdir(parents=True)
    (output / 'assets' / 'stale.txt').write_text('stale')
    engine_build.sync_tree(source, output / 'assets', output)
    check((output / 'assets' / 'current.txt').read_text() == 'current', 'synced file present')
    check(not (output / 'assets' / 'stale.txt').exists(), 'stale file removed')


def module_lines_are_ignored_with_a_warning(root):
    manifest = root / 'engine.project'
    manifest.write_text('name demo\nmodule entry\nmodule gameplay\nsource src/*.c\n')
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        project = engine_build.read_manifest(manifest)
    check(project['name'] == 'demo', 'a manifest with module lines still reads')
    check('module' in err.getvalue() and 'whole engine' in err.getvalue(),
          'module lines are reported as no longer needed')
    check('module' not in project, 'module lines leave nothing behind for the build to act on')


def same_named_sources_get_separate_objects(root):
    project = root / 'project'
    objects = root / 'objects'
    a = project / 'src' / 'util.c'
    b = project / 'lib' / 'util.c'
    check(engine_build.object_for(project, objects, a) != engine_build.object_for(project, objects, b),
          'two util.c files in different folders compile to different objects')


def compiles_only_what_changed(root):
    cc = shutil.which(os.environ.get('CC', 'cc'))
    if not cc:
        print('engine-build tests: skipping compile checks, no C compiler')
        return
    project = root / 'compiled'
    (project / 'src').mkdir(parents=True)
    (project / 'lib').mkdir()
    (project / 'src' / 'util.h').write_text('int helper(void);\n')
    (project / 'src' / 'util.c').write_text('#include "util.h"\nint helper(void) { return 1; }\n')
    (project / 'lib' / 'util.c').write_text('int other(void) { return 2; }\n')
    sources = [project / 'src' / 'util.c', project / 'lib' / 'util.c']
    objects = project / 'build' / 'objects'
    built, paths = engine_build.compile_sources(cc, ['-std=c99'], [], sources, project, objects)
    check(built == 2 and all(pathlib.Path(p).is_file() for p in paths),
          'a first build compiles every source, same-named ones included')
    built, _ = engine_build.compile_sources(cc, ['-std=c99'], [], sources, project, objects)
    check(built == 0, 'a second build with nothing changed compiles nothing')
    time.sleep(0.01)
    header = project / 'src' / 'util.h'
    header.write_text('int helper(void);\n/* changed */\n')
    future = time.time() + 5
    os.utime(header, (future, future))
    built, _ = engine_build.compile_sources(cc, ['-std=c99'], [], sources, project, objects)
    check(built == 1, 'changing a header recompiles only the source that includes it')
    built, _ = engine_build.compile_sources(cc, ['-std=c99', '-O1'], [], sources, project, objects)
    check(built == 2, 'changing the compile flags recompiles everything')


def main():
    for test in (sync_tree_replaces_stale_files, module_lines_are_ignored_with_a_warning,
                 same_named_sources_get_separate_objects, compiles_only_what_changed):
        with tempfile.TemporaryDirectory() as temporary:
            test(pathlib.Path(temporary))
    print('engine-build tests: PASS')


if __name__ == '__main__':
    main()
