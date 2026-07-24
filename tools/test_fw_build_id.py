#!/usr/bin/env python3
# Host test for fw_build_id.py (#159). Runs anywhere with plain python3 — no PlatformIO,
# no toolchain — so the build-identity + archive logic is verified without an esp32 build:
#     python3 tools/test_fw_build_id.py
#
# Importing fw_build_id is safe here: its `Import("env")` raises NameError off SCons, which
# the module catches (_under_scons = False), so none of the SCons wiring runs on import.

import os
import sys
import tempfile
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw_build_id as fw  # noqa: E402


def test_build_id_format():
    dt = datetime(2026, 7, 24, 14, 35, 7, tzinfo=timezone.utc)
    assert fw.compute_build_id("b699a3e", False, dt) == "b699a3e-260724-143507"
    assert fw.compute_build_id("b699a3e", True, dt) == "b699a3e-dirty-260724-143507"
    assert fw.compute_build_id("dev", False, dt) == "dev-260724-143507"


def test_build_id_unique_per_build():
    # Two builds one second apart get distinct ids -> distinct archive filenames.
    a = fw.compute_build_id("b699a3e", False, datetime(2026, 7, 24, 14, 35, 7, tzinfo=timezone.utc))
    b = fw.compute_build_id("b699a3e", False, datetime(2026, 7, 24, 14, 35, 8, tzinfo=timezone.utc))
    assert a != b


def test_archive_copy():
    with tempfile.TemporaryDirectory() as d:
        binp = os.path.join(d, "firmware.bin")
        with open(binp, "wb") as f:
            f.write(b"\x00" * 128)
        arch = os.path.join(d, "build_archive")
        dest = fw.archive_firmware(binp, arch, "esp32", "b699a3e-260724-143507")
        assert os.path.basename(dest) == "tinkle-esp32-b699a3e-260724-143507.bin"
        assert os.path.getsize(dest) == 128
        # env in the name keeps esp32 and esp32_sim builds from colliding.
        dest_sim = fw.archive_firmware(binp, arch, "esp32_sim", "b699a3e-260724-143507")
        assert dest_sim != dest and os.path.exists(dest_sim)


def test_app_size_gate():
    with tempfile.TemporaryDirectory() as d:
        ok = os.path.join(d, "ok.bin")
        with open(ok, "wb") as f:
            f.write(b"\x00" * 1024)
        assert fw.check_app_size(ok) == 1024  # under slot -> returns size, no raise

        big = os.path.join(d, "big.bin")
        with open(big, "wb") as f:
            f.write(b"\x00" * (fw.APP_SLOT_BYTES + 1))
        try:
            fw.check_app_size(big)
            raise AssertionError("expected SystemExit for an over-slot image")
        except SystemExit:
            pass


if __name__ == "__main__":
    tests = sorted((n, f) for n, f in globals().items()
                   if n.startswith("test_") and callable(f))
    for name, fn in tests:
        fn()
        print("PASS", name)
    print("\n%d fw_build_id tests passed" % len(tests))
