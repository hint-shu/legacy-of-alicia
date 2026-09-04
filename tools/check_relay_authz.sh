#!/usr/bin/env bash
# check_relay_authz.sh — build-гейт: классификатор relay-нагрузок ПОЛОН.
#
# ЗАЧЕМ. R71-6b авторизует relay по действующему лицу ВНУТРИ нагрузки. Правило
# тотально ровно до тех пор, пока в switch назван КАЖДЫЙ тип. Новый тип, забытый
# в классификаторе, становится неавторизованным каналом молча.
# Контроль слепоты обязателен: «0 пропущенных» имеет смысл только рядом с
# «и я нашёл N перечислений», иначе сломанный разбор читается как чистота.
#
# ИСПОЛЬЗОВАНИЕ
#   bash tools/check_relay_authz.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_relay_authz.sh
#
# ENV
#   ROOT           по умолчанию — репозиторий, в котором лежит этот скрипт
#   RELAY_MIN_IDS  по умолчанию 11 — пол слепоты; поднимать сознательно, никогда
#                  не опускать ради зелёного прогона
#
# ВЫХОД: 0 полон · 1 есть пропущенные · 2 разбор слеп (файлов/имён меньше пола)
set -uo pipefail
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ENUM="$ROOT/include/libserver/network/command/proto/relay/RelayMessageDefinitions.hpp"
IMPL="$ROOT/include/server/race/RelayAuthz.hpp"
MIN_IDS="${RELAY_MIN_IDS:-11}"        # сегодня их ровно 11; поднимать сознательно
[ -r "$ENUM" ] && [ -r "$IMPL" ] || { echo "ОСТАНОВ: нет $ENUM или $IMPL"; exit 2; }

# имена элементов между `enum class RelayCommandId` и закрывающей `};`
IDS="$(awk '/enum class RelayCommandId/{f=1;next} f&&/^};/{exit} f' "$ENUM" \
       | sed -nE 's/^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=.*/\1/p')"
N="$(printf '%s\n' "$IDS" | grep -c .)"
[ "$N" -ge "$MIN_IDS" ] || { echo "ОСТАНОВ: разобрано $N имён (< $MIN_IDS) — разбор слеп"; exit 2; }

MISS=0
while IFS= read -r id; do
  [ -n "$id" ] || continue
  grep -qF "case RelayCommandId::$id:" "$IMPL" \
    || { echo "  ✗ не классифицирован: $id"; MISS=$((MISS+1)); }
done <<< "$IDS"

echo "=== relay-authz gate ==="
echo "дерево               : $ROOT"
echo "типов нагрузки       : $N (минимум $MIN_IDS)"
echo "не классифицировано  : $MISS"
[ "$MISS" -eq 0 ] && { echo "=== ИТОГ: ПОЛОН ✓ ==="; exit 0; }
echo "=== ИТОГ: ОСТАНОВ ✗ ==="; exit 1
