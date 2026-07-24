# fw_build_id.py — build identity, per-build archive, and OTA slot-size guard (#126, #159).
#
# PlatformIO extra_script (pre) for the esp32/esp32_sim envs. Three jobs:
#
# 1. Inject FW_BUILD as a string macro, surfaced by /api/status as "build" — the
#    only way to observe from the phone that an OTA actually swapped the image.
#    Identity is "<sha>[-dirty]-<UTC yymmdd-HHMMSS>" (#159). The timestamp matters:
#    the git short-sha alone is unchanged across reflashes that only differ in a
#    gitignored build flag (e.g. a re-baked poopdeck_secret.ini), so two genuinely
#    different images showed an IDENTICAL build string and an OTA looked like it
#    never took. A per-build UTC timestamp makes the phone-visible identity change
#    on every flash, and makes each archived filename unique. Falls back to "dev"
#    outside git.
#
# 2. Archive the linked image per build (#159): copy firmware.bin to
#    build_archive/tinkle-<env>-<FW_BUILD>.bin (gitignored) so every build leaves a
#    distinct, sortable file behind — clean them up manually whenever.
#
# 3. HARD GATE the linked image against the OTA app-slot size. The device runs the
#    arduino-esp32 default 4 MB partition table (app0/app1 = 1280 KB each + otadata
#    — already dual-slot; no repartition was ever needed, see DEC-022). A build that
#    outgrows a slot would otherwise fail at FLASH time — or worse, brick the OTA
#    path in the field. Fail here, loudly, instead.
#
# The pure helpers (compute_build_id / git_sha_and_dirty / archive_firmware /
# check_app_size) are import-safe and host-tested by tools/test_fw_build_id.py; the
# SCons wiring at the bottom runs only under PlatformIO.

import os
import shutil
import subprocess
from datetime import datetime, timezone

APP_SLOT_BYTES = 0x140000  # 1280 KB — app0/app1 in the default.csv table


def compute_build_id(sha, dirty, dt):
    """Build identity string: '<sha>[-dirty]-<UTC yymmdd-HHMMSS>'. `dt` is a datetime
    (UTC); second-granularity so two builds a minute apart never collide."""
    return "%s%s-%s" % (sha, "-dirty" if dirty else "", dt.strftime("%y%m%d-%H%M%S"))


def git_sha_and_dirty(project_dir):
    """(short-sha, dirty) from git, or ('dev', False) outside a repo / on error.
    `dirty` reflects tracked-tree changes; a gitignored build flag never dirties it
    (that's exactly why the timestamp, not '-dirty', carries per-flash uniqueness)."""
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=project_dir, text=True
        ).strip()
    except Exception:
        return "dev", False
    try:
        dirty = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=project_dir, text=True
        ).strip())
    except Exception:
        dirty = False
    return sha, dirty


def archive_firmware(bin_path, archive_dir, env_name, build_id):
    """Copy the linked image to <archive_dir>/tinkle-<env>-<build_id>.bin (creating
    the dir). Returns the destination path. Metadata-preserving copy (copy2)."""
    os.makedirs(archive_dir, exist_ok=True)
    dest = os.path.join(archive_dir, "tinkle-%s-%s.bin" % (env_name, build_id))
    shutil.copy2(bin_path, dest)
    return dest


def check_app_size(bin_path):
    """Return the image size in bytes; raise SystemExit if it exceeds the OTA app
    slot (a flash/OTA-time failure, surfaced here at build time instead)."""
    size = os.path.getsize(bin_path)
    pct = 100.0 * size / APP_SLOT_BYTES
    print("fw_build_id: firmware.bin %d bytes — %.0f%% of the %d-byte OTA slot"
          % (size, pct, APP_SLOT_BYTES))
    if size > APP_SLOT_BYTES:
        raise SystemExit(
            "fw_build_id: firmware.bin is %d bytes — over the %d-byte app slot. "
            "OTA (and flash) would fail. Trim the image or revisit the partition "
            "table (DEC-022)." % (size, APP_SLOT_BYTES)
        )
    return size


# --- SCons wiring (runs only under PlatformIO; import-safe for the host test) -------
try:
    Import("env")  # noqa: F821 — a SCons builtin, undefined on a plain import
    _under_scons = True
except NameError:
    _under_scons = False

if _under_scons:
    _sha, _dirty = git_sha_and_dirty(env["PROJECT_DIR"])  # noqa: F821
    _build_id = compute_build_id(_sha, _dirty, datetime.now(timezone.utc))
    env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(_build_id))])  # noqa: F821

    def _post_build(source, target, env):  # noqa: F811
        path = target[0].get_abspath()
        check_app_size(path)  # prints size; raises if over the slot
        dest = archive_firmware(
            path, os.path.join(env["PROJECT_DIR"], "build_archive"),
            env["PIOENV"], _build_id,
        )
        print("fw_build_id: archived build -> %s" % dest)

    env.AddPostAction("$BUILD_DIR/firmware.bin", _post_build)  # noqa: F821
