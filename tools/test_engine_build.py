#!/usr/bin/env python3
"""Focused regression checks for engine-build's filesystem and compile contracts, and for engine-new's
Scheme project, run by trench from the build tree and, with --sdk DIR, from an SDK outside it."""
import argparse
import contextlib
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

import engine_build

TOOLS = pathlib.Path(__file__).resolve().parent
ENGINE = TOOLS.parent


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


def new_project(root, name='starter'):
    """engine-new --language scheme into root/name; answers the project directory."""
    project = root / name
    made = subprocess.run([sys.executable, str(TOOLS / 'engine_new.py'), str(project), '--language', 'scheme'],
                          capture_output=True, text=True)
    check(made.returncode == 0, f'engine-new --language scheme runs: {made.stderr.strip()}')
    return project


def run_headless(trench, project, cwd):
    """trench run <project> --headless --ticks 60 from cwd; answers (exit status, all output)."""
    ran = subprocess.run([str(trench), 'run', str(project), '--headless', '--ticks', '60',
                          '--print-count', 'walker'],
                         capture_output=True, text=True, cwd=cwd, timeout=120)
    return ran.returncode, ran.stdout + ran.stderr


def scheme_project_runs(root):
    """What engine-new writes for Scheme is a store project trench runs as it is."""
    project = new_project(root)
    manifest = (project / 'engine.project').read_text().split('\n')
    check('name starter' in manifest and 'game starter.scm' in manifest,
          'the manifest names the project and its game file')
    game = (project / 'starter.scm').read_text()
    check(all(part in game for part in ('(define-kind game', '(tilemap', '(on (player-joined p)',
                                        '(define-actions', '(input-vector', '(light', '(draw-hud)')),
          'the game file is a define-kind game with a floor, a player-joined spawn, actions, a light and a HUD')
    check('trench run .' in (project / 'README.md').read_text(), 'the README says how to run it')
    check(not (project / 'src').exists(), 'a Scheme project has no C sources to build')
    trench = ENGINE / 'build' / 'core' / 'trench'
    if not trench.is_file():
        print('engine-build tests: skipping the run of the Scheme project, no build/core/trench')
        return
    status, output = run_headless(trench, project, root)
    check(status == 0 and 'ERROR' not in output and 'count walker 1' in output,
          f'the generated project runs 60 ticks headless with one walker and no ERROR:\n{output}')

    # Expected failures: a directory that is not empty is refused, and so is a project whose game
    # file is missing.
    refused = subprocess.run([sys.executable, str(TOOLS / 'engine_new.py'), str(project), '--language', 'scheme'],
                             capture_output=True, text=True)
    check(refused.returncode != 0 and 'not empty' in refused.stderr, 'engine-new refuses a directory that is not empty')
    (project / 'starter.scm').unlink()
    status, output = run_headless(trench, project, root)
    check(status != 0, 'a project whose game file is missing does not run')


def sdk_trench_runs_outside_the_tree(root, sdk):
    """The SDK's trench finds its prelude, shaders and font under share/engine/, with nothing of the
    engine's checkout in reach: the runtime half of the SDK is copied outside the tree and run from
    there, on a project outside it."""
    sdk = pathlib.Path(sdk).resolve()
    check((sdk / 'bin' / 'trench').is_file() and (sdk / 'share' / 'engine' / 'scheme' / 'kinds.scm').is_file(),
          f'{sdk} has bin/trench and share/engine/scheme/kinds.scm')
    moved = root / 'sdk'
    (moved / 'bin').mkdir(parents=True)
    shutil.copy2(sdk / 'bin' / 'trench', moved / 'bin' / 'trench')
    shutil.copytree(sdk / 'share', moved / 'share')
    check(ENGINE not in moved.resolve().parents, 'the copied SDK is outside the engine tree')
    project = new_project(root)
    for trench in (sdk / 'bin' / 'trench', moved / 'bin' / 'trench'):
        status, output = run_headless(trench, project, root)
        check(status == 0 and 'ERROR' not in output and 'count walker 1' in output,
              f'{trench} run <project> --headless --ticks 60 works from outside the engine tree:\n{output}')
    # Expected failure: without share/engine there is no prelude to find, and it says so.
    shutil.rmtree(moved / 'share')
    status, output = run_headless(moved / 'bin' / 'trench', project, root)
    check(status != 0 and 'no Scheme prelude' in output, 'an SDK trench without share/engine says what it lacks')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', help='also run the SDK\'s trench on a generated project, from outside the tree')
    args = parser.parse_args()
    for test in (sync_tree_replaces_stale_files, module_lines_are_ignored_with_a_warning,
                 same_named_sources_get_separate_objects, compiles_only_what_changed, scheme_project_runs):
        with tempfile.TemporaryDirectory() as temporary:
            test(pathlib.Path(temporary))
    if args.sdk:
        with tempfile.TemporaryDirectory() as temporary:
            sdk_trench_runs_outside_the_tree(pathlib.Path(temporary), args.sdk)
    print('engine-build tests: PASS' + (f' (and the SDK at {args.sdk})' if args.sdk else ''))


if __name__ == '__main__':
    main()
