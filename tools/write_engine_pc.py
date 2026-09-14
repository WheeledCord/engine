#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Write engine.pc for an SDK tree, with the paths it was put at."""
import argparse
import pathlib

RELEASE = '0.1.0'

parser = argparse.ArgumentParser()
parser.add_argument('--prefix', required=True, help='where the SDK tree lives')
parser.add_argument('--revision', default='unknown', help='the engine revision this was built from')
parser.add_argument('--out', required=True, type=pathlib.Path)
args = parser.parse_args()

# The version says which engine this is, not merely which release series: two SDKs built from
# different revisions have to be tellable apart by anything that depends on one.
version = f'{RELEASE}-{args.revision}' 

args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(f"""prefix={args.prefix}
exec_prefix=${{prefix}}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include
bindir=${{prefix}}/bin
datadir=${{prefix}}/share/engine
revision={args.revision}

Name: engine
Description: C engine on raylib, with gameplay entities and scripting
Version: {version}
Cflags: -I${{includedir}} -DGRAPHICS_API_OPENGL_21
Libs: -L${{libdir}} -lscript -lgameplay -lcore -lraylib
Libs.private: -lm -lpthread -ldl -lGL -lX11
""")
