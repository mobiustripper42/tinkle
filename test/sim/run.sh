#!/usr/bin/env bash
# Run the Wokwi sim-tier e2e scenarios (DEC-004 tier 2) headless.
#
# Each scenario drives the real esp32_sim firmware through the FLOW switch and
# asserts actuator pin states with expect-pin -- no serial, no SPA. Phone-only
# (DEC-019): there are no buttons to trigger a run headless, and the CLI can't
# reach the SPA, so this tier covers the fail-dry checks that need no run --
# boot-safe-state and idle-unexpected-flow. The run-driven scenarios live in the
# interactive VS Code SPA session (see README.md / MANUAL.md).
#
# Why expect-pin and not serial/SPA: wokwi-cli's serial stream is broken against
# the current API (a `bytes` field parse crash, the upstream "Blank Terminal
# Problem"), and [[net.forward]] is a VS Code-extension feature the CLI does not
# implement. Pin assertions sidestep both.
#
# expect-pin asserts are deterministic -- scenario `delay` is SIMULATION time, so the
# firmware timeline is fixed relative to the steps regardless of how fast the cloud
# runs the sim. The one non-determinism is the cloud itself: the free CI tier rate-
# limits rapid back-to-back launches (a ~1s startup failure) and runs the sim at a
# variable 3-4x slowdown. So: a generous wall-clock --timeout, and retry of transient
# (non-assertion) failures with backoff. A real assertion failure is never retried.
#
# Prereqs:
#   - wokwi-cli on PATH            (curl -L https://wokwi.com/ci/install.sh | sh)
#   - WOKWI_CLI_TOKEN exported     (https://wokwi.com/dashboard/ci)
#   - pio run -e esp32_sim         (wokwi.toml points at the built binary)
#
# Usage:  test/sim/run.sh            # all scenarios
#         test/sim/run.sh 02 05      # only matching scenarios
set -u
cd "$(dirname "$0")/../.." || exit 2          # project root (wokwi.toml lives here)
ROOT="$PWD"
SIM_DIR="test/sim"
TIMEOUT_MS="${WOKWI_TIMEOUT_MS:-120000}"      # wall-clock ceiling; sim runs ~3-4x slow
MAX_TRIES="${WOKWI_MAX_TRIES:-3}"             # retries for transient cloud failures
GAP_S="${WOKWI_GAP_S:-5}"                     # spacing between launches (be gentle on the API)

if ! command -v wokwi-cli >/dev/null 2>&1; then
  echo "wokwi-cli not found on PATH -- see header for install." >&2; exit 2
fi
if [ -z "${WOKWI_CLI_TOKEN:-}" ]; then
  echo "WOKWI_CLI_TOKEN not set -- get one at https://wokwi.com/dashboard/ci" >&2; exit 2
fi
if [ ! -f .pio/build/esp32_sim/firmware.bin ]; then
  echo "esp32_sim not built -- run: pio run -e esp32_sim" >&2; exit 2
fi
# screenshots/ is gitignored (runtime artifact), so it's absent on a fresh clone.
# take-screenshot can't create the parent dir -- without this, every screenshot-
# bearing scenario fails on first run after checkout, and (since the error text
# lacks 'expected to be') run_one() burns its retries on it. Create it up front.
mkdir -p "$ROOT/$SIM_DIR/screenshots"

filter=("$@")
match() { [ ${#filter[@]} -eq 0 ] && return 0; for f in "${filter[@]}"; do [[ "$1" == *"$f"* ]] && return 0; done; return 1; }

# Run one scenario, retrying transient cloud failures. Returns 0 pass / 1 real fail.
run_one() {
  local name="$1" try out rc
  for (( try=1; try<=MAX_TRIES; try++ )); do
    # cwd = test/sim so screenshots/ (a cwd-relative save-to) resolves there; scenario
    # passed absolute because wokwi-cli resolves --scenario against the project-dir arg.
    out="$( cd "$SIM_DIR" && wokwi-cli "$ROOT" --scenario "$ROOT/$SIM_DIR/$name" --timeout "$TIMEOUT_MS" 2>&1 )"
    rc=$?
    echo "$out" | grep -E 'expect-pin|Scenario completed|Error|quota' | sed 's/^/    /'
    if [ "$rc" -eq 0 ]; then return 0; fi
    if echo "$out" | grep -qiE 'quota|upgrade to a paid plan'; then
      echo "    Free-tier Wokwi CI monthly minutes are exhausted -- aborting (resets monthly)." >&2
      exit 3                                  # hard stop: no retry helps a spent monthly quota
    fi
    # COUPLING: this distinguishes a real assertion failure from a transient cloud
    # failure by wokwi-cli's wording ('GPIO ... expected to be X but was Y'). If a
    # future wokwi-cli changes that phrasing, a real failure would be mis-read as
    # transient and retried/masked. Pinned to wokwi-cli 0.26.1 (see README prereqs).
    if echo "$out" | grep -q 'expected to be'; then
      return 1                                # real assertion failure -- do not retry
    fi
    echo "    (transient cloud failure, try $try/$MAX_TRIES) -- backing off" >&2
    sleep $(( GAP_S * try ))
  done
  return 1
}

pass=0; fail=0; failed=()
first=1
for y in "$SIM_DIR"/[0-9]*.yaml; do
  name="$(basename "$y")"
  match "$name" || continue
  [ "$first" -eq 1 ] || sleep "$GAP_S"; first=0
  printf '\n=== %s ===\n' "$name"
  if run_one "$name"; then echo "PASS $name"; pass=$((pass+1));
  else echo "FAIL $name"; fail=$((fail+1)); failed+=("$name"); fi
done

printf '\n----- %d passed, %d failed -----\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || { printf 'failed: %s\n' "${failed[*]}"; exit 1; }
