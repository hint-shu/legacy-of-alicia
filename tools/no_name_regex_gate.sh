#!/usr/bin/env bash
#
# no_name_regex_gate.sh — build gate (LOA-fix R73, backlog #130-C8):
# no regular expression is compiled anywhere in the data-access layer.
#
# WHY THIS EXISTS
#   `FileDataSource::RetrieveCharacterUidByName` and `IsUserNameUnique` used to
#   build a std::regex OUT OF A NAME THAT ARRIVES ON THE WIRE (up to ~8190 bytes:
#   Stream.cpp reads to the NUL under CommandServer.cpp's MaxCommandDataSize).
#   A name like `(a{200}){200}` explodes the libstdc++ automaton at CONSTRUCTION
#   time; a name like `[a-` throws regex_error out of the handler — one [error]
#   line per packet, from six authenticated handlers. R73 removed both.
#
# WHY THE SCOPE IS A DIRECTORY, NOT A FILE
#   Keying the gate on the two functions that had the defect would be a list of
#   sites of size two: a lookup added tomorrow in a NEW file of the same layer
#   would be invisible. The property is "the data-access layer compiles no
#   regular expressions", and a new file inherits it for free.
#
# STATED RADIUS (honest, do not oversell this gate)
#   It covers src/libserver/data/ and include/libserver/data/ only. A regex built
#   from a client name under
#   src/server/** is NOT caught here. That residual class is covered for the two
#   sites this round fixes by the ladder's disappearing string and by neg-d; it
#   is not covered globally.
#
# HOW IT PROVES IT CAN FAIL
#   Before scanning the tree it greps its own pattern out of a canary file it
#   writes itself. If the pattern or grep were broken, "0 offenders" would be a
#   false green — so a canary that does not match is ОСТАНОВ, not a pass.
#
#   ★A SECOND CANARY, IN THE FORMS THE OLD GATE COULD NOT SEE (review iteration
#   7). The gate used to grep the RAW text, so `#include /**/ <regex>` and
#   `std/**/::/**/regex rg(name)` — both perfectly ordinary C++ once comments
#   are removed — produced zero hits: the class-closing gate would have accepted
#   the regression it exists to refuse. The tree is now scanned AFTER the same
#   translation phases the two Python gates apply (line splicing, comment
#   removal, digraphs — `tools/secret_write_gate.py --normalize`), and the second
#   canary is written in exactly those two forms. If normalisation is missing or
#   broken, that canary misses and the gate stops instead of printing ЧИСТО.
#
#   ★A TOKEN OF THE PROGRAM, NOT A RUN OF LETTERS IN A STRING (review iteration
#   9, finding 9). The shared normaliser deliberately KEEPS literal contents —
#   the secret gate reads a file's class out of them — and this gate greps the
#   same text, so an ordinary diagnostic `QuietLogError("std::regex failed")`
#   was an offender. A false-red gate gets switched off exactly like a false-green
#   one, so this detection now reads `--normalize-code`: the same translation
#   phases PLUS blanked literal contents. `<regex>` is not a literal and survives.
#
#   ★AND THE FORMS THAT CANNOT BE READ AT ALL ARE A HARD STOP (review iteration
#   9, finding 9; the policy is R72's, see tools/check_lobby_auth_gate.sh check
#   0c). `namespace s = std;` + `s::regex rg(name);` is exactly the regression
#   this gate refuses, and contains the string `std::regex` nowhere; so does a
#   name assembled with `##`, and so does `#include HDR`. Resolving those means
#   writing a preprocessor and an alias table — a second compiler inside a build
#   gate. The honest answer is to refuse to certify: code 2, file and line named.
#   The detector is shared with the two Python gates
#   (`tools/secret_write_gate.py --unsupported`) so the three cannot drift apart.
#
# USAGE
#   bash tools/no_name_regex_gate.sh
#   bash tools/no_name_regex_gate.sh --selftest
#   ROOT=/tmp/some/other/checkout bash tools/no_name_regex_gate.sh
#
# DEPENDENCIES
#   python3 and tools/secret_write_gate.py (the shared normaliser). Their absence
#   is ОСТАНОВ, never a silent raw-text scan.
#
# ENV
#   ROOT               default: the repository this script lives in
#   REGEX_MIN_FILES    default 14 — refuse to run against fewer files than this
#                      (today the two data directories hold 16). Raise it
#                      deliberately; never lower it to make a run go green.
#
# EXIT CODES
#   0 clean · 1 offenders found (printed) · 2 not scannable / blind
set -uo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
REGEX_MIN_FILES="${REGEX_MIN_FILES:-14}"

