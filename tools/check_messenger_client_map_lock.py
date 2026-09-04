#!/usr/bin/env python3
"""Гейт R78: каждое обращение к карте клиентов мессенджера, которое может
совпасть с ЧУЖИМ ПОТОКОМ, стоит под замком `_clientsMutex`.

★ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ. Ревью R78 (итерация 2, находка 2) сказало прямо:
«ни один из негативов a…j не фальсифицирует пропуски синхронизации». Это верно —
гонку данных нельзя воспроизвести на стенде детерминированно, поэтому единственная
проверка, которая УМЕЕТ ПОКРАСНЕТЬ, — структурная: перечислить обращения и
потребовать замок там, где он обязан быть. Правка без проверки, падающей на
непочиненном коде, доказательством не является.

★ЧТО ИМЕННО СЧИТАЕТСЯ ОБЯЗАННЫМ. Внешних входов в `MessengerDirector`, которые
трогают карту, ровно три (перепись `GetMessengerDirector().` по дереву):
`GetClientByCharacterUid` (зовут ранч и заезд), `SendStallionReward` (зовёт
`BreedingMarket` с потока ранча) и `IsCharacterOnline` (делегирует первому).
К ним добавлены три места, которые МЕНЯЮТ СТРУКТУРУ карты и потому опасны для
любого копирующего с чужого потока: вставка на подключении, удаление на разрыве
и фаза 1 вытеснения (её приводит сюда R78).

★ЧЕГО ГЕЙТ НЕ УТВЕРЖДАЕТ. Он НЕ говорит, что карта синхронизирована полностью:
остальные обращения — обработчики чат-протокола на СВОЁМ потоке, и главное из
них, `GetClientContext`, отдаёт ССЫЛКУ внутрь карты, которую вызывающие держат
через выходы в чужой код. Закрыть это — работа размера отдельного раунда
(для лобби ею был R64-3). Гейт стережёт ровно ту границу, которую раунд провёл.

КОДЫ ВОЗВРАТА
  0 — чисто
  1 — обращение без замка там, где он обязан быть
  2 — ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА (файл не найден, функция исчезла, обращений
      подозрительно мало) — слепота это СТОП, а не «чисто»
"""

import re
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent
SOURCE = "src/server/messenger/MessengerDirector.cpp"

#: Функции, у которых КАЖДОЕ обращение к карте обязано стоять под замком.
MUST_LOCK = {
    # читают карту с ЧУЖИХ потоков
    "GetClientByCharacterUid",
    "SendStallionReward",
    # меняют СТРУКТУРУ карты (вставка/удаление -> рехэш и инвалидация)
    "HandleClientConnected",
    "HandleClientDisconnected",
    # пишет значения чужих записей (это и приводит сюда R78)
    "EvictOtherSessionsOfCharacter",
}

#: Ниже этого числа обращений файл заведомо не тот — проверка слепа.
MIN_ACCESSES = 12

FUNC_RE = re.compile(
    r"^(?:void|bool|std::optional<[^>]*>|MessengerDirector::ClientContext&|"
    r"Config::Messenger&)\s+MessengerDirector::(\w+)")
LOCK_RE = re.compile(r"std::(?:shared_lock|unique_lock)\s+lock\(\s*"
                     r"(?:director\.)?_clientsMutex\s*\)")
ACCESS_RE = re.compile(r"(?<!_)_clients\b(?!Mutex)")


class Invalid(Exception):
    """Проверка недействительна (exit 2)."""


def _strip_comment(line: str) -> str:
    """Убрать `//`-комментарий: упоминание `_clients` в прозе — не обращение."""
    idx = line.find("//")
    return line if idx < 0 else line[:idx]


def analyse(text: str):
    """Вернуть (обращения, нарушения).

    Обращение считается защищённым, если объявление замка стоит ВЫШЕ него и в
    том же или охватывающем блоке фигурных скобок. Глубина считается по коду с
    вырезанными комментариями — иначе скобка из прозы сдвинула бы весь учёт.
    """
    lines = text.splitlines()
    func = None
    #: Глубина, НА КОТОРОЙ начиналась текущая функция. ★Ноль тут не годится:
    #: определения лежат внутри `namespace server { … }`, то есть уже на
    #: глубине 1. Первая редакция сравнивала с нулём, не находила НИ ОДНОЙ
    #: функции и печатала «0 обращений» — её остановил порог слепоты
    #: MIN_ACCESSES, а не удача.
    func_depth = 0
    depth = 0
    #: глубины, на которых открыт замок, для текущей функции
    lock_depths: list[int] = []
    accesses = []
    violations = []
    seen_functions = set()

    for number, raw in enumerate(lines, 1):
        code = _strip_comment(raw)

        match = FUNC_RE.match(raw)
        if match and func is None:
            func = match.group(1)
            func_depth = depth
            seen_functions.add(func)
            lock_depths = []

        if LOCK_RE.search(code):
            # Замок объявлен на ТЕКУЩЕЙ глубине и действует до её закрытия.
            lock_depths.append(depth)

        if ACCESS_RE.search(code) and func is not None:
            protected = bool(lock_depths)
            accesses.append((number, func, protected))
            if func in MUST_LOCK and not protected:
                violations.append((number, func, raw.strip()))

        opened = code.count("{")
        closed = code.count("}")
        depth += opened
        if closed:
            depth -= closed
            # Замки, чей блок закрылся, перестают действовать.
            lock_depths = [d for d in lock_depths if d < depth]
            if func is not None and depth <= func_depth:
                func = None
                lock_depths = []

    return accesses, violations, seen_functions


