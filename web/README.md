# Tinkle SPA (`web/`)

The phone UI. A single static `index.html` (inline CSS + JS) that is gzipped and
embedded into ESP32 flash, served by `ESPAsyncWebServer` at `http://tinkle.local`.
It is a **thin client over the §10 REST API** — no business logic in the browser,
no fail-dry role, no external fetches. See `docs/tinkle_firmware_spec.md` §10.1.

## Dev without hardware

Develop the whole UI on a laptop against a **mock API server** that returns canned
`/api/status`, `/api/schedule`, etc. — no ESP32 in the loop. Iterate in a normal
browser, then build + embed only when it's right.

## Build & embed (Phase 4)

A small build step (`build-spa`, TBD) gzips `web/index.html` → a PROGMEM header the
firmware serves. Target **< 50 KB gzipped**. Output lives in `web/dist/` (gitignored).

## Constraints

- Mobile-first, large touch targets, high-contrast, sunlight-readable, one-handed.
- Degrade gracefully: on API failure show last-known state + a "disconnected"
  banner, never a blank screen.
- Losing the phone, the page, or Wi-Fi must never affect a running or scheduled run.
