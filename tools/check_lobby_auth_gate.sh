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
# ★AND WHAT THE SECOND VERSION STILL COULD NOT SEE (Codex finding 1, iteration 3)
#   Checks (7)-(9) compared FILE SETS. Two files that must be on those lists anyway —
#   the lobby header and RanchDirector.cpp — were therefore free real estate: an inline
#   raw registrar written into the header, or an alias-qualified foreign member
#   (`using L = LobbyNetworkHandler; void L::SneakyMember()`) written into
#   RanchDirector.cpp, changed no file set, changed no 33/3 count, and all three
#   censuses stayed green. The invariant had MOVED, not closed. Sets are now gone; the
#   census counts CONTENT per file — five numbers each, pinned by name (WANT_CENSUS),
#   including how many registrations name a LOBBY-protocol command, which is what
#   catches a swap that leaves the totals untouched. Alias spellings (`using`,
#   `typedef`, `#define`) are resolved to a fixed point before the member column is
#   counted, and the member column is BOUNDED by the include census (check 11): only a
#   translation unit that sees the class definition can define its members at all.
#
# ★AND THE GATE PROVES ITSELF ON EVERY RUN
#   Before judging the tree it parses a synthetic fixture that contains a commented-out
#   registration, a multi-line raw registration and a multi-line authenticated one,
#   and it stops unless the parse reports exactly 1/1/0. A checker written by form
#   rides for free on nobody's self-check but its own; this is that self-check.
#   The repository census has its OWN fixture (check 0b) carrying exactly the two
#   evasions above — an inline raw registrar inside an allowlisted header and an
#   alias-qualified foreign member inside an allowlisted .cpp — and the gate stops
#   unless it sees both.
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
# ★ЧТО ЗАКРЫВАЕТ СЛЕДУЮЩАЯ ПЕРЕПИСЬ (находка Codex, итерация 3 — BLOCK).
#   Итерация 2 сравнивала НАБОРЫ ФАЙЛОВ: «в каких файлах вообще встречается вызов
#   Register*CommandHandler» и «в каких встречается `LobbyNetworkHandler::`». Оба
#   набора уже содержали `include/server/lobby/LobbyNetworkHandler.hpp` и
#   `src/server/ranch/RanchDirector.cpp` — а значит внутри УЖЕ РАЗРЕШЁННОГО файла
#   можно было завести и сырую регистрацию (встроенный регистратор прямо в
#   заголовке лобби), и чужого члена лобби под ПСЕВДОНИМОМ
#   (`using L = LobbyNetworkHandler; void L::SneakyMember()` в RanchDirector.cpp):
#   набор файлов не менялся, счёт 33/3 не менялся, все три переписи оставались
#   зелёными. Инвариант не был закрыт — он ПЕРЕЕХАЛ.
#   Закрывается переписью НЕ ПО НАБОРУ ФАЙЛОВ, А ПО СОДЕРЖИМОМУ КАЖДОГО ФАЙЛА:
#   для каждого файла дерева пять чисел, и все пять пиновány поимённо.
#     REG      — вызовов/упоминаний `Register*CommandHandler` (сырой вход);
#     REGP     — регистраций `Register*Handler<protocol::…>` (любых);
#     LOBBYCMD — из них тех, чья КОМАНДА принадлежит лобби-протоколу
#                (`AcCmdCL*`/`AcCmdCl*`/`AcCmdLC*`). Ловит подмену: чужой файл
#                зарегистрировал лобби-команду вместо своей — REG и REGP те же,
#                LOBBYCMD не тот;
#     WRAP     — упоминаний обёрток раунда;
#     MEM      — определений/квалификаций члена класса лобби, ★С РАЗВОРОТОМ
#                ПСЕВДОНИМОВ: `using`, `typedef` и `#define`, до неподвижной точки.
#                Голый грep `LobbyNetworkHandler::` не видел ни одного из трёх.
#   Любая из двух дыр находки меняет как минимум одно число в своей строке.
WANT_CENSUS="include/libserver/network/chatter/ChatterServer.hpp 1 0 0 0 0
include/libserver/network/command/CommandServer.hpp 2 0 0 1 0
include/server/lobby/LobbyNetworkHandler.hpp 2 0 0 3 0
src/server/chat/AllChatDirector.cpp 3 3 0 0 0
src/server/chat/PrivateChatDirector.cpp 3 3 0 0 0
src/server/lobby/LobbyNetworkHandler.cpp 0 36 36 36 68
src/server/messenger/MessengerDirector.cpp 17 17 0 0 0
src/server/race/RaceNetworkHandler.cpp 39 39 0 0 0
src/server/ranch/RanchDirector.cpp 40 40 0 0 0"
# Пол охвата переписи: файлов со строкой в переписи не меньше этого. Ноль строк —
# это «обход упал», а не «дерево чистое».
MIN_CENSUS_ROWS=8
# ★ГДЕ КЛАСС ЛОББИ ВООБЩЕ ВИДЕН. Определить член класса можно только там, где видно
# его определение, то есть только в единице трансляции, включающей заголовок лобби.
# Заголовок не включает НИ ОДИН другой заголовок (проверяется ниже), поэтому прямые
# включения — это и есть полный транзитивный список. Он и делает перепись MEM
# исчерпывающей: она считает не «по всему дереву на всякий случай», а по конечному
# пинованному множеству TU, где чужой член физически может существовать.
WANT_VISIBILITY="src/server/lobby/LobbyDirector.cpp src/server/lobby/LobbyNetworkHandler.cpp"

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