def judge(tree: Path) -> int:
    path = tree / SOURCE
    if not path.is_file():
        raise Invalid(f"нет файла {path}")
    text = path.read_text(encoding="utf-8")

    accesses, violations, seen = analyse(text)

    if len(accesses) < MIN_ACCESSES:
        raise Invalid(
            f"обращений к карте найдено {len(accesses)}, минимум {MIN_ACCESSES} — "
            "разбор не сработал, «ноль нарушений» здесь ничего не значит")
    missing = sorted(MUST_LOCK - seen)
    if missing:
        raise Invalid(
            f"функции {missing} в файле нет — гейт стерёг бы то, чего не существует")

    guarded = [a for a in accesses if a[1] in MUST_LOCK]
    print("=== gate: замок над картой клиентов мессенджера ===")
    print(f"дерево            : {tree}")
    print(f"обращений к карте : {len(accesses)} (минимум {MIN_ACCESSES})")
    print(f"обязаны быть под замком : {len(guarded)} в {len(MUST_LOCK)} функциях")
    print(f"нарушений         : {len(violations)} (ожидалось 0)")
    if violations:
        print("\nнарушители:")
        for number, func, code in violations:
            print(f"  {SOURCE}:{number}  {func}: {code[:80]}")
        print("\n=== ИТОГ: НАРУШЕНО ✗ ===")
        return 1
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


def selftest() -> int:
    """★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ: снимаем каждый замок по очереди и
    требуем, чтобы гейт покраснел ровно на нём."""
    path = TREE / SOURCE
    if not path.is_file():
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: нет {path}")
        return 2
    original = path.read_text(encoding="utf-8")

    print("=== самопроверка гейта замка карты клиентов ===")
    accesses, violations, _ = analyse(original)
    if violations:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: исходное дерево уже нарушено "
              f"({len(violations)})")
        return 2
    print(f"  исходное дерево чисто: {len(accesses)} обращений, 0 нарушений ✓")

    failures = 0
    lock_lines = [n for n, line in enumerate(original.splitlines(), 1)
                  if LOCK_RE.search(_strip_comment(line))]
    if len(lock_lines) < 4:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: объявлений замка найдено "
              f"{len(lock_lines)}, ожидалось не меньше 4")
        return 2

    for number in lock_lines:
        lines = original.splitlines()
        # Канарейка: замок снят (строка выброшена), всё остальное на месте.
        canary = "\n".join(lines[:number - 1] + lines[number:])
        _, canary_violations, _ = analyse(canary)
        if canary_violations:
            print(f"  ✓ канарейка «снят замок со строки {number}» поймана "
                  f"({len(canary_violations)} нарушений)")
        else:
            # Замок в функции, за которой гейт не следит, — это ЗАЯВЛЕННЫЙ предел.
            print(f"  · строка {number}: замок не в поднадзорной функции — "
                  "гейт молчит, как и заявлено")

    caught = 0
    for number in lock_lines:
        lines = original.splitlines()
        canary = "\n".join(lines[:number - 1] + lines[number:])
        if analyse(canary)[1]:
            caught += 1
    if caught < 4:
        print(f"САМОПРОВЕРКА ПРОВАЛЕНА: поймано {caught} канареек из "
              f"{len(lock_lines)}, ожидалось не меньше 4")
        failures += 1

    if failures:
        print("=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ ===")
        return 2
    print(f"=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ (поймано {caught} канареек) ===")
    return 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    tree = Path(sys.argv[1]) if len(sys.argv) > 1 else TREE
    try:
        return judge(tree)
    except Invalid as exc:
        print(f"=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: {exc} ===")
        return 2


if __name__ == "__main__":
    sys.exit(main())
