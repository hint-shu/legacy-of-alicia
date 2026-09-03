#!/usr/bin/env python3
"""path_anchored_gate.py — build gate (LOA-fix R73-12, правка ревью, итерация 6).

ЧТО ЭТО ЗА СВОЙСТВО
    Раунд объявил инвариант: НИ ОДИН путь под `data/` не проходится по
    символической ссылке. Пять итераций ревью показывали один и тот же способ,
    которым это утверждение оказывалось ложным: инвариант чинили у ЧИТАТЕЛЯ,
    потом у ПИСАТЕЛЯ, потом у ОБХОДА — и каждый раз оставалось действие, о
    котором его не спросили. Шестая итерация нашла три сразу: создание каталога
    (`create_directories`), удаление записи (`std::filesystem::remove` в
    пятнадцати методах `Delete*`) и уборка временных файлов.

    `O_NOFOLLOW` защищает только ПОСЛЕДНИЙ компонент пути, а `std::filesystem`
    разрешает путь именами целиком. Поэтому подмена ПРОМЕЖУТОЧНОГО каталога
    (`data/characters/equipment -> /tmp/theirs`) уводила создание, удаление и
    уборку в чужое дерево, и ни один из уже написанных поясов этого не видел.

    Свойство, которое проверяет этот гейт, формулируется без перечисления мест:

        в слое доступа к данным ни одна операция файловой системы не
        адресуется ПУТЁМ — всё идёт через помощников, привязанных к
        ДЕСКРИПТОРУ каталога (`AtomicFile.hpp`).

    Именно поэтому гейт нужен помимо самих правок: правка чинит пятнадцать
    существующих мест, гейт отвечает за шестнадцатое, которое напишут через
    полгода по образцу соседей.

ЧТО ИМЕННО ЗАПРЕЩЕНО И ПОЧЕМУ ИМЕННО ЭТО
    remove / remove_all         — удаление по пути идёт СКВОЗЬ промежуточные ссылки
    create_directory(-ies)      — создание по пути идёт сквозь них же
    rename                      — подмена по пути, окно между открытием и заменой
    (recursive_)directory_iterator — обход по пути + бросающее продвижение
    ifstream / ofstream / fstream  — открытие по пути идёт ПО ссылке и по FIFO

    Разрешённая замена у каждого своя и живёт в `server::util`:
    `RemoveManagedFile`, `CreateManagedDirectories`, `WriteFileAtomically`,
    `ListRegularFiles`, `ReadManagedFile`, `SweepStaleTemporaries`.

РАДИУС, СКАЗАННЫЙ ЧЕСТНО (не продавать этот гейт дороже, чем он стоит)
    Он смотрит `src/libserver/data/` и `include/libserver/data/`. Сам
    `include/libserver/util/AtomicFile.hpp` НЕ входит намеренно: он и есть
    реализация помощников, и запретить ему системные вызовы значило бы
    запретить существование. Код под `src/server/**`, работающий с файлами
    напрямую, этим гейтом НЕ покрыт.

КАК ОН ДОКАЗЫВАЕТ, ЧТО УМЕЕТ ПАДАТЬ
    `--selftest` гоняет анализатор по фикстурам в памяти: по одной на каждое
    запрещённое имя (обязана дать нарушение), плюс фикстура, где те же имена
    стоят В КОММЕНТАРИИ и В СТРОКОВОМ ЛИТЕРАЛЕ (обязана быть чистой — ложно
    красный гейт отключают так же, как ложно зелёный), плюс фикстура с
    разрезанным переводом строки именем (обязана быть замечена: компилятор
    склеивает строки раньше, чем читает имена).

ИСПОЛЬЗОВАНИЕ
    python3 tools/path_anchored_gate.py
    ROOT=/tmp/checkout python3 tools/path_anchored_gate.py
    python3 tools/path_anchored_gate.py --selftest

КОДЫ ВОЗВРАТА
    0 чисто · 1 найдены нарушения · 2 дерево не читается / гейт слеп
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# ★БЕЗ `__pycache__` РЯДОМ С ИСХОДНИКАМИ: гейт запускают и по чистому клону
#  (см. `tools/round/build_from_branch.sh`), а мусор в рабочем дереве делает
#  «дерево чистое» неправдой ровно в тот момент, когда это утверждение нужно.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

# ★ОБЩИЕ ФАЗЫ С `secret_write_gate.py`, А НЕ ВТОРАЯ КОПИЯ. Склейка строк,
#  вычистка комментариев и обнуление содержимого литералов — это фазы
#  трансляции, а не частность одного гейта; две копии одной фазы умеют
#  разъехаться молча, и тогда один гейт видит форму, которой другой не видит.
from secret_write_gate import (  # noqa: E402
    blank_literal_contents,
    normalize_alternative_tokens,
    splice_line_continuations,
    strip_comments,
    unreadable_literals,
)

SCOPES = ("src/libserver/data", "include/libserver/data")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".inl")
# Пол слепоты: сегодня в этих двух каталогах 16 файлов.
MIN_FILES = int(os.environ.get("PATH_GATE_MIN_FILES", "14"))

#! Имя -> чем его заменять. Сообщение обязано называть замену: гейт, который
#  только запрещает, заставляет автора выдумывать обход.
FORBIDDEN = {
    "remove_all": "server::util::RemoveManagedFile (по одной записи)",
    "remove": "server::util::RemoveManagedFile",
    "create_directories": "server::util::CreateManagedDirectories",
    "create_directory": "server::util::CreateManagedDirectories",
    "rename": "server::util::WriteFileAtomically",
    "recursive_directory_iterator": "server::util::ListRegularFiles",
    "directory_iterator": "server::util::ListRegularFiles",
    "ifstream": "server::util::ReadManagedFile",
    "ofstream": "server::util::WriteFileAtomically",
    "fstream": "server::util::ReadManagedFile / WriteFileAtomically",
}

#! ПСЕВДОНИМ ПРОСТРАНСТВА ИМЁН — НАРУШЕНИЕ САМ ПО СЕБЕ (правка ревью, итерация 7).
#
#  ★ЗАЧЕМ. `namespace fs = std::filesystem;` + `fs::remove(path);` — обычнейший
#  способ написать ровно то, что гейт запрещает, и он давал НОЛЬ нарушений:
#  ключ гейта — квалификатор `std`/`filesystem` перед именем, а после псевдонима
#  ни того, ни другого в тексте нет. То есть заявленное «правило всего слоя»
#  было правилом одного НАПИСАНИЯ.
#
#  ★ПОЧЕМУ ЗАПРЕТ, А НЕ РАЗРЕШЕНИЕ ПСЕВДОНИМА. Разрешать значит вести таблицу
#  псевдонимов по всей единице трансляции, включая псевдоним псевдонима и
#  псевдоним, объявленный в заголовке, — то есть писать второй компилятор.
#  Запретить дешевле и честнее: в этих двух каталогах сегодня НОЛЬ псевдонимов
#  файловой системы (проверено), помощники зовутся полным именем, и тот, кому
#  псевдоним понадобится, обязан сперва научить гейт его читать.
#
#  То же и для `using namespace std::filesystem;` и `using std::filesystem::X;`:
#  после них имя пишется голым, и ключ по квалификатору не срабатывает.
ALIAS_PATTERNS = (
  (re.compile(r"\bnamespace\s+\w+\s*=\s*(?:::)?(?:std\s*::\s*)?filesystem\b"),
   "псевдоним пространства имён файловой системы"),
  (re.compile(r"\busing\s+namespace\s+(?:::)?(?:std\s*::\s*)?filesystem\b"),
   "`using namespace` файловой системы"),
  (re.compile(r"\busing\s+(?:::)?(?:std\s*::\s*)?filesystem\s*::\s*\w+"),
   "`using`-объявление имени файловой системы"),
)

#! `std::filesystem::remove`, `filesystem::remove`, `std::remove` — все формы
#  квалификации одного имени. Ключ — САМО ИМЯ с квалификатором `std`/
#  `filesystem` перед ним; голое `remove(` (например `bucket.erase`-подобные
#  методы контейнеров) не ловится намеренно: `std::ranges::remove` в этом слое
#  тоже мог бы встретиться и не имеет отношения к файловой системе.
PATTERN = re.compile(
    r"\b(?:std\s*::\s*)?(?:filesystem\s*::\s*)?(" + "|".join(FORBIDDEN) + r")\b")
QUALIFIED = re.compile(r"\b(?:std|filesystem)\s*::")


def analyse(text: str, origin: str) -> list[tuple[str, str]]:
    """Нарушения одной единицы трансляции."""
    spliced, source_line = splice_line_continuations(text)
    code = strip_comments(spliced)
    # ★ТЕ ЖЕ ФАЗЫ, ЧТО У СЕКРЕТНОГО ГЕЙТА, ВКЛЮЧАЯ СЫРЫЕ ЛИТЕРАЛЫ И ДИГРАФЫ
    # (правка ревью, итерация 7): один лексер на оба гейта, потому что две копии
    # одной фазы умеют разъехаться молча.
    scan = normalize_alternative_tokens(blank_literal_contents(code))

    violations: list[tuple[str, str]] = []

    def where(offset: int) -> str:
        line = source_line[offset] if offset < len(source_line) else 0
        return f"{origin}:{line}"

    for offset in unreadable_literals(spliced):
        violations.append(
            (where(offset),
             "незакрытый сырой строковый литерал — дальше файл не читается, "
             "и «ноль нарушений» здесь ничего не значит"))

    for pattern, what in ALIAS_PATTERNS:
        for alias in pattern.finditer(scan):
            violations.append(
                (where(alias.start()),
                 f"{what} — после него запрещённое имя пишется без "
                 f"квалификатора, и правило слоя перестаёт быть правилом"))

    for match in PATTERN.finditer(scan):
        # Только КВАЛИФИЦИРОВАННОЕ обращение: `std::` или `filesystem::` перед
        # именем — квалификатор входит в САМО совпадение. Иначе поле структуры с
        # именем `rename` и метод `guild.rename()` стали бы нарушениями, то есть
        # гейт был бы ложно-красным, а такой отключают ровно так же, как
        # ложно-зелёный.
        if not QUALIFIED.search(match.group(0)):
            continue
        line = source_line[match.start()] if match.start() < len(source_line) else 0
        name = match.group(1)
        violations.append(
            (f"{origin}:{line}",
             f"`{name}` адресуется ПУТЁМ — путь разрешается по именам и "
             f"проходит промежуточные символические ссылки; замена: "
             f"{FORBIDDEN[name]}"))
    return violations


def collect_sources(root: Path) -> tuple[list[Path], list[str]]:
    errors: list[str] = []
    sources: list[Path] = []
    for name in SCOPES:
        base = root / name
        if not base.is_dir():
            errors.append(f"{base}: область отсутствует или не каталог")
            continue
        for directory, _sub, files in os.walk(
                base,
                onerror=lambda error: errors.append(
                    f"{error.filename}: {error.strerror}")):
            for file_name in files:
                if file_name.endswith(SOURCE_SUFFIXES):
                    sources.append(Path(directory) / file_name)
    return sorted(sources), errors


SELFTEST_FIXTURES = [
    ("удаление по пути", 1, "  std::filesystem::remove(dataFilePath);\n"),
    ("создание каталога по пути", 1,
     "  std::filesystem::create_directories(root);\n"),
    ("создание одного каталога по пути", 1,
     "  std::filesystem::create_directory(root);\n"),
    ("переименование по пути", 1, "  std::filesystem::rename(a, b);\n"),
    ("обход по пути", 1,
     "  for (const auto& e : std::filesystem::directory_iterator(root)) {}\n"),
    ("рекурсивный обход по пути", 1,
     "  for (const auto& e : std::filesystem::recursive_directory_iterator(r)) {}\n"),
    ("чтение потоком по пути", 1, "  std::ifstream file(path);\n"),
    ("запись потоком по пути", 1, "  std::ofstream file(path);\n"),
    ("те же имена в комментарии и в литерале — не использование", 0,
     '  // std::filesystem::remove и std::ifstream остались только в прозе\n'
     '  QuietLogWarn("std::filesystem::remove failed for {}", path);\n'),
    ("поле с именем rename — не файловая операция", 0,
     "  command.rename = true;\n  guild.rename();\n"),
    ("имя, разрезанное переводом строки, всё равно имя", 1,
     "  std::filesystem::rem\\\nove(dataFilePath);\n"),
    # ★ФИКСТУРЫ ИТЕРАЦИИ 7: формы, на которых проба ревью получила ноль.
    ("псевдоним пространства имён прячет запрещённое имя", 1,
     "  namespace fs = std::filesystem;\n  fs::remove(path);\n"),
    ("`using namespace` файловой системы — то же самое", 1,
     "  using namespace std::filesystem;\n  remove(path);\n"),
    # Два нарушения намеренно: и само `using`-объявление, и квалифицированное
    # имя внутри него. Гейт называет обе причины, а не выбирает одну.
    ("`using`-объявление имени — то же самое", 2,
     "  using std::filesystem::remove;\n  remove(path);\n"),
    ("сырой литерал с кавычкой и слэшами не сбивает лексер", 1,
     '  const char* s = R"(a \" // b)";\n  std::filesystem::remove(p);\n'),
    ("незакрытый сырой литерал — не «чисто»", 1,
     '  const char* s = R"delim(never closed;\n'),
    ("разрешённые помощники — чисто", 0,
     "  server::util::RemoveManagedFile(dataFilePath);\n"
     "  server::util::CreateManagedDirectories(root);\n"
     "  const auto listing = server::util::ListRegularFiles(root);\n"),
]


def selftest() -> int:
    ok = True
    for name, expected, fixture in SELFTEST_FIXTURES:
        violations = analyse(fixture, f"<fixture {name}>")
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

    violations: list[tuple[str, str]] = []
    for path in sources:
        text = path.read_text(encoding="utf-8", errors="replace")
        violations += analyse(text, str(path.relative_to(root)))

    print("=== path-anchored gate ===")
    print(f"дерево          : {root}")
    print(f"области         : {' '.join(SCOPES)}")
    print(f"файлов прочитано: {len(sources)} (минимум {MIN_FILES})")
    print(f"нарушений       : {len(violations)} (ожидалось 0)")

    if len(sources) < MIN_FILES:
        print(f"ОСТАНОВ: файлов {len(sources)}, ожидалось не меньше {MIN_FILES} — "
              f"ноль нарушений на неполном обходе ничего не доказывает.")
        return 2

    if violations:
        print("\nнарушители:")
        for location, reason in violations:
            print(f"  {location}: {reason}")
        print("\n=== ИТОГ: ПРОВАЛ ✗ — путь под data/ адресуется именем, а не дескриптором ===")
        return 1

    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