# --- (7)-(9) REPOSITORY-WIDE CENSUS, BY CONTENT AND NOT BY FILE SET. Sources are
# --- scanned stripped, so a mention inside a comment never counts as code. Each file
# --- carries five numbers (see WANT_CENSUS above for what each one is and which hole
# --- it closes); a file that scores zero on all five is not listed at all.
CENSUS_PY=$(cat <<'PY'
import os
import re
import sys

STRIP_SRC = sys.stdin.read()
_ns = {}
exec(STRIP_SRC.split("with open(")[0], _ns)
strip = _ns["strip_comments_and_strings"]

# A raw dispatcher entry point, by NAME: this is what a sneaky member would call.
REG_RE = re.compile(r"\bRegister[A-Za-z]*CommandHandler\b")
# An actual registration of a protocol command, whatever wrapper it goes through.
REGP_RE = re.compile(r"\bRegister[A-Za-z]*Handler\s*<\s*protocol::")
# ...of which the ones whose COMMAND belongs to the lobby protocol namespace.
# Ranch/race/chat use AcCmdCR*, AcCmdRC*, AcCmdUser*, RanchCommand*, ChatCmd*, so this
# separates "a lobby command was registered here" from "this file registers its own".
LOBBYCMD_RE = re.compile(
  r"\bRegister[A-Za-z]*Handler\s*<\s*protocol::AcCmd(?:C[Ll]|LC)")
WRAP_RE = re.compile(
  r"\bRegister(?:AuthenticatedHandler|PreAuthHandler|GatedCommandHandler)\b")

# ★ALIASES ARE RESOLVED, NOT ASSUMED AWAY. `void L::SneakyMember()` in a file that
# says `using L = LobbyNetworkHandler;` defines a MEMBER of the lobby class and has
# access to the private dispatcher — while the plain grep for `LobbyNetworkHandler::`
# sees nothing at all. Three spellings introduce such a name, and they chain.
ALIAS_USING_RE = re.compile(r"\busing\s+(\w+)\s*=\s*([A-Za-z_:][\w:]*)\s*;")
ALIAS_TYPEDEF_RE = re.compile(r"\btypedef\s+([A-Za-z_:][\w:]*)\s+(\w+)\s*;")
ALIAS_DEFINE_RE = re.compile(
  r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([A-Za-z_:][\w:]*)[ \t]*$", re.M)

# ★ВКЛЮЧЕНИЕ ИЩЕТСЯ ПО СЫРОМУ ТЕКСТУ, НО ПОДТВЕРЖДАЕТСЯ ПО ОЧИЩЕННОМУ. Имя файла в
# `#include "…"` — строковый литерал, и очистка его гасит: искать включение в
# очищенном тексте значило бы не найти НИ ОДНОГО и объявить «класс не виден нигде» —
# ложно-зелёный ровно того вида, ради которого весь этот гард и написан. Поэтому
# строка берётся сырой, а директивой её признаёт очищенная копия той же строки
# (закомментированное включение включением не станет).
INCLUDE_DIRECTIVE_RE = re.compile(r"^[ \t]*#[ \t]*include\b")
INCLUDE_NAME_RE = re.compile(r"LobbyNetworkHandler\.hpp")


def includes_lobby_header(raw, clean):
    raw_lines = raw.split("\n")
    clean_lines = clean.split("\n")
    for index, line in enumerate(raw_lines):
        if not INCLUDE_NAME_RE.search(line):
            continue
        if index < len(clean_lines) and INCLUDE_DIRECTIVE_RE.search(clean_lines[index]):
            return True
    return False


def lobby_names(clean):
    """Every name that denotes LobbyNetworkHandler in this file. Fixed point."""
    names = {"LobbyNetworkHandler"}
    for _ in range(8):
        grew = False
        for match in ALIAS_USING_RE.finditer(clean):
            if (match.group(2).split("::")[-1] in names
                    and match.group(1) not in names):
                names.add(match.group(1))
                grew = True
        for match in ALIAS_TYPEDEF_RE.finditer(clean):
            if (match.group(1).split("::")[-1] in names
                    and match.group(2) not in names):
                names.add(match.group(2))
                grew = True
        for match in ALIAS_DEFINE_RE.finditer(clean):
            if (match.group(2).split("::")[-1] in names
                    and match.group(1) not in names):
                names.add(match.group(1))
                grew = True
        if not grew:
            break
    return names


def census(root):
    rows = []
    includers = []
    header_includers = []
    for base in ("src", "include"):
        for dirpath, _, filenames in os.walk(os.path.join(root, base)):
            for name in filenames:
                if not name.endswith((".cpp", ".hpp", ".h", ".cc", ".cxx")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root)
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    raw = handle.read()
                clean = strip(raw)
                member = 0
                for lobby in lobby_names(clean):
                    member += len(
                      re.findall(r"\b" + re.escape(lobby) + r"\s*::", clean))
                counts = (
                  len(REG_RE.findall(clean)),
                  len(REGP_RE.findall(clean)),
                  len(LOBBYCMD_RE.findall(clean)),
                  len(WRAP_RE.findall(clean)),
                  member)
                if any(counts):
                    rows.append("%s %s" % (rel, " ".join(str(c) for c in counts)))
                if includes_lobby_header(raw, clean):
                    includers.append(rel)
                    if not rel.endswith(".cpp"):
                        header_includers.append(rel)
    return sorted(rows), sorted(includers), sorted(header_includers)


rows, includers, header_includers = census(sys.argv[1])
print("CENSUS_BEGIN")
print("\n".join(rows))
print("CENSUS_END")
print("VISIBILITY=%s" % " ".join(includers))
print("HEADER_VISIBILITY=%s" % " ".join(header_includers))
PY
)

run_census() { printf '%s' "$PARSE_PY" | python3 -c "$CENSUS_PY" "$1"; }

# --- (0b) THE CENSUS PROVES ITSELF, on a fixture holding EXACTLY the two evasions the
# --- third review named: an inline raw registrar inside an already-allowlisted header,
# --- and an alias-qualified foreign member of the lobby class inside an
# --- already-allowlisted .cpp. If the census cannot see them here, it is not allowed
# --- to say the tree is clean.
CPROBE="$(mktemp -d)"
trap 'rm -f "$PROBE"; rm -rf "$CPROBE"' EXIT
mkdir -p "$CPROBE/include/decoy" "$CPROBE/src/decoy"
cat > "$CPROBE/include/decoy/Header.hpp" <<'FIXTURE'
class Decoy
{
  void InlineRegistrar()
  {
    // An inline raw registrar inside a file the file-set checks already allow.
    _commandServer.RegisterCommandHandler<protocol::AcCmdCLSneaky>(
      [this](const ClientId clientId, const auto& command) {});
  }
};
FIXTURE
cat > "$CPROBE/src/decoy/Foreign.cpp" <<'FIXTURE'
using L = LobbyNetworkHandler;
#define LL L
void LL::SneakyMember()
{
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRanchSnack>(
    [this](const ClientId clientId, const auto& command) {});
}
FIXTURE
CPROBE_OUT="$(run_census "$CPROBE")" || { echo "ОСТАНОВ: перепись упала на своей фикстуре"; exit 2; }
CPROBE_HDR="$(printf '%s\n' "$CPROBE_OUT" | sed -n 's|^include/decoy/Header.hpp ||p')"
CPROBE_FRG="$(printf '%s\n' "$CPROBE_OUT" | sed -n 's|^src/decoy/Foreign.cpp ||p')"
if [ "$CPROBE_HDR" != "1 1 1 0 0" ] || [ "$CPROBE_FRG" != "1 1 0 0 1" ]; then
  echo "ОСТАНОВ: перепись провалила СВОЮ фикстуру."
  echo "         встроенный регистратор в заголовке: '${CPROBE_HDR:-нет строки}' (ожидалось '1 1 1 0 0')"
  echo "         чужой член под псевдонимом:         '${CPROBE_FRG:-нет строки}' (ожидалось '1 1 0 0 1')"
  echo "         Это ровно те два обхода, ради которых перепись перестала сравнивать"
  echo "         НАБОРЫ ФАЙЛОВ и стала считать содержимое. Считать дерево ею нельзя."
  exit 2
fi

# --- (7) THE TREE ITSELF, file by file, number by number.
TREE_OUT="$(run_census "$ROOT")" || { echo "ОСТАНОВ: обход дерева упал"; exit 2; }
CENSUS="$(printf '%s\n' "$TREE_OUT" | sed -n '/^CENSUS_BEGIN$/,/^CENSUS_END$/p' | sed '1d;$d')"
VISIBILITY="$(printf '%s\n' "$TREE_OUT" | sed -n 's/^VISIBILITY=//p')"
HEADER_VISIBILITY="$(printf '%s\n' "$TREE_OUT" | sed -n 's/^HEADER_VISIBILITY=//p')"
CENSUS_ROWS="$(printf '%s\n' "$CENSUS" | grep -c .)"

echo
echo "перепись по файлам (REG REGP LOBBYCMD WRAP MEM), строк: $CENSUS_ROWS (пол: >= $MIN_CENSUS_ROWS)"
printf '%s\n' "$CENSUS" | sed 's/^/  /'
if [ "$CENSUS_ROWS" -lt "$MIN_CENSUS_ROWS" ]; then
  echo "ОСТАНОВ: строк переписи $CENSUS_ROWS при поле $MIN_CENSUS_ROWS — обход поехал,"
  echo "         «нигде ничего лишнего» на пустой переписи это слепота, а не новость"
  echo "=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ==="
  exit 2
fi
if [ "$CENSUS" != "$WANT_CENSUS" ]; then
  echo "  ✗ перепись разошлась с ожидаемой. Расхождение (< дерево / > ожидание):"
  diff <(printf '%s\n' "$CENSUS") <(printf '%s\n' "$WANT_CENSUS") \
    | sed 's/^/      /' || true
  echo "    Числа пиновány поимённо ИМЕННО потому, что набора файлов мало: внутри уже"
  echo "    разрешённого файла помещаются и встроенный сырой регистратор, и чужой член"
  echo "    лобби под псевдонимом. Изменилось число — изменилось содержимое, и новая"
  echo "    точка регистрации обязана быть объявлена здесь вместе с решением об"
  echo "    авторизации."
  RC=1
fi

# --- (11) AND THE CENSUS OF MEMBERS IS BOUNDED, not hopeful. A member of the lobby
# --- class can only be defined where the class definition is VISIBLE — that is, in a
# --- translation unit that includes the lobby header. No header includes it, so the
# --- direct includers are the whole transitive list, and the MEM column above is
# --- exhaustive over a finite pinned set rather than over a regex's imagination.
echo "видят класс лобби    : $VISIBILITY"
echo "ожидались            : $WANT_VISIBILITY"
if [ "$VISIBILITY" != "$WANT_VISIBILITY" ]; then
  echo "  ✗ заголовок лобби включён не там, где раунд его оставил. Каждая такая единица"
  echo "    трансляции МОЖЕТ определить член класса и достать приватный диспетчер."
  RC=1
fi
if [ -n "$HEADER_VISIBILITY" ]; then
  echo "  ✗ заголовок лобби включён из ЗАГОЛОВКА ($HEADER_VISIBILITY) — видимость класса"
  echo "    расползается транзитивно, и список выше перестаёт быть полным."
  RC=1
fi
# --- (10) AND NOBODY IS A FRIEND. A friend declaration hands the private dispatcher
# --- to a non-member, and then check (9) is not enough either.
HDR_FRIENDS="$(printf '%s' "$HDR_CLEAN" | grep -c '\bfriend\b')"
echo "friend в заголовке   : $HDR_FRIENDS (ожидалось 0)"
if [ "$HDR_FRIENDS" -ne 0 ]; then
  echo "  ✗ заголовок лобби объявляет friend. Приватный диспетчер становится доступен"
  echo "    не-члену, и перепись членов класса (9) перестаёт быть исчерпывающей."
  RC=1
fi

echo
if [ "$RC" -eq 0 ]; then
  echo "=== ИТОГ: ЧИСТО ✓ ==="
else
  echo "=== ИТОГ: ПРОВАЛ ✗ — регистрация лобби-хендлера без решения об авторизации ==="
fi
exit "$RC"
