#!/usr/bin/env python3
"""Run the updater's host tests (manifest parsing, version order, notes) with a native Jansson prefix.

Build a host Jansson once from the pinned source the 3DS build downloads:

  cmake -S build-update-deps/src/jansson-2.14 -B /tmp/jb -DJANSSON_BUILD_SHARED_LIBS=OFF \\
        -DJANSSON_WITHOUT_TESTS=ON -DJANSSON_BUILD_DOCS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \\
        -DCMAKE_INSTALL_PREFIX=/tmp/jansson-host && cmake --build /tmp/jb && cmake --install /tmp/jb
  platform/n3ds/tests/run-update-tests.py --jansson-prefix /tmp/jansson-host [saved-releases.json ...]

(or any system Jansson prefix). Extra arguments are saved GET /releases responses to parse as well.
"""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--jansson-prefix', type=pathlib.Path, required=True)
parser.add_argument('live', nargs='*', type=pathlib.Path)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
tests = root / 'platform/n3ds/tests'
source = root / 'platform/n3ds/update'
prefix = args.jansson_prefix.resolve()
with tempfile.TemporaryDirectory(prefix='ctr3d-updater-test-') as temporary:
    out = pathlib.Path(temporary) / 'manifest-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-I' + str(prefix / 'include'), '-I' + str(source),
                    '-fsanitize=address,undefined', str(tests / 'update_manifest_test.c'),
                    str(source / 'update_manifest.c'), str(prefix / 'lib/libjansson.a'), '-o', str(out)], check=True)
    subprocess.run([str(out)] + [str(p) for p in args.live], cwd=temporary, check=True)