# ═══════════════════════════════════════════════════════════════════════════
# --selftest: ЧЕТЫРЕ ФИКСТУРЫ ЧЕРЕЗ НАСТОЯЩИЙ ВХОД (правка ревью, итерация 9).
#
# ★ЗАЧЕМ ЧЕРЕЗ ВХОД, А НЕ ЧЕРЕЗ ШАБЛОН. Канарейки ниже доказывают, что ШАБЛОН
# умеет совпадать. Они ничего не говорят о том, что делает ВЕСЬ скрипт с
# настоящим деревом: и ложный плюс на литерале, и слепота к псевдониму жили не в
# шаблоне, а в том, какой текст до шаблона доходит. Поэтому здесь строится
# настоящее дерево на диске и по нему запускается ЭТОТ ЖЕ скрипт.
if [ "${1:-}" = "--selftest" ]; then
  SELFTEST_TOOLS="$(cd "$(dirname "$SELF")" && pwd)"
  SELF_OK=0
  selftest_case() {
    # $1 имя · $2 ожидаемый код · $3 ожидаемая подстрока · $4 содержимое файла
    SANDBOX="$(mktemp -d)"
    mkdir -p "$SANDBOX/src/libserver/data" "$SANDBOX/include/libserver/data" \
             "$SANDBOX/tools"
    ln -s "$SELFTEST_TOOLS/secret_write_gate.py" "$SANDBOX/tools/secret_write_gate.py"
    printf '%s\n' "$4" > "$SANDBOX/src/libserver/data/Probe.cpp"
    OUT="$(ROOT="$SANDBOX" REGEX_MIN_FILES=1 bash "$SELF" 2>&1)"
    RC=$?
    rm -rf "$SANDBOX"
    PROBLEM=""
    [ "$RC" -eq "$2" ] || PROBLEM="код $RC, ожидался $2"
    case "$OUT" in
      *"$3"*) ;;
      *) PROBLEM="$PROBLEM${PROBLEM:+; }в выводе нет «$3»" ;;
    esac
    if [ -z "$PROBLEM" ]; then
      echo "  [ok] через вход: $1"
    else
      echo "  [ПРОВАЛ] через вход: $1 — $PROBLEM"
      printf '%s\n' "$OUT" | sed 's/^/        /'
      SELF_OK=1
    fi
  }

  echo "=== no-name-regex gate: самопроверка ==="
  selftest_case "склейка токенов собирает имя, которого в тексте нет" 2 \
    "макрос-склейка" \
    '#define JOIN(a, b) a##b
void Lookup(const std::string& name) { std::JOIN(reg, ex) rg(name); }'
  selftest_case "псевдоним самого \`std\` прячет регулярку" 2 \
    "псевдоним-std" \
    'namespace s = std;
void Lookup(const s::string& name) { s::regex rg(name); }'
  selftest_case "включение, названное макросом" 2 \
    "включение-через-макрос" \
    '#define HDR <regex>
#include HDR'
  selftest_case "\`std::regex\` ВНУТРИ ЛИТЕРАЛА — не программа, а проза" 0 \
    "ЧИСТО" \
    '#include <string>
