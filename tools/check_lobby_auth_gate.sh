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
# ★THE THREAT MODEL, STATED ONCE AND ON PURPOSE (lead directive, iteration 4)
#   This gate — and every static gate in tools/ — guards against an HONEST REGRESSION
#   by one of us: a handler registered without an authorization decision because the
#   wrapper felt inconvenient, a pre-login command quietly added, a member sliding out
#   of the class. It is NOT a defence against a committer who deliberately hides a
#   registration from a text scanner: we own this repository and read every diff, and
#   a static checker that tried to win that argument would have to become a C++
#   preprocessor and then a compiler. Where the source uses a form this gate cannot
#   read, the honest answer is therefore neither "expand it" nor "assume it is fine"
#   but REFUSE TO CERTIFY — code 2 with the file and the line named (check 0c). The
#   forms refused are exactly the ones that change SPELLING without changing meaning:
#   token pasting, a macro body that names a registration, a macro-named include, a
#   spliced include. What remains after that refusal is documented residual risk.
#
# ★AND THE PART THAT IS NOT A SNAPSHOT (check 12)
#   The per-file census is a pinned table, and a pinned table has one cure for every
#   disagreement: update the expectation. That is enough for drift and not enough for
#   a property — rewriting a row is how an invariant gets approved away. So three
#   rules stand next to the table with no expected value at all, only a prohibition:
#   a lobby command registered through the raw dispatcher entry point (anywhere), a
#   lobby command registered outside the lobby's own translation unit, and a member of
#   the lobby class defined outside it. Each of the four evasions this gate has been
#   shown is caught by at least one of the three, and none of them can be rebaselined.
#
# USAGE
#   bash tools/check_lobby_auth_gate.sh
#   ROOT=/tmp/some/other/checkout bash tools/check_lobby_auth_gate.sh
#
# EXIT CODES
#   0 clean · 1 the invariant is broken (offending lines are printed) · 2 the check is
#     invalid (files missing, the parser or the census failed its own fixture, template
#     names absent from the header, coverage floor not met, or the tree uses a
#     preprocessing form this gate cannot read — in each case "zero raw registrations"
#     would be blindness, not news)
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


def splice_lines(text):
    """Join backslash-newline line splices, KEEPING the total line count.

    ★A LINE SPLICE IS A PREPROCESSING FORM, AND EVERY REGEX BELOW READS RAW SPELLING
    (R72-fix4-1). `Register\\<newline>CommandHandler` is one token to the compiler and
    two invisible halves to a grep. The newlines that were consumed are re-emitted at
    the END of the joined logical line, so an offence still reports the line it starts
    on and every line after it keeps its true number.
    """
    lines = text.split("\n")
    out = []
    index = 0
    while index < len(lines):
        buffer = lines[index]
        eaten = 0
        while buffer.endswith("\\") and index + 1 < len(lines):
            buffer = buffer[:-1] + lines[index + 1]
            eaten += 1
            index += 1
        out.append(buffer + "\n" * eaten)
        index += 1
    return "\n".join(out)


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
    report(strip_comments_and_strings(splice_lines(handle.read())))
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
# (0c) ★UNSUPPORTED PREPROCESSING IS A HARD STOP, NOT A BLIND SPOT
#      (R72-fix4-1/2, Codex findings 1 and 2, iteration 4).
#
# Every check in this file reads the SPELLING of the source. The preprocessor can
# change the spelling without changing the meaning, and two legal forms made the whole
# gate green on a tree that had a raw lobby registration in it:
#
#   #define RAW_REGISTER _commandServer.Register##CommandHandler
#   RAW_REGISTER<protocol::AcCmdCLRequestPersonalInfo>(...);   // ← re-opens the hole
#
#   #define LH "server/lobby/LobbyNetworkHandler.hpp"
#   #include LH                                                // ← invisible includer
#
# Reproduced on a copy of this very tree: rc=0, "ЧИСТО ✓", output byte-identical to the
# clean run in the first case; the visibility line still named two files in the second.
#
# ★WHAT THIS GATE IS FOR, AND WHAT IT IS NOT FOR. It guards against an HONEST
# regression by one of us — a handler registered without an authorization decision
# because the wrapper was inconvenient. It is not, and cannot be, a defence against a
# committer deliberately hiding a registration behind the preprocessor: we own the
# repository and read every diff. The answer to an unsupported form is therefore not
# "expand it" (that would mean shipping a C++ preprocessor inside a build gate) but
# REFUSE TO CERTIFY: code 2, the file and the line named. A gate that cannot read the
# tree must say so instead of saying "clean".
#
# Line splices are the exception, because they are cheap and honest to support: they
# are JOINED before anything is counted (splice_lines above), so a registration broken
# across lines is counted like any other. A spliced #include is still refused, because
# the include census pairs raw and cleaned lines and a spliced directive is exactly
# where that pairing stops being obvious.
PP_PY=$(cat <<'PY'
import os
import re
import sys

STRIP_SRC = sys.stdin.read()
_ns = {}
exec(STRIP_SRC.split("with open(")[0], _ns)
strip = _ns["strip_comments_and_strings"]
splice = _ns["splice_lines"]

DIRECTIVE_RE = re.compile(r"^[ \t]*#[ \t]*(\w+)")
# A supported include names its file literally. The trailing comment is allowed on
# purpose: `#include <ranges> // for views::filter` is an ordinary line, and a stop
# that fired on it would be a wrong diagnosis, not a strict one.
INCLUDE_OK_RE = re.compile(
  r"^[ \t]*#[ \t]*include[ \t]*(?:\"[^\"]*\"|<[^>]*>)[ \t]*(?://.*|/\*.*)?$")
DEFINE_HEAD_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)(\([^)]*\))?")
BODY_NAMES_RE = re.compile(r"Register|Handler|Command")


