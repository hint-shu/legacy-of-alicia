#!/usr/bin/env bash
#
# check_lobby_auth_gate.sh — build gate: no lobby handler is registered without an
# explicit authorization decision, and the decision is taken BEFORE the packet is
# deserialized.
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
# ★WHAT THE FIRST VERSION OF THIS GATE COULD NOT SEE (Codex finding 3, iteration 1)
#   It grepped ONE .cpp with regexes anchored at the start of a line. Two holes:
#     (a) a registration written across two lines was invisible, and every expected
#         count stayed green;
#     (b) the lobby header publicly exposed the raw CommandServer (GetCommandServer),
#         so a registration could be made from ANY other translation unit — outside
#         the single file this gate reads.
#   Hole (b) is not closed by a gate at all, it is closed by the TYPE: the accessor is
#   gone and the dispatcher is private, so the only place a lobby handler CAN be
#   registered is the file below. This gate now proves that boundary instead of
#   assuming it:
#     • the counting parser strips comments and string literals and then ignores line
#       breaks entirely, so a multi-line registration is counted like any other;
#     • the header must not hand the dispatcher out (check 5);
#     • the authenticated wrapper must go through RegisterGatedCommandHandler, i.e.
#       the gate that runs BEFORE C::Read (check 6) — a wrapper moved back onto the
#       plain RegisterCommandHandler would let an unauthenticated socket reach the
#       deserializer with an undersized frame, which is one [error] line per packet;
#     • no OTHER file in the repository registers a lobby handler (check 7).
#
# ★AND THE GATE PROVES ITSELF ON EVERY RUN
#   Before judging the tree it parses a synthetic fixture that contains a commented-out
#   registration, a multi-line raw registration and a multi-line authenticated one,
#   and it stops unless the parse reports exactly 1/1/0. A checker written by form
#   rides for free on nobody's self-check but its own; this is that self-check.
#
# USAGE
#   bash tools/check_lobby_auth_gate.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_lobby_auth_gate.sh
#
# EXIT CODES
#   0 clean · 1 the invariant is broken (offending lines are printed) · 2 the check is
#     invalid (files missing, the parser failed its own fixture, template names absent
#     from the header, or coverage floor not met — in which case "zero raw
#     registrations" would be blindness, not news)
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$ROOT/src/server/lobby/LobbyNetworkHandler.cpp"
HDR="$ROOT/include/server/lobby/LobbyNetworkHandler.hpp"

# Measured on the tree at 2026-09-02 (round R72): 36 = 33 + 3.
WANT_AUTH=33
WANT_PRE=3
MIN_CALLS=30
WANT_PREAUTH_SET="AcCmdCLCheckWaitingSeqno AcCmdCLLogin AcCmdCLQueryServerTime"
# The three files that are ALLOWED to name the registration wrappers: the lobby
# header defines them, the lobby source uses them, the command server declares the
# gated entry point they are built on.
WANT_REGISTRAR_FILES="include/libserver/network/command/CommandServer.hpp include/server/lobby/LobbyNetworkHandler.hpp src/server/lobby/LobbyNetworkHandler.cpp"

for f in "$SRC" "$HDR"; do
  [ -f "$f" ] || { echo "ОСТАНОВ: нет файла $f — считать нечего"; exit 2; }
done

command -v python3 >/dev/null 2>&1 || { echo "ОСТАНОВ: нет python3 — разбор невозможен"; exit 2; }

