#!/usr/bin/env bash
# T44 — smoke harness.
#
# Runs the engine --headless for a bounded duration and asserts it didn't
# crash. Two modes:
#   * default      — full run WITH game data: also asserts the stage table
#                    loaded (≥5 stages) and reports komnata transitions.
#   * --boot-only  — no-data boot check: assert the binary starts and exits
#                    cleanly (including the rc=1 "data root not found" path)
#                    with no crash / sanitizer finding, then skip the data-
#                    dependent assertions. This is what CI runs — the runner
#                    has the embedded PE blob but no external Dane_*.dta, so a
#                    full run can't make in-game progress.
#
# Usage:
#   ./tools/smoke-runner.sh                   # default: 30s, dist/wacki, with data
#   ./tools/smoke-runner.sh -d 60             # 60s run
#   ./tools/smoke-runner.sh -b debug          # use dist/wacki-debug (ASAN+UBSan)
#   ./tools/smoke-runner.sh -B path/to/bin    # explicit binary path
#   ./tools/smoke-runner.sh --boot-only -d 5  # CI no-data boot check
#
# Exit codes:
#   0 = all assertions passed
#   1 = build artifact missing
#   2 = data-dependent assertion failed (stages didn't load / no progress)
#   3 = crash / sanitizer finding
set -u

DUR=30
BIN=dist/wacki
BOOT_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d) DUR="$2"; shift 2 ;;
    -b) [[ "$2" == "debug" ]] && BIN=dist/wacki-debug; shift 2 ;;
    -B) BIN="$2"; shift 2 ;;
    --boot-only) BOOT_ONLY=1; shift ;;
    -h|--help)
      grep -E '^# ' "$0" | sed 's/^# \?//'
      exit 0 ;;
    *)
      echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

cd "$(dirname "$0")/.." || exit 1

if [[ ! -x "$BIN" ]]; then
  echo "[smoke] missing binary: $BIN (run \`make\` or \`make debug\`)" >&2
  exit 1
fi

LOG=$(mktemp -t wacki-smoke.XXXXXX)
mode=""; [[ "$BOOT_ONLY" == 1 ]] && mode=" (boot-only)"
echo "[smoke] $BIN --headless for ${DUR}s$mode → $LOG"

# Disable ASAN leak detection (we don't aim for leak-free yet) and bound the
# runtime so CI never hangs. timeout returns 124 when it had to kill the
# process (expected for a headless run that reaches the main loop).
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  timeout "$DUR" "$BIN" --headless >"$LOG" 2>&1
RC=$?

# Accepted exit codes: 0 = clean exit, 124 = timed out (reached the main loop),
# 137 = SIGKILL, 143 = SIGTERM. In --boot-only the no-data path also returns 1
# (main() bails after FindDataRoot fails — the expected, non-crash outcome with
# no Dane_*.dta present). Anything else (134 SIGABRT, 139 SIGSEGV, …) is a crash.
ok_rc="0 124 137 143"
[[ "$BOOT_ONLY" == 1 ]] && ok_rc="0 1 124 137 143"
case " $ok_rc " in
  *" $RC "*) ;;
  *)
    echo "[smoke] binary aborted (rc=$RC) — last 20 lines:"
    tail -20 "$LOG"
    rm "$LOG"
    exit 3 ;;
esac

if grep -qE "AddressSanitizer|UndefinedBehaviorSanitizer.*runtime error|heap-buffer|stack-buffer|use-after-free" "$LOG"; then
  echo "[smoke] sanitizer finding(s):"
  grep -E "AddressSanitizer|runtime error|heap-buffer|use-after-free" "$LOG" | head -10
  rm "$LOG"
  exit 3
fi

# Boot-only stops here: the binary started and exited cleanly with no crash and
# no sanitizer findings, which is all CI can assert without the data archives.
if [[ "$BOOT_ONLY" == 1 ]]; then
  echo "[smoke] boot-only PASS (rc=$RC, no crash, no sanitizer findings)"
  rm "$LOG"
  exit 0
fi

KOMNATA_COUNT=$(grep -cE "^\[real\] komnata|^\[load-komnata\]" "$LOG" || true)
STAGE_COUNT=$(grep -cE "^\[stage\] [1-5] @" "$LOG" || true)

echo "[smoke] stages parsed:   $STAGE_COUNT (expect ≥ 5)"
echo "[smoke] komnata events:  $KOMNATA_COUNT"

if [[ "$STAGE_COUNT" -lt 5 ]]; then
  echo "[smoke] FAIL — stage table didn't load fully" >&2
  tail -20 "$LOG"
  rm "$LOG"
  exit 2
fi

# Optional progress assertion — only enforced if user passed --strict
# via env. Default just reports counts (some headless runs make no
# in-game progress because there's no fake input driver yet).
if [[ "${SMOKE_STRICT:-0}" == "1" && "$KOMNATA_COUNT" -lt 1 ]]; then
  echo "[smoke] FAIL — no komnata transitions observed (SMOKE_STRICT=1)" >&2
  tail -20 "$LOG"
  rm "$LOG"
  exit 2
fi

echo "[smoke] PASS"
rm "$LOG"
exit 0
