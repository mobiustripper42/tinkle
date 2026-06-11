# Tinkle SPA (`web/`)

The phone UI. A single static `index.html` (inline CSS + JS) that is gzipped and
embedded into ESP32 flash, served by `ESPAsyncWebServer` at `http://tinkle.local`.
It is a **thin client over the §10 REST API** — no business logic in the browser,
no fail-dry role, no external fetches. See `docs/tinkle_firmware_spec.md` §10.1.

## Dev without hardware

The mock API is **in the page** (#58): open `web/index.html` straight from
`file://` (or append `?mock=1` to the served URL) and every screen runs against
an in-memory stub — runs count down, calibration walks its phases, settings
persist for the session. No server, no ESP32. Iterate in a normal browser, then
just build: the embed step picks up the same file.

## Build & embed (#59)

`tools/build_spa.py` runs as a PlatformIO `extra_scripts` **pre-action on every
`esp32`/`esp32_sim` build**: it gzips `web/index.html` (deterministic, mtime=0)
into the generated, gitignored `src/esp32/spa_gz.h` (PROGMEM array + ETag).
There is no manual step to forget and the served UI can't go stale. The **< 50 KB
gzipped** budget is a hard build gate — overrun fails the build. Served with
`Content-Encoding: gzip`, `Cache-Control: no-cache` + ETag (a reload after
reflash revalidates in one 304 round trip).

## Constraints

- Mobile-first, large touch targets, high-contrast, sunlight-readable, one-handed.
- Degrade gracefully: on API failure show last-known state + a "disconnected"
  banner, never a blank screen.
- Losing the phone, the page, or Wi-Fi must never affect a running or scheduled run.
- STOP ALL is visible on every screen and never asks for confirmation.
