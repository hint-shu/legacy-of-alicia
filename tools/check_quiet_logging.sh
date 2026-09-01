#!/usr/bin/env bash
#
# check_quiet_logging.sh — build gate: no raw spdlog logging calls in our sources.
#
# WHY THIS EXISTS
#   Logging from a `noexcept` path (or from a destructor) can itself throw — a full
#   disk, a broken pipe on the log sink — and then std::terminate takes the whole
#   server down. Every logging call in this codebase therefore goes through the
#   swallowing wrappers in include/libserver/util/QuietLog.hpp
#   (util::QuietLogError / QuietLogWarn / QuietLogInfo / QuietLogDebug /
#    QuietLogCritical / QuietLogTrace).
#
#   Historically that invariant was maintained by a sweep inside the 36k-line
#   apply_patches.py canon, guarded by a counter (SWEEP_EXPECTED_CALLS = 485).
#   The canon is no longer the source of truth; the invariant is. This gate states
#   it directly as a property of the tree: outside QuietLog.hpp itself, the number
#   of raw `spdlog::<level>(` calls under src/ and include/ must be exactly ZERO.
#   A property beats a list of sites — a new file with a raw call is caught the same
#   way an old one is.
#
# USAGE
#   bash tools/check_quiet_logging.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_quiet_logging.sh
#
# EXIT CODES
#   0 no raw calls · 1 raw calls found (they are printed) · 2 the tree is not
#     scannable (missing src/ or include/, or the exclusion file is gone — in which
#     case a zero count would be meaningless rather than good news)
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
EXCLUDE_REL="include/libserver/util/QuietLog.hpp"
PATTERN='spdlog::(trace|debug|info|warn|error|critical)\('

for d in src include; do
  [ -d "$ROOT/$d" ] || { echo "ОСТАНОВ: нет каталога $ROOT/$d — считать нечего"; exit 2; }
done

# Blindness guard: if the wrapper header itself disappeared or stopped containing the
# raw calls it is supposed to be the only home of, then "0 offenders" would be a
# false green produced by a broken scan rather than by a clean tree.
if [ ! -f "$ROOT/$EXCLUDE_REL" ]; then
  echo "ОСТАНОВ: нет $EXCLUDE_REL — проверка не может доказать, что она вообще что-то видит"
  exit 2
fi
WRAPPED="$(grep -cE "$PATTERN" "$ROOT/$EXCLUDE_REL" || true)"
if [ "$WRAPPED" -eq 0 ]; then
  echo "ОСТАНОВ: в $EXCLUDE_REL нет ни одного '$PATTERN' — регекс или файл сломаны, ноль читать нельзя"
  exit 2
fi

OFFENDERS="$(cd "$ROOT" && grep -rnE --binary-files=without-match "$PATTERN" src include \
             | grep -v "^$EXCLUDE_REL:" || true)"

if [ -z "$OFFENDERS" ]; then
  COUNT=0
else
  COUNT="$(printf '%s\n' "$OFFENDERS" | grep -c .)"
fi

echo "=== quiet-logging gate ==="
echo "дерево          : $ROOT"
echo "исключение      : $EXCLUDE_REL (в нём $WRAPPED вызовов — так и задумано)"
echo "сырых spdlog::  : $COUNT (ожидалось 0)"

if [ "$COUNT" -ne 0 ]; then
  echo
  echo "нарушители:"
  printf '%s\n' "$OFFENDERS" | sed 's/^/  /'
  echo
  echo "=== ИТОГ: ПРОВАЛ ✗ — заменить на util::QuietLog<Level>(…) ==="
  exit 1
fi

echo "=== ИТОГ: ЧИСТО ✓ ==="
exit 0
