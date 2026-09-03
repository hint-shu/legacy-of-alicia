#!/usr/bin/env bash
# check_race_rejection_throttle.sh — build-гейт: ЖАЛОБА, КОТОРУЮ ЗАКАЗЫВАЕТ КЛИЕНТ,
# НЕ ПИШЕТСЯ СЫРОЙ.
#
# ЗАЧЕМ. Гард, который отбрасывает пакет и пишет строку НА КАЖДЫЙ пакет, меняет один
# отказ в обслуживании на другой: R57 нашёл ровно это (15 350 строк [error] за час),
# ревью 5 раунда R71 нашло пять таких же строк в семействе наводки — причём под
# `_raceInstancesMutex`, то есть ценой задержки КАЖДОЙ комнаты процесса.
# ★★ЭТОТ ГЕЙТ — ПОМОЩНИК, А НЕ ДОКАЗАТЕЛЬСТВО. СКАЗАНО ВСЛУХ ПО РЕВЬЮ 8 #1 (BLOCK).
#
# Итерация 8 объявила его «тотальным правилом». ЭТО БЫЛО НЕВЕРНО, и ревью назвало
# оба обхода поимённо:
#   * он МЕРИТ ФОРМУ, а не дефект: `if (!_t.Allow(s)) QuietLogWarn(...)` — то есть
#     ровно обратная логика, флудящая по построению, — парсер проходит, потому что
#     `Throttle.Allow(` стоит рядом;
#   * он читает ОДИН `.cpp` и не видит жалобу, которую печатает ДИСПЕТЧЕР
#     (`CommandServer.cpp`) по броску из хендлера; вынести лог в помощник — тот же
#     обход ([[gate-by-form-gives-false-completeness]], [[checker-written-by-form]]).
# Настоящий тотальный инвариант живёт ТЕПЕРЬ В КОДЕ, в одной точке разветвления:
# дроссель с фиксированной ёмкостью на границе «хендлер бросил» в
# `CommandServer.cpp` (`KeyedLogThrottle`, R71-31). Улика — НЕ этот скрипт, а
# флуд-арки стенда (`P-flood-nodragon`, `P-flood-notinrace`): контроль печатает
# строку на пакет, кандидат — не больше потолка дросселя.
#
# Что скрипт всё-таки полезно ловит: сырой `QuietLog*` в тронутых раундом хендлерах,
# то есть РЕГРЕССИЮ ПО НЕВНИМАТЕЛЬНОСТИ. Держим его ровно в этом качестве.
#
# ЧТО СЧИТАЕТСЯ ЗАДРОССЕЛЕННЫМ. Вызов лога, стоящий В ОБЛАСТИ ДЕЙСТВИЯ дросселя.
# Область открывается строкой с `Throttle.Allow(` и живёт пять СОДЕРЖАТЕЛЬНЫХ строк
# (не пустых, не комментариев) — этого хватает на форму
# `if (_xThrottle.Allow(s)) { <подготовка аргумента> QuietLogWarn(...) }`, которая
# в этом файле встречается, — и ЗАКРЫВАЕТСЯ ДОСРОЧНО на `return;` или на строке,
# начинающейся с `}`. Досрочное закрытие — главное: без него сырая жалоба соседней
# ветки проехала бы на дросселе предыдущей ([[a-blind-checker-says-clean]]).
#
# КОНТРОЛЬ СЛЕПОТЫ ОБЯЗАТЕЛЕН: «0 сырых» имеет смысл только рядом с «и я нашёл N
# хендлеров и M вызовов лога». Разбор, который ничего не нашёл, — не чистота.
#
# ИСПОЛЬЗОВАНИЕ
#   bash tools/check_race_rejection_throttle.sh
#   ROOT=/tmp/other/checkout bash tools/check_race_rejection_throttle.sh
#
# ENV
#   ROOT              по умолчанию — репозиторий, в котором лежит скрипт
#   THROTTLE_MIN_FN   пол по числу найденных хендлеров (по умолчанию 8)
#   THROTTLE_MIN_LOG  пол по числу найденных вызовов лога внутри них (умолч. 20)
#
# ВЫХОД: 0 правило соблюдено · 1 есть сырые вызовы · 2 разбор слеп
set -uo pipefail
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$ROOT/src/server/race/RaceNetworkHandler.cpp"
MIN_FN="${THROTTLE_MIN_FN:-8}"
MIN_LOG="${THROTTLE_MIN_LOG:-20}"
[ -r "$SRC" ] || { echo "ОСТАНОВ: нет $SRC"; exit 2; }

