#!/usr/bin/env bash
#
# negative_shape.sh CAND_REF NEG_REF EXPECTED_FILES [EXPECTED_HUNKS]
#
# WHY THIS EXISTS
#   A negative arm proves the ladder can fail. It is only worth that if it removes
#   EXACTLY ONE protection: a negative that drifted into changing three files no
#   longer isolates anything, and — worse — a negative whose edit silently did not
#   apply is byte-identical to the candidate, so the ladder "catches" nothing and
#   reports a false green. Both happened in earlier rounds (two sed edits that never
#   landed were caught only because the injection was proven before the run).
#   This gate states the shape as a number and refuses anything else.
#
# WHAT IT DOES
#   Asserts that `git diff CAND_REF..NEG_REF` touches exactly EXPECTED_FILES files
#   and, when EXPECTED_HUNKS is given, exactly EXPECTED_HUNKS hunks. Prints the stat.
#
# USAGE
#   bash tools/round/negative_shape.sh r70-topic r70-neg-a 1
#   bash tools/round/negative_shape.sh r70-topic r70-neg-b 1 2
#
# ENV
#   REPO  default: the repository this script lives in
#
# EXIT CODES  0 shape matches · 1 shape differs, refs missing, or the diff is empty
set -uo pipefail

die() { echo "ОСТАНОВ: $*" >&2; exit 1; }

[ $# -ge 3 ] || die "usage: negative_shape.sh CAND_REF NEG_REF EXPECTED_FILES [EXPECTED_HUNKS]"
CAND="$1"; NEG="$2"; WANT_FILES="$3"; WANT_HUNKS="${4:-}"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$SELF_DIR/../.." && pwd)}"

case "$WANT_FILES" in ''|*[!0-9]*) die "EXPECTED_FILES должно быть числом, дано '$WANT_FILES'" ;; esac
[ "$WANT_FILES" -ge 1 ] || die "негатив, не меняющий ни одного файла, ничего не изолирует"
if [ -n "$WANT_HUNKS" ]; then
  case "$WANT_HUNKS" in ''|*[!0-9]*) die "EXPECTED_HUNKS должно быть числом, дано '$WANT_HUNKS'" ;; esac
  [ "$WANT_HUNKS" -ge 1 ] || die "EXPECTED_HUNKS должно быть >= 1"
fi

git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$REPO — не git-репозиторий"
CAND_SHA="$(git -C "$REPO" rev-parse --verify "$CAND^{commit}" 2>/dev/null)" || die "ref '$CAND' не найден"
NEG_SHA="$(git -C "$REPO" rev-parse --verify "$NEG^{commit}" 2>/dev/null)"  || die "ref '$NEG' не найден"
[ "$CAND_SHA" != "$NEG_SHA" ] || die "'$CAND' и '$NEG' — один и тот же коммит: негатива нет"

FILES="$(git -C "$REPO" diff --name-only "$CAND_SHA..$NEG_SHA" | grep -c . )"
HUNKS="$(git -C "$REPO" diff "$CAND_SHA..$NEG_SHA" | grep -c '^@@' )"

echo "=== negative shape ==="
echo "кандидат : $CAND -> $CAND_SHA"
echo "негатив  : $NEG -> $NEG_SHA"
echo
git -C "$REPO" diff --stat "$CAND_SHA..$NEG_SHA" | sed 's/^/  /'
echo
echo "файлов : $FILES (ожидалось $WANT_FILES)"
if [ -n "$WANT_HUNKS" ]; then
  echo "ханков : $HUNKS (ожидалось $WANT_HUNKS)"
else
  echo "ханков : $HUNKS (ожидание не задано)"
fi

RC=0
[ "$FILES" -eq "$WANT_FILES" ] || { echo "  ✗ число изменённых файлов не совпало"; RC=1; }
if [ -n "$WANT_HUNKS" ]; then
  [ "$HUNKS" -eq "$WANT_HUNKS" ] || { echo "  ✗ число ханков не совпало"; RC=1; }
fi
# A diff that exists on paper but changes no lines is not a negative either.
[ "$HUNKS" -ge 1 ] || { echo "  ✗ дифф не содержит ни одного ханка"; RC=1; }

echo
if [ "$RC" -eq 0 ]; then
  echo "=== ИТОГ: ФОРМА НЕГАТИВА СОВПАЛА ✓ ==="
else
  echo "=== ИТОГ: ПРОВАЛ ✗ — негатив не той формы, лесенку на нём гонять нельзя ==="
fi
exit "$RC"
