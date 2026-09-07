#!/usr/bin/env python3
"""Гейт R82: `MessengerDirector::GetClientContext` отдаёт КОПИЮ, и НИКТО не
связывает её ссылкой. Тотальный инвариант контракта R64-3 в мессенджере.

★ЗАЧЕМ ИМЕННО ТАКАЯ ФОРМА, А НЕ «СПИСОК МЕСТ, ГДЕ НУЖЕН ЗАМОК». До R82 карту
клиентов мессенджера читали ~16 обработчиков, и каждый брал `ClientContext&` —
ссылку ВНУТРЬ карты — и держал её через выходы в чужой код (`GetCharacter`,
`GetDataDirector`, `QueueCommand`), пока чужой поток фазы 1 гашения
(`CloseSessionsOfCharacter` с потока лобби, GM-бан с потока чат-команд) правил
те же поля. Починить это списком «в каждом из шестнадцати мест скопируй скаляр
под коротким замком» нельзя: список отстаёт от кода, и семнадцатый обработчик
снова возьмёт ссылку, а никакая проверка не покраснеет
([[total-invariant-beats-list-of-sites]]). Возврат КОПИЕЙ — инвариант, который
не умеет разъехаться: он запрещает саму возможность взять ссылку.

★ЧТО ГЕЙТ УТВЕРЖДАЕТ (две вещи, обе структурно):
  1. декларация в `.hpp` — by-value: тип возврата `ClientContext`, НЕ
     `ClientContext&`;
  2. ни один вызывающий не биндит результат ссылкой: `auto&` / `const auto&` /
     `ClientContext&` перед `= GetClientContext(` — красный. Допустимо только
     `auto` / `const auto` (копия) либо использование во временном выражении.

★КЛЮЧУЕТСЯ РОВНО НА `GetClientContext(`, И НИКОГДА НА `GetClientContextLocked(`.
Матч — `GetClientContext(?!Locked)`. Внутренний путь `GetClientContextLocked`
СПЕЦИАЛЬНО возвращает `ClientContext&` (вызывающий держит замок и ему нужна
живая карта, а не снимок), и его единственный вызывающий `HandleChatterLogin`
СПЕЦИАЛЬНО биндит `auto&`/`const auto&`. Без негативного lookahead гейт
покраснел бы и на декларации-ссылке `*Locked`, и на каждой строке гарда повтора
и обеих точек записи входа — то есть был бы всегда-красным на верном дереве.

КОДЫ ВОЗВРАТА
  0 — чисто
  1 — контракт нарушен (возврат ссылкой или биндинг ссылкой)
  2 — ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА (файла нет, сайтов подозрительно мало) —
      слепота это СТОП, а не «чисто»
"""

import re
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent
HEADER = "include/server/messenger/MessengerDirector.hpp"
SOURCE = "src/server/messenger/MessengerDirector.cpp"

#: ★Негативный lookahead — вся суть ключа (см. докстринг).
CALL_RE = re.compile(r"\bGetClientContext(?!Locked)\s*\(")

#: Декларация в заголовке. Ловим ОБЕ формы, чтобы отличать их друг от друга, а
#: не «не находить» нарушение.
DECL_VALUE_RE = re.compile(
    r"\bClientContext\s+GetClientContext(?!Locked)\s*\(")
DECL_REF_RE = re.compile(
    r"\bClientContext\s*&\s*GetClientContext(?!Locked)\s*\(")

#: Связывание результата ССЫЛКОЙ. `auto&`, `const auto&`, `ClientContext&`,
#: `const MessengerDirector::ClientContext&` — любая форма с амперсандом.
REF_BIND_RE = re.compile(
    r"(?:^|[^\w])(?:const\s+)?(?:auto|[\w:]*ClientContext)\s*&\s*\w+\s*=\s*"
    r"(?:[\w:]*\.)?GetClientContext(?!Locked)\s*\(")

#: Ниже этого числа просмотренных сайтов разбор заведомо не сработал.
#: Замер на кандидате R82: 17 в `.cpp` (определение + 16 вызовов) и 1 в `.hpp`.
MIN_SITES = 16


class Invalid(Exception):
    """Проверка недействительна (exit 2)."""


def _strip(line: str) -> str:
    """Убрать `//`-комментарий и строковые литералы: в прозе врезок R82 слова
    `GetClientContext` и `ClientContext&` встречаются постоянно, и без вырезания
    гейт судил бы комментарии, а не код."""
    idx = line.find("//")
    line = line if idx < 0 else line[:idx]
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def analyse(header_text: str, source_text: str):
    """Вернуть (сайтов, нарушений)."""
    sites = 0
    violations = []

    decl_by_value = False
    for number, raw in enumerate(header_text.splitlines(), 1):
        code = _strip(raw)
        if not CALL_RE.search(code):
            continue
        sites += 1
        if DECL_REF_RE.search(code):
            violations.append(
                (HEADER, number, "декларация возвращает ССЫЛКУ", raw.strip()))
        elif DECL_VALUE_RE.search(code):
            decl_by_value = True

    if not decl_by_value:
        violations.append(
            (HEADER, 0, "by-value декларации GetClientContext в заголовке НЕТ",
             "ожидалось `ClientContext GetClientContext(`"))

    for number, raw in enumerate(source_text.splitlines(), 1):
        code = _strip(raw)
        if not CALL_RE.search(code):
            continue
        sites += 1
        if REF_BIND_RE.search(code):
            violations.append(
                (SOURCE, number, "результат связан ССЫЛКОЙ", raw.strip()))
        elif DECL_REF_RE.search(code):
            violations.append(
                (SOURCE, number, "определение возвращает ССЫЛКУ", raw.strip()))

    return sites, violations