// std::regex здесь только в комментарии
void Lookup(const std::string& name)
{
  QuietLogError("std::regex compilation failed for {}", name);
  const char* note = R"(std::regex is banned in this layer)";
  const char letter = '"'"'"'"'"';
}'
  selftest_case "настоящая регулярка по-прежнему ловится" 1 \
    "ПРОВАЛ" \
    '#include <regex>
void Lookup(const std::string& name) { const std::regex rg(name); }'

  if [ "$SELF_OK" -ne 0 ]; then
    echo "=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ — гейт не доказал, что умеет падать ==="
    exit 2
  fi
  echo "=== ИТОГ САМОПРОВЕРКИ: гейт умеет падать ✓ ==="
  exit 0
fi
SCOPE="src/libserver/data include/libserver/data"
# ★ПРОБЕЛЫ ДОПУСКАЮТСЯ ВЕЗДЕ, ГДЕ ИХ ДОПУСКАЕТ КОМПИЛЯТОР (правка ревью,
# итерация 7). Текст сюда приходит НОРМАЛИЗОВАННЫМ (склейка строк + вычистка
# комментариев + диграфы, `tools/secret_write_gate.py --normalize`), а
# комментарий между токенами оставляет после себя пробелы: `std/**/::/**/regex`
# становится `std    ::    regex`. Прежний шаблон, писанный без пробелов, на
# такой — совершенно законной — записи давал НОЛЬ совпадений.
PATTERN='std[[:space:]]*::[[:space:]]*(basic_)?regex|#[[:space:]]*include[[:space:]]*<[[:space:]]*regex[[:space:]]*>'
NORMALIZER="$ROOT/tools/secret_write_gate.py"

for d in $SCOPE; do
  [ -d "$ROOT/$d" ] || { echo "ОСТАНОВ: нет каталога $ROOT/$d — считать нечего"; exit 2; }
done

# Blindness guard #1: the pattern must be able to match something.
# ★СОЗДАНИЕ И ЗАПИСЬ КАНАРЕЙКИ ПРОВЕРЯЮТСЯ (правка ревью, итерация 1). Прежняя
# редакция брала `$(mktemp)` без проверки: при недоступном каталоге временных
# файлов CANARY оставался пустым, `grep` не давал числа, числовое сравнение
# падало с ошибкой — и скрипт, не имея `set -e`, шёл дальше и печатал «ЧИСТО ✓».
# Проверка, которая сама умеет молча ослепнуть, хуже отсутствующей.
CANARY="$(mktemp 2>/dev/null)" || CANARY=""
if [ -z "$CANARY" ] || [ ! -f "$CANARY" ]; then
  echo "ОСТАНОВ: не удалось создать канареечный файл (mktemp) — проверить нечем,"
  echo "         а «ноль нарушителей» без канарейки читать нельзя."
  exit 2
fi
trap 'rm -f "$CANARY"' EXIT
if ! {
  echo '#include <regex>'
  echo 'const std::regex rg(name);'
  echo 'std::basic_regex<char> other(name);'
} > "$CANARY"; then
  echo "ОСТАНОВ: не удалось записать канареечный файл '$CANARY'."
  exit 2
fi
CANARY_HITS="$(grep -acE "$PATTERN" "$CANARY" || true)"
case "$CANARY_HITS" in
  ''|*[!0-9]*)
    echo "ОСТАНОВ: канарейка вернула не число ('$CANARY_HITS') — grep не отработал."
    exit 2
    ;;
esac
if [ "$CANARY_HITS" -lt 3 ]; then
  echo "ОСТАНОВ: канарейка дала $CANARY_HITS совпадений из 3 — регекс или grep сломаны,"
  echo "         ноль нарушителей читать нельзя."
  exit 2
fi

