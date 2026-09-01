#!/usr/bin/env bash
#
# check_duplicate_includes.sh — build gate (LOA-fix R73): no source file includes
# the same header twice.
#
# WHY THIS EXISTS
#   A duplicated #include is not a compile error and not a behaviour change, so it
#   survives for years and then costs a round its measurement: the ladder compares
#   `nm -C -S` sizes of functions in a translation unit, and an include set that
#   nobody can account for makes "this control moved" unattributable. R69's harness
#   already carried a duplicate-include lint, but it ran over a per-round `touched`
#   LIST — it answered "did I break the file I edited?".  The property answers "is
#   the tree clean?", and a NEW file with a duplicate include is caught the same way
#   an old one is ([[total-invariant-beats-list-of-sites]]).
#
# HOW IT PROVES IT CAN FAIL
#   Before scanning, it runs its own pipeline over a canary file it writes itself,
#   which contains one deliberate duplicate. If the canary does not come back as an
#   offender, the pipeline is broken and "0 offenders" would be a false green — that
#   is ОСТАНОВ, not a pass.
#
# WHAT COUNTS AS A DUPLICATE
#   Two #include lines that are textually identical after trailing whitespace is
#   stripped, within one file. `#include <x>` and `#include "x"` are different
#   spellings and are NOT folded together — this gate reports what it can prove,
#   not what it guesses.
#
# USAGE
#   bash tools/check_duplicate_includes.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_duplicate_includes.sh
#
# ENV
#   ROOT             default: the repository this script lives in
#   DUP_MIN_FILES    default 150 — refuse to run against fewer source files than
#                    this (today src/ + include/ + tests/ hold ~170).
#
# EXIT CODES
#   0 clean · 1 duplicates found (printed) · 2 not scannable / blind
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
DUP_MIN_FILES="${DUP_MIN_FILES:-150}"
SCOPE="src include tests"

for d in $SCOPE; do
  [ -d "$ROOT/$d" ] || { echo "ОСТАНОВ: нет каталога $ROOT/$d — считать нечего"; exit 2; }
done

# The one place the duplicate-detection pipeline is written; the canary and the tree
# scan both go through it, so a broken pipeline cannot be clean on one and blind on
# the other.
duplicates_in() {
  grep -E '^[[:space:]]*#[[:space:]]*include[[:space:]]' "$1" 2>/dev/null \
    | sed 's/[[:space:]]*$//' | sort | uniq -d
}

# Blindness guard #1: the canary must come back dirty.
CANARY="$(mktemp)"
trap 'rm -f "$CANARY"' EXIT
{
  echo '#include <string>'
  echo '#include <vector>'
  echo '#include <string>'
} > "$CANARY"
if [ -z "$(duplicates_in "$CANARY")" ]; then
  echo "ОСТАНОВ: канарейка с заведомым дублем прочиталась как чистая —"
  echo "         конвейер сломан, ноль нарушителей читать нельзя."
  exit 2
fi

# Blindness guard #2: count what will actually be read.
FILES="$(cd "$ROOT" && find $SCOPE -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inl' \) | sort)"
FOUND="$(printf '%s\n' "$FILES" | grep -c . || true)"
if [ "$FOUND" -lt "$DUP_MIN_FILES" ]; then
  echo "ОСТАНОВ: найдено $FOUND исходников, ожидалось не меньше $DUP_MIN_FILES —"
  echo "         ноль дублей на неполном дереве это не «чисто», это слепота."
  exit 2
fi

OFFENDERS=""
SCANNED=0
while IFS= read -r relative; do
  [ -n "$relative" ] || continue
  SCANNED=$((SCANNED + 1))
  DUPS="$(duplicates_in "$ROOT/$relative")"
  if [ -n "$DUPS" ]; then
    while IFS= read -r line; do
      OFFENDERS="${OFFENDERS}${relative}: ${line}"$'\n'
    done <<< "$DUPS"
  fi
done <<< "$FILES"

if [ -z "$OFFENDERS" ]; then
  COUNT=0
else
  COUNT="$(printf '%s' "$OFFENDERS" | grep -c .)"
fi

echo "=== duplicate-include gate ==="
echo "дерево          : $ROOT"
echo "область         : $SCOPE"
echo "просканировано  : $SCANNED файлов из $FOUND найденных (минимум $DUP_MIN_FILES)"
echo "дублей          : $COUNT (ожидалось 0)"

if [ "$SCANNED" -ne "$FOUND" ]; then
  echo "ОСТАНОВ: обход прочитал $SCANNED из $FOUND файлов."
  exit 2
fi

if [ "$COUNT" -ne 0 ]; then
  echo
  echo "нарушители:"
  printf '%s' "$OFFENDERS" | sed 's/^/  /'
  echo
  echo "=== ИТОГ: ПРОВАЛ ✗ — убрать повторный #include ==="
  exit 1
fi

echo "=== ИТОГ: ЧИСТО ✓ ==="
exit 0
