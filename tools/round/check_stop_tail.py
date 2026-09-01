#!/usr/bin/env python3
"""check_stop_tail.py — держит блок достижений R70 ПОСЛЕДНИМ оператором RaceInstance::Stop().

ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ
  Корректность врезки достижений (backlog #58) опирается на позиционный довод:
  `RaceInstance::Tick` глотает исключения, а `_stage = Stage::Waiting` стоит ПОСЛЕ
  `Stop()` в `TickFinishing`, поэтому бросок в середине `Stop()` заставляет позвать
  её ещё раз. Блок, стоящий ПОСЛЕДНИМ и сам не бросающий, при таком повторе
  исполняется ровно один раз. Довод верен ровно до того дня, когда кто-нибудь
  допишет ещё один оператор в конец `Stop()` — а в очереди такие раунды есть
  (R75 и R76, причём R76 планирует встать «самым последним шагом»).
  Утверждение, которое никто не проверяет, умирает молча. Здесь оно — проверка.

ЧТО ИМЕННО ПРОВЕРЯЕТСЯ
  1. В файле ровно ОДИН маркер блока (MARKER).
  2. Маркер лежит внутри тела `void RaceInstance::Stop()`.
  3. Между последней закрывающей скобкой внешнего `catch` блока и закрывающей
     скобкой самой функции нет НИ ОДНОГО оператора: только пустые строки,
     комментарии и одиночные `}`.

САМОПРОВЕРКА (гейт сперва доказывает себя)
  `--self-test` строит две фикстуры из НАСТОЯЩЕГО файла и требует, чтобы гейт на
  каждой упал:
    фикстура 1 — после блока вставлен оператор `DoSomething();`;
    фикстура 2 — маркер удалён вовсе («маркер не найден» не имеет права читаться
                 как «всё хорошо»).
  Плюс требует, чтобы на неизменённом файле гейт проходил.

КОДЫ ВОЗВРАТА
  0 — блок последний · 1 — не последний / маркера нет / скобки не сошлись
  2 — самопроверка гейта провалилась (гейту нельзя верить)
"""

import argparse
import os
import re
import sys
import tempfile

DEFAULT_PATH = "src/server/race/RaceInstance.cpp"
FUNCTION_SIGNATURE = "void RaceInstance::Stop()"
MARKER = "// === LOA (R70, backlog #58)"


