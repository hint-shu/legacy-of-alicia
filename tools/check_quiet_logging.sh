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
# WHY `--binary-files=text` AND THE FILE-COUNT GUARD (do not remove them)
#   With the previous `--binary-files=without-match`, a single NUL byte anywhere in
#   a source file made grep declare that file binary and report it as having no
#   matches at all — an injected raw spdlog::error() next to that byte was invisible
#   and the gate printed "ЧИСТО ✓" with EXIT 0 (proven on fixture verify/qtree4,
#   R70-prep). Text mode fixes the blindness; the count guard proves grep actually
#   opened every file, because "0 offenders" is only good news when the scan is
#   known to have been complete.
#
# USAGE
#   bash tools/check_quiet_logging.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_quiet_logging.sh
#
# ENV
#   ROOT             default: the repository this script lives in
#   QUIET_MIN_FILES  default 100 — refuse to run against fewer source files than
#                    this (today src/ + include/ hold 156). Raise it deliberately;
#                    never lower it to make a run go green.
#
# EXIT CODES
#   0 no raw calls · 1 raw calls found (they are printed) · 2 the tree is not
#     scannable (missing src/ or include/, the exclusion file is gone, too few
#     files, a symlink under src/ or include/, or grep did not open every file — in
#     which case a zero count would be meaningless rather than good news)
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
QUIET_MIN_FILES="${QUIET_MIN_FILES:-100}"
EXCLUDE_REL="include/libserver/util/QuietLog.hpp"
PATTERN='spdlog::(trace|debug|info|warn|error|critical)\('

for d in src include; do
  [ -d "$ROOT/$d" ] || { echo "ОСТАНОВ: нет каталога $ROOT/$d — считать нечего"; exit 2; }
done

# Blindness guard #1: if the wrapper header itself disappeared or stopped containing
# the raw calls it is supposed to be the only home of, then "0 offenders" would be a
# false green produced by a broken scan rather than by a clean tree.
if [ ! -f "$ROOT/$EXCLUDE_REL" ]; then
  echo "ОСТАНОВ: нет $EXCLUDE_REL — проверка не может доказать, что она вообще что-то видит"
  exit 2
fi
WRAPPED="$(grep -acE "$PATTERN" "$ROOT/$EXCLUDE_REL" || true)"
if [ "$WRAPPED" -eq 0 ]; then
  echo "ОСТАНОВ: в $EXCLUDE_REL нет ни одного '$PATTERN' — регекс или файл сломаны, ноль читать нельзя"
  exit 2
fi

# Blindness guard #2: how many files exist vs how many grep actually opened.
# `grep -rc --binary-files=text ''` emits exactly one "<file>:<n>" line per file it
# reads, so the two numbers must agree; if grep skipped anything (binary, unreadable,
# a truncated walk), the offender count below is measured on less than the whole tree.
FOUND="$(cd "$ROOT" && find src include -type f | wc -l)"
SCANNED="$(cd "$ROOT" && grep -rac --binary-files=text '' src include 2>/dev/null | wc -l)"

if [ "$FOUND" -lt "$QUIET_MIN_FILES" ]; then
  echo "ОСТАНОВ: под src/ и include/ найдено $FOUND файлов, ожидалось не меньше $QUIET_MIN_FILES"
  echo "         ноль нарушителей на неполном дереве — это не «чисто», это слепота."
  exit 2
fi
if [ "$SCANNED" -ne "$FOUND" ]; then
  echo "ОСТАНОВ: grep открыл $SCANNED файлов из $FOUND — часть дерева не просканирована,"
  echo "         поэтому «0 нарушителей» ничего не доказывает."
  exit 2
fi

# Blindness guard #3: symlinks. `find -type f` does not list them and `grep -r` does
# not follow them, so BOTH numbers above agree while a symlinked source file — which
# the compiler happily builds — is never opened: a raw call behind it is invisible and
# the gate still prints "ЧИСТО ✓" (proven on a fixture, R70-prep). Refusing to run is
# the simpler safe answer than teaching the walk to follow links: the repository holds
# zero symlinks under src/ and include/ today, so this can only fire on a real change,
# and it fires as "undefined" (2), never as "clean".
LINKS="$(cd "$ROOT" && find src include -type l | wc -l)"
if [ "$LINKS" -ne 0 ]; then
  echo "ОСТАНОВ: под src/ и include/ найдено $LINKS символических ссылок —"
  echo "         ни find -type f, ни grep -r их не читают, поэтому «0 нарушителей» слепо."
  (cd "$ROOT" && find src include -type l | sed 's/^/  /')
  exit 2
fi

OFFENDERS="$(cd "$ROOT" && grep -rnE --binary-files=text "$PATTERN" src include \
             | grep -v "^$EXCLUDE_REL:" || true)"

if [ -z "$OFFENDERS" ]; then
  COUNT=0
else
  COUNT="$(printf '%s\n' "$OFFENDERS" | grep -c .)"
fi

echo "=== quiet-logging gate ==="
echo "дерево          : $ROOT"
echo "исключение      : $EXCLUDE_REL (в нём $WRAPPED вызовов — так и задумано)"
echo "просканировано  : $SCANNED файлов из $FOUND найденных (минимум $QUIET_MIN_FILES)"
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
