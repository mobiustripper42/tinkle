#pragma once
#include <stdint.h>
#include "run_controller.h"   // RunState, Fault

// Display — what the TM1637 4-digit panel + button LED rings show, derived from the
// run state (firmware spec §12, and the LED rings at the end of §11).
//
// Platform-independent (src/core): renderDisplay() is a pure function of the inputs
// and the current millis() (blink/flash phases are derived from it), host-tested
// with explicit time values. The TM1637 wire protocol and the chosen driver library
// stay in src/esp32 — this layer only decides glyphs, the colon, and LED modes.
//
// Display is read-only status (§12): it never gates actuation and has no input role.

namespace tinkle {

// One rendered frame for the 4-cell display. glyphs are left-to-right; ' ' = blank.
// The colon sits between cells 1 and 2 (TM1637 hardware colon). Whole-frame flashing
// (FAULT) is folded in here: on the off phase the cells are blank and colon is off.
struct DisplayFrame {
    char glyphs[4];
    bool colon;
};

// Frame equality — the esp32 shim pushes to the TM1637 only when the frame changes
// (bit-banged writes cost real microseconds; never write every loop tick, §2 / DEC-005).
inline bool operator==(const DisplayFrame& a, const DisplayFrame& b) {
    return a.colon == b.colon &&
           a.glyphs[0] == b.glyphs[0] && a.glyphs[1] == b.glyphs[1] &&
           a.glyphs[2] == b.glyphs[2] && a.glyphs[3] == b.glyphs[3];
}
inline bool operator!=(const DisplayFrame& a, const DisplayFrame& b) { return !(a == b); }

// Everything the renderer needs, pulled from RunController (+ a future Clock, §13).
struct DisplayInputs {
    RunState state        = RunState::Idle;
    Fault    fault        = Fault::None;
    uint32_t remainingSec = 0;       // RUNNING countdown; 0 otherwise
    bool     clockValid   = false;   // wall clock synced? (false until Clock lands, #2.2)
    uint8_t  hours        = 0;       // 0..23 wall clock, valid only if clockValid
    uint8_t  minutes      = 0;       // 0..59
};

// Render the panel for this instant. Pure; blink/flash phases come from nowMs.
// glyphs are ASCII ('0'-'9', 'E', '-', ' ') — the esp32 shim hands them straight to
// TM1637_RT::displayPChar, whose encoder maps them 1:1; the colon rides the high bit.
DisplayFrame renderDisplay(const DisplayInputs& in, uint32_t nowMs);

// Button LED rings (§11 tail / §12): off = idle / that zone not running; solid =
// that zone running; slow blink = fault/attention. Mode only — the esp32 layer turns
// Blink into an on/off level from millis().
//
// With the 3-button = 3-zone model (DEC-006) there is no dedicated stop ring, so the
// fault/attention blink lives on the zone rings: while a fault is latched, every ring
// blinks (the panel as a whole says "attention"), overriding the running/idle level.
enum class LedMode : uint8_t { Off, Solid, Blink };
LedMode zoneLedMode(int activeZone, uint8_t zoneIndex, bool faulted);
// Solid iff this zone is running; Blink (all rings) while faulted; else Off.

} // namespace tinkle