def spliced_starts(text):
    """Indices of logical lines that were assembled out of two or more source lines."""
    starts = set()
    lines = text.split("\n")
    index = 0
    logical = 0
    while index < len(lines):
        eaten = 0
        while lines[index].endswith("\\") and index + 1 < len(lines):
            eaten += 1
            index += 1
        if eaten:
            starts.add(logical)
        index += 1
        logical += 1
    return starts


def scan(path):
    """Every unsupported preprocessing form in one file: (line, reason, text)."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        original = handle.read()
    joined = splice(original)
    clean = strip(joined)
    raw_lines = joined.split("\n")
    clean_lines = clean.split("\n")
    assembled = spliced_starts(original)
    stops = []
    for index, clean_line in enumerate(clean_lines):
        directive = DIRECTIVE_RE.match(clean_line)
        if directive is None:
            continue
        name = directive.group(1)
        raw_line = raw_lines[index] if index < len(raw_lines) else ""
        if name == "include":
            if index in assembled:
                stops.append((index + 1, "включение-со-склейкой", raw_line.strip()))
            elif not INCLUDE_OK_RE.match(raw_line):
                stops.append((index + 1, "включение-через-макрос", raw_line.strip()))
            continue
        if name == "define":
            head = DEFINE_HEAD_RE.match(clean_line)
            if head is None:
                continue
            body = clean_line[head.end():]
            if "##" in body:
                stops.append((index + 1, "макрос-склейка", raw_line.strip()))
            elif BODY_NAMES_RE.search(body):
                stops.append((index + 1, "макрос-регистрация", raw_line.strip()))
    return stops


def walk(root):
    found = []
    for base in ("src", "include"):
        for dirpath, _, filenames in os.walk(os.path.join(root, base)):
            for name in sorted(filenames):
                if not name.endswith((".cpp", ".hpp", ".h", ".cc", ".cxx")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root)
                for line, reason, text in scan(path):
                    found.append("%s:%d: %s: %s" % (rel, line, reason, text))
    return sorted(found)


print("PP_BEGIN")
print("\n".join(walk(sys.argv[1])))
print("PP_END")
PY
)

run_pp() { printf '%s' "$PARSE_PY" | python3 -c "$PP_PY" "$1"; }

# --- (0c-i) THE STOP PROVES ITSELF, on a fixture holding one file per rule plus a
# --- perfectly ordinary file that must NOT be flagged. A stop that fires on everything
# --- is as useless as one that fires on nothing.
PPROBE="$(mktemp -d)"
trap 'rm -f "$PROBE"; rm -rf "$PPROBE"' EXIT
mkdir -p "$PPROBE/include/decoy" "$PPROBE/src/decoy"
cat > "$PPROBE/src/decoy/Paste.cpp" <<'FIXTURE'
#define RAW_REGISTER _commandServer.Register##CommandHandler
FIXTURE
cat > "$PPROBE/src/decoy/NameOnly.cpp" <<'FIXTURE'
#define LOBBY_REG RegisterAuthenticatedHandler
FIXTURE
cat > "$PPROBE/src/decoy/MacroInclude.cpp" <<'FIXTURE'
#define LH "server/lobby/LobbyNetworkHandler.hpp"
#include LH
FIXTURE
printf '#include \\\n  "server/lobby/LobbyNetworkHandler.hpp"\n' > "$PPROBE/src/decoy/SplicedInclude.cpp"
cat > "$PPROBE/include/decoy/Clean.hpp" <<'FIXTURE'
#ifndef DECOY_CLEAN_HPP
#define DECOY_CLEAN_HPP
// #define RAW_REGISTER _commandServer.Register##CommandHandler
#include <cstdint>
#include "server/lobby/LobbyNetworkHandler.hpp"
#define DECOY_LIMIT 5
#endif
FIXTURE
WANT_PP_PROBE="src/decoy/MacroInclude.cpp:2: включение-через-макрос: #include LH
src/decoy/NameOnly.cpp:1: макрос-регистрация: #define LOBBY_REG RegisterAuthenticatedHandler
src/decoy/Paste.cpp:1: макрос-склейка: #define RAW_REGISTER _commandServer.Register##CommandHandler
src/decoy/SplicedInclude.cpp:1: включение-со-склейкой: #include   \"server/lobby/LobbyNetworkHandler.hpp\""
PPROBE_OUT="$(run_pp "$PPROBE")" || { echo "ОСТАНОВ: разбор препроцессорных форм упал на своей фикстуре"; exit 2; }
PPROBE_LIST="$(printf '%s\n' "$PPROBE_OUT" | sed -n '/^PP_BEGIN$/,/^PP_END$/p' | sed '1d;$d' | grep -v '^$' || true)"
if [ "$PPROBE_LIST" != "$WANT_PP_PROBE" ]; then
  echo "ОСТАНОВ: проверка препроцессорных форм провалила СВОЮ фикстуру."
  echo "  увидела:"
  printf '%s\n' "$PPROBE_LIST" | sed 's/^/    /'
  echo "  ожидалось:"
  printf '%s\n' "$WANT_PP_PROBE" | sed 's/^/    /'
  echo "  Либо она перестала видеть форму, которой прячут регистрацию, либо начала"
  echo "  ругаться на обычный файл. Судить дерево ею нельзя."
  exit 2
fi

# --- (0c-ii) AND NOW THE TREE.
PP_OUT="$(run_pp "$ROOT")" || { echo "ОСТАНОВ: разбор препроцессорных форм упал на дереве"; exit 2; }
PP_LIST="$(printf '%s\n' "$PP_OUT" | sed -n '/^PP_BEGIN$/,/^PP_END$/p' | sed '1d;$d' | grep -v '^$' || true)"
PP_COUNT="$(printf '%s' "$PP_LIST" | grep -c . || true)"
echo "препроцессорных форм вне поддержки: $PP_COUNT (ожидалось 0)"
if [ "$PP_COUNT" -ne 0 ]; then
  echo "ОСТАНОВ: дерево использует препроцессорную форму, которую этот гард читать не"
  echo "         умеет. Ни один счёт ниже не имеет силы: имя, собранное из ## или"
  echo "         подставленное макросом, не совпадёт ни с одним регексом, и «сырых"
  echo "         регистраций 0» будет означать «я не смотрел»."
  printf '%s\n' "$PP_LIST" | sed 's/^/  ✗ /'
  echo "         Пиши регистрацию и включение обычным образом — либо, если форма"
  echo "         действительно нужна, расширяй гард сознательно."
  echo "=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ==="
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
    sys.stdout.write(namespace["strip_comments_and_strings"](
        namespace["splice_lines"](handle.read())))
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
splice = _ns["splice_lines"]

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


class AliasNotConverged(Exception):
    """The alias closure did not reach a fixed point inside its own bound."""


def lobby_names(clean):
    """Every name that denotes LobbyNetworkHandler in this file. Fixed point.

    ★ПОТОЛОК ЗДЕСЬ ВЫВЕДЕН, А НЕ УГАДАН (R72-fix4-2, Codex finding 2, итерация 4).
    Раньше стояло `range(8)`: восемь проходов и молча наружу. Девять псевдонимов,
    записанных В ОБРАТНОМ ПОРЯДКЕ (каждый ссылается на следующий), сходятся ровно за
    девять проходов — то есть цепочка из десяти строк выводила класс лобби из-под
    колонки MEM, и гард об этом не сообщал. Каждый проход добавляет минимум одно имя,
    а имён не может быть больше, чем объявлений псевдонимов в файле; поэтому потолок
    считается ПО ФАЙЛУ, и достижение потолка без сходимости — не «хватит», а ОСТАНОВ.
    """
    names = {"LobbyNetworkHandler"}
    budget = (len(ALIAS_USING_RE.findall(clean))
              + len(ALIAS_TYPEDEF_RE.findall(clean))
              + len(ALIAS_DEFINE_RE.findall(clean)) + 2)
    for _ in range(budget):
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
            return names
    raise AliasNotConverged()


# ★СЕМАНТИЧЕСКИЕ ЗАПРЕТЫ — НЕ ТАБЛИЦА, КОТОРУЮ МОЖНО ПЕРЕСОГЛАСОВАТЬ
# (R72-fix4-3, Codex finding 3, итерация 4). Перепись выше — это СНИМОК: пять чисел на
# файл, пиноvanных поимённо. Снимок ловит любое изменение, но лечится он одинаково —
# «обнови ожидание». Приписать в заголовок лобби сырой регистратор (это ровно негатив
# neg-h) и поправить его строку с `2 0 0 3 0` на `3 1 1 3 0` — и гард снова зелёный,
# хотя решения об авторизации в дереве стало на одно меньше. Поэтому рядом со снимком
# стоят ТРИ ПРАВИЛА, у которых нет ожидаемого значения, только запрет; их нельзя
# «пересогласовать», их можно только выполнить:
#   (A) лобби-команда, зарегистрированная СЫРЫМ входом, — нигде и никогда;
#   (B) лобби-команда, зарегистрированная где-либо, кроме единственной единицы
#       трансляции лобби, — то есть решение об авторизации принято не там, где живут
#       обёртки;
#   (C) член класса лобби, определённый вне его собственной единицы трансляции, —
#       чужой файл, дотянувшийся до приватного диспетчера (в любом написании: имя
#       разворачивается по псевдонимам).
LOBBY_TU = os.path.join("src", "server", "lobby", "LobbyNetworkHandler.cpp")
RAW_LOBBY_RE = re.compile(
  r"\bRegister[A-Za-z]*CommandHandler\s*<\s*protocol::AcCmd(?:C[Ll]|LC)")


def census(root):
    rows = []
    includers = []
    header_includers = []
    violations = []
    alias_stops = []
    for base in ("src", "include"):
        for dirpath, _, filenames in os.walk(os.path.join(root, base)):
            for name in filenames:
                if not name.endswith((".cpp", ".hpp", ".h", ".cc", ".cxx")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root)
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    raw = splice(handle.read())
                clean = strip(raw)
                member = 0
                try:
                    names = lobby_names(clean)
                except AliasNotConverged:
                    alias_stops.append(rel)
                    continue
                for lobby in names:
                    member += len(
                      re.findall(r"\b" + re.escape(lobby) + r"\s*::", clean))
                raw_lobby = len(RAW_LOBBY_RE.findall(clean))
                lobby_cmd = len(LOBBYCMD_RE.findall(clean))
                counts = (
                  len(REG_RE.findall(clean)),
                  len(REGP_RE.findall(clean)),
                  lobby_cmd,
                  len(WRAP_RE.findall(clean)),
                  member)
                if any(counts):
                    rows.append("%s %s" % (rel, " ".join(str(c) for c in counts)))
                if raw_lobby:
                    violations.append(
                      "%s: A: лобби-команд, зарегистрированных сырым входом: %d"
                      % (rel, raw_lobby))
                if lobby_cmd and rel != LOBBY_TU:
                    violations.append(
                      "%s: B: лобби-команд, зарегистрированных вне единицы трансляции"
                      " лобби: %d" % (rel, lobby_cmd))
                if member and rel != LOBBY_TU:
                    violations.append(
                      "%s: C: обращений к члену класса лобби вне его единицы"
                      " трансляции: %d" % (rel, member))
                if includes_lobby_header(raw, clean):
                    includers.append(rel)
                    if not rel.endswith(".cpp"):
                        header_includers.append(rel)
    return (sorted(rows), sorted(includers), sorted(header_includers),
            sorted(violations), sorted(alias_stops))


rows, includers, header_includers, violations, alias_stops = census(sys.argv[1])
print("CENSUS_BEGIN")
print("\n".join(rows))
print("CENSUS_END")
print("VIOLATIONS_BEGIN")
print("\n".join(violations))
print("VIOLATIONS_END")
print("ALIAS_STOPS=%s" % " ".join(alias_stops))
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
trap 'rm -f "$PROBE"; rm -rf "$PPROBE" "$CPROBE"' EXIT
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
# ★ПОДМЕНА ЧЕРЕЗ ОБЁРТКУ: сырого входа нет, счёт REG нулевой, но лобби-команда
# зарегистрирована НЕ В ТОЙ единице трансляции — это правило (B), и снимок его не
# ловит, потому что у чужого файла просто появилась бы своя строка.
cat > "$CPROBE/src/decoy/Swap.cpp" <<'FIXTURE'
void SwapRegistrar()
{
  RegisterAuthenticatedHandler<protocol::AcCmdCLSwapped>(
    [this](const ClientId clientId, const auto& command) {});
}
FIXTURE
# ★ЦЕПОЧКА ПСЕВДОНИМОВ В ОБРАТНОМ ПОРЯДКЕ, ДЕВЯТЬ ЗВЕНЬЕВ. Каждый проход замыкания
# разрешает ровно одно звено, поэтому прежний потолок `range(8)` не доходил до `A1`, и
# `void A1::Sneak()` не попадал в колонку MEM: класс лобби выводился из-под переписи
# десятью строками текста. Потолок теперь выводится из числа объявлений псевдонимов в
# файле, и эта фикстура — его доказательство, а не украшение.
cat > "$CPROBE/src/decoy/Chain.cpp" <<'FIXTURE'
using A1 = A2;
using A2 = A3;
using A3 = A4;
using A4 = A5;
using A5 = A6;
using A6 = A7;
using A7 = A8;
using A8 = A9;
using A9 = LobbyNetworkHandler;
void A1::Sneak() {}
FIXTURE
WANT_CPROBE_ROWS="include/decoy/Header.hpp 1 1 1 0 0
src/decoy/Chain.cpp 0 0 0 0 1
src/decoy/Foreign.cpp 1 1 0 0 1
src/decoy/Swap.cpp 0 1 1 1 0"
WANT_CPROBE_VIOL="include/decoy/Header.hpp: A: лобби-команд, зарегистрированных сырым входом: 1
include/decoy/Header.hpp: B: лобби-команд, зарегистрированных вне единицы трансляции лобби: 1
src/decoy/Chain.cpp: C: обращений к члену класса лобби вне его единицы трансляции: 1
src/decoy/Foreign.cpp: C: обращений к члену класса лобби вне его единицы трансляции: 1
src/decoy/Swap.cpp: B: лобби-команд, зарегистрированных вне единицы трансляции лобби: 1"
CPROBE_OUT="$(run_census "$CPROBE")" || { echo "ОСТАНОВ: перепись упала на своей фикстуре"; exit 2; }
CPROBE_ROWS="$(printf '%s\n' "$CPROBE_OUT" | sed -n '/^CENSUS_BEGIN$/,/^CENSUS_END$/p' | sed '1d;$d' | grep -v '^$' || true)"
CPROBE_VIOL="$(printf '%s\n' "$CPROBE_OUT" | sed -n '/^VIOLATIONS_BEGIN$/,/^VIOLATIONS_END$/p' | sed '1d;$d' | grep -v '^$' || true)"
if [ "$CPROBE_ROWS" != "$WANT_CPROBE_ROWS" ] || [ "$CPROBE_VIOL" != "$WANT_CPROBE_VIOL" ]; then
  echo "ОСТАНОВ: перепись провалила СВОЮ фикстуру."
  echo "  строки, которые она увидела:"
  printf '%s\n' "$CPROBE_ROWS" | sed 's/^/    /'
  echo "  ожидались:"
  printf '%s\n' "$WANT_CPROBE_ROWS" | sed 's/^/    /'
  echo "  запреты, которые она назвала:"
  printf '%s\n' "$CPROBE_VIOL" | sed 's/^/    /'
  echo "  ожидались:"
  printf '%s\n' "$WANT_CPROBE_VIOL" | sed 's/^/    /'
  echo "  В фикстуре лежат ЧЕТЫРЕ обхода: встроенный регистратор в разрешённом"
  echo "  заголовке, чужой член лобби под псевдонимом, лобби-команда, зарегистрированная"
  echo "  обёрткой в чужой единице трансляции, и девятизвенная цепочка псевдонимов."
  echo "  Не видит хоть один — судить дерево ею нельзя."
  exit 2
fi

# --- (7) THE TREE ITSELF, file by file, number by number.
TREE_OUT="$(run_census "$ROOT")" || { echo "ОСТАНОВ: обход дерева упал"; exit 2; }
CENSUS="$(printf '%s\n' "$TREE_OUT" | sed -n '/^CENSUS_BEGIN$/,/^CENSUS_END$/p' | sed '1d;$d')"
VIOLATIONS="$(printf '%s\n' "$TREE_OUT" | sed -n '/^VIOLATIONS_BEGIN$/,/^VIOLATIONS_END$/p' | sed '1d;$d' | grep -v '^$' || true)"
ALIAS_STOPS="$(printf '%s\n' "$TREE_OUT" | sed -n 's/^ALIAS_STOPS=//p')"
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

# --- (12) THE RULES THAT CANNOT BE REBASELINED. The census above is a SNAPSHOT: five
# --- pinned numbers per file. It notices every change — and every change has the same
# --- cure, "update the expectation". Adding neg-h's raw registrar to the lobby header
# --- and rewriting its row from `2 0 0 3 0` to `3 1 1 3 0` makes the snapshot agree
# --- again while the tree has one authorization decision FEWER than before (Codex
# --- finding 3, iteration 4). These three rules have no expected value to update —
# --- only a prohibition — so the same edit stays red until the registration goes
# --- through a wrapper in the lobby's own translation unit.
if [ -n "$ALIAS_STOPS" ]; then
  echo "ОСТАНОВ: замыкание псевдонимов не сошлось в файлах: $ALIAS_STOPS"
  echo "         Потолок выведен из числа объявлений псевдонимов в файле, значит"
  echo "         несходимость означает поломку самого замыкания, а не глубокую цепочку."
  echo "=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ==="
  exit 2
fi
echo "запретов нарушено    : $(printf '%s' "$VIOLATIONS" | grep -c . || true) (ожидалось 0)"
if [ -n "$VIOLATIONS" ]; then
  echo "  ✗ нарушены правила, у которых нет ожидаемого значения — только запрет:"
  printf '%s\n' "$VIOLATIONS" | sed 's/^/      /'
  echo "    (A) лобби-команда, зарегистрированная сырым входом, — нигде и никогда;"
  echo "    (B) лобби-команда регистрируется только в единице трансляции лобби, где"
  echo "        живут обёртки и где принимается решение об авторизации;"
  echo "    (C) член класса лобби определяется только в его собственной единице"
  echo "        трансляции — иначе чужой файл дотянулся до приватного диспетчера."
  echo "    Эти три не лечатся правкой ожидаемой переписи: у них нет ожидания."
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