def _read(tree: Path):
    header = tree / HEADER
    source = tree / SOURCE
    if not header.is_file():
        raise Invalid(f"нет файла {header}")
    if not source.is_file():
        raise Invalid(f"нет файла {source}")
    return (header.read_text(encoding="utf-8"),
            source.read_text(encoding="utf-8"))


def judge(tree: Path) -> int:
    header_text, source_text = _read(tree)
    sites, violations = analyse(header_text, source_text)

    if sites < MIN_SITES:
        raise Invalid(
            f"сайтов GetClientContext( найдено {sites}, минимум {MIN_SITES} — "
            "разбор не сработал, «ноль нарушений» здесь ничего не значит")

    print("=== gate: GetClientContext мессенджера отдаёт КОПИЮ ===")
    print(f"дерево                   : {tree}")
    print(f"сайтов GetClientContext( : {sites} (минимум {MIN_SITES}); "
          "GetClientContextLocked( исключён намеренно")
    print(f"нарушений                : {len(violations)} (ожидалось 0)")
    if violations:
        print("\nнарушители:")
        for where, number, what, code in violations:
            print(f"  {where}:{number}  {what}: {code[:80]}")
        print("\n=== ИТОГ: НАРУШЕНО ✗ ===")
        return 1
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


def selftest() -> int:
    """★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ: впрыскиваем каждое из двух нарушений
    по очереди и требуем красного ДО того, как гейт что-либо судит."""
    try:
        header_text, source_text = _read(TREE)
    except Invalid as exc:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: {exc}")
        return 2

    print("=== самопроверка гейта «контекст уходит копией» ===")
    sites, violations = analyse(header_text, source_text)
    if violations:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: исходное дерево уже нарушено "
              f"({len(violations)})")
        return 2
    if sites < MIN_SITES:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: сайтов {sites}, "
              f"минимум {MIN_SITES}")
        return 2
    print(f"  исходное дерево чисто: {sites} сайтов, 0 нарушений ✓")

    failures = 0

    # Канарейка 1 — декларация возвращает ссылку (форма негатива `neg-ref`).
    canary_header = DECL_VALUE_RE.sub(
        "ClientContext& GetClientContext(", header_text, count=1)
    if canary_header == header_text:
        print("  ✗ канарейку 1 не удалось впрыснуть (декларация не найдена)")
        failures += 1
    else:
        _, canary_violations = analyse(canary_header, source_text)
        if canary_violations:
            print("  ✓ канарейка «декларация возвращает ClientContext&» поймана")
        else:
            print("  ✗ канарейка «декларация возвращает ClientContext&» НЕ поймана")
            failures += 1

    # Канарейка 2 — один вызывающий связывает результат ссылкой
    # (форма негатива `neg-refbind`).
    canary_source, replaced = re.subn(
        r"const auto clientContext = GetClientContext\(",
        "const auto& clientContext = GetClientContext(",
        source_text, count=1)
    if not replaced:
        print("  ✗ канарейку 2 не удалось впрыснуть (сайта по значению нет)")
        failures += 1
    else:
        _, canary_violations = analyse(header_text, canary_source)
        if canary_violations:
            print("  ✓ канарейка «вызывающий связал результат const auto&» поймана")
        else:
            print("  ✗ канарейка «вызывающий связал результат const auto&» "
                  "НЕ поймана")
            failures += 1

    # Канарейка 3 — гейт НЕ должен краснеть на `GetClientContextLocked`:
    # ложный красный так же вреден, как пропущенный дефект.
    _, locked_violations = analyse(
        header_text,
        source_text + "\n  auto& ctx = GetClientContextLocked(clientId, false);\n")
    if locked_violations:
        print("  ✗ ЛОЖНЫЙ КРАСНЫЙ: гейт покраснел на GetClientContextLocked")
        failures += 1
    else:
        print("  ✓ GetClientContextLocked ссылкой — молчит, как и заявлено")

    if failures:
        print("=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ ===")
        return 2
    print("=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ (3 канарейки) ===")
    return 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    tree = Path(sys.argv[1]) if len(sys.argv) > 1 else TREE
    try:
        return judge(tree)
    except Invalid as exc:
        print(f"=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ✗ : {exc} ===")
        return 2


if __name__ == "__main__":
    sys.exit(main())
