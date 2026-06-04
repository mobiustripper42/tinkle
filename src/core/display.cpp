#include "display.h"

namespace tinkle {

namespace {

constexpr uint32_t BLINK_HALF_MS = 500;   // 1 Hz colon blink / fault flash (§12)

// True for the "on" half of the 1 Hz square wave. The only wart at the millis() wrap
// (~49.7 days) is a single stretched on-phase — purely cosmetic on a display.
bool phaseOn(uint32_t nowMs) { return (nowMs / BLINK_HALF_MS) % 2u == 0u; }

// §12 fault codes: E1 no-flow, E2 unexpected flow, E3 watchdog. CalRange/Clock get
// E4/E5 so every latchable fault shows something rather than a blank.
char faultDigit(Fault f) {
    switch (f) {
        case Fault::NoFlow:         return '1';
        case Fault::UnexpectedFlow: return '2';
        case Fault::Watchdog:       return '3';
        case Fault::CalRange:       return '4';
        case Fault::Clock:          return '5';
        default:                    return '0';
    }
}

void setDigits(DisplayFrame& f, uint8_t a, uint8_t b) {
    // Two zero-padded two-digit fields: a -> cells 0,1 ; b -> cells 2,3.
    f.glyphs[0] = char('0' + (a / 10) % 10);
    f.glyphs[1] = char('0' + a % 10);
    f.glyphs[2] = char('0' + (b / 10) % 10);
    f.glyphs[3] = char('0' + b % 10);
}

} // namespace

DisplayFrame renderDisplay(const DisplayInputs& in, uint32_t nowMs) {
    DisplayFrame f = { { ' ', ' ', ' ', ' ' }, false };

    switch (in.state) {
        case RunState::Fault:
            // "E" + code, flashing at 1 Hz — blank on the off phase.
            if (phaseOn(nowMs)) { f.glyphs[0] = 'E'; f.glyphs[1] = faultDigit(in.fault); }
            break;

        case RunState::Running: {
            // MM:SS countdown, colon blinking at 1 Hz. Caps at 99:59 (the §15 software
            // ceiling is 20 min, so this never truncates a real run).
            uint32_t mm = in.remainingSec / 60;
            uint32_t ss = in.remainingSec % 60;
            if (mm > 99) { mm = 99; ss = 59; }
            setDigits(f, (uint8_t)mm, (uint8_t)ss);
            f.colon = phaseOn(nowMs);
            break;
        }

        case RunState::Idle:
            // Wall clock HH:MM, colon steady on; "--:--" until the clock syncs (§12).
            if (in.clockValid) setDigits(f, in.hours, in.minutes);
            else { f.glyphs[0] = f.glyphs[1] = f.glyphs[2] = f.glyphs[3] = '-'; }
            f.colon = true;
            break;

        default:
            // Transitional states (PREP/OPEN_*/STOP_*/CLOSE_*/SETTLE, §12 "implementer's
            // choice"): a steady dash row says "busy, not counting".
            f.glyphs[0] = f.glyphs[1] = f.glyphs[2] = f.glyphs[3] = '-';
            break;
    }
    return f;
}

LedMode zoneLedMode(int activeZone, uint8_t zoneIndex) {
    return ((int)zoneIndex == activeZone) ? LedMode::Solid : LedMode::Off;
}

LedMode stopLedMode(bool faulted) {
    return faulted ? LedMode::Blink : LedMode::Off;
}

} // namespace tinkle