# ХЕНДЛЕРЫ ПОД ПРАВИЛОМ — ровно те функции, которых раунд касается.
FUNCS="RaceNetworkHandler::HandleUseMagicItem
RaceNetworkHandler::HandleRequestMagicItem
RaceNetworkHandler::HandleStartMagicTarget
RaceNetworkHandler::HandleChangeMagicTarget
RaceNetworkHandler::HandleActivateSkillEffect
RaceNetworkHandler::ScheduleSkillEffect
RaceNetworkHandler::HandleRelay
RaceNetworkHandler::HandleUserRaceItemGet"

REPORT="$(awk -v funcs="$FUNCS" '
BEGIN {
  n = split(funcs, f, "\n");
  for (i = 1; i <= n; i++) if (f[i] != "") want[f[i]] = 1;
  cur = ""; depth = 0; fn = 0; logs = 0; raw = 0; armed = 0;
}
{
  line = $0;
  # начало интересующей функции: сигнатура на уровне файла
  if (cur == "") {
    for (name in want) {
      if (index(line, name "(") > 0 && line !~ /^[[:space:]]/) {
        cur = name; seen[name] = 1; fn++; depth = 0; started = 0; armed = 0;
        break;
      }
    }
  }
  if (cur != "") {
    o = gsub(/\{/, "{", line); c = gsub(/\}/, "}", line);
    depth += o - c;
    if (o > 0) started = 1;

    if (line ~ /server::util::QuietLog(Warn|Error)\(/) {
      logs++;
      if (armed <= 0) { raw++; printf "  ✗ сырая жалоба: %s:%d  в %s\n", FILENAME, NR, cur; }
    }
    # область дросселя живёт только по СОДЕРЖАТЕЛЬНЫМ строкам
    t = line; sub(/^[[:space:]]+/, "", t);
    if (t != "" && t !~ /^\/\// && t !~ /^\/\*/ && t !~ /^\*/) {
      if (index(t, "Throttle.Allow(") > 0) armed = 5;
      else if (t ~ /^return[; ]/ || t ~ /^\}/) armed = 0;
      else if (armed > 0) armed--;
    }
    if (started && depth <= 0) cur = "";
  }
}
END {
  for (name in want) if (!(name in seen)) printf "  ! НЕ НАЙДЕН хендлер: %s\n", name;
  printf "FN=%d LOGS=%d RAW=%d\n", fn, logs, raw;
}' "$SRC")"

echo "$REPORT" | grep -v '^FN=' || true
STATS="$(printf '%s\n' "$REPORT" | grep '^FN=')"
FN="${STATS#FN=}"; FN="${FN%% *}"
LOGS="$(printf '%s' "$STATS" | sed -E 's/.*LOGS=([0-9]+).*/\1/')"
RAW="$(printf '%s' "$STATS" | sed -E 's/.*RAW=([0-9]+).*/\1/')"

echo "=== race-rejection-throttle gate ==="
echo "дерево                : $ROOT"
echo "хендлеров под правилом: $FN (минимум $MIN_FN)"
echo "вызовов лога в них    : $LOGS (минимум $MIN_LOG)"
echo "сырых (без дросселя)  : $RAW"

if [ "$FN" -lt "$MIN_FN" ] || [ "$LOGS" -lt "$MIN_LOG" ]; then
  echo "=== ИТОГ: РАЗБОР СЛЕП ✗ ==="; exit 2
fi
[ "$RAW" -eq 0 ] && { echo "=== ИТОГ: ПРАВИЛО СОБЛЮДЕНО ✓ ==="; exit 0; }
echo "=== ИТОГ: ОСТАНОВ ✗ ==="; exit 1
