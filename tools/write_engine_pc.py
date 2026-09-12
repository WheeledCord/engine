#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Write engine.pc for an SDK tree, with the paths it was put at."""
import argparse
import pathlib

VERSION = '0.1.0'

parser = argparse.ArgumentParser()
parser.add_argument('--prefix', required=True, help='where the SDK tree lives')
parser.add_argument('--out', required=True, type=pathlib.Path)
args = parser.parse_args()

args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(f"""prefix={args.prefix}
exec_prefix=${{prefix}}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include
bindir=${{prefix}}/bin
datadir=${{prefix}}/share/engine

Name: engine
Description: C engine on raylib, with gameplay entities and scripting
Version: {VERSION}
Cflags: -I${{includedir}} -DGRAPHICS_API_OPENGL_21
Libs: -L${{libdir}} -lscript -lgameplay -lcore -lraylib
Libs.private: -lm -lpthread -ldl -lGL -lX11
""")
