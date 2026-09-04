#!/usr/bin/env bash
#
# r75_record_access_gate.sh — INV-9: ни одна строка, ДОБАВЛЕННАЯ раундом R75
# внутри RaceInstance::Stop(), не содержит прямого `.Mutable(` / `.Immutable(`.
#
# ЗАЧЕМ ГЕЙТ ПО СВОЙСТВУ ДИФФА, А НЕ ПО СОДЕРЖИМОМУ ФАЙЛА
#   Свойства «этот доступ добавлен этим раундом» у файла на диске НЕ СУЩЕСТВУЕТ.
#   Гейт «любой .Mutable( внутри Stop()» был бы КРАСНЫМ на кандидате (их там 12,
#   и раунд намеренно их не конвертирует), а гейт «ровно 12» — это «гейт по формам
#   = ложная полнота»: удали один старый прямой доступ, добавь один новый, число
#   сойдётся и дефект проедет. Свойство, которое ДЕЙСТВИТЕЛЬНО существует и
#   проверяемо, — свойство ДЕЛЬТЫ: среди строк с префиксом '+' в
#   `git diff BASE..рабочее дерево`, попавших в диапазон строк Stop() ПОСТ-образа,
#   не должно быть ни одного прямого доступа к записи.
#
#   Причина самого инварианта: новый блок пер-курсовых рекордов стоит на участке
#   между начислениями и рассылкой табло. Прямой `Mutable` на непрогруженной
#   записи БРОСАЕТ — и вся комната осталась бы без результата заезда. Пояс
#   util::TryMutate это исключает, но только если им действительно пользуются.
#
# СЛЕПОТА — ЭТО ОСТАНОВ, А НЕ «ЧИСТО»
#   Пустой дифф или ненайденная функция дают код 2. Проверка, которая не смогла
#   ничего осмотреть, не имеет права читаться как успех.
#
# EXIT  0 чисто · 1 нарушение · 2 гейт слеп (останов) · 3 ошибка использования
#
# ENV   BASE  база сравнения, по умолчанию main
#
# САМОПРОГОН (гейт сперва доказывает, что умеет упасть)
#   bash tools/r75_record_access_gate.sh --self-test
set -uo pipefail

FILE="src/server/race/RaceInstance.cpp"
BASE="${BASE:-main}"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SELF_DIR" || { echo "ОСТАНОВ: не найден корень репозитория" >&2; exit 3; }

# Диапазон тела Stop() в ПОСТ-образе (рабочее дерево). Числа не хардкодятся.
stop_range() {
  local start end
  start="$(grep -n '^void RaceInstance::Stop()$' "$FILE" | head -1 | cut -d: -f1)"
  [ -n "$start" ] || return 1
  end="$(awk -v s="$start" 'NR>s && /^}$/ {print NR; exit}' "$FILE")"
  [ -n "$end" ] || return 1
  echo "$start $end"
}

run_gate() {
  local range start end diff added violations
  range="$(stop_range)" || {
    echo "ОСТАНОВ: тело RaceInstance::Stop() не найдено в $FILE (переименовали?)" >&2
    return 2; }
  read -r start end <<<"$range"

  diff="$(git diff "$BASE" -- "$FILE")" || {
    echo "ОСТАНОВ: git diff против '$BASE' не выполнился" >&2; return 2; }
  if [ -z "$diff" ]; then
    echo "ОСТАНОВ: дифф против '$BASE' ПУСТ — гейту нечего осматривать" >&2
    return 2
  fi

  # Строки '+' с их номерами в ПОСТ-образе (по заголовкам ханков).
  added="$(printf '%s\n' "$diff" | awk '
    /^@@/ { split($3, a, ","); n = substr(a[1], 2) + 0; next }
    /^\+\+\+/ { next }
    /^\+/ { print n": "substr($0, 2); n++; next }
    /^-/ { next }
    { n++ }
  ')"

  violations="$(printf '%s\n' "$added" | awk -v s="$start" -v e="$end" -F: '
    { ln = $1 + 0 }
    ln >= s && ln <= e && (/\.Mutable\(/ || /\.Immutable\(/) { print }
  ')"

  echo "  файл            : $FILE"
  echo "  база            : $BASE"
  echo "  тело Stop()     : строки $start-$end (пост-образ)"
  echo "  добавлено строк : $(printf '%s\n' "$added" | grep -c . )"

  if [ -n "$violations" ]; then
    echo "  === ИТОГ: НАРУШЕНИЕ INV-9 ==="
    printf '%s\n' "$violations"
    return 1
  fi
  echo "  === ИТОГ: добавленных прямых .Mutable(/.Immutable( внутри Stop() нет ✓ ==="
  return 0
}

self_test() {
  local rc fails=0
  echo "=== САМОПРОГОН r75_record_access_gate.sh ==="

  echo "--- фикстура 1: прямой .Mutable( в добавленной строке внутри Stop()"
  cp "$FILE" "$FILE.selftest.bak"
  awk '
    /=== LOA-fix \(R75, #14\): ПЕР-КУРСОВЫЕ РЕКОРДЫ ПЕРСОНАЖА/ && !done {
      print "  someRecord.Mutable([](int&){});  // фикстура самопрогона"; done=1 }
    { print }' "$FILE.selftest.bak" > "$FILE"
  run_gate >/dev/null 2>&1; rc=$?
  cp "$FILE.selftest.bak" "$FILE"; rm -f "$FILE.selftest.bak"
  if [ "$rc" -eq 1 ]; then echo "    гейт упал (код 1) ✓"; else
    echo "    ПРОВАЛ: ожидался код 1, получен $rc"; fails=$((fails+1)); fi

  echo "--- фикстура 2: слепота (BASE == рабочее дерево, дифф пуст)"
  BASE="$(git rev-parse HEAD)" run_gate >/dev/null 2>&1; rc=$?
  if [ "$rc" -eq 2 ]; then echo "    гейт остановился (код 2) ✓"; else
    echo "    ПРОВАЛ: ожидался код 2, получен $rc"; fails=$((fails+1)); fi

  echo "--- фикстура 3: слепота (функция не найдена)"
  cp "$FILE" "$FILE.selftest.bak"
  sed 's/^void RaceInstance::Stop()$/void RaceInstance::StopRenamed()/' \
    "$FILE.selftest.bak" > "$FILE"
  run_gate >/dev/null 2>&1; rc=$?
  cp "$FILE.selftest.bak" "$FILE"; rm -f "$FILE.selftest.bak"
  if [ "$rc" -eq 2 ]; then echo "    гейт остановился (код 2) ✓"; else
    echo "    ПРОВАЛ: ожидался код 2, получен $rc"; fails=$((fails+1)); fi

  [ "$fails" -eq 0 ] && { echo "=== САМОПРОГОН ПРОЙДЕН ✓ ==="; return 0; }
  echo "=== САМОПРОГОН ПРОВАЛЕН ($fails) ==="; return 1
}

case "${1:-}" in
  --self-test) self_test; exit $? ;;
  "") run_gate; exit $? ;;
  *) echo "usage: $0 [--self-test]" >&2; exit 3 ;;
esac
