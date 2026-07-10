# OTA firmware update — operator runbook

Push new firmware to the controller over the LAN — no laptop-to-tunnel cable trip.
First done successfully 2026-07-10 (bee-grace → curl). Mechanics: DEC-022 / spec §10;
the run-safety gate: `Api::postOtaBegin` + the `requestRun` inhibit.

## 1. Build the image

On any machine with the repo + PlatformIO (bee-grace, mill-dev, WSL — all fine):

```bash
cd ~/tinkle
git checkout main && git pull
~/.platformio/penv/bin/pio run -e esp32          # build ONLY — no -t upload, no cable
```

Output: `.pio/build/esp32/firmware.bin` (~900 KB; the build prints its % of the
1280 KB OTA slot and fails loudly if it ever outgrows it). The build stamps the git
short-sha in as `FW_BUILD` — note the sha, it's how you verify the flash took.

## 2. Pick the right address (they are NOT interchangeable)

| From | Use |
|---|---|
| Android phone | `http://tinkle` (router DNS — Android has no mDNS) |
| Windows browser | either name |
| bee-grace / Linux shell | `http://tinkle.local` (avahi; the bare router name doesn't resolve there) |

## 3. Upload

**Terminal (the bee-grace charm path):**

```bash
curl -F "firmware=@.pio/build/esp32/firmware.bin" http://tinkle.local/api/ota
```

The `-F` (multipart) is REQUIRED — a raw-body POST is silently discarded by the
server's upload plumbing. Success answer: `{"flashed":true,"rebooting":true}`.

**Phone / browser:** open the SPA → Settings → Firmware → pick the `.bin`
(from Windows, WSL builds are at `\\wsl$\<distro>\home\<you>\tinkle\.pio\build\esp32\firmware.bin`)
→ UPLOAD & FLASH. Progress %, then "Flashed — rebooting. Reconnecting…".

If the build was made with an `ota_secret.ini`, add the key: curl
`-H "X-OTA-Key: <key>"`, or fill the "Update key" field in the SPA. A key-less
build ignores the header entirely.

## 4. Verify

Wait ~30 s for the reboot, then check the sha:

- **Home screen → Device card → Firmware** row (or Settings → Firmware → "Running build")
- or `curl http://tinkle.local/api/status | grep -o '"build":"[^"]*"'`

Sha matches step 1 → the flash took.

## What the guardrails do (so refusals aren't surprises)

- **409 "run active or queued — OTA refused":** the gate only accepts from IDLE or
  FAULT with an empty queue, and no run — scheduled or manual — can START while an
  upload is writing flash. Wait for the run to finish and retry. Never bypass this;
  it's what keeps a scheduled run from firing mid-flash.
- **401 "bad OTA key":** the running firmware was built with a secret; supply it.
- **Interrupted/truncated upload:** discarded before it can become the boot target
  (`Update.end` validates first) — the old firmware keeps running and the run
  inhibit lifts on disconnect. Just retry.
- **Fail-dry holds throughout:** OTA is only accepted with the pump un-commanded,
  so the heartbeat is parked and the ATtiny relay already de-armed before the first
  byte is written. A reboot mid-anything fails dry by design.

## Recovery of last resort

A *valid but broken* image (boots, then misbehaves) auto-reverts nothing — the stock
bootloader has no rollback (DEC-022). Recovery is the USB cable on bee-grace:

```bash
~/.platformio/penv/bin/pio run -e esp32 -t upload --upload-port /dev/ttyUSB0
```

OTA is a convenience on top of USB, not a replacement for it.
