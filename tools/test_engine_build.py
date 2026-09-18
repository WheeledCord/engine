#!/usr/bin/env python3
"""Focused regression checks for engine-build's filesystem contracts."""
import pathlib
import tempfile

import engine_build


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        source = root / 'source'
        output = root / 'build'
        source.mkdir()
        (source / 'current.txt').write_text('current')
        (output / 'assets').mkdir(parents=True)
        (output / 'assets' / 'stale.txt').write_text('stale')
        engine_build.sync_tree(source, output / 'assets', output)
        assert (output / 'assets' / 'current.txt').read_text() == 'current'
        assert not (output / 'assets' / 'stale.txt').exists()
    print('engine-build tests: PASS')


if __name__ == '__main__':
    main()
