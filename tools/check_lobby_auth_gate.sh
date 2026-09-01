#!/usr/bin/env bash
#
# check_lobby_auth_gate.sh — build gate: no lobby handler is registered without an
# explicit authorization decision.
#
# WHY THIS EXISTS
#   Until R72 the lobby answered ANY socket that finished the TCP handshake: a census
#   of the dispatch table found 13 of 36 command handlers that never looked at the
#   client context at all — among them RequestPersonalInfo (guild name, introduction,
#   level and experience for an arbitrary characterUid, enumerable by increment),
#   DeclineInviteToGuild on behalf of any character, and two pre-login log floods.
#   The root was not one forgotten call: it was that each handler decided for itself.
#   A list of sites always falls behind the code, so the decision moved to
#   registration, where it is taken exactly once per command and cannot be skipped.
#   This gate states that as a property of the source: zero raw registrations, a known
#   split, and — this is the part numbers alone do not give — the pre-login set
#   compared BY NAME.
#
# WHY THE SET AND NOT JUST THE COUNTS
#   The counts add up for a WRONG set too: moving RequestPersonalInfo into the
#   pre-login list and QueryServerTime out of it keeps 33/3. Form agrees; the property
#   does not. Hence check (4), which compares the sorted names verbatim.
#
# WHY EVERY REGEX IS ANCHORED AT THE START OF THE LINE
#   The constructor carries a comment block that mentions RegisterCommandHandler by
#   name (deliberately — it tells the next author what breaks this gate). An unanchored
#   count would read that comment as a registration and fail a correct tree. Comment
#   lines start with `//`, so anchoring at column zero makes them physically invisible
#   to the count.
#
# USAGE
#   bash tools/check_lobby_auth_gate.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_lobby_auth_gate.sh
#
# EXIT CODES
#   0 clean · 1 the invariant is broken (offending lines are printed) · 2 the check is
#     invalid (files missing, template names absent from the header, or coverage floor
#     not met — in which case "zero raw registrations" would be blindness, not news)
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$ROOT/src/server/lobby/LobbyNetworkHandler.cpp"
HDR="$ROOT/include/server/lobby/LobbyNetworkHandler.hpp"

# Measured on the tree at 2026-09-02 (round R72): 36 = 33 + 3.
WANT_AUTH=33
WANT_PRE=3
MIN_CALLS=30
WANT_PREAUTH_SET="AcCmdCLCheckWaitingSeqno AcCmdCLLogin AcCmdCLQueryServerTime"

for f in "$SRC" "$HDR"; do
  [ -f "$f" ] || { echo "ОСТАНОВ: нет файла $f — считать нечего"; exit 2; }
done

RAW_LINES="$(grep -nE '^[[:space:]]*_commandServer\.RegisterCommandHandler<protocol::' "$SRC")"
RAW="$(printf '%s' "$RAW_LINES" | grep -c . )"
AUTH="$(grep -cE '^[[:space:]]*RegisterAuthenticatedHandler<protocol::' "$SRC")"
PRE="$(grep -cE '^[[:space:]]*RegisterPreAuthHandler<protocol::' "$SRC")"
CALLS="$(grep -cE '^[[:space:]]*(_commandServer\.)?Register[A-Za-z]*Handler<protocol::' "$SRC")"
PREAUTH_SET="$(grep -oE '^[[:space:]]*RegisterPreAuthHandler<protocol::[A-Za-z0-9_]+' "$SRC" \
  | sed 's/.*protocol:://' | sort | tr '\n' ' ' | sed 's/ $//')"

echo "=== lobby auth gate ==="
echo "файл                 : ${SRC#"$ROOT"/}"
echo "сырых регистраций    : $RAW (ожидалось 0)"
echo "аутентифицированных  : $AUTH (ожидалось $WANT_AUTH)"
echo "пред-логинных        : $PRE (ожидалось $WANT_PRE)"
echo "всего регистраций    : $CALLS (пол охвата: >= $MIN_CALLS)"
echo "пред-логинный набор  : $PREAUTH_SET"
echo "ожидаемый набор      : $WANT_PREAUTH_SET"

# --- Blindness controls. "Zero raw registrations" is only news when the count could
# --- have been non-zero: a typo in a template name would give RAW=0, AUTH=0, PRE=0
# --- and a green verdict on a tree with no registrations at all.
INVALID=0
if ! grep -q 'RegisterAuthenticatedHandler' "$HDR"; then
  echo "ОСТАНОВ: в заголовке нет RegisterAuthenticatedHandler — имя шаблона поехало, счёт читать нельзя"
  INVALID=1
fi
if ! grep -q 'RegisterPreAuthHandler' "$HDR"; then
  echo "ОСТАНОВ: в заголовке нет RegisterPreAuthHandler — имя шаблона поехало, счёт читать нельзя"
  INVALID=1
fi
if [ "$CALLS" -lt "$MIN_CALLS" ]; then
  echo "ОСТАНОВ: регистраций всего $CALLS при поле $MIN_CALLS — конструктор не мог усохнуть втрое;"
  echo "         это сломанный регекс или не тот файл, а не чистое дерево"
  INVALID=1
fi
# ★СУММА СЧИТАЕТСЯ С RAW. Иначе самый важный дефект — сырая регистрация — вышел бы
# кодом 2 «регексы разошлись» вместо кода 1 с указанием строки, то есть гард
# диагностировал бы себя вместо дерева. С RAW в сумме это остаётся честным
# контролем дрейфа регексов и не крадёт диагноз у настоящего нарушителя.
if [ "$((RAW + AUTH + PRE))" -ne "$CALLS" ]; then
  echo "ОСТАНОВ: $RAW + $AUTH + $PRE != $CALLS — счётные регексы разошлись между собой"
  INVALID=1
fi
[ "$INVALID" -eq 0 ] || { echo "=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ==="; exit 2; }

RC=0
if [ "$RAW" -ne 0 ]; then
  echo "  ✗ регистрация без решения об авторизации (сырой RegisterCommandHandler):"
  printf '%s\n' "$RAW_LINES" | sed 's/^/      /'
  RC=1
fi
if [ "$AUTH" -ne "$WANT_AUTH" ]; then
  echo "  ✗ аутентифицированных регистраций $AUTH, ожидалось $WANT_AUTH"
  RC=1
fi
if [ "$PRE" -ne "$WANT_PRE" ]; then
  echo "  ✗ пред-логинных регистраций $PRE, ожидалось $WANT_PRE"
  RC=1
fi
if [ "$PREAUTH_SET" != "$WANT_PREAUTH_SET" ]; then
  echo "  ✗ пред-логинный НАБОР не тот. Числа сходятся и у неверного множества —"
  echo "    поэтому сверяется поимённо. Расхождение:"
  for cmd in $PREAUTH_SET; do
    case " $WANT_PREAUTH_SET " in *" $cmd "*) ;; *) echo "      лишняя пред-логинная команда: $cmd" ;; esac
  done
  for cmd in $WANT_PREAUTH_SET; do
    case " $PREAUTH_SET " in *" $cmd "*) ;; *) echo "      пропала пред-логинная команда: $cmd" ;; esac
  done
  RC=1
fi

echo
if [ "$RC" -eq 0 ]; then
  echo "=== ИТОГ: ЧИСТО ✓ ==="
else
  echo "=== ИТОГ: ПРОВАЛ ✗ — регистрация лобби-хендлера без решения об авторизации ==="
fi
exit "$RC"
