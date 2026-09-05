#!/usr/bin/env python3
"""Гейт R78 (ревю #4, BLOCK): под замком карты клиентов мессенджера НЕТ НИ
ОДНОГО ВЫХОДА, который сам берёт этот же замок или синхронно рвёт соединение.

★ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ. Ревю #4 нашло ровно такой выход: ветка провала
перепроверки ключа звала `ChatterServer::DisconnectClient` НЕ ВЫЙДЯ из
`unique_lock(_clientsMutex)`. Разрыв синхронный (`Client::End()` →
`OnClientDisconnected` → `MessengerDirector::HandleClientDisconnected`) и на том
же потоке снова берёт нерекурсивный `std::shared_mutex`. На нашем glibc это не
зависание, а `EDEADLK`, проглоченный `catch(...)` стража, — то есть запись НЕ
снимается и в карте навсегда остаётся полусвязанный зомби.

★ПОЧЕМУ СТРУКТУРНАЯ ПРОВЕРКА, А НЕ ТОЛЬКО СТЕНД. Стендовая ячейка
`mid-login-revoke` воспроизводит окно ГОНКОЙ: она честно краснеет на непочиненном
коде, но её срабатывание вероятностное. Этот гейт краснеет ВСЕГДА и мгновенно,
поэтому он — первая линия, а стенд — доказательство того, что окно вообще
достижимо в рантайме.

ЧТО СЧИТАЕТСЯ ЗАПРЕТНЫМ ВЫХОДОМ (список именованный, а не «всё подряд»):
  * `DisconnectClient` / `End()` — синхронно уводят в уборку разрыва, а та
    берёт `_clientsMutex` стражем записи;
  * `EvictOtherSessionsOfCharacter`, `CloseSessionsOfCharacter`,
    `DisconnectUnboundSessions`, `DrainPendingDisconnects` — берут замок сами
    и/или рвут соединения;
  * `GetClientByCharacterUid`, `IsCharacterOnline`, `SendStallionReward`,
    `BroadcastPresenceOfCharacter`, `HandleChatterUpdateState` — берут
    `_clientsMutex` сами (последняя — через рассылку).

КОДЫ ВОЗВРАТА
  0 — чисто
  1 — под замком найден запретный выход
  2 — ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА (файла нет, замков/вызовов подозрительно мало) —
      слепота это СТОП, а не «чисто»
"""

import re
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent
SOURCE = "src/server/messenger/MessengerDirector.cpp"

#: Вызовы, которых под замком быть не может. Имя — как оно стоит в коде.
FORBIDDEN = (
    "DisconnectClient",
    "End",
    "EvictOtherSessionsOfCharacter",
    "CloseSessionsOfCharacter",
    "DisconnectUnboundSessions",
    "DrainPendingDisconnects",
    "GetClientByCharacterUid",
    "IsCharacterOnline",
    "SendStallionReward",
    "BroadcastPresenceOfCharacter",
    "HandleChatterUpdateState",
)
FORBIDDEN_RE = re.compile(r"\b(" + "|".join(FORBIDDEN) + r")\s*\(")

LOCK_RE = re.compile(r"std::(?:shared_lock|unique_lock)\s+lock\(\s*"
                     r"(?:director\.)?_clientsMutex\s*\)")

#: Ниже этих чисел разбор заведомо не сработал.
MIN_LOCKS = 6
MIN_CALLS = 8


class Invalid(Exception):
    """Проверка недействительна (exit 2)."""


def _strip(line: str) -> str:
    """Убрать `//`-комментарий и строковые литералы (скобки из прозы сдвинули
    бы весь учёт глубины — тот же урок, что у соседнего гейта)."""
    idx = line.find("//")
    line = line if idx < 0 else line[:idx]
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def analyse(text: str):
    """Вернуть (замков, вызовов, нарушений)."""
    depth = 0
    lock_depths: list[int] = []
    locks = 0
    calls = 0
    violations = []

    for number, raw in enumerate(text.splitlines(), 1):
        code = _strip(raw)

        if LOCK_RE.search(code):
            lock_depths.append(depth)
            locks += 1

        for match in FORBIDDEN_RE.finditer(code):
            calls += 1
            if lock_depths:
                violations.append((number, match.group(1), raw.strip()))

        depth += code.count("{")
        closed = code.count("}")
        if closed:
            depth -= closed
            # Замок действует на ВСЕХ операторах своей глубины и снимается,
            # только когда блок закрылся (сравнение нестрогое — ошибка на
            # единицу здесь уже была поймана у соседнего гейта).
            lock_depths = [d for d in lock_depths if d <= depth]

    return locks, calls, violations


def judge(tree: Path) -> int:
    path = tree / SOURCE
    if not path.is_file():
        raise Invalid(f"нет файла {path}")

    locks, calls, violations = analyse(path.read_text(encoding="utf-8"))

    if locks < MIN_LOCKS:
        raise Invalid(f"объявлений замка найдено {locks}, минимум {MIN_LOCKS} — "
                      "разбор не сработал, «ноль нарушений» ничего не значит")
    if calls < MIN_CALLS:
        raise Invalid(f"поднадзорных вызовов найдено {calls}, минимум {MIN_CALLS} — "
                      "разбор не сработал")

    print("=== gate: под замком карты клиентов нет выходов в чужой код ===")
    print(f"дерево              : {tree}")
    print(f"объявлений замка    : {locks} (минимум {MIN_LOCKS})")
    print(f"поднадзорных вызовов: {calls} (минимум {MIN_CALLS})")
    print(f"нарушений           : {len(violations)} (ожидалось 0)")
    if violations:
        print("\nнарушители:")
        for number, name, code in violations:
            print(f"  {SOURCE}:{number}  {name}: {code[:80]}")
        print("\n=== ИТОГ: НАРУШЕНО ✗ ===")
        return 1
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


def selftest() -> int:
    """★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ. Для КАЖДОГО объявления замка вставляем
    сразу под него запретный вызов и требуем, чтобы гейт покраснел именно на нём.
    Канарейка ставится внутрь блока замка, то есть воспроизводит ровно ту форму,
    которую нашло ревю #4."""
    path = TREE / SOURCE
    if not path.is_file():
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: нет {path}")
        return 2
    original = path.read_text(encoding="utf-8")

    print("=== самопроверка гейта «под замком без выходов» ===")
    locks, calls, violations = analyse(original)
    if violations:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: исходное дерево уже нарушено "
              f"({len(violations)})")
        return 2
    if locks < MIN_LOCKS or calls < MIN_CALLS:
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: замков {locks}, вызовов {calls}")
        return 2
    print(f"  исходное дерево чисто: {locks} замков, {calls} вызовов, "
          f"0 нарушений ✓")

    lines = original.splitlines()
    lock_lines = [n for n, line in enumerate(lines, 1)
                  if LOCK_RE.search(_strip(line))]

    caught = 0
    for number in lock_lines:
        canary_lines = list(lines)
        canary_lines.insert(
            number, "    _chatterServer.DisconnectClient(clientId);")
        _, _, canary_violations = analyse("\n".join(canary_lines))
        if canary_violations:
            caught += 1
            print(f"  ✓ канарейка «разрыв под замком строки {number}» поймана")
        else:
            print(f"  ✗ канарейка на строке {number} НЕ поймана")

    if caught != len(lock_lines):
        print(f"=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ (поймано {caught} из "
              f"{len(lock_lines)}) ===")
        return 2
    print(f"=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ (поймано {caught} канареек из "
          f"{len(lock_lines)}) ===")
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
