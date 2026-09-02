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
#
# ★СТАТУС И stderr `grep` ЧИТАЮТСЯ, А НЕ ГЛУШАТСЯ (правка ревью, итерация 3).
# Прежняя редакция гнала `grep ... 2>/dev/null | sed | sort | uniq -d`: статус
# принадлежал `uniq`, а сообщение `grep` уходило в никуда. Файл, у которого
# первый байт прочёлся (проверка `readable_file` прошла), но чтение целиком
# отказало — EIO, отозванный доступ, исчезнувший монтаж, — давал ПУСТО, то есть
# ровно то же, что «дублей нет», и гейт печатал «ЧИСТО ✓», ни разу не заглянув
# внутрь. `grep` различает три исхода: 0 — нашёл, 1 — НЕ нашёл (это норма и
# именно это раньше было неотличимо от беды), ≥2 — ошибка чтения.
#
# Возврат: 0 — файл прочитан (дубли, если есть, напечатаны), 2 — прочитать не
# удалось; вызывающий обязан читать код возврата, иначе проверка снова станет
# слепой.
duplicates_in() {
  DUP_RAW="$(grep -E '^[[:space:]]*#[[:space:]]*include[[:space:]]' "$1" 2>"$GREP_ERR")"
  DUP_RC=$?
  if [ "$DUP_RC" -ge 2 ] || [ -s "$GREP_ERR" ]; then
    return 2
  fi
  printf '%s\n' "$DUP_RAW" | sed 's/[[:space:]]*$//' | sort | uniq -d
  # `pipefail` включён (см. `set` выше), поэтому отказ ЛЮБОЙ стадии — sed без
  # памяти, sort без места под временный файл — виден здесь и означает то же
  # самое: результат пуст не потому, что дублей нет.
  DUP_PIPE_RC=$?
  [ "$DUP_PIPE_RC" -eq 0 ] || return 2
  return 0
}

# ★ЧИТАЕМОСТЬ ФАЙЛА — ОТДЕЛЬНЫЙ ВОПРОС К `duplicates_in` (правка ревью,
# итерация 2). `duplicates_in` глушит stderr и на нечитаемом файле возвращает
# ПУСТО — ровно то же, что «дублей нет». Прежняя редакция считала такой файл
# просканированным, поэтому счётчик «просканировано» сходился с «найдено», и
# гейт печатал «ЧИСТО ✓», ни разу не заглянув внутрь. Пустой результат обязан
# отличаться от непрочитанного файла ПРОВЕРКОЙ, а не надеждой.
readable_file() {
  [ -r "$1" ] && head -c 1 "$1" >/dev/null 2>&1
}

# Blindness guard #1: the canary must come back dirty.
# Создание и запись канарейки проверяются по тому же правилу, что и в
# `no_name_regex_gate.sh`: непроверенный `mktemp` — это класс, а не сайт.
GREP_ERR="$(mktemp 2>/dev/null)" || GREP_ERR=""
if [ -z "$GREP_ERR" ] || [ ! -f "$GREP_ERR" ]; then
  echo "ОСТАНОВ: не удалось создать файл для ошибок чтения (mktemp) — проверить нечем."
  exit 2
fi
trap 'rm -f "$GREP_ERR"' EXIT

CANARY="$(mktemp 2>/dev/null)" || CANARY=""
if [ -z "$CANARY" ] || [ ! -f "$CANARY" ]; then
  echo "ОСТАНОВ: не удалось создать канареечный файл (mktemp) — проверить нечем."
  exit 2
fi
trap 'rm -f "$CANARY" "$GREP_ERR"' EXIT
if ! {
  echo '#include <string>'
  echo '#include <vector>'
  echo '#include <string>'
} > "$CANARY"; then
  echo "ОСТАНОВ: не удалось записать канареечный файл '$CANARY'."
  exit 2
fi
CANARY_DUPS="$(duplicates_in "$CANARY")"
CANARY_RC=$?
if [ "$CANARY_RC" -ne 0 ] || [ -z "$CANARY_DUPS" ]; then
  echo "ОСТАНОВ: канарейка с заведомым дублем прочиталась как чистая (код $CANARY_RC) —"
  echo "         конвейер сломан, ноль нарушителей читать нельзя."
  exit 2
fi

# ★И ТРЕТИЙ ИСХОД: файл БЕЗ единой строки `#include` обязан вернуться чистым и с
# кодом 0. `grep` отвечает на «не нашёл» кодом 1, и если читать любой ненулевой
# код как беду, гейт станет ложно-КРАСНЫМ на честном файле (в дереве такой есть:
# include/libserver/Constants.hpp). Три исхода `grep` — 0/1/≥2 — обязаны быть
# различимы все три, иначе различение не доказано.
NO_INCLUDES="$(mktemp 2>/dev/null)" || NO_INCLUDES=""
if [ -z "$NO_INCLUDES" ] || [ ! -f "$NO_INCLUDES" ]; then
  echo "ОСТАНОВ: не удалось создать канарейку без включений (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$GREP_ERR" "$NO_INCLUDES"' EXIT
