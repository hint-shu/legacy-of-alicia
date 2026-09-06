#!/usr/bin/env python3
"""Гейт R82: под замком карты клиентов ALL-CHAT нет НИ ОДНОГО выхода в чужой
код — сиблинг `check_messenger_disconnect_outside_lock.py` (R78, ревю #4 BLOCK).

★ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ, ЕСЛИ КОД УЖЕ ВЕРЕН. R80 сделал `HandleChatterChat`
и `HandleChatterInputState` правильными: self-чтение идёт под коротким
`shared_lock` в локаль, рассылка — по снимку-под-замком, а все выходы
(`GetCharacter`, `GetUserByCharacterUid`, `ProcessChatMessage`, `QueueCommand`)
стоят ВНЕ замка. Но постоянной проверки, которая покраснела бы, если следующий
раунд затащит выход обратно под замок, у all-chat НЕ БЫЛО: у мессенджера такой
сиблинг есть с R78, а у all-chat был только гейт «замок вообще присутствует»
(`check_allchat_client_map_lock.py`), который про содержимое блока ничего не
утверждает и прямо это говорит в своём докстринге. Верный код без гейта — это
верный код ровно до следующей правки ([[total-invariant-beats-list-of-sites]]).

★ЧЕМ ЭТО ГРОЗИТ ИМЕННО ЗДЕСЬ. `_clientsMutex` — `std::shared_mutex`, он НЕ
рекурсивный. `DisconnectClient` синхронно уводит в `Client::End()` →
`OnClientDisconnected` → `AllChatDirector::HandleClientDisconnected`, а тот
снова берёт этот же замок на том же потоке. На нашем gcc-15/glibc это не
зависание, а `EDEADLK`, проглоченный `catch(...)` стража: запись из `_clients`
НЕ удаляется, и в карте навсегда остаётся полусвязанный зомби. Ровно этот класс
(R59/R78) гейт и стережёт.

ЧТО СЧИТАЕТСЯ ЗАПРЕТНЫМ ВЫХОДОМ (список ИМЕНОВАННЫЙ, а не «всё подряд»):
  * `DisconnectClient` / `End()` — синхронно уводят в уборку разрыва, а та
    берёт `_clientsMutex` стражем записи;
  * `EvictOtherSessionsOfCharacter`, `CloseSessionsOfCharacter`,
    `DrainPendingDisconnects` — берут замок сами и/или рвут соединения;
  * `QueueCommand` — уходит в сетевой слой чужого клиента; держать под ним
    замок карты значит держать его на время сериализации кадра;
  * `GetCharacter`, `GetDataDirector`, `GetLobbyDirector`, `SnapshotUsers`,
    `GetUserByCharacterUid`, `ProcessChatMessage` — чужие подсистемы со своими
    замками: под нашим замком это готовая инверсия порядка блокировок.

★ОТЛИЧИЕ РЕГУЛЯРКИ ОТ СИБЛИНГА, И ПОЧЕМУ ОНО ОБЯЗАТЕЛЬНО. У мессенджера все
поднадзорные имена зовутся без шаблонных аргументов, поэтому там хватает
`\\bИМЯ\\s*\\(`. В all-chat главный выход пишется как
`_chatterServer.QueueCommand<decltype(notify)>(…)` — между именем и скобкой
стоит `<…>`, и регулярка соседа НЕ НАШЛА БЫ НИ ОДНОГО из шести вызовов.
Гейт, слепой к тому единственному выходу, который тут реально бывает, был бы
«зелёной наклейкой» ([[a-blind-checker-says-clean]]), поэтому имя допускает
необязательный шаблонный список. Это единственное содержательное расхождение с
формой соседа; остальное — `LOCK_RE`, учёт глубины скобок, `_strip`, коды
возврата — унаследовано без изменений (копия, а не параметризация чужого
отревьюенного файла R78: дороже менять его ради удобства).

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
SOURCE = "src/server/chat/AllChatDirector.cpp"

#: Вызовы, которых под замком быть не может. Имя — как оно стоит в коде.
FORBIDDEN = (
    "DisconnectClient",
    "End",
    "EvictOtherSessionsOfCharacter",
    "CloseSessionsOfCharacter",
    "DrainPendingDisconnects",
    "QueueCommand",
    "GetCharacter",
    "GetDataDirector",
    "GetLobbyDirector",
    "SnapshotUsers",
    "GetUserByCharacterUid",
    "ProcessChatMessage",
)
#: ★`(?:<[^<>;{}]*>)?` — необязательные шаблонные аргументы (см. докстринг).
FORBIDDEN_RE = re.compile(
    r"\b(" + "|".join(FORBIDDEN) + r")\s*(?:<[^<>;{}]*>)?\s*\(")

LOCK_RE = re.compile(r"std::(?:shared_lock|unique_lock)\s+lock\(\s*"
                     r"(?:director\.)?_clientsMutex\s*\)")

#: Ниже этих чисел разбор заведомо не сработал. Замер на `a1171d0b`:
#: 13 объявлений замка, 23 поднадзорных вызова. Пороги — чуть ниже замера,
#: чтобы ловить слепоту разбора, а не мелкую правку файла.
MIN_LOCKS = 10
MIN_CALLS = 18


class Invalid(Exception):
    """Проверка недействительна (exit 2)."""


def _strip(line: str) -> str:
    """Убрать `//`-комментарий и строковые литералы (скобки из прозы сдвинули
    бы весь учёт глубины — тот же урок, что у соседних гейтов)."""
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

    print("=== gate: под замком карты клиентов all-chat нет выходов в чужой код ===")
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
    которую в мессенджере нашло ревю #4 R78.

    ★ВТОРАЯ КАНАРЕЙКА — ШАБЛОННАЯ (`QueueCommand<…>`): без неё «13 из 13» не
    доказывало бы, что гейт видит ЕДИНСТВЕННЫЙ выход, который в этом файле
    реально бывает под рукой у рассылки."""
    path = TREE / SOURCE
    if not path.is_file():
        print(f"САМОПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА: нет {path}")
        return 2
    original = path.read_text(encoding="utf-8")

    print("=== самопроверка all-chat гейта «под замком без выходов» ===")
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

    canaries = (
        ("разрыв", "    _chatterServer.DisconnectClient(clientId);"),
        ("шаблонная рассылка",
         "    _chatterServer.QueueCommand<decltype(notify)>("
         "clientId, [](){ return notify; });"),
    )

    caught = 0
    expected = len(lock_lines) * len(canaries)
    for label, canary in canaries:
        for number in lock_lines:
            canary_lines = list(lines)
            canary_lines.insert(number, canary)
            _, _, canary_violations = analyse("\n".join(canary_lines))
            if canary_violations:
                caught += 1
                print(f"  ✓ канарейка «{label} под замком строки {number}» поймана")
            else:
                print(f"  ✗ канарейка «{label}» на строке {number} НЕ поймана")

    if caught != expected:
        print(f"=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ (поймано {caught} из "
              f"{expected}) ===")
        return 2
    print(f"=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ (поймано {caught} канареек из "
          f"{expected}) ===")
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
