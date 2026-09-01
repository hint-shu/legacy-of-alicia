#!/usr/bin/env bash
#
# no_name_regex_gate.sh — build gate (LOA-fix R73, backlog #130-C8):
# no regular expression is compiled anywhere in the data-access layer.
#
# WHY THIS EXISTS
#   `FileDataSource::RetrieveCharacterUidByName` and `IsUserNameUnique` used to
#   build a std::regex OUT OF A NAME THAT ARRIVES ON THE WIRE (up to ~8190 bytes:
#   Stream.cpp reads to the NUL under CommandServer.cpp's MaxCommandDataSize).
#   A name like `(a{200}){200}` explodes the libstdc++ automaton at CONSTRUCTION
#   time; a name like `[a-` throws regex_error out of the handler — one [error]
#   line per packet, from six authenticated handlers. R73 removed both.
#
# WHY THE SCOPE IS A DIRECTORY, NOT A FILE
#   Keying the gate on the two functions that had the defect would be a list of
#   sites of size two: a lookup added tomorrow in a NEW file of the same layer
#   would be invisible. The property is "the data-access layer compiles no
#   regular expressions", and a new file inherits it for free.
#
# STATED RADIUS (honest, do not oversell this gate)
#   It covers src/libserver/data/ and include/libserver/data/ only. A regex built
#   from a client name under
#   src/server/** is NOT caught here. That residual class is covered for the two
#   sites this round fixes by the ladder's disappearing string and by neg-d; it
#   is not covered globally.
#
# HOW IT PROVES IT CAN FAIL
#   Before scanning the tree it greps its own pattern out of a canary file it
#   writes itself. If the pattern or grep were broken, "0 offenders" would be a
#   false green — so a canary that does not match is ОСТАНОВ, not a pass.
#
# USAGE
#   bash tools/no_name_regex_gate.sh
#   ROOT=/tmp/some/other/checkout bash tools/no_name_regex_gate.sh
#
# ENV
#   ROOT               default: the repository this script lives in
#   REGEX_MIN_FILES    default 14 — refuse to run against fewer files than this
#                      (today the two data directories hold 16). Raise it
#                      deliberately; never lower it to make a run go green.
#
# EXIT CODES
#   0 clean · 1 offenders found (printed) · 2 not scannable / blind
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
REGEX_MIN_FILES="${REGEX_MIN_FILES:-14}"
SCOPE="src/libserver/data include/libserver/data"
PATTERN='std::regex|std::basic_regex|#[[:space:]]*include[[:space:]]*<regex>'

for d in $SCOPE; do
  [ -d "$ROOT/$d" ] || { echo "ОСТАНОВ: нет каталога $ROOT/$d — считать нечего"; exit 2; }
done

# Blindness guard #1: the pattern must be able to match something.
CANARY="$(mktemp)"
trap 'rm -f "$CANARY"' EXIT
{
  echo '#include <regex>'
  echo 'const std::regex rg(name);'
  echo 'std::basic_regex<char> other(name);'
} > "$CANARY"
CANARY_HITS="$(grep -acE "$PATTERN" "$CANARY" || true)"
if [ "$CANARY_HITS" -lt 3 ]; then
  echo "ОСТАНОВ: канарейка дала $CANARY_HITS совпадений из 3 — регекс или grep сломаны,"
  echo "         ноль нарушителей читать нельзя."
  exit 2
fi

# Blindness guard #2: how many files exist vs how many grep actually opened.
FOUND="$(cd "$ROOT" && find $SCOPE -type f | wc -l)"
SCANNED="$(cd "$ROOT" && grep -rac --binary-files=text '' $SCOPE 2>/dev/null | wc -l)"

if [ "$FOUND" -lt "$REGEX_MIN_FILES" ]; then
  echo "ОСТАНОВ: под «$SCOPE» найдено $FOUND файлов, ожидалось не меньше $REGEX_MIN_FILES"
  echo "         ноль нарушителей на неполном дереве — это не «чисто», это слепота."
  exit 2
fi
if [ "$SCANNED" -ne "$FOUND" ]; then
  echo "ОСТАНОВ: grep открыл $SCANNED файлов из $FOUND — часть дерева не просканирована."
  exit 2
fi

OFFENDERS="$(cd "$ROOT" && grep -rnE --binary-files=text "$PATTERN" $SCOPE || true)"
if [ -z "$OFFENDERS" ]; then
  COUNT=0
else
  COUNT="$(printf '%s\n' "$OFFENDERS" | grep -c .)"
fi

echo "=== no-name-regex gate ==="
echo "дерево          : $ROOT"
echo "область         : $SCOPE"
echo "                  (радиус заявлен в шапке — src/server/** сюда НЕ входит)"
echo "просканировано  : $SCANNED файлов из $FOUND найденных (минимум $REGEX_MIN_FILES)"
echo "регулярок       : $COUNT (ожидалось 0)"

if [ "$COUNT" -ne 0 ]; then
  echo
  echo "нарушители:"
  printf '%s\n' "$OFFENDERS" | sed 's/^/  /'
  echo
  echo "=== ИТОГ: ПРОВАЛ ✗ — имя с провода не имеет права становиться программой ==="
  exit 1
fi

echo "=== ИТОГ: ЧИСТО ✓ ==="
exit 0
