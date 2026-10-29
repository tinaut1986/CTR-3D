#!/usr/bin/env python3
"""Host test of the 60 fps logic clock (game/MAIN/MainFps60.c) and the frame
pacing policy (platform/n3ds/n3ds_framepace.c): platform/n3ds/tests/run-fps60-tests.py
(FPS60_TEST_VERBOSE=1 prints the policy's [PACE] decisions)."""
import pathlib
import subprocess
import tempfile

tests = pathlib.Path(__file__).resolve().parent
root = tests.parents[2]
with tempfile.TemporaryDirectory(prefix='ctr3d-fps60-test-') as temporary:
    out = pathlib.Path(temporary) / 'fps60-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(tests / 'fps60_stub'), '-I' + str(root / 'include'), '-I' + str(root / 'platform/n3ds'),
                    str(tests / 'fps60_test.c'), '-o', str(out)], check=True)
    subprocess.run([str(out)], check=True)
