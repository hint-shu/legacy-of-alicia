#!/usr/bin/env bash
# check_chat_sweep_thread.sh — гейт R80: РАЗВЁРТКА ЖИВЁТ НА СВОЁМ СЕТЕВОМ ПОТОКЕ.
#
# ЗАЧЕМ. Несущий инвариант раунда: обход карты клиентов чат-директора и решение
# «кого жать» исполняет ПОТОК СВОЕЙ ЧАТ-СЛУЖБЫ, а не поток задач директора.
# `Tick()` приходит из `RunDirectorTaskLoop` (50 Гц, ЧУЖОЙ поток), и любая
# уборка, переехавшая туда, правила бы `Server::_addressStates` — карту, которая
# живёт ВОВСЕ БЕЗ ЗАМКА и меняется приёмом и разрывом соединения. R78 уже ловил
# этот класс: первая редакция дренажа печаталась потоком директора, и предикат
# идентичности потока поймал это на стенде.
#
# Самая дешёвая и самая точная формулировка того же свойства текстом: тело
# `Tick()` обоих чат-директоров обязано остаться ПУСТЫМ.
#
# ★ЧЕГО ГЕЙТ НЕ УТВЕРЖДАЕТ. Он читает ТЕКСТ, а не поток управления: развёртку,
# вызванную из `Tick()` через третью функцию, он не увидит. Это сторож
# регрессии, а не доказательство. Настоящий страж инварианта — арка стенда
# `thread-identity`, читающая, каким потоком напечатана строка `Reaped`.
#
# ИСПОЛЬЗОВАНИЕ
#   bash tools/check_chat_sweep_thread.sh
#   ROOT=/tmp/other/checkout bash tools/check_chat_sweep_thread.sh
#
# ENV
#   ROOT  по умолчанию — репозиторий, в котором лежит этот скрипт
#
# ВЫХОД: 0 оба Tick() пусты · 1 в чьём-то Tick() появился код
#        2 РАЗБОР СЛЕП (файла нет, функция не найдена, тело не закрылось) —
#          «чисто» без находки читать нельзя
set -uo pipefail
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# файл : полное имя функции
TARGETS=(
  "src/server/messenger/MessengerDirector.cpp:MessengerDirector::Tick"
  "src/server/chat/AllChatDirector.cpp:AllChatDirector::Tick"
)

# Тело функции `void <QUAL>()` как ТЕКСТ БЕЗ КОММЕНТАРИЕВ и без строковых
# литералов. Печатает тело; выходит 2, если разбор не состоялся.
#
# ★КОММЕНТАРИИ ВЫРЕЗАЮТСЯ ПОТОМУ, ЧТО ОБА ТЕЛА СОСТОЯТ ИЗ НИХ ЦЕЛИКОМ: R78
# оставил в `MessengerDirector::Tick()` длинный разбор «почему дренажа здесь
# нет», и гейт, считающий строки, объявил бы его нарушителем. Проверяется
# наличие КОДА, а не пустота файла.
body_of() {
  local file="$1" qual="$2"
  awk -v qual="$qual" '
    BEGIN { started = 0; depth = 0; inblock = 0 }
    {
      line = $0
      # вырезать /* … */ в одну строку и // до конца строки
      gsub(/\/\*[^*]*\*+([^\/*][^*]*\*+)*\//, " ", line)
      if (inblock) {
        if (line ~ /\*\//) { sub(/^.*\*\//, " ", line); inblock = 0 } else { next }
      }
      if (line ~ /\/\*/) { sub(/\/\*.*$/, " ", line); inblock = 1 }
      sub(/\/\/.*$/, " ", line)
      gsub(/"([^"\\]|\\.)*"/, "\"\"", line)

      if (!started) {
        if (line ~ ("^[[:space:]]*void[[:space:]]+" qual "\\(\\)[[:space:]]*$")) {
          started = 1
        }
        next
      }
      n = gsub(/\{/, "{", line); depth += n
      if (depth > 0) opened = 1
      m = gsub(/\}/, "}", line); depth -= m
      if (opened) {
        print line
        if (depth <= 0) { print "@@CLOSED@@"; exit }
      }
    }
    END { if (!started) print "@@NOTFOUND@@" }
  ' "$file"
}

# ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ. Три фикстуры строятся из ЖИВОГО дерева, а
# не из выдуманных кусков: фикстура, не похожая на поднадзорный файл, доказывает
# только то, что гейт умеет читать выдумку.
#   1. код в Tick() мессенджера  -> 1
#   2. код в Tick() all-chat     -> 1
#   3. функция переименована     -> 2 (слепота, а не «чисто»)
#   4. файла нет                 -> 2
if [ "${1:-}" = "--self-test" ] || [ "${1:-}" = "--selftest" ]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  FAIL=0
  echo "=== самопроверка гейта развёртки ==="

  run_fixture() {
    local name="$1" want="$2" dir="$3"
    ROOT="$dir" bash "${BASH_SOURCE[0]}" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then
      echo "  ✓ $name -> $got (ожидалось $want)"
    else
      echo "  ✗ $name -> $got, ожидалось $want"
      FAIL=$((FAIL + 1))
    fi
  }

  # контроль: живое дерево обязано быть чистым, иначе судить фикстуры нечем
  run_fixture "живое дерево" 0 "$ROOT"

  for pair in \
    "messenger:src/server/messenger/MessengerDirector.cpp:MessengerDirector::Tick" \
    "allchat:src/server/chat/AllChatDirector.cpp:AllChatDirector::Tick"
  do
    tag="${pair%%:*}"; rest="${pair#*:}"
    rel="${rest%%:*}"; qual="${rest#*:}"
    d="$TMP/code-$tag"
    mkdir -p "$d/src/server/messenger" "$d/src/server/chat"
    cp "$ROOT/src/server/messenger/MessengerDirector.cpp" "$d/src/server/messenger/"
    cp "$ROOT/src/server/chat/AllChatDirector.cpp" "$d/src/server/chat/"
    # вставить вызов ПОСЛЕДНЕЙ строкой тела: awk доводит до закрывающей скобки
    awk -v qual="$qual" '
      BEGIN { started = 0; depth = 0; opened = 0; done = 0 }
      {
        if (!done && !started && $0 ~ ("^[[:space:]]*void[[:space:]]+" qual "\\(\\)[[:space:]]*$")) {
          started = 1; print; next
        }
        if (started && !done) {
          line = $0
          sub(/\/\/.*$/, " ", line)
          n = gsub(/\{/, "{", line); depth += n
          if (depth > 0) opened = 1
          m = gsub(/\}/, "}", line); depth -= m
          if (opened && depth <= 0) {
            print "  SweepChatSockets();"
            print; done = 1; started = 0; next
          }
        }
        print
      }' "$ROOT/$rel" > "$d/$rel"
    run_fixture "код в $qual()" 1 "$d"
  done

  d="$TMP/renamed"
  mkdir -p "$d/src/server/messenger" "$d/src/server/chat"
  cp "$ROOT/src/server/messenger/MessengerDirector.cpp" "$d/src/server/messenger/"
  sed 's/^void AllChatDirector::Tick()$/void AllChatDirector::TickRenamed()/' \
    "$ROOT/src/server/chat/AllChatDirector.cpp" > "$d/src/server/chat/AllChatDirector.cpp"
  run_fixture "функция переименована (слепота)" 2 "$d"

  run_fixture "файла нет (слепота)" 2 "$TMP/absent"

  if [ "$FAIL" -eq 0 ]; then
    echo "=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ ==="
    exit 0
  fi
  echo "=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ ($FAIL) ==="
  exit 2
fi

RC=0
FOUND=0
echo "=== gate: развёртка чат-сокетов исполняется своим сетевым потоком ==="
echo "дерево : $ROOT"

for target in "${TARGETS[@]}"; do
  rel="${target%%:*}"
  qual="${target#*:}"
  path="$ROOT/$rel"
  if [ ! -r "$path" ]; then
    echo "  ✗ РАЗБОР СЛЕП: нет файла $rel"
    exit 2
  fi
  out="$(body_of "$path" "$qual")"
  if printf '%s\n' "$out" | grep -qF '@@NOTFOUND@@'; then
    echo "  ✗ РАЗБОР СЛЕП: в $rel не найдено определение 'void $qual()'"
    exit 2
  fi
  if ! printf '%s\n' "$out" | grep -qF '@@CLOSED@@'; then
    echo "  ✗ РАЗБОР СЛЕП: тело $qual() не закрылось — скобки не сошлись"
    exit 2
  fi
  FOUND=$((FOUND + 1))
  # Код = всё, что осталось, кроме скобок, пробелов и маркера конца.
  code="$(printf '%s\n' "$out" \
    | grep -vF '@@CLOSED@@' \
    | tr -d '[:space:]{}')"
  if [ -n "$code" ]; then
    echo "  ✗ $qual() содержит КОД: '$code'"
    echo "     развёртка обязана жить в HandleNetworkTick(), а не здесь:"
    echo "     Tick() приходит с потока задач директора, чужого для карты"
    echo "     клиентов и для Server::_addressStates (та вовсе без замка)."
    RC=1
  else
    echo "  ✓ $qual() пуст"
  fi
done

echo "проверено функций : $FOUND (ожидалось ${#TARGETS[@]})"
if [ "$FOUND" -ne "${#TARGETS[@]}" ]; then
  echo "=== ИТОГ: РАЗБОР СЛЕП ✗ ==="
  exit 2
fi
if [ "$RC" -eq 0 ]; then
  echo "=== ИТОГ: ЧИСТО ✓ ==="
else
  echo "=== ИТОГ: НАРУШЕНО ✗ ==="
fi
exit "$RC"
