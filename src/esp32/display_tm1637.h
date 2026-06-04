#pragma once
#include <Arduino.h>
#include <TM1637.h>
#include "pins.h"
#include "../core/display.h"

// ESP32 binding for the TM1637 4-digit panel (firmware spec §12; library choice
// DEC-015). renderDisplay() in src/core produces ASCII glyphs + a colon flag; this
// shim hands the glyphs straight to TM1637_RT::displayPChar (whose encoder maps
// '0'-'9'/'E'/'-'/' ' 1:1) and rides the colon on the high bit of cell 1. DEC-005.
//
// Bit-banged TM1637 writes cost real microseconds, so show() pushes ONLY when the
// frame changes — never every loop tick (the ≤10 ms budget, §2). LED rings are
// plain digitalWrite and are driven separately in the loop (negligible cost).

namespace tinkle {

class DisplayTM1637 {
public:
    void begin() {
        tm_.begin(TM_CLK, TM_DIO, 4);        // 4-digit module
        tm_.setBrightness(3);
        tm_.displayClear();
        hasLast_ = false;
    }

    // Push the frame iff it changed since the last push (the §2 change-gate).
    void show(const DisplayFrame& f) {
        if (hasLast_ && f == last_) return;
        char buf[4] = { f.glyphs[0], f.glyphs[1], f.glyphs[2], f.glyphs[3] };
        if (f.colon) buf[1] = (char)(buf[1] | 0x80);   // colon = DP bit on cell 1
        tm_.displayPChar(buf);
        last_    = f;
        hasLast_ = true;
    }

private:
    TM1637       tm_;
    DisplayFrame last_    = {};
    bool         hasLast_ = false;
};

} // namespace tinkle