# Blindness guard #1b: THE NORMALIZER MUST BE THERE AND MUST WORK (правка ревью,
# итерация 7). Ищем мы теперь не по сырому тексту, а по нормализованному; если
# нормализатор отсутствует или падает, «ноль нарушителей» означает «мы ничего не
# прочитали». Канарейка написана в тех самых формах, на которых прежний гейт
# давал ноль: комментарий между токенами.
command -v python3 >/dev/null 2>&1 || {
  echo "ОСТАНОВ: нет python3 — нормализовать текст нечем, а грепать сырой текст"
  echo "         значит снова не видеть \`std/**/::/**/regex\`."
  exit 2
}
[ -f "$NORMALIZER" ] || {
  echo "ОСТАНОВ: нет '$NORMALIZER' — нормализовать текст нечем."
  exit 2
}
CANARY2="$(mktemp 2>/dev/null)" || CANARY2=""
if [ -z "$CANARY2" ] || [ ! -f "$CANARY2" ]; then
  echo "ОСТАНОВ: не удалось создать вторую канарейку (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$CANARY2"' EXIT
if ! {
  echo '#include /**/ <regex>'
  echo 'std/**/::/**/regex rg(name);'
} > "$CANARY2"; then
  echo "ОСТАНОВ: не удалось записать вторую канарейку '$CANARY2'."
  exit 2
fi
CANARY2_HITS="$(python3 "$NORMALIZER" --normalize-code "$CANARY2" | grep -cE "$PATTERN" || true)"
case "$CANARY2_HITS" in
  ''|*[!0-9]*)
    echo "ОСТАНОВ: нормализованная канарейка вернула не число ('$CANARY2_HITS')."
    exit 2
    ;;
esac
if [ "$CANARY2_HITS" -lt 2 ]; then
  echo "ОСТАНОВ: нормализованная канарейка дала $CANARY2_HITS совпадений из 2 —"
  echo "         нормализатор или шаблон сломаны, ноль нарушителей читать нельзя."
  exit 2
fi

# Blindness guard #1c: ★И ОБРАТНАЯ КАНАРЕЙКА — ТЕКСТ В ЛИТЕРАЛЕ НЕ ЕСТЬ ПРОГРАММА
# (правка ревью, итерация 9, находка 9). Общий нормализатор НАМЕРЕННО сохраняет
# содержимое литералов: секретный гейт читает из них класс файла. Этот гейт
# грепал тот же текст, и обычная диагностика `QuietLogError("std::regex ...")`
# была НАРУШИТЕЛЕМ — ложно-красный гейт отключают ровно так же, как ложно-зелёный.
# Поэтому детекция читает `--normalize-code` (те же фазы + обнулённое содержимое
# литералов), а эта канарейка обязана дать НОЛЬ: если обнуление отвалится, гейт
# остановится здесь, а не начнёт ругаться на честное дерево.
CANARY3="$(mktemp 2>/dev/null)" || CANARY3=""
if [ -z "$CANARY3" ] || [ ! -f "$CANARY3" ]; then
  echo "ОСТАНОВ: не удалось создать третью канарейку (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$CANARY2" "$CANARY3"' EXIT
if ! {
  echo 'QuietLogError("std::regex compilation failed for {}", name);'
  echo 'const char* note = R"(std::regex is banned in this layer)";'
} > "$CANARY3"; then
  echo "ОСТАНОВ: не удалось записать третью канарейку '$CANARY3'."
  exit 2
fi
CANARY3_HITS="$(python3 "$NORMALIZER" --normalize-code "$CANARY3" | grep -cE "$PATTERN" || true)"
case "$CANARY3_HITS" in
  ''|*[!0-9]*)
    echo "ОСТАНОВ: обратная канарейка вернула не число ('$CANARY3_HITS')."
    exit 2
    ;;
esac
if [ "$CANARY3_HITS" -ne 0 ]; then
  echo "ОСТАНОВ: обратная канарейка дала $CANARY3_HITS совпадений вместо 0 — текст"
  echo "         ВНУТРИ строкового литерала читается как программа, то есть гейт"
  echo "         ложно-красный. Судить дерево им нельзя."
  exit 2
