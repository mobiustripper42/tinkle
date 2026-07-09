# fw_build_id.py — build identity + OTA slot-size guard (#126).
#
# PlatformIO extra_script (pre) for the esp32/esp32_sim envs. Two jobs:
#
# 1. Inject FW_BUILD (the git short-sha) as a string macro, surfaced by
#    /api/status as "build" — the only way to observe from the phone that an OTA
#    actually swapped the image. Falls back to "dev" outside git.
#
# 2. HARD GATE the linked image against the OTA app-slot size. The device runs
#    the arduino-esp32 default 4 MB partition table (app0/app1 = 1280 KB each +
#    otadata — already dual-slot; no repartition was ever needed, see DEC-022).
#    A build that outgrows a slot would otherwise fail at FLASH time — or worse,
#    brick the OTA path in the field. Fail here, loudly, instead.

Import("env")  # noqa: F821 — provided by the SCons/PlatformIO runtime

import subprocess

APP_SLOT_BYTES = 0x140000  # 1280 KB — app0/app1 in the default.csv table

try:
    sha = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=env["PROJECT_DIR"], text=True,  # noqa: F821
    ).strip()
except Exception:
    sha = "dev"

env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(sha))])  # noqa: F821


def check_app_size(source, target, env):
    import os
    path = target[0].get_abspath()
    size = os.path.getsize(path)
    pct = 100.0 * size / APP_SLOT_BYTES
    print("fw_build_id: firmware.bin %d bytes — %.0f%% of the %d-byte OTA slot"
          % (size, pct, APP_SLOT_BYTES))
    if size > APP_SLOT_BYTES:
        raise SystemExit(
            "fw_build_id: firmware.bin is %d bytes — over the %d-byte app slot. "
            "OTA (and flash) would fail. Trim the image or revisit the partition "
            "table (DEC-022)." % (size, APP_SLOT_BYTES)
        )


env.AddPostAction("$BUILD_DIR/firmware.bin", check_app_size)  # noqa: F821
