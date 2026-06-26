# Tinkle — Product Specification

## Overview
Tinkle is an automated drip-irrigation controller for Bay Branch Farm's high
tunnels. It replaces a single 110V smart-plug-on-the-pump (pump on/off, no zone
control) with scheduled, per-zone watering and reliable manual override. V1 wires
and runs the **Red Tunnel** (2 zones) plus a **general-purpose hose-outlet zone**
(Zone 3, separate from the tunnel) and a Dosatron fertigation diverter — three zone
channels in all — on a controller, driver board, and enclosure **sized for three
tunnels**. Z1/Z2 are plumbed now; Z3's valve is wired now, plumbed when that line
goes in (build-for-three).

The detailed hardware and firmware contracts live in `docs/tinkle_v1_spec.md`,
`docs/tinkle_firmware_spec.md`, and `docs/tinkle_wiring.html`. This file is the
scope guardrail.

## Philosophy
- **Fail dry.** A missed watering cycle is harmless; a valve stuck open overnight
  floods a bed and dead-heads the pump. The whole architecture prevents
  *runaway-on*, not the missed cycle.
- **Local autonomy.** Watering never depends on the network, a server, or the
  cloud. The schedule runs from local flash; config is over local Wi-Fi.
- **Build for three, populate one.** Controller/driver/enclosure are sized for an
  eventual three-tunnel farm; only Red is wired now. Expansion is "land more
  wires," never a redesign.
- **Scheduled now, closed-loop later.** V1 is time + duration only. Soil-moisture-
  driven irrigation is V2, after both sensors and valves have earned a season.

## Stack
- **Controller:** ESP32 DevKitC (38-pin), Arduino-ESP32, PlatformIO.
- **Watchdog:** ATtiny85 (separate binary) — independent hardware fail-dry backstop.
- **UI:** vanilla HTML/CSS/JS SPA, gzipped into flash, served by ESPAsyncWebServer.
- **Persistence:** NVS / Preferences.
- **Power:** fixed AC→24V supply (Mean Well LRS-150-24, mounts **outside** the enclosure) —
  valves + pump on 24V; a single **24→5V buck** feeds the flow sensor and the ESP32 (fed 5V
  on 5V/VIN, it makes its own 3.3V — no separate 3.3V buck). Pump on the watchdog-armed 24V
  (the fail-dry source gate). See DEC-020.
- **Dev/test tiers:** native unit tests (host) → Wokwi sim → breadboard bench
  (LED/pulse stand-ins) → wet hardware (final gate). See DEC-004.

## Roles / interfaces
- **Operator (phone):** sets schedules, default durations, fert policy, max-runtime,
  runs calibration, starts/stops manual runs — via the local web UI.
- **Operator (at the box):** no on-box controls (DEC-019, v1.5 phone-only). An **optional**
  AC master switch on the Mean Well input is the physical kill / service disconnect
  (whole-system → fail-dry, no firmware); without it, the AC breaker / unplug is the kill. A
  single onboard LED blinks ~1 Hz to show the firmware is alive. All start / stop /
  fault-clear is via the phone. (V1 originally had 3 zone buttons + a TM1637 panel — DEC-006, cut
  by DEC-019.)
- **Headless:** the schedule runs from flash with no phone, no network present.

## V1 Scope

### Phase 0 — Scaffold & contracts
Repo, seeds workflow, PlatformIO skeleton (esp32/attiny85/native), `pins.h` from
the wiring doc, native test harness + Wokwi sim config, docs.

### Phase 1 — Actuation core
`ValveDriver` (on/off FET per valve — zones + two diverter legs, no master, pump relay, safe state),
`RunController` state machine (§4), non-blocking loop. (The original buttons + TM1637 display were
cut to phone-only by DEC-019; the modules are git-recoverable.)

### Phase 2 — Persistence + Scheduler + Clock
NVS write-on-change, schedule model + evaluation, fert policy (one fert run/day +
override), NTP with free-run fallback.

### Phase 3 — Flow + Calibration
Hall-pulse ISR, rate window, no-flow / unexpected-flow faults, web calibration mode.

### Phase 4 — Web API + SPA
ESPAsyncWebServer endpoints (§10), then the vanilla SPA (6 screens, + a 7th
**History** screen — DEC-018) + gzip-embed pipeline (< 50 KB).

### Phase 5 — Watchdog + integration
ATtiny85 sketch (DEC-003 encoding), safety relay, ESP32 `Watchdog` module, run the
§17 acceptance checklist.

### Phase 6 — Bench validation + wet confirm
Breadboard bench (LED/pulse stand-ins) full §17 pass; then wet confirm with real
parts — calibrate K, confirm `ZONE_TRAVEL_MS` / `DIVERTER_TRAVEL_MS`. Parts-gated.

## Not V1
- **Green Tunnel and any third tunnel hardware** — chassis/board/enclosure headroom
  only; no second pump/valves/sensor wired.
- **Closed-loop / sensor-driven irrigation** — Soundings soil-moisture sensors
  driving valves is V2.
- **Low-tank pump lockout** — consuming the Soundings tank-cluster level sensor to
  lock out the pump before it runs the catchment dry is V2. Seam recorded in
  DEC-017 (a future `TankMonitor` gate); source is Soundings `docs/tank-level-sensor.md`.
- **Server, MQTT broker, database, Grafana** — telemetry is local-only in V1; MQTT
  publishing to the Soundings stack comes later. (A *local* on-device run/fault history
  served over the same local API — DEC-018 — is in scope; this line bars *remote* telemetry.)
- **Any physical operator UI** — V1 is phone-only (DEC-019). No buttons, no display, no on-box
  menu; the SPA is the sole interface and the AC master switch is the only physical control. Hard
  line: do not re-add a panel (buttons/display/encoder) as "convenience" — that's a deliberate V1.5
  cut, not an oversight.
- **Second Green irrigation tank** — separate decision, parked.