fi

# Blindness guard #2: how many files exist vs how many grep actually opened.
#
# ★ОШИБКИ ОБХОДА ЛОВЯТСЯ, А НЕ ГЛУШАТСЯ (правка ревью, итерация 2). Прежняя
# редакция брала `find ... | wc -l` и `grep ... 2>/dev/null`: нечитаемый подкаталог
# выпадал ИЗ ОБОИХ чисел разом, равенство «просканировано == найдено» продолжало
# держаться, и при достаточном остатке читаемых файлов гейт печатал «ЧИСТО ✓» —
# хотя нарушитель мог лежать ровно в том подкаталоге, куда мы не заглянули.
# Симметричная слепота не видна по числам ПО ПОСТРОЕНИЮ, поэтому её ловит не
# сверка счётчиков, а код возврата и непустой stderr обхода.
#
# ★КОД ВОЗВРАТА ЧИТАЕТСЯ БЕЗ ТРУБЫ. `find ... | wc -l` отдал бы код `wc`, то есть
# всегда ноль: труба глотает статус того, кто нас интересует.
SCAN_ERR="$(mktemp 2>/dev/null)" || SCAN_ERR=""
if [ -z "$SCAN_ERR" ] || [ ! -f "$SCAN_ERR" ]; then
  echo "ОСТАНОВ: не удалось создать файл для ошибок обхода (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$CANARY2" "$CANARY3" "$SCAN_ERR"' EXIT

FILE_LIST="$(cd "$ROOT" && find $SCOPE -type f 2>"$SCAN_ERR")"
FIND_RC=$?
if [ "$FIND_RC" -ne 0 ] || [ -s "$SCAN_ERR" ]; then
  echo "ОСТАНОВ: обход дерева (find) завершился с кодом $FIND_RC и сообщениями:"
  sed 's/^/         /' "$SCAN_ERR"
  echo "         часть дерева недоступна — «ноль нарушителей» читать нельзя."
  exit 2
fi
FOUND="$(printf '%s\n' "$FILE_LIST" | grep -c . || true)"

if [ "$FOUND" -lt "$REGEX_MIN_FILES" ]; then
  echo "ОСТАНОВ: под «$SCOPE» найдено $FOUND файлов, ожидалось не меньше $REGEX_MIN_FILES"
  echo "         ноль нарушителей на неполном дереве — это не «чисто», это слепота."
  exit 2
fi

# ★СКАНИРУЕТСЯ НОРМАЛИЗОВАННЫЙ ТЕКСТ, ФАЙЛ ЗА ФАЙЛОМ (правка ревью, итерация 7).
# `grep -r` по сырому дереву не видел ни `#include /**/ <regex>`, ни
# `std/**/::/**/regex` — обе формы компилируются и обе давали ноль. Каждый файл
# прогоняется через те же фазы трансляции, что видят два питоновских гейта;
# отказ нормализатора на ЛЮБОМ файле — ОСТАНОВ, а не пропуск.
SCANNED=0
OFFENDERS=""
UNREADABLE=""
NORMALIZED="$(mktemp 2>/dev/null)" || NORMALIZED=""
if [ -z "$NORMALIZED" ] || [ ! -f "$NORMALIZED" ]; then
  echo "ОСТАНОВ: не удалось создать файл для нормализованного текста (mktemp)."
  exit 2
fi
trap 'rm -f "$CANARY" "$CANARY2" "$CANARY3" "$SCAN_ERR" "$NORMALIZED"' EXIT

