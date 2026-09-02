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

CALL = "WriteFileAtomically("
SECRET_KIND = '"User file"'
# The tree is small; a floor keeps "0 violations" from meaning "I read nothing".
MIN_CALLS = int(os.environ.get("SECRET_MIN_CALLS", "18"))


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


def analyse(text: str, origin: str) -> tuple[list[tuple[str, str]], int]:
    """Return (violations, number of calls seen) for one translation unit."""
    violations: list[tuple[str, str]] = []
    seen = 0
    for match in re.finditer(re.escape(CALL), text):
        # The declaration itself is not a call.
        line_start = text.rfind("\n", 0, match.start()) + 1
        line = text[line_start : text.find("\n", match.start())]
        if "inline void" in line or "void WriteFileAtomically" in line:
            continue
        parsed = split_arguments(text, match.end() - 1)
        location = f"{origin}:{text.count(chr(10), 0, match.start()) + 1}"
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
        is_user_file = what == SECRET_KIND
        is_secret = sensitivity.endswith("FileSensitivity::Secret")
        is_public = sensitivity.endswith("FileSensitivity::Public")
        if not (is_secret or is_public):
            violations.append(
                (location, f"четвёртый аргумент не класс конфиденциальности: {sensitivity}"))
            continue
        if is_user_file and not is_secret:
            violations.append(
                (location, f'{SECRET_KIND} записывается как {sensitivity} — '
                           f"файл с хешем пароля стал бы читаемым всем"))
        if is_secret and not is_user_file:
            violations.append(
                (location, f"{what} объявлен Secret, хотя секрет несёт только "
                           f"{SECRET_KIND} — биконъюнкция нарушена в другую сторону"))
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


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()

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
        if CALL not in text:
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
