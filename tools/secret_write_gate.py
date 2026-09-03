#!/usr/bin/env python3
"""secret_write_gate.py — build gate (LOA-fix R73, backlog #206).

WHY THIS EXISTS
    `WriteFileAtomically` gained a required `FileSensitivity` parameter so that the
    compiler enumerates every atomic write and forces each one to answer "does this
    file carry a secret?".  The compiler can force the question to be *answered*; it
    cannot force the answer to be *true*.  This gate states the missing half as a
    property of the tree:

        the `what` literal is "User file"  <=>  the sensitivity is Secret

    A biconditional, not a count.  `== 2 Secret calls` would go green the day a third
    account writer is added with the wrong class, and red the day a legitimate one is
    added with the right class.  The property is about the KIND of the file.

    It also asserts that every call passes exactly four arguments, so a call that
    somehow compiles without the parameter — a careless merge that reintroduces a
    default value — is caught here rather than in production.

WHY IT KEYS ON THE IDENTIFIER, NOT ON THE STRING "WriteFileAtomically("
    Iteration 4 of the review showed the claimed totality was false by SYNTAX, not
    by coverage: `WriteFileAtomically` followed by a newline before `(` compiles and
    was not seen at all; `auto writer = &WriteFileAtomically;` was not seen either;
    and a label spelled `"User " "file"` is one string to the compiler but two to a
    literal comparison.  A gate that misses the forms an author would actually reach
    for is a list of shapes, not a property.

    So the unit of analysis is now every OCCURRENCE OF THE IDENTIFIER in the
    comment-stripped text:
      * identifier + optional whitespace + '('  -> analysed as a call (4 args, class)
      * the declaration itself                  -> skipped
      * anything else (address-of, alias, macro,
        a name passed as a value)               -> VIOLATION, because the gate cannot
                                                   see what class such a use passes
    Comments are stripped first: the header explains itself in prose, and prose is
    not code.  Adjacent string literals are folded, so the compiler's view of the
    label and the gate's view are the same view.

WHAT ITERATION 5 OF THE REVIEW ADDED
    Direct probes showed the gate still answering by SPELLING rather than by meaning,
    in both directions:
      * `"User\\040file"` written as Public reported ZERO violations — the same label
        to the compiler, a different byte string to a raw comparison.  Escapes are now
        DECODED (per literal, because \\x is maximal-munch), and a label carrying an
        escape the gate cannot decode is refused rather than silently read as
        "not a User file".
      * `flag ? Public : Secret` passed as Secret because the expression merely ENDS
        in `FileSensitivity::Secret`.  The fourth argument must now BE exactly one
        enumerator; a class chosen at run time is unreadable, and unreadable is a
        violation.
      * conversely, the identifier inside an ordinary string — a log line naming the
        function — was reported as a violation.  A false RED gets a gate switched
        off, so literal CONTENTS are blanked before the identifier scan (the argument
        text is still read from the untouched source, and lengths match so the two
        views share offsets).

WHAT ITERATION 6 OF THE REVIEW ADDED
    Two direct probes showed the gate still answering by SPELLING rather than by the
    meaning the compiler assigns:
      * `#define Secret Public` followed by an otherwise perfect Secret call compiles
        as Public and was reported clean — the gate read the token `Secret` in the
        source and never asked what that token MEANS after preprocessing.
      * `WriteFileAtom` + backslash + newline + `ically(...)` is ONE identifier
        (translation phase 2 splices the line before anything else looks at it) and
        was not seen at all.
    The gate now performs the compiler's own first two phases in the compiler's own
    order: line splices are joined FIRST (phase 2), comments are blanked SECOND
    (phase 3), and only then are identifiers and literals read.  Line numbers are
    carried through the splice so a report still points at the source line.
    Redefinition is refused outright rather than emulated: a `#define`/`#undef` of
    `WriteFileAtomically`, `FileSensitivity`, `Secret` or `Public`, and any `##`
    pasting that touches one of those names, is a violation by itself — the gate
    cannot know what a macro means without being a preprocessor, and "cannot know"
    must be loud, not silent.

HOW IT PROVES IT CAN FAIL
    `--selftest` runs the same analyser over in-memory fixtures: a clean pair, a
    "User file" written as Public, a non-user file written as Secret, and a
    three-argument call.  All three defects must be reported, and the clean fixture
    must not be.  A gate that has not been shown to fail is not a gate.

USAGE
    python3 tools/secret_write_gate.py
    ROOT=/tmp/checkout python3 tools/secret_write_gate.py
    python3 tools/secret_write_gate.py --selftest

EXIT CODES
    0 clean · 1 violations found (printed) · 2 the tree is not scannable / blind
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

IDENTIFIER = "WriteFileAtomically"
SECRET_KIND = "User file"
# The tree is small; a floor keeps "0 violations" from meaning "I read nothing".
MIN_CALLS = int(os.environ.get("SECRET_MIN_CALLS", "18"))


#! ИМЕНА, ПЕРЕОПРЕДЕЛЕНИЕ КОТОРЫХ ГЕЙТ ОТКАЗЫВАЕТСЯ ЧИТАТЬ (правка ревью,
#  итерация 6). `#define Secret Public` компилируется как `Public`, а гейт видел
#  бы букву `Secret`. Эмулировать препроцессор здесь значило бы написать второй
#  компилятор; честный ответ — объявить такое переопределение нарушением само по
#  себе: в этом дереве оно не нужно ни для чего.
GUARDED_NAMES = ("WriteFileAtomically", "FileSensitivity", "Secret", "Public")
MACRO_PATTERN = re.compile(
    r"^[ \t]*#[ \t]*(define|undef)[ \t]+(" + "|".join(GUARDED_NAMES) + r")\b",
    re.MULTILINE)
#! ЛЮБАЯ СКЛЕЙКА ТОКЕНОВ — НАРУШЕНИЕ, А НЕ ТОЛЬКО СКЛЕЙКА С ОХРАНЯЕМЫМ ИМЕНЕМ.
#  `Sec ## ret` не содержит ни одного охраняемого имени и даёт `Secret`; правило
#  «## рядом с охраняемым именем» такую форму не видит по построению — то есть
#  было бы списком написаний, а не свойством. Гейт читает ИДЕНТИФИКАТОРЫ;
#  склейка строит идентификатор, которого в тексте нет, и прочитать его гейт не
#  может. В продакшн-дереве сегодня НОЛЬ склеек (проверено), поэтому запрет
#  ничего не стоит: тот, кому она понадобится, обязан сперва научить гейт её
#  читать.
PASTE_PATTERN = re.compile(r"##")


def splice_line_continuations(text: str) -> tuple[str, list[int]]:
    """Выполняет ФАЗУ 2 трансляции: снимает `\\` перед переводом строки.

    ★ЗАЧЕМ (правка ревью, итерация 6). `WriteFileAtom\\<перевод строки>ically(...)`
    — для компилятора ОДИН идентификатор, а для поиска по тексту два куска,
    которых гейт не видел вовсе. Проба ревью возвращала ноль вызовов и ноль
    нарушений на файле, который пишет хеш пароля как `Public`.

    ★ПОРЯДОК ФАЗ — КОМПИЛЯТОРНЫЙ: сначала склейка (фаза 2), потом комментарии
    (фаза 3). Обратный порядок читал бы `// комментарий, заканчивающийся \\`
    иначе, чем компилятор.

    Возвращает склеенный текст и НОМЕР ИСХОДНОЙ СТРОКИ для каждого символа:
    отчёт обязан указывать на строку файла, а не на строку своего внутреннего
    представления."""
    out: list[str] = []
    lines: list[int] = []
    line = 1
    index = 0
    size = len(text)
    while index < size:
        if text[index] == "\\" and index + 1 < size and text[index + 1] == "\n":
            line += 1
            index += 2
            continue
        out.append(text[index])
        lines.append(line)
        if text[index] == "\n":
            line += 1
        index += 1
    return "".join(out), lines


#! НАЧАЛО СЫРОГО ЛИТЕРАЛА (`R"delim(` с любым допустимым префиксом).
#
#  ★ЗАЧЕМ (правка ревью, итерация 7). Сырой литерал — это форма, в которой `"`
#  и `//` НЕ являются ни кавычкой, ни комментарием. Рукописный лексер, не
#  знающий о ней, на первом же таком литерале ТЕРЯЕТ СИНХРОНИЗАЦИЮ: дальше он
#  читает код как строку, а строку как код, и любой плохой вызов ПОСЛЕ него
#  становится невидимым — при этом пол числа вызовов держится существующими
#  вызовами ДО него, то есть гейт остаётся зелёным. Проба ревью это показала.
RAW_STRING_START = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f\n]{0,16})\(')


def raw_string_span(text: str, index: int) -> tuple[int, int] | None:
    """Если по смещению `index` начинается сырой литерал — вернуть (начало
    содержимого, конец литерала). `конец == -1` означает НЕЗАКРЫТЫЙ литерал:
    такой файл гейт прочитать не может и обязан сказать это вслух."""
    if index > 0 and (text[index - 1].isalnum() or text[index - 1] == "_"):
        return None
    match = RAW_STRING_START.match(text, index)
    if match is None:
        return None
    terminator = ")" + match.group(1) + '"'
    end = text.find(terminator, match.end())
    if end < 0:
        return match.end(), -1
    return match.end(), end + len(terminator)


def unreadable_literals(text: str) -> list[int]:
    """Смещения НЕЗАКРЫТЫХ сырых литералов. Пусто — файл читаем."""
    problems: list[int] = []
    index = 0
    size = len(text)
    while index < size:
        symbol = text[index]
        pair = text[index : index + 2]
        if pair == "//":
            end = text.find("\n", index)
            index = size if end < 0 else end
            continue
        if pair == "/*":
            end = text.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        if symbol in "RuUL":
            span = raw_string_span(text, index)
            if span is not None:
                if span[1] < 0:
                    problems.append(index)
                    break
                index = span[1]
                continue
        if symbol in "\"'":
            quote = symbol
            index += 1
            while index < size:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        index += 1
    return problems


#! АЛЬТЕРНАТИВНЫЕ ТОКЕНЫ ПРЕПРОЦЕССОРА (диграфы). `%:` — это `#`, `%:%:` — это
#  `##`, и обе формы компилируются ровно как их привычные написания.
#
#  ★ЗАЧЕМ (правка ревью, итерация 7). `%:define Secret Public` — законный C++,
#  делающий `Secret` синонимом `Public`; гейт, ищущий `#define`, его не видел,
#  и вызов, НАПИСАННЫЙ как Secret, компилировался как Public при зелёном гейте.
#
#  ★ДЛИНА СОХРАНЯЕТСЯ (`%:` -> `# `), потому что смещения этого текста делятся с
#  текстом аргументов: сдвиг на один байт сделал бы отчёт указывающим не туда, а
#  разбор аргументов — читающим не то.
#
#  ★СКОБОЧНЫЕ ДИГРАФЫ (`<%`, `%>`, `<:`, `:>`) НАМЕРЕННО НЕ НОРМАЛИЗУЮТСЯ. Они
#  не могут изменить ни ИМЯ вызываемого, ни ПЕРЕДАВАЕМЫЙ перечислитель, а у `<:`
#  есть исключение лексера (`<::` не диграф), которое наивная замена прочитала бы
#  неверно — то есть завела бы ложно-красный там, где сегодня чисто.
def normalize_alternative_tokens(scan: str) -> str:
    """Приводит диграфы препроцессора к привычному написанию, сохраняя длину."""
    return scan.replace("%:%:", "##  ").replace("%:", "# ")


def strip_comments(text: str) -> str:
    """Заменяет содержимое комментариев пробелами, сохраняя длину и переводы строк.

    ★ЗАЧЕМ. Заголовок объясняет себя прозой и называет функцию по имени десятки
    раз. Ключ по ИДЕНТИФИКАТОРУ без этого шага сделал бы каждое такое упоминание
    нарушением, то есть гейт стал бы ложно-КРАСНЫМ на честном дереве — а
    ложно-красный так же вреден, как ложно-зелёный: его отключают.

    Строковые и символьные литералы НЕ трогаются: именно из них читается класс
    файла."""
    out: list[str] = []
    index = 0
    size = len(text)
    while index < size:
        symbol = text[index]
        pair = text[index : index + 2]
        if pair == "//":
            end = text.find("\n", index)
            end = size if end < 0 else end
            out.append(" " * (end - index))
            index = end
            continue
        if pair == "/*":
            end = text.find("*/", index + 2)
            end = size if end < 0 else end + 2
            out.append("".join(
                character if character == "\n" else " "
                for character in text[index:end]))
            index = end
            continue
        # ★СЫРОЙ ЛИТЕРАЛ ПЕРЕПИСЫВАЕТСЯ ЦЕЛИКОМ И НЕ РАЗБИРАЕТСЯ: внутри него
        # `//` — это два символа, а не комментарий (правка ревью, итерация 7).
        if symbol in "RuUL":
            span = raw_string_span(text, index)
            if span is not None:
                end = size if span[1] < 0 else span[1]
                out.append(text[index:end])
                index = end
                continue
        if symbol in "\"'":
            quote = symbol
            out.append(symbol)
            index += 1
            while index < size:
                if text[index] == "\\":
                    out.append(text[index : index + 2])
                    index += 2
                    continue
                out.append(text[index])
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        out.append(symbol)
        index += 1
    return "".join(out)


def blank_literal_contents(code: str) -> str:
    """Заменяет СОДЕРЖИМОЕ строковых и символьных литералов пробелами, сохраняя
    длину, кавычки и переводы строк.

    ★ЗАЧЕМ (правка ревью, итерация 5). Единица анализа — ИДЕНТИФИКАТОР, и до
    этой правки его искали в тексте вместе с литералами: обычная диагностика
    `QuietLogError("WriteFileAtomically failed for {}", path)` объявлялась
    «использованием без прямого вызова», то есть НАРУШЕНИЕМ. Ложно-красный гейт
    отключают, поэтому он так же вреден, как ложно-зелёный.

    Длина сохраняется побайтово: смещения совпадают с исходным текстом, и
    разбор аргументов идёт по НЕТРОНУТОМУ тексту — класс файла читается именно
    из литералов."""
    out: list[str] = []
    index = 0
    size = len(code)
    while index < size:
        symbol = code[index]
        # ★СОДЕРЖИМОЕ СЫРОГО ЛИТЕРАЛА ОБНУЛЯЕТСЯ ТАК ЖЕ, КАК У ОБЫЧНОГО, но его
        # границы читаются по своему правилу (правка ревью, итерация 7).
        if symbol in "RuUL":
            span = raw_string_span(code, index)
            if span is not None:
                end = size if span[1] < 0 else span[1]
                out.append("".join(
                    "\n" if character == "\n" else " "
                    for character in code[index:end]))
                index = end
                continue
        if symbol in "\"'":
            quote = symbol
            out.append(symbol)
            index += 1
            while index < size:
                if code[index] == "\\":
                    pair = code[index : index + 2]
                    out.append("".join(
                        "\n" if character == "\n" else " " for character in pair))
                    index += 2
                    continue
                if code[index] == quote:
                    out.append(quote)
                    index += 1
                    break
                out.append("\n" if code[index] == "\n" else " ")
                index += 1
            continue
        out.append(symbol)
        index += 1
    return "".join(out)


SIMPLE_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'",
    "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?",
}


def decode_escapes(raw: str) -> str | None:
    """Раскрывает C-экранирование внутри литерала. `None`, если встретилось
    что-то, чего гейт прочитать не может.

    ★ЗАЧЕМ (правка ревью, итерация 5). `"User\\040file"` — для компилятора ровно
    та же строка «User file», что и `"User file"`, а для сравнения по сырым
    байтам — другая. Проба ревью показала ноль нарушений на файле с хешем
    пароля, объявленном `Public`, ровно из-за этого. Ярлык обязан сравниваться
    в ТОМ ЖЕ виде, в каком его видит компилятор; всё, что гейт раскрыть не
    умеет, объявляется нечитаемым, а не «не User file»."""
    out: list[str] = []
    index = 0
    size = len(raw)
    while index < size:
        symbol = raw[index]
        if symbol != "\\":
            out.append(symbol)
            index += 1
            continue
        index += 1
        if index >= size:
            return None
        escape = raw[index]
        if escape in SIMPLE_ESCAPES:
            out.append(SIMPLE_ESCAPES[escape])
            index += 1
            continue
        if escape == "x":
            index += 1
            digits = ""
            while index < size and raw[index] in "0123456789abcdefABCDEF":
                digits += raw[index]
                index += 1
            if not digits:
                return None
            out.append(chr(int(digits, 16)))
            continue
        if escape in "01234567":
            digits = ""
            while index < size and len(digits) < 3 and raw[index] in "01234567":
                digits += raw[index]
                index += 1
            out.append(chr(int(digits, 8)))
            continue
        # \u, \U, \e и всё прочее — гейт не берётся это читать.
        return None
    return "".join(out)


#! Метка «этот кусок пришёл из сырого литерала»: символ, который не может
#  встретиться в исходнике (U+0000).
RAW_MARKER = "\x00"


def fold_string_literals(argument: str) -> str | None:
    """Склеивает соседние строковые литералы в один канонический литерал.

    `"User " "file"` — для компилятора ОДНА строка «User file», и биконъюнкция
    обязана видеть её так же. Возвращает `None`, если аргумент не состоит целиком
    из строковых литералов (переменная, вызов, макрос) — такой ярлык гейт
    прочитать не может и обязан это сказать, а не промолчать."""
    pieces: list[str] = []
    index = 0
    size = len(argument)
    while index < size:
        symbol = argument[index]
        if symbol.isspace():
            index += 1
            continue
        # ★СЫРОЙ ЛИТЕРАЛ — ЭТО ТОЖЕ ЯРЛЫК, И ЧИТАЕТСЯ ОН ДОСЛОВНО (правка ревью,
        # итерация 7). Без этой ветки `R"(User file)"` объявлялся бы нечитаемым
        # ярлыком, то есть гейт был бы ложно-КРАСНЫМ на законной записи.
        if symbol in "RuUL":
            span = raw_string_span(argument, index)
            if span is not None:
                if span[1] < 0:
                    return None
                # содержимое: от начала после `(` до закрывающей `)`
                closing = argument.rfind(")", span[0], span[1])
                if closing < 0:
                    return None
                pieces.append(RAW_MARKER + argument[span[0]:closing])
                index = span[1]
                continue
        if symbol != '"':
            return None
        index += 1
        current: list[str] = []
        closed = False
        while index < size:
            if argument[index] == "\\":
                current.append(argument[index : index + 2])
                index += 2
                continue
            if argument[index] == '"':
                index += 1
                closed = True
                break
            current.append(argument[index])
            index += 1
        if not closed:
            return None
        pieces.append("".join(current))
    if not pieces:
        return None
    # ★РАСКРЫТИЕ ЭКРАНИРОВАНИЯ — ПОСЛЕДНИЙ ШАГ, И ПО ЛИТЕРАЛУ ОТДЕЛЬНО: у `\x`
    # длина последовательности МАКСИМАЛЬНАЯ, поэтому склеить сперва, а раскрыть
    # потом значило бы прочитать `"User\x20" "file"` иначе, чем компилятор.
    decoded: list[str] = []
    for piece in pieces:
        # В сыром литерале экранирования нет по определению: `\n` там — два
        # символа. Раскрывать его как обычный означало бы прочитать ярлык иначе,
        # чем его читает компилятор.
        if piece.startswith(RAW_MARKER):
            decoded.append(piece[len(RAW_MARKER):])
            continue
        value = decode_escapes(piece)
        if value is None:
            return None
        decoded.append(value)
    return "".join(decoded)


def split_arguments(text: str, start: int) -> tuple[list[str], int] | None:
    """Return the argument list of the call whose '(' is at `start`, and the index
    just past its ')'. Balanced-paren extraction, aware of string and char literals,
    so `std::format("{}, {}", a, b)` is one argument and a ',' inside a literal does
    not split anything."""
    assert text[start] == "("
    depth = 0
    args: list[str] = []
    current: list[str] = []
    index = start
    in_string = False
    in_char = False
    while index < len(text):
        symbol = text[index]
        if in_string:
            if symbol == "\\":
                current.append(text[index : index + 2])
                index += 2
                continue
            if symbol == '"':
                in_string = False
        elif in_char:
            if symbol == "\\":
                current.append(text[index : index + 2])
                index += 2
                continue
            if symbol == "'":
                in_char = False
        elif symbol == '"':
            in_string = True
        elif symbol == "'":
            in_char = True
        elif symbol in "([{":
            depth += 1
            if depth == 1:
                index += 1
                continue
        elif symbol in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(current))
                return [a.strip() for a in args], index + 1
        elif symbol == "," and depth == 1:
            args.append("".join(current))
            current = []
            index += 1
            continue
        current.append(symbol)
        index += 1
    return None


IDENTIFIER_PATTERN = re.compile(r"\b" + re.escape(IDENTIFIER) + r"\b")
#! Ровно ОДНО перечислимое значение, возможно с квалификацией пространств имён.
#  Ни тернарного оператора, ни вызова, ни переменной: класс, выбираемый в
#  рантайме, гейт прочитать не может и обязан это сказать.
SENSITIVITY_PATTERN = re.compile(
    r"(?:::)?(?:[A-Za-z_]\w*::)*FileSensitivity::(Secret|Public)")
DECLARATION_PATTERN = re.compile(
    r"(?:inline\s+)?(?:void|auto)\s+" + re.escape(IDENTIFIER) + r"\s*\(")


def analyse(text: str, origin: str) -> tuple[list[tuple[str, str]], int]:
    """Return (violations, number of calls seen) for one translation unit."""
    violations: list[tuple[str, str]] = []
    seen = 0
    # ★ФАЗЫ В ПОРЯДКЕ КОМПИЛЯТОРА: склейка строк, затем комментарии, затем
    # содержимое литералов (правка ревью, итерация 6).
    spliced, source_line = splice_line_continuations(text)

    def where(offset: int) -> str:
        line = source_line[offset] if offset < len(source_line) else 0
        return f"{origin}:{line}"

    # ★НЕЗАКРЫТЫЙ СЫРОЙ ЛИТЕРАЛ — ЭТО «НЕ МОГУ ПРОЧИТАТЬ», А НЕ «ЧИСТО» (правка
    # ревью, итерация 7). Дальше по файлу лексер всё равно разъедется, и любой
    # плохой вызов после него стал бы невидимым.
    for offset in unreadable_literals(spliced):
        violations.append(
            (where(offset),
             "незакрытый сырой строковый литерал — дальше файл не читается, "
             "и «ноль нарушений» здесь ничего не значит"))

    code = strip_comments(spliced)
    # ★ИДЕНТИФИКАТОР ИЩЕТСЯ В ТЕКСТЕ БЕЗ СОДЕРЖИМОГО ЛИТЕРАЛОВ, А АРГУМЕНТЫ
    # РАЗБИРАЮТСЯ ПО ИСХОДНОМУ (правка ревью, итерация 5). Длина сохранена, то
    # есть смещения у обоих видов одни и те же.
    # ★И ДИГРАФЫ ПРИВОДЯТСЯ К ПРИВЫЧНОМУ НАПИСАНИЮ (правка ревью, итерация 7):
    # `%:define Secret Public` — это `#define`, и читаться он обязан так же.
    scan = normalize_alternative_tokens(blank_literal_contents(code))

    # ★ПЕРЕОПРЕДЕЛЕНИЕ ОХРАНЯЕМОГО ИМЕНИ — НАРУШЕНИЕ САМО ПО СЕБЕ. Гейт не
    # препроцессор и не может сказать, во что превратится `Secret` после
    # `#define Secret Public`; «не могу прочитать» обязано звучать, а не
    # молчать.
    for macro in MACRO_PATTERN.finditer(scan):
        violations.append(
            (where(macro.start()),
             f"переопределение охраняемого имени `{macro.group(2)}` "
             f"(#{macro.group(1)}) — после препроцессора класс файла означает "
             f"не то, что написано в исходнике"))
    for paste in PASTE_PATTERN.finditer(scan):
        violations.append(
            (where(paste.start()),
             "склейка токенов (##) — гейт читает идентификаторы, а склейка "
             "строит идентификатор, которого в тексте нет"))

    for match in IDENTIFIER_PATTERN.finditer(scan):
        location = where(match.start())

        # The declaration itself is not a call.
        declaration = DECLARATION_PATTERN.search(scan, max(0, match.start() - 32))
        if declaration is not None and declaration.end() >= match.end() \
                and declaration.start() <= match.start():
            continue

        # ★ИДЕНТИФИКАТОР БЕЗ СКОБКИ — ЭТО НЕ «НЕ ВЫЗОВ», А НЕПРОСМАТРИВАЕМЫЙ
        # ВЫЗОВ. `&WriteFileAtomically`, `auto writer = WriteFileAtomically;`,
        # макрос-псевдоним: класс конфиденциальности такого использования гейт
        # прочитать не может, и молчание здесь и было бы той самой дырой.
        tail = scan[match.end():]
        offset = len(tail) - len(tail.lstrip())
        if not tail[offset : offset + 1] == "(":
            violations.append(
                (location, f"{IDENTIFIER} используется без прямого вызова "
                           f"(указатель/псевдоним/макрос) — класс конфиденциальности "
                           f"такого использования не читается"))
            continue

        parsed = split_arguments(code, match.end() + offset)
        if parsed is None:
            violations.append((location, "не удалось разобрать список аргументов"))
            continue
        args, _ = parsed
        seen += 1
        if len(args) != 4:
            violations.append(
                (location, f"аргументов {len(args)}, а должно быть 4 "
                           f"(класс конфиденциальности обязателен): {args}"))
            continue
        what, sensitivity = args[2], args[3]
        label = fold_string_literals(what)
        if label is None:
            violations.append(
                (location, f"ярлык файла не читаемый строковый литерал ({what}) — "
                           f"гейт не может решить, секрет это или нет"))
            continue
        is_user_file = label == SECRET_KIND
        # ★ТОЧНОЕ ВЫРАЖЕНИЕ, А НЕ «ЗАКАНЧИВАЕТСЯ НА» (правка ревью, итерация 5).
        # `flag ? FileSensitivity::Public : FileSensitivity::Secret` кончается на
        # `::Secret` и проходил как секрет, хотя класс выбирается В РАНТАЙМЕ и
        # гейт его не знает. Проверка формы принимала выражение за значение.
        enum_match = SENSITIVITY_PATTERN.fullmatch(
            "".join(sensitivity.split()))
        is_secret = enum_match is not None and enum_match.group(1) == "Secret"
        is_public = enum_match is not None and enum_match.group(1) == "Public"
        if not (is_secret or is_public):
            violations.append(
                (location, f"четвёртый аргумент не класс конфиденциальности: {sensitivity}"))
            continue
        if is_user_file and not is_secret:
            violations.append(
                (location, f'"{SECRET_KIND}" записывается как {sensitivity} — '
                           f"файл с хешем пароля стал бы читаемым всем"))
        if is_secret and not is_user_file:
            violations.append(
                (location, f"{what} объявлен Secret, хотя секрет несёт только "
                           f'"{SECRET_KIND}" — биконъюнкция нарушена в другую сторону'))
    return violations, seen


#! ВСЕ ПРОДАКШН-КОРНИ, А НЕ ОДИН `src` (правка ревью, итерация 3).
#
#  Прежняя редакция перечисляла только `src/**/*.cpp` и `src/**/*.hpp`, а
#  объявление `WriteFileAtomically` и три четверти утилит живут в `include/`.
#  Встроенный (`inline`) писатель, добавленный в заголовок с парой
#  `"User file", FileSensitivity::Public`, компилировался бы и оставался
#  НЕВИДИМЫМ для гейта, а пол «не меньше 18 вызовов» продолжали бы держать
#  существующие вызовы из `src` — то есть гейт молчал бы не потому, что чисто, а
#  потому, что не смотрел. Заявленная тотальность («компилятор перечисляет ВСЕ
#  записи») обязана иметь тотальный обход.
#
#  `tests` намеренно НЕ входит: тест вправе написать не-User файл как `Secret`,
#  чтобы проверить сам режим, и биконъюнкция там была бы ложно-красной. Гейт
#  описывает продакшн-дерево, а не проверочные приспособления.
PRODUCTION_ROOTS = ("src", "include")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".inl")


def collect_sources(root: Path) -> tuple[list[Path], list[str]]:
    """Все исходники продакшн-корней + СПИСОК ОТКАЗОВ ОБХОДА.

    ★`rglob` глотает ошибки обхода (нечитаемый подкаталог просто не даёт
    файлов), поэтому обход идёт через `os.walk` с `onerror`: «не прочитали»
    обязано отличаться от «там пусто», иначе ноль нарушений ничего не значит."""
    errors: list[str] = []
    sources: list[Path] = []
    for name in PRODUCTION_ROOTS:
        base = root / name
        if not base.is_dir():
            errors.append(f"{base}: продакшн-корень отсутствует или не каталог")
            continue
        for directory, _subdirectories, files in os.walk(
                base, onerror=lambda error: errors.append(f"{error.filename}: {error.strerror}")):
            for file_name in files:
                if file_name.endswith(SOURCE_SUFFIXES):
                    sources.append(Path(directory) / file_name)
    return sorted(sources), errors


SELFTEST_FIXTURES = [
    ("clean", 0, '''
  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "User file", server::util::FileSensitivity::Secret);
  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Character file", server::util::FileSensitivity::Public);
'''),
    ("user file written as Public", 1, '''
  server::util::WriteFileAtomically(
    p, json.dump(2), "User file", server::util::FileSensitivity::Public);
'''),
    ("non-user file written as Secret", 1, '''
  server::util::WriteFileAtomically(
    p, json.dump(2), "Horse file", server::util::FileSensitivity::Secret);
'''),
    ("three-argument call", 1, '''
  server::util::WriteFileAtomically(p, json.dump(2), "User file");
'''),
    ("comma inside a literal is not an argument separator", 0, '''
  server::util::WriteFileAtomically(
    p, std::format("{}, {}", a, b), "Meta file", server::util::FileSensitivity::Public);
'''),
    # ★ЧЕТЫРЕ ФИКСТУРЫ ИТЕРАЦИИ 4 — по одной на каждую форму, которую прежний
    # гейт НЕ ВИДЕЛ (пробы ревью возвращали ноль замеченных вызовов и ноль
    # нарушений). Пока их не было, «тотальность» была утверждением, а не
    # свойством.
    ("call split by a newline before the parenthesis is still a call", 1, '''
  server::util::WriteFileAtomically
    (p, json.dump(2), "User file", server::util::FileSensitivity::Public);
'''),
    ("taking the address of the writer hides its class", 1, '''
  const auto writer = &server::util::WriteFileAtomically;
  writer(p, json.dump(2), "User file", server::util::FileSensitivity::Public);
'''),
    ("an alias assignment hides its class too", 1, '''
  auto WriteUserFile = server::util::WriteFileAtomically;
'''),
    ("adjacent string literals are one label to the compiler", 1, '''
  server::util::WriteFileAtomically(
    p, json.dump(2), "User " "file", server::util::FileSensitivity::Public);
'''),
    ("a non-literal label cannot be classified and is refused", 1, '''
  server::util::WriteFileAtomically(
    p, json.dump(2), what, server::util::FileSensitivity::Public);
'''),
    ("the name mentioned in a comment is prose, not a call", 0, '''
  // WriteFileAtomically is the only writer; see WriteFileAtomically above.
  /* WriteFileAtomically */
  server::util::WriteFileAtomically(
    p, json.dump(2), "User file", server::util::FileSensitivity::Secret);
'''),
    # ★ЧЕТЫРЕ ФИКСТУРЫ ИТЕРАЦИИ 5 — по одной на каждую форму, на которой пробы
    # ревью получили НЕ ТОТ вердикт: две ложно-зелёные и одна ложно-красная.
    ("an octal-escaped label is the same label to the compiler", 1, r'''
  server::util::WriteFileAtomically(
    p, json.dump(2), "User\040file", server::util::FileSensitivity::Public);
'''),
    ("a hex-escaped label is the same label to the compiler", 1, r'''
  server::util::WriteFileAtomically(
    p, json.dump(2), "User\x20" "file", server::util::FileSensitivity::Public);
'''),
    ("a runtime-chosen class is not a class the gate can read", 1, '''
  server::util::WriteFileAtomically(
    p, json.dump(2), "User file",
    flag ? server::util::FileSensitivity::Public
         : server::util::FileSensitivity::Secret);
'''),
    ("the identifier inside an ordinary string is not a use", 0, '''
  server::util::QuietLogError("WriteFileAtomically failed for {}", p);
  const char* hint = "see WriteFileAtomically(path, payload, what, class)";
  server::util::WriteFileAtomically(
    p, json.dump(2), "User file", server::util::FileSensitivity::Secret);
'''),
    # ★ДВЕ ФИКСТУРЫ ИТЕРАЦИИ 6 — ровно те две формы, на которых прямая проба
    # ревью получила «ноль нарушений» от компилирующегося как `Public` кода.
    ("redefining Secret makes the source spelling meaningless", 1, '''
#define Secret Public
  server::util::WriteFileAtomically(
    p, q, "User file", server::util::FileSensitivity::Secret);
'''),
    ("a line splice inside the identifier is still one identifier", 1,
     "\n  server::util::WriteFileAtom\\\nically(\n"
     "    p, q, \"User file\", server::util::FileSensitivity::Public);\n"),
    ("pasting tokens to build the class is unreadable", 1, '''
#define CLASS(x) server::util::FileSensitivity::Sec ## ret
'''),
    ("an ordinary line splice in an unrelated macro is not a violation", 0, '''
#define LOG_TWICE(x) \\
  do { log(x); log(x); } while (false)
  server::util::WriteFileAtomically(
    p, q, "User file", server::util::FileSensitivity::Secret);
'''),
    # ★ТРИ ФИКСТУРЫ ИТЕРАЦИИ 7 — обе формы, на которых прямая проба ревью снова
    # получила «ноль нарушений» от компилирующегося как `Public` кода, плюс
    # контроль ложно-красного на законной записи с сырым литералом.
    ("a digraph #define is a #define", 1, '''
%:define Secret Public
  server::util::WriteFileAtomically(
    p, q, "User file", server::util::FileSensitivity::Secret);
'''),
    ("a raw string carrying a quote and a comment does not desynchronise", 1, '''
  const char* sql = R"(SELECT "x" -- // not a comment)";
  server::util::WriteFileAtomically(
    p, q, "User file", server::util::FileSensitivity::Public);
'''),
    ("a raw-string label is read the way the compiler reads it", 1, '''
  server::util::WriteFileAtomically(
    p, q, R"(User file)", server::util::FileSensitivity::Public);
'''),
    ("a legitimate raw-string label as Secret is clean", 0, '''
  server::util::WriteFileAtomically(
    p, q, R"(User file)", server::util::FileSensitivity::Secret);
'''),
    ("an unterminated raw string is unreadable, not clean", 1, '''
  const char* broken = R"delim(never closed;
  server::util::WriteFileAtomically(
    p, q, "Horse file", server::util::FileSensitivity::Public);
'''),
    ("a digraph paste is a paste", 1, '''
#define CLASS(x) server::util::FileSensitivity::Sec %:%: ret
'''),
    ("the declaration itself is not a call", 0, '''
inline void WriteFileAtomically(
  const std::filesystem::path& path,
  const std::string_view payload,
  const std::string_view what,
  const FileSensitivity sensitivity)
{
}
'''),
]


def selftest() -> int:
    ok = True
    for name, expected, fixture in SELFTEST_FIXTURES:
        violations, _ = analyse(fixture, f"<fixture {name}>")
        got = len(violations)
        verdict = "ok" if got == expected else "ПРОВАЛ"
        if got != expected:
            ok = False
        print(f"  [{verdict}] {name}: нарушений {got}, ожидалось {expected}")
        for location, reason in violations:
            print(f"        {location}: {reason}")
    if not ok:
        print("=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ — гейт не доказал, что умеет падать ===")
        return 2
    print("=== ИТОГ САМОПРОВЕРКИ: гейт умеет падать ✓ ===")
    return 0


def normalize_file(path: Path) -> str:
    """Текст файла ПОСЛЕ первых фаз трансляции: склейка строк, вычистка
    комментариев, диграфы. Содержимое литералов СОХРАНЕНО.

    ★ЗАЧЕМ ОТДЕЛЬНЫЙ ВЫХОД (правка ревью, итерация 7). `no_name_regex_gate.sh`
    искал запрещённое написание ГРЕПОМ по сырому тексту, и `#include /**/ <regex>`
    или `std/**/::/**/regex` — законный C++ после вычистки комментариев — давали
    ноль совпадений. Три гейта обязаны видеть ОДИН И ТОТ ЖЕ текст, иначе «класс
    закрыт» держится ровно до первого автора, который напишет иначе."""
    text = path.read_text(encoding="utf-8", errors="replace")
    spliced, _ = splice_line_continuations(text)
    return normalize_alternative_tokens(strip_comments(spliced))


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()

    # ★РЕЖИМ «ПОКАЖИ НОРМАЛИЗОВАННЫЙ ТЕКСТ» — общий вход для остальных гейтов.
    if "--normalize" in sys.argv:
        index = sys.argv.index("--normalize")
        if index + 1 >= len(sys.argv):
            print("ОСТАНОВ: --normalize требует путь к файлу", file=sys.stderr)
            return 2
        target = Path(sys.argv[index + 1])
        try:
            sys.stdout.write(normalize_file(target))
        except OSError as error:
            print(f"ОСТАНОВ: {target}: {error}", file=sys.stderr)
            return 2
        return 0

    root = Path(os.environ.get("ROOT", Path(__file__).resolve().parent.parent))
    sources, walk_errors = collect_sources(root)
    if walk_errors:
        print(f"ОСТАНОВ: обход дерева под {root} отказал:")
        for line in walk_errors:
            print(f"         {line}")
        print("         часть дерева не прочитана — «0 нарушений» читать нельзя.")
        return 2
    if not sources:
        roots = ", ".join(f"{root}/{name}" for name in PRODUCTION_ROOTS)
        print(f"ОСТАНОВ: под {roots} не найдено ни одного исходника — считать нечего")
        return 2

    violations: list[tuple[str, str]] = []
    calls = 0
    for path in sources:
        text = path.read_text(encoding="utf-8", errors="replace")
        # ★РАННИЙ ВЫХОД СЧИТАЕТ СКЛЕЕННЫЙ ТЕКСТ, А НЕ СЫРОЙ (правка ревью,
        # итерация 6): `WriteFileAtom\<перевод строки>ically` в сыром тексте
        # идентификатора НЕ содержит, и файл не читался бы вовсе. Охраняемые
        # имена проверяются в том же тексте — переопределение обязано быть
        # видно даже в файле без единого вызова.
        spliced_text, _ = splice_line_continuations(text)
        if IDENTIFIER not in spliced_text and not any(
                name in spliced_text for name in GUARDED_NAMES[1:]):
            continue
        found, seen = analyse(text, str(path.relative_to(root)))
        violations += found
        calls += seen

    print("=== secret-write gate ===")
    print(f"дерево          : {root}")
    print(f"области         : {' '.join(PRODUCTION_ROOTS)}")
    print(f"файлов прочитано: {len(sources)}")
    print(f"вызовов найдено : {calls} (минимум {MIN_CALLS})")
    print(f"нарушений       : {len(violations)} (ожидалось 0)")

    # Blindness guard: zero violations on zero calls is not good news.
    if calls < MIN_CALLS:
        print(f"ОСТАНОВ: вызовов {calls}, ожидалось не меньше {MIN_CALLS} — "
              f"ноль нарушений на неполном обходе ничего не доказывает.")
        return 2

    if violations:
        print("\nнарушители:")
        for location, reason in violations:
            print(f"  {location}: {reason}")
        print("\n=== ИТОГ: ПРОВАЛ ✗ — класс файла и класс конфиденциальности разошлись ===")
        return 1

    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