while IFS= read -r RELATIVE; do
  [ -n "$RELATIVE" ] || continue
  # ★СНАЧАЛА — ЧИТАЕТСЯ ЛИ ФАЙЛ ВООБЩЕ (правка ревью, итерация 9, находка 9).
  # `namespace s = std;` + `s::regex rg(name);` — ровно та регрессия, ради которой
  # гейт существует, и слова `std::regex` в ней нет. Детектор общий с двумя
  # питоновскими гейтами, чтобы три останова не разъехались молча.
  FORMS="$(python3 "$NORMALIZER" --unsupported "$ROOT/$RELATIVE" 2>"$SCAN_ERR")"
  if [ $? -ne 0 ]; then
    echo "ОСТАНОВ: проверка препроцессорных форм в '$RELATIVE' не удалась:"
    sed 's/^/         /' "$SCAN_ERR"
    exit 2
  fi
  if [ -n "$FORMS" ]; then
    UNREADABLE="$UNREADABLE$FORMS
"
  fi
  if ! python3 "$NORMALIZER" --normalize-code "$ROOT/$RELATIVE" > "$NORMALIZED" 2>"$SCAN_ERR"; then
    echo "ОСТАНОВ: нормализация '$RELATIVE' не удалась:"
    sed 's/^/         /' "$SCAN_ERR"
    echo "         непрочитанный файл делает «ноль нарушителей» бессмысленным."
    exit 2
  fi
  SCANNED=$((SCANNED + 1))
  HITS="$(grep -nE "$PATTERN" "$NORMALIZED" || true)"
  if [ -n "$HITS" ]; then
    OFFENDERS="$OFFENDERS$(printf '%s\n' "$HITS" | sed "s|^|$RELATIVE:|")
"
  fi
done <<EOF
$FILE_LIST
EOF

if [ "$SCANNED" -ne "$FOUND" ]; then
  echo "ОСТАНОВ: нормализовано $SCANNED файлов из $FOUND — часть дерева не просканирована."
  exit 2
fi

# ★НЕПРОЧИТЫВАЕМАЯ ФОРМА — ОСТАНОВ, А НЕ СЛЕПОЕ ПЯТНО. Ни один счёт ниже не
# имеет силы: имя, собранное из `##`, и `std`, переименованный псевдонимом, не
# совпадут с шаблоном по построению, и «регулярок 0» будет означать «я не
# смотрел». Код 2 и названная строка — политика гардов R72.
if [ -n "$UNREADABLE" ]; then
  UNREADABLE_COUNT="$(printf '%s\n' "$UNREADABLE" | grep -c . || true)"
  echo "=== no-name-regex gate ==="
  echo "дерево          : $ROOT"
  echo "просканировано  : $SCANNED файлов"
  echo "форм вне чтения : $UNREADABLE_COUNT (ожидалось 0)"
  echo "ОСТАНОВ: дерево использует форму, которую этот гейт читать не умеет."
  printf '%s\n' "$UNREADABLE" | grep . | sed 's/^/  ✗ /'
  echo "         Пиши обычным образом либо расширяй гейт сознательно."
  echo "=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ==="
  exit 2
fi

if [ -z "$OFFENDERS" ]; then
  COUNT=0
else
  COUNT="$(printf '%s\n' "$OFFENDERS" | grep -c .)"
fi

echo "=== no-name-regex gate ==="
echo "дерево          : $ROOT"
echo "область         : $SCOPE"
echo "                  (радиус заявлен в шапке — src/server/** сюда НЕ входит)"
echo "просканировано  : $SCANNED файлов из $FOUND найденных (минимум $REGEX_MIN_FILES)"
echo "регулярок       : $COUNT (ожидалось 0)"

if [ "$COUNT" -ne 0 ]; then
  echo
  echo "нарушители:"
  printf '%s\n' "$OFFENDERS" | sed 's/^/  /'
  echo
  echo "=== ИТОГ: ПРОВАЛ ✗ — имя с провода не имеет права становиться программой ==="
  exit 1
fi

echo "=== ИТОГ: ЧИСТО ✓ ==="
exit 0
