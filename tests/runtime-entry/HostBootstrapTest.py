# SPDX-License-Identifier: GPL-3.0-only
import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
import tempfile
import uuid


def run_valid(executable, environment):
    event = None
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateEventW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
        kernel.CreateEventW.restype = wintypes.HANDLE
        kernel.SetEvent.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        name = r"Local\AuroraHostEntryFixture-" + uuid.uuid4().hex
        environment["AURORA_SUNSHINE_STOP_EVENT"] = name
        event = kernel.CreateEventW(None, True, False, name)
        if not event:
            raise ctypes.WinError(ctypes.get_last_error())

    try:
        with subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=environment) as child:
            try:
                if event and not kernel.SetEvent(event):
                    raise ctypes.WinError(ctypes.get_last_error())
                output, errors = child.communicate(b"a" * 64, timeout=5)
            finally:
                if child.poll() is None:
                    child.kill()
            assert child.returncode == 0 and output.strip() == b"host stopped", (child.returncode, output, errors)

        rejected = subprocess.run([executable], input=b"a" * 63, capture_output=True,
                                  env=environment, timeout=5)
        assert rejected.returncode == 1 and not rejected.stdout, rejected
        assert b"64-digit hexadecimal" in rejected.stderr, rejected

        with open(os.devnull, "rb") as direct_input:
            rejected = subprocess.run([executable], stdin=direct_input, capture_output=True,
                                      env=environment, timeout=5)
            assert rejected.returncode == 1 and not rejected.stdout, rejected
    finally:
        if event:
            kernel.CloseHandle(event)


if __name__ == "__main__":
    executable = sys.argv[1]
    environment = os.environ.copy()
    environment.pop("AURORA_SUNSHINE_PROFILE", None)
    environment.pop("AURORA_SUNSHINE_STOP_EVENT", None)
    result = subprocess.run([executable, "--aurora-runtime-info"], env=environment,
                            capture_output=True, timeout=5, check=True)
    assert result.stdout.strip() == b"aurora-host/1", result

    rejected = subprocess.run([executable], env=environment, input=b"a" * 64,
                              capture_output=True, timeout=5)
    assert rejected.returncode == 1 and not rejected.stdout, rejected

    with tempfile.TemporaryDirectory() as profile:
        environment["AURORA_SUNSHINE_PROFILE"] = profile
        run_valid(executable, environment)