echo 'int main() { return 0; }' > "$NO_INCLUDES"
NO_INCLUDES_DUPS="$(duplicates_in "$NO_INCLUDES")"
NO_INCLUDES_RC=$?
if [ "$NO_INCLUDES_RC" -ne 0 ] || [ -n "$NO_INCLUDES_DUPS" ]; then
  echo "ОСТАНОВ: файл без включений вернул код $NO_INCLUDES_RC и вывод"
  echo "         '$NO_INCLUDES_DUPS' — «не нашёл» спутано с «не прочитал»."
  exit 2
fi

# ★КАНАРЕЙКА ДОКАЗЫВАЕТ И ВТОРОЙ ИСХОД: файл, чтение которого ОТКАЗЫВАЕТ,
# обязан отличаться от файла без дублей. Без этой проверки «умеет вернуть 2»
# оставалось бы намерением, а не свойством (правка ревью, итерация 3). Под root
# биты прав не действуют, поэтому улика ставится только под обычным
# пользователем — ложно-зелёная проверка хуже отсутствующей.
if [ "$(id -u)" -ne 0 ]; then
  UNREADABLE="$(mktemp 2>/dev/null)" || UNREADABLE=""
  if [ -z "$UNREADABLE" ] || [ ! -f "$UNREADABLE" ]; then
    echo "ОСТАНОВ: не удалось создать файл для проверки нечитаемости (mktemp)."
    exit 2
  fi
  trap 'rm -f "$CANARY" "$GREP_ERR" "$NO_INCLUDES" "$UNREADABLE"' EXIT
  echo '#include <string>' > "$UNREADABLE"
  chmod 000 "$UNREADABLE"
  duplicates_in "$UNREADABLE" >/dev/null
  UNREADABLE_RC=$?
  if [ "$UNREADABLE_RC" -ne 2 ]; then
    echo "ОСТАНОВ: нечитаемый файл вернул код $UNREADABLE_RC вместо 2 —"
    echo "         «пусто» и «не прочитали» снова неотличимы, гейт слеп."
    exit 2
  fi
  chmod 600 "$UNREADABLE"
fi

# Blindness guard #2: count what will actually be read.
#
# ★КОД ВОЗВРАТА `find` ЧИТАЕТСЯ БЕЗ ТРУБЫ (правка ревью, итерация 2): в
# `find ... | sort` статус принадлежит `sort` и всегда равен нулю, поэтому
# нечитаемый подкаталог выпадал из обхода молча.
SCAN_ERR="$(mktemp 2>/dev/null)" || SCAN_ERR=""
if [ -z "$SCAN_ERR" ] || [ ! -f "$SCAN_ERR" ]; then
  echo "ОСТАНОВ: не удалось создать файл для ошибок обхода (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$GREP_ERR" "$NO_INCLUDES" "$UNREADABLE" "$SCAN_ERR"' EXIT

FILES_RAW="$(cd "$ROOT" && find $SCOPE -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inl' \) 2>"$SCAN_ERR")"
FIND_RC=$?
if [ "$FIND_RC" -ne 0 ] || [ -s "$SCAN_ERR" ]; then
  echo "ОСТАНОВ: обход дерева (find) завершился с кодом $FIND_RC и сообщениями:"
  sed 's/^/         /' "$SCAN_ERR"
  echo "         часть дерева недоступна — «ноль дублей» читать нельзя."
  exit 2
fi
FILES="$(printf '%s\n' "$FILES_RAW" | sort)"
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
  if ! readable_file "$ROOT/$relative"; then
    echo "ОСТАНОВ: файл '$relative' не читается — duplicates_in вернул бы по нему"
    echo "         пусто, что неотличимо от «дублей нет». Слепое «чисто» запрещено."
    exit 2
  fi
  DUPS="$(duplicates_in "$ROOT/$relative")"
  DUP_STATUS=$?
  if [ "$DUP_STATUS" -ne 0 ]; then
    echo "ОСТАНОВ: чтение файла '$relative' отказало уже ПОСЛЕ проверки читаемости"
    echo "         (код $DUP_STATUS) — пустой результат неотличим от «дублей нет»."
    sed 's/^/         /' "$GREP_ERR"
    exit 2
  fi
  # Счётчик растёт ТОЛЬКО после доказанного чтения: «просканировано» обязано
  # означать «прочитано», иначе число сходится, а гейт слеп.
  SCANNED=$((SCANNED + 1))
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
