// flow-pulse — Wokwi custom chip standing in for the hall flow sensor (#62).
//
// Square wave on OUT while EN is high: toggle every 33 ms => ~15 pulses/s, which
// at the K = 450 p/gal seed reads as ~2.0 GPM — a believable drip-zone flow.
// EN is INPUT_PULLUP, so with nothing wired the chip just pulses (the happy
// path); the diagram wires EN to a slide switch so the operator can kill flow
// mid-run (-> FAULT_NO_FLOW after the sim's 3 s grace) or feed pulses while idle
// (-> FAULT_UNEXPECTED_FLOW past 50 pulses / 5 s). OUT idles low when disabled.

#include "wokwi-api.h"
#include <stdlib.h>

typedef struct {
  pin_t out;
  pin_t en;
  uint32_t level;
} chip_t;

static void on_timer(void *user_data) {
  chip_t *chip = (chip_t *)user_data;
  if (pin_read(chip->en)) {
    chip->level = !chip->level;
    pin_write(chip->out, chip->level);
  } else if (chip->level) {
    chip->level = 0;
    pin_write(chip->out, LOW);
  }
}

void chip_init(void) {
  chip_t *chip = (chip_t *)malloc(sizeof(chip_t));
  chip->out   = pin_init("OUT", OUTPUT);
  chip->en    = pin_init("EN", INPUT_PULLUP);
  chip->level = 0;
  pin_write(chip->out, LOW);

  const timer_config_t config = {
    .callback  = on_timer,
    .user_data = chip,
  };
  timer_t timer = timer_init(&config);
  timer_start(timer, 33000, true);   // 33 ms repeating (micros) => ~15 Hz pulse rate
}