def strip_comments_and_strings(line: str, in_block_comment: bool):
    """Возвращает (код без комментариев и строковых литералов, в блочном комментарии)."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                i = n
            else:
                in_block_comment = False
                i = end + 2
            continue
        two = line[i:i + 2]
        if two == "//":
            break
        if two == "/*":
            in_block_comment = True
            i += 2
            continue
        ch = line[i]
        if ch in ("'", '"'):
            quote = ch
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            # Строковый литерал в счёт скобок не входит и «кодом» не считается.
            out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block_comment


def find_function_body(lines):
    """Границы тела Stop(): (индекс строки с '{', индекс строки с закрывающей '}')."""
    start = None
    for index, line in enumerate(lines):
        if FUNCTION_SIGNATURE in line and not line.lstrip().startswith("//"):
            start = index
            break
    if start is None:
        return None, None, "сигнатура «%s» не найдена" % FUNCTION_SIGNATURE

    depth = 0
    opened = False
    in_block_comment = False
    for index in range(start, len(lines)):
        code, in_block_comment = strip_comments_and_strings(lines[index], in_block_comment)
        for ch in code:
            if ch == "{":
                depth += 1
                opened = True
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return start, index, None
    return start, None, "скобки тела Stop() не сошлись — файл разобран неверно"


def check(path):
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    problems = []
    marker_lines = [i for i, line in enumerate(lines) if MARKER in line]
    if len(marker_lines) != 1:
        problems.append(
            "маркер «%s» найден %d раз(а), ожидался ровно один"
            % (MARKER, len(marker_lines)))

    start, end, error = find_function_body(lines)
    if error:
        problems.append(error)
        return problems

    if len(marker_lines) == 1:
        marker = marker_lines[0]
        if not (start < marker < end):
            problems.append(
                "маркер стоит на строке %d, вне тела Stop() (%d..%d)"
                % (marker + 1, start + 1, end + 1))
        else:
            # Всё, что стоит между маркером и концом функции, обязано быть либо
            # телом самого блока, либо мусором из скобок/комментариев. Ищем
            # ПОСЛЕДНИЙ оператор функции: любую строку кода, лежащую после
            # закрывающей скобки внешнего catch.
            tail_problem = check_tail(lines, marker, end)
            if tail_problem:
                problems.append(tail_problem)
    return problems


def check_tail(lines, marker, end):
    """После блока в Stop() не должно остаться ни одного оператора."""
    # Идём от маркера, считая скобки; блок кончается там, где глубина
    # относительно маркера возвращается к нулю ПОСЛЕ последнего catch.
    in_block_comment = False
    depth = 0
    block_end = None
    for index in range(marker, end):
        code, in_block_comment = strip_comments_and_strings(lines[index], in_block_comment)
        for ch in code:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    block_end = index
    if block_end is None:
        return "не нашёл конца блока достижений: скобки не сошлись"

    in_block_comment = False
    # Перематываем состояние блочного комментария до block_end + 1.
    for index in range(0, block_end + 1):
        _, in_block_comment = strip_comments_and_strings(lines[index], in_block_comment)

    for index in range(block_end + 1, end):
        code, in_block_comment = strip_comments_and_strings(lines[index], in_block_comment)
        stripped = re.sub(r"[\s}]", "", code)
        if stripped:
            return (
                "после блока достижений в Stop() стоит оператор — строка %d: %s"
                % (index + 1, lines[index].strip()))
    return None


def self_test(path):
    """Гейт обязан упасть на двух фикстурах и пройти на настоящем файле."""
    with open(path, encoding="utf-8") as handle:
        original = handle.read()

    ok = True
    if check(path):
        print("САМОПРОВЕРКА: гейт не проходит на неизменённом файле — чинить код или гейт")
        ok = False
    else:
        print("САМОПРОВЕРКА: неизменённый файл — ПРОХОДИТ ✓")

    lines = original.splitlines()
    _, end, error = find_function_body(lines)
    if error:
        print("САМОПРОВЕРКА: %s" % error)
        return 2

    fixtures = []
    # Фикстура 1: оператор после блока, ПЕРЕД закрывающей скобкой функции.
    injected = lines[:end] + ["  DoSomething();"] + lines[end:]
    fixtures.append(("оператор после блока", "\n".join(injected) + "\n"))
    # Фикстура 2: маркера нет вовсе.
    fixtures.append(
        ("маркер удалён", original.replace(MARKER, "// === (маркер снят фикстурой)")))

    directory = tempfile.mkdtemp(prefix="check_stop_tail_")
    for name, content in fixtures:
        fixture_path = os.path.join(directory, "fixture.cpp")
        with open(fixture_path, "w", encoding="utf-8") as handle:
            handle.write(content)
        problems = check(fixture_path)
        if problems:
            print("САМОПРОВЕРКА: фикстура «%s» — ПАДАЕТ ✓ (%s)" % (name, problems[0]))
        else:
            print("САМОПРОВЕРКА: фикстура «%s» — НЕ УПАЛА ✗ гейт слеп" % name)
            ok = False
        os.unlink(fixture_path)
    os.rmdir(directory)
    return 0 if ok else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", default=DEFAULT_PATH)
    parser.add_argument("--self-test", action="store_true",
                        help="доказать, что гейт умеет упасть, и выйти")
    args = parser.parse_args()

    if not os.path.exists(args.path):
        print("ОСТАНОВ: файл %s не найден" % args.path)
        return 1

    if args.self_test:
        return self_test(args.path)

    problems = check(args.path)
    if problems:
        print("=== ИТОГ: ПРОВАЛ ✗ блок достижений R70 не является последним в Stop() ===")
        for problem in problems:
            print("  ✗ %s" % problem)
        return 1
    print("=== ИТОГ: блок достижений R70 — последний оператор Stop() ✓ ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