# ---------------------------------------------------------------------------
# The parser. Comments and string/char literals are blanked (newlines kept, so line
# numbers stay true), then the text is read as ONE stream: line breaks inside a
# registration stop being a hiding place.
PARSE_PY=$(cat <<'PY'
import re
import sys


def strip_comments_and_strings(text):
    """Blank comments and literals, KEEP newlines (line numbers must stay true)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if two == "/*":
            out.append("  ")
            i += 2
            while i < n and text[i:i + 2] != "*/":
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        if c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append(" ")
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


RAW_RE = re.compile(r"_commandServer\s*\.\s*Register[A-Za-z]*CommandHandler\s*<\s*protocol::")
AUTH_RE = re.compile(r"\bRegisterAuthenticatedHandler\s*<\s*protocol::")
PRE_RE = re.compile(r"\bRegisterPreAuthHandler\s*<\s*protocol::")
CALL_RE = re.compile(r"(?:_commandServer\s*\.\s*)?\bRegister[A-Za-z]*Handler\s*<\s*protocol::")
PRESET_RE = re.compile(r"\bRegisterPreAuthHandler\s*<\s*protocol::([A-Za-z0-9_]+)")


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def report(clean):
    raw_lines = [line_of(clean, m.start()) for m in RAW_RE.finditer(clean)]
    print("RAW=%d" % len(raw_lines))
    print("AUTH=%d" % len(AUTH_RE.findall(clean)))
    print("PRE=%d" % len(PRE_RE.findall(clean)))
    print("CALLS=%d" % len(CALL_RE.findall(clean)))
    print("PRESET=%s" % " ".join(sorted(PRESET_RE.findall(clean))))
    print("RAWLINES=%s" % " ".join(str(x) for x in raw_lines))


with open(sys.argv[1], "r", encoding="utf-8", errors="replace") as handle:
    report(strip_comments_and_strings(handle.read()))
PY
)

parse() { printf '%s' "$PARSE_PY" | python3 - "$1"; }

# ---------------------------------------------------------------------------
# (0) SELF-CHECK OF THE PARSER, on every run and before anything is judged.
PROBE="$(mktemp)"
trap 'rm -f "$PROBE"' EXIT
cat > "$PROBE" <<'FIXTURE'
// _commandServer.RegisterCommandHandler<protocol::AcCmdCLCommentedOut>(
//   [this](const ClientId clientId, const auto& command) {});
/* _commandServer.RegisterCommandHandler<protocol::AcCmdCLBlockCommented>( */
void Fixture()
{
  const char* text = "_commandServer.RegisterCommandHandler<protocol::AcCmdCLInAString>(";
  _commandServer
    .RegisterCommandHandler<protocol::AcCmdCLSplitOverLines>(
      [this](const ClientId clientId, const auto& command) {});
  RegisterAuthenticatedHandler<
    protocol::AcCmdCLAlsoSplit>(
      [this](const ClientId clientId, const auto& command) {});
}
FIXTURE

PROBE_OUT="$(parse "$PROBE")" || { echo "ОСТАНОВ: разбор фикстуры упал"; exit 2; }
eval "$(printf '%s\n' "$PROBE_OUT" | sed -n 's/^\(RAW\|AUTH\|PRE\)=/PROBE_\1=/p')"
if [ "${PROBE_RAW:-x}" != "1" ] || [ "${PROBE_AUTH:-x}" != "1" ] || [ "${PROBE_PRE:-x}" != "0" ]; then
  echo "ОСТАНОВ: разбор провалил СВОЮ фикстуру (RAW=${PROBE_RAW:-?} AUTH=${PROBE_AUTH:-?} PRE=${PROBE_PRE:-?}, ожидалось 1/1/0)."
  echo "         Он либо перестал видеть многострочную регистрацию, либо начал считать"
  echo "         закомментированную. Считать дерево таким разбором нельзя."
  exit 2
fi

# ---------------------------------------------------------------------------
# (1) The tree itself.
SRC_OUT="$(parse "$SRC")" || { echo "ОСТАНОВ: разбор $SRC упал"; exit 2; }
eval "$(printf '%s\n' "$SRC_OUT" | sed -n 's/^\(RAW\|AUTH\|PRE\|CALLS\)=/\1=/p')"
PREAUTH_SET="$(printf '%s\n' "$SRC_OUT" | sed -n 's/^PRESET=//p')"
RAWLINES="$(printf '%s\n' "$SRC_OUT" | sed -n 's/^RAWLINES=//p')"

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
HDR_CLEAN="$(printf '%s' "$PARSE_PY" | python3 -c '
import sys
src = sys.stdin.read()
namespace = {}
exec(src.split("with open(")[0], namespace)
with open(sys.argv[1], "r", encoding="utf-8", errors="replace") as handle:
    sys.stdout.write(namespace["strip_comments_and_strings"](handle.read()))
' "$HDR")" || { echo "ОСТАНОВ: разбор $HDR упал"; exit 2; }

hdr_count() { printf '%s' "$HDR_CLEAN" | grep -oF "$1" | grep -c . ; }

# ★ЗДЕСЬ ТОЛЬКО ДВА ИМЕНИ, И ЭТО НЕ НЕДОСМОТР. RegisterGatedCommandHandler
# проверяется ниже как СВОЙСТВО ДЕРЕВА (код 1 с диагнозом), а не как исправность
# счёта: обёртка, вернувшаяся на разбор-до-ворот, — настоящий дефект, и гард обязан
# назвать его дефектом, а не «проверка недействительна». Ровно та же причина, по
# которой RAW входит в контрольную сумму ниже.
for name in RegisterAuthenticatedHandler RegisterPreAuthHandler; do
  if [ "$(hdr_count "$name")" -eq 0 ]; then
    echo "ОСТАНОВ: в заголовке нет $name — имя шаблона поехало, счёт читать нельзя"
    INVALID=1
  fi
done
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
  echo "  ✗ регистрация без решения об авторизации (сырой Register*CommandHandler),"
  echo "    строки: $RAWLINES"
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

# --- (5) THE BOUNDARY. The dispatcher must not leave the class: with an accessor in
# --- place, a registration can be made from any other translation unit and every
# --- count above stays green.
HDR_GETCS="$(hdr_count 'GetCommandServer')"
echo "GetCommandServer в заголовке : $HDR_GETCS (ожидалось 0)"
if [ "$HDR_GETCS" -ne 0 ]; then
  echo "  ✗ заголовок лобби снова отдаёт CommandServer наружу. Пока это так, «сырых"
  echo "    регистраций 0 в одном .cpp» не означает ничего: регистрацию можно завести"
  echo "    из любой другой единицы трансляции."
  RC=1
fi

# --- (6) THE GATE MUST STAND BEFORE DESERIALIZATION. RegisterAuthenticatedHandler is
# --- allowed exactly one dispatcher call, and it must be the GATED one; the single
# --- plain RegisterCommandHandler in the header belongs to RegisterPreAuthHandler.
HDR_GATED="$(hdr_count '_commandServer.RegisterGatedCommandHandler<C>')"
HDR_PLAIN="$(hdr_count '_commandServer.RegisterCommandHandler<C>')"
echo "обёртки: gated=$HDR_GATED plain=$HDR_PLAIN (ожидалось 1 и 1)"
if [ "$HDR_GATED" -ne 1 ] || [ "$HDR_PLAIN" -ne 1 ]; then
  echo "  ✗ обёртки регистрации перестали быть теми, за что их принимает раунд."
  echo "    Аутентифицированная обязана идти через RegisterGatedCommandHandler (решение"
  echo "    ДО C::Read), пред-логинная — через обычный RegisterCommandHandler. Обёртка,"
  echo "    вернувшаяся на обычный вход, снова пускает незалогиненный сокет в"
  echo "    десериализацию: укороченный кадр = строка [error] на КАЖДЫЙ пакет."
  RC=1
fi

# --- (7) REPOSITORY-WIDE: nobody else registers lobby handlers. Sources are scanned
# --- stripped, so a mention inside a comment does not count as a registrar.
REGISTRAR_FILES="$(printf '%s' "$PARSE_PY" | python3 -c '
import os
import re
import sys
src = sys.stdin.read()
namespace = {}
exec(src.split("with open(")[0], namespace)
strip = namespace["strip_comments_and_strings"]
root = sys.argv[1]
names = re.compile(r"\bRegister(?:AuthenticatedHandler|PreAuthHandler|GatedCommandHandler)\b")
found = set()
for base in ("src", "include"):
    for dirpath, _, filenames in os.walk(os.path.join(root, base)):
        for name in filenames:
            if not name.endswith((".cpp", ".hpp", ".h", ".cc", ".cxx")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                if names.search(strip(handle.read())):
                    found.add(os.path.relpath(path, root))
print(" ".join(sorted(found)))
' "$ROOT")" || { echo "ОСТАНОВ: обход дерева упал"; exit 2; }

echo "файлы-регистраторы   : $REGISTRAR_FILES"
echo "ожидались            : $WANT_REGISTRAR_FILES"
if [ "$REGISTRAR_FILES" != "$WANT_REGISTRAR_FILES" ]; then
  echo "  ✗ имена обёрток регистрации встречаются НЕ ТАМ, где раунд их оставил."
  echo "    Свойство «решение об авторизации принимается ровно в одном месте» доказуемо"
  echo "    только пока список файлов известен поимённо."
  RC=1
fi

echo
if [ "$RC" -eq 0 ]; then
  echo "=== ИТОГ: ЧИСТО ✓ ==="
else
  echo "=== ИТОГ: ПРОВАЛ ✗ — регистрация лобби-хендлера без решения об авторизации ==="
fi
exit "$RC"
