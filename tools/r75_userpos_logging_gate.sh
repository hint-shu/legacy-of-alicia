#!/usr/bin/env bash
#
# r75_userpos_logging_gate.sh — INV-10: HandleRaceUserPos не ПРИОБРЕТАЕТ ни одной
# строки логирования.
#
# ЗАЧЕМ ЭТОТ ИНВАРИАНТ
#   Именно этот хендлер дал 15 350 строк [error] за час (R57/#195): он вызывается
#   на КАЖДЫЙ пакет позиции каждого гонщика (каденция 3.83 Гц на гонщика). Одна
#   безобидная строка лога здесь — это отказ дисковой подсистемы под нагрузкой.
#   R75 добавляет в этот хендлер накопление планирования, то есть трогает ровно
#   ту функцию, про которую дом-правило уже писано кровью.
#
# ★ПОЧЕМУ ГЕЙТ ПО ДЕЛЬТЕ, А НЕ «РОВНО НОЛЬ» (ОТСТУПЛЕНИЕ ОТ СПЕКИ, §3.1)
#   Спека требовала абсолютного нуля. На ЭТОМ дереве это ложно-красный: в теле
#   HandleRaceUserPos уже живёт ОДНА пред-существующая строка — QuietLogDebug
#   детектора мести (блок R-revenge, #13), и она есть на чистой базе тоже
#   (проверено: `git show main:…` даёт то же самое). Гейт с порогом «== 0» был бы
#   красным ДО единой правки раунда, и «починили» бы его удалением чужой отладки.
#   Честное свойство, которое раунд действительно обязан держать, — ДЕЛЬТА:
#   среди строк, ДОБАВЛЕННЫХ раундом внутри этой функции, логирования нет.
#   (Тот же вид свойства и та же форма проверки, что у INV-9.)
#
# СЛЕПОТА — ЭТО ОСТАНОВ
#   Пустой дифф, ненайденная функция или подозрительно короткое тело → код 2.
#
# EXIT  0 чисто · 1 нарушение · 2 гейт слеп (останов) · 3 ошибка использования
# ENV   BASE  база сравнения, по умолчанию main
set -uo pipefail

FILE="src/server/race/RaceNetworkHandler.cpp"
FUNC="^void RaceNetworkHandler::HandleRaceUserPos\("
BASE="${BASE:-main}"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SELF_DIR" || { echo "ОСТАНОВ: не найден корень репозитория" >&2; exit 3; }

func_range() {
  local start end
  start="$(grep -nE "$FUNC" "$FILE" | head -1 | cut -d: -f1)"
  [ -n "$start" ] || return 1
  end="$(awk -v s="$start" 'NR>s && /^}$/ {print NR; exit}' "$FILE")"
  [ -n "$end" ] || return 1
  echo "$start $end"
}

run_gate() {
  local range start end diff added violations len
  range="$(func_range)" || {
    echo "ОСТАНОВ: HandleRaceUserPos не найдена в $FILE (переименовали?)" >&2
    return 2; }
  read -r start end <<<"$range"
  len=$((end - start + 1))
  if [ "$len" -lt 20 ]; then
    echo "ОСТАНОВ: тело HandleRaceUserPos подозрительно коротко ($len строк)" >&2
    return 2
  fi

  diff="$(git diff "$BASE" -- "$FILE")" || {
    echo "ОСТАНОВ: git diff против '$BASE' не выполнился" >&2; return 2; }
  if [ -z "$diff" ]; then
    echo "ОСТАНОВ: дифф против '$BASE' ПУСТ — гейту нечего осматривать" >&2
    return 2
  fi

  added="$(printf '%s\n' "$diff" | awk '
    /^@@/ { split($3, a, ","); n = substr(a[1], 2) + 0; next }
    /^\+\+\+/ { next }
    /^\+/ { print n": "substr($0, 2); n++; next }
    /^-/ { next }
    { n++ }
  ')"

  violations="$(printf '%s\n' "$added" | awk -v s="$start" -v e="$end" -F: '
    { ln = $1 + 0 }
    ln >= s && ln <= e && (/QuietLog/ || /spdlog::/) { print }
  ')"

  echo "  файл                 : $FILE"
  echo "  база                 : $BASE"
  echo "  HandleRaceUserPos    : строки $start-$end ($len строк, пост-образ)"
  echo "  добавлено строк всего: $(printf '%s\n' "$added" | grep -c . )"

  if [ -n "$violations" ]; then
    echo "  === ИТОГ: НАРУШЕНИЕ INV-10 (хендлер приобрёл логирование) ==="
    printf '%s\n' "$violations"
    return 1
  fi
  echo "  === ИТОГ: добавленного логирования в HandleRaceUserPos нет ✓ ==="
  return 0
}

self_test() {
  local rc fails=0
  echo "=== САМОПРОГОН r75_userpos_logging_gate.sh ==="

  echo "--- фикстура 1: QuietLogWarn в добавленной строке внутри хендлера"
  cp "$FILE" "$FILE.selftest.bak"
  awk '
    /=== LOA-fix \(R75, #14 Ф2\): ПЛАНИРОВАНИЕ =/ && !done {
      print "      server::util::QuietLogWarn(\"x\");  // фикстура самопрогона"; done=1 }
    { print }' "$FILE.selftest.bak" > "$FILE"
  run_gate >/dev/null 2>&1; rc=$?
  cp "$FILE.selftest.bak" "$FILE"; rm -f "$FILE.selftest.bak"
  if [ "$rc" -eq 1 ]; then echo "    гейт упал (код 1) ✓"; else
    echo "    ПРОВАЛ: ожидался код 1, получен $rc"; fails=$((fails+1)); fi

  echo "--- фикстура 2: слепота (BASE == рабочее дерево, дифф пуст)"
  BASE="$(git rev-parse HEAD)" run_gate >/dev/null 2>&1; rc=$?
  if [ "$rc" -eq 2 ]; then echo "    гейт остановился (код 2) ✓"; else
    echo "    ПРОВАЛ: ожидался код 2, получен $rc"; fails=$((fails+1)); fi

  echo "--- фикстура 3: слепота (функция переименована)"
  cp "$FILE" "$FILE.selftest.bak"
  sed 's/^void RaceNetworkHandler::HandleRaceUserPos(/void RaceNetworkHandler::HandleRaceUserPosRenamed(/' \
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
