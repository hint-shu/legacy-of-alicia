#!/usr/bin/env python3
"""check_track_journal.py — два инварианта ЖУРНАЛА ТРАССЫ (R76, backlog #30 этап 1).

ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ
  Раунд R76 добавил пер-заездный журнал на `tracker::RaceTracker::Racer` и печать
  одной аудит-строки на гонщика в конце заезда. Два его свойства держатся НЕ на
  коде, а на дисциплине следующего правщика, и потому обязаны проверяться машиной:

  I4 — `RaceNetworkHandler::HandleRaceUserPos` НЕ ПРИОБРЕТАЕТ ЛОГИРОВАНИЯ.
       Путь удалённо-управляемый: дефект #195 дал 15 350 строк `[error]` за час
       именно на этом обработчике. Раунд врезал в него четыре блока и НИ ОДНОГО
       лога; следующий раунд, дописывающий сюда `QuietLogInfo("…")` «на время
       отладки», должен упереться в красную сборку, а не в ревью через неделю.
       ★ЧИСЛО, А НЕ НОЛЬ: в теле обработчика УЖЕ живёт один `QuietLogDebug`
       (детектор мести R13, до R76). Порог `== 0` был бы КРАСНЫМ на чистом
       дереве, и «чинили» бы его удалением чужой отладочной строки.

  I6 — ЖУРНАЛ ПЕР-ЗАЕЗДНЫЙ: каждое поле блока R76 обнуляется в `HandleStartRace`.
       Сегодня это защита «на будущее» (`Tracker::Clear()` сносит гонщиков
       целиком, `AddRacer` отдаёт свежего), но ровно та же защита стоит у
       `finishCounted` и у телеметрии R24 — по той же причине: переиспользование
       трекера без `Clear()` тихо унесло бы чужой заезд в следующий.
       ★ИМЕНА ИЗВЛЕКАЮТСЯ ПО СВОЙСТВУ, А НЕ ПЕРЕПИСАНЫ СПИСКОМ. Список разошёлся
       бы с кодом в тот день, когда добавят девятое поле, и гейт молча перестал
       бы его стеречь. Число полей в скрипте НЕ ЗАШИТО — зашит КОНТРОЛЬ СЛЕПОТЫ
       «извлечено >= 8»: гейт, осмотревший ноль полей, обязан ОСТАНОВИТЬСЯ, а не
       доложить «чисто».

ПОЧЕМУ В `tools/`, А НЕ В ХАРНЕССЕ РАУНДА (решение лида, R76)
  Харнесс живёт один раунд и уезжает в зеркало. Инвариант живёт столько же,
  сколько код, который он стережёт. Прецедент — `check_lobby_auth_gate.sh`
  (R72), `check_field_init.py` (R72), `r75_*_gate.sh` (R75): проверка, чей
  вердикт никто не читает, — не проверка.

СЛЕПОТА — ЭТО ОСТАНОВ, А НЕ «ЧИСТО»
  Ненайденный блок, ненайденная функция, подозрительно короткое тело — код 2.

КОДЫ ВОЗВРАТА
  0 — оба инварианта держатся
  1 — инвариант нарушен
  2 — гейт слеп (осматривать нечего) либо самопроверка провалилась
"""

import argparse
import os
import re
import sys

#: Заголовки блоков пер-заездных полей в `Racer`. Ищутся ДОСЛОВНО: тот же приём,
#: что у `check_stop_tail.py` с маркером блока достижений.
#: ★ТРИ МАРКЕРА, А НЕ ОДИН (R81). Инвариант «пер-заездное поле обнуляется в
#: HandleStartRace» одинаков у всех трёх блоков; стеречь один из них значило бы
#: объявить, что у двух других это свойство держится на честном слове.
#: Пара «тег раунда, текст маркера»: тег нужен, чтобы блок закрывался ЧУЖИМ
#: маркером, а не собственным продолжением.
BLOCK_MARKS = (
    ("R76", "LOA-fix (R76, backlog #30 этап 1): ПЕР-ЗАЕЗДНЫЙ ЖУРНАЛ ТРАССЫ"),
    ("R75", "LOA-fix (R75, #14 Ф2): ПЛАНИРОВАНИЕ И ЦЕПОЧКА РЫВКОВ"),
    ("R81", "LOA-fix (R81, #270/#273): ПЕР-ЗАЕЗДНЫЕ СЧЁТЧИКИ РЫВКОВ И МАСТЕРСТВА"),
)

#: Объявление данных-члена внутри структуры `Racer` (отступ 4 пробела, тип,
#: имя, инициализатор `{…}`). `static constexpr` отсеивается отдельно — это
#: константы блока, а не пер-заездное состояние.
#: ★РАСШИРЕНО R81, И ОБА РАСШИРЕНИЯ ИЗМЕРЕНЫ, А НЕ ПРИДУМАНЫ:
#:  (а) ХВОСТОВОЙ КОММЕНТАРИЙ после `;` — прежняя якорилась на `$` сразу за
#:      точкой с запятой и такое поле не видела вовсе;
#:  (б) МНОГОСТРОЧНЫЙ ИНИЦИАЛИЗАТОР (`{` в конце строки) — без него гейт не
#:      видел ТРИ `time_point` блока R75 (`lastLandingTimePoint`,
#:      `glideMarkTimePoint`, `lastSpurTimePoint`) и `slidingSince` блока R81,
#:      то есть стерёг 7 полей из 10 и молчал об этом.
FIELD_RE = re.compile(
    r"^\s{4}(?!static\b)(?!return\b)(?:\[\[[^\]]*\]\]\s*)?[A-Za-z_][\w:<>,\s]*?"
    r"\b(?P<name>[a-z][A-Za-z0-9_]*)\s*"
    r"(?:\{[^{}]*\}\s*;|\{\s*$|=[^;]*;)"
    r"\s*(?://.*)?$")

#: Настоящий вызов обёртки тихого лога: имя целиком и открывающая скобка.
LOG_CALL_RE = re.compile(r"\bQuietLog[A-Za-z]+\s*\(")

HEADER_PATH = "include/server/tracker/RaceTracker.hpp"
HANDLER_PATH = "src/server/race/RaceNetworkHandler.cpp"

START_RACE_SIGNATURE = "void RaceNetworkHandler::HandleStartRace("
USER_POS_SIGNATURE = "void RaceNetworkHandler::HandleRaceUserPos("

#: Контроль слепоты, а НЕ ожидаемое число полей: следующее поле обязано попасть
#: под тот же инвариант само собой.
#: ★ЧИСЛО ИЗМЕРЕНО НА ДЕРЕВЕ, А НЕ НАЗНАЧЕНО: R76 = 8, R75 = 10, R81 = 8.
#: «Починить» его вниз — это ровно тот приём, который сам гейт запрещает в своей
#: шапке: пол, не отслеживающий длину блоков, превращается в декорацию.
MIN_FIELDS = 26
#: Столько вызовов логирования в теле `HandleRaceUserPos` стояло ДО R76.
EXPECTED_LOG_CALLS = 1
#: Тело обработчика позиции — сотни строк. Разбор, давший меньше, неверен.
MIN_HANDLER_LINES = 100


def read_lines(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read().splitlines()


def one_block_fields(lines, tag, mark):
    """Имена нестатических данных-членов ОДНОГО блока — ПО СВОЙСТВУ, не списком.

    Блок закрывается ДВУМЯ способами, и второй появился в R81:
      * соседним маркером ЧУЖОГО раунда — так было всегда;
      * строкой `};`, то есть КОНЦОМ СТРУКТУРЫ. Блок, стоящий последним в
        `Racer`, чужого маркера после себя не имеет, и разбор перелезал через
        `};` и собирал поля соседних `ItemDeck`/`Event`/`TeamInfo` — ДЕСЯТЬ
        чужих полей, которые в `HandleStartRace` не сбрасываются никогда.
        Измерено: без терминатора блок R75 давал 17 полей вместо 10, а блок R81
        — 10 ЧУЖИХ и НИ ОДНОГО своего.
    """
    start = next((i for i, line in enumerate(lines) if mark in line), None)
    if start is None:
        return None, "блок %s в %s не найден — гейту нечего осматривать" % (
            tag, HEADER_PATH)
    names = []
    for index in range(start, len(lines)):
        line = lines[index]
        # Конец структуры закрывает блок так же жёстко, как чужой маркер.
        if index > start and line.strip() == "};":
            break
        # Соседний блок другого раунда закрывает наш.
        if index > start and ("LOA-fix (R" in line or "LOA (R" in line) \
                and tag not in line:
            break
        stripped = line.strip()
        if stripped.startswith("//"):
            continue
        if "static constexpr" in line:
            continue
        found = FIELD_RE.match(line)
        if found:
            names.append(found.group("name"))
    return names, None


def block_fields(lines):
    """Имена полей ВСЕХ трёх пер-заездных блоков. Имена не пересекаются."""
    names = []
    for tag, mark in BLOCK_MARKS:
        found, error = one_block_fields(lines, tag, mark)
        if error:
            return None, error
        names.extend(found)
    return names, None


def function_body(lines, signature):
    """Границы тела функции по ДОСЛОВНОЙ сигнатуре; номера строк не зашиты.

    Комментарии вырезаются перед счётом скобок: `// }` в конце строки иначе
    закрыл бы функцию раньше времени.
    """
    start = next((i for i, line in enumerate(lines) if signature in line), None)
    if start is None:
        return None, None
    depth = 0
    opened = False
    for index in range(start, len(lines)):
        code = re.sub(r"//.*", "", lines[index])
        for ch in code:
            if ch == "{":
                depth += 1
                opened = True
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return start, index
    return start, None


def check(header_lines, handler_lines):
    """@returns (problems, notes, code) — code 0 чисто · 1 нарушение · 2 слепота."""
    problems = []
    notes = []

    # ---- I6 ---------------------------------------------------------------
    names, error = block_fields(header_lines)
    if error:
        return [error], notes, 2
    if len(names) < MIN_FIELDS:
        return ([
            "из блока R76 извлечено %d полей (%s) — меньше контроля слепоты %d; "
            "гейт не осмотрел блок, верить ему нельзя"
            % (len(names), ", ".join(names) or "ни одного", MIN_FIELDS)],
            notes, 2)
    notes.append("I6: полей журнала извлечено %d: %s" % (len(names), ", ".join(names)))

    start, end = function_body(handler_lines, START_RACE_SIGNATURE)
    if start is None or end is None:
        return (["тело HandleStartRace не найдено в %s (переименовали?)" % HANDLER_PATH],
                notes, 2)
    body = "\n".join(handler_lines[start:end + 1])
    notes.append("I6: тело HandleStartRace — строки %d-%d" % (start + 1, end + 1))
    for name in names:
        hits = len(re.findall(r"racer\.%s\b" % re.escape(name), body))
        if hits != 1:
            problems.append(
                "I6: поле журнала `%s` встречается в HandleStartRace %d раз(а), "
                "ожидался ровно 1 — журнал перестал быть пер-заездным" % (name, hits))

    # ---- I4 ---------------------------------------------------------------
    start, end = function_body(handler_lines, USER_POS_SIGNATURE)
    if start is None or end is None:
        return (["тело HandleRaceUserPos не найдено в %s (переименовали?)" % HANDLER_PATH],
                notes, 2)
    body_lines = handler_lines[start:end + 1]
    if len(body_lines) < MIN_HANDLER_LINES:
        return ([
            "тело HandleRaceUserPos — всего %d строк(и); разбор явно неверен, "
            "гейт слеп" % len(body_lines)], notes, 2)
    logs = sum(1 for line in body_lines if LOG_CALL_RE.search(line))
    notes.append("I4: тело HandleRaceUserPos — строки %d-%d (%d строк), "
                 "вызовов QuietLog*: %d" % (start + 1, end + 1, len(body_lines), logs))
    if logs != EXPECTED_LOG_CALLS:
        problems.append(
            "I4: в HandleRaceUserPos %d вызов(ов) QuietLog*, ожидался %d "
            "(предсуществующий QuietLogDebug детектора мести R13) — "
            "удалённо-управляемый путь приобрёл логирование" % (logs, EXPECTED_LOG_CALLS))

    return problems, notes, (1 if problems else 0)


def self_test(header_path, handler_path):
    """Гейт обязан упасть на четырёх фикстурах и пройти на настоящем дереве.

    Фикстуры строятся из НАСТОЯЩИХ файлов, а не пишутся руками: гейт,
    проверенный на выдуманном входе, зелен ровно на том входе.
    """
    header_lines = read_lines(header_path)
    handler_lines = read_lines(handler_path)
    ok = True

    problems, _notes, code = check(header_lines, handler_lines)
    if code == 0:
        print("САМОПРОВЕРКА: неизменённое дерево — ПРОХОДИТ ✓")
    else:
        print("САМОПРОВЕРКА: неизменённое дерево НЕ ПРОХОДИТ ✗ (%s)" % (problems,))
        ok = False

    # Фикстура 1 (I4): в тело обработчика позиции добавлен вызов лога.
    start, end = function_body(handler_lines, USER_POS_SIGNATURE)
    if start is None or end is None:
        print("САМОПРОВЕРКА: HandleRaceUserPos не найден — фикстуру 1 не построить")
        return 2
    mutated = list(handler_lines)
    mutated.insert(end, '  server::util::QuietLogInfo("фикстура самопроверки");')
    problems, _n, code = check(header_lines, mutated)
    if code == 1 and any(p.startswith("I4") for p in problems):
        print("САМОПРОВЕРКА: фикстура «добавлено логирование» — ПАДАЕТ ✓")
    else:
        print("САМОПРОВЕРКА: фикстура I4 НЕ УПАЛА ✗ (код %d)" % code)
        ok = False

    # Фикстура 2 (I6): из HandleStartRace убран сброс одного поля журнала.
    names, error = block_fields(header_lines)
    if error:
        print("САМОПРОВЕРКА: %s" % error)
        return 2
    victim = names[-1]
    mutated = [line for line in handler_lines
               if not re.search(r"racer\.%s\s*=" % re.escape(victim), line)]
    problems, _n, code = check(header_lines, mutated)
    if code == 1 and any(p.startswith("I6") and victim in p for p in problems):
        print("САМОПРОВЕРКА: фикстура «снят сброс `%s`» — ПАДАЕТ ✓" % victim)
    else:
        print("САМОПРОВЕРКА: фикстура I6 НЕ УПАЛА ✗ (код %d)" % code)
        ok = False

    # Фикстура 3 (слепота): заголовок блока переименован — НА КАЖДЫЙ из трёх
    # маркеров. Один общий прогон доказывал бы только один блок.
    for tag, mark in BLOCK_MARKS:
        blinded = [line.replace(mark, "(блок скрыт фикстурой)")
                   for line in header_lines]
        _p, _n, code = check(blinded, handler_lines)
        if code == 2:
            print("САМОПРОВЕРКА: фикстура «блок %s не найден» — ОСТАНОВ (2) ✓" % tag)
        else:
            print("САМОПРОВЕРКА: фикстура слепоты %s НЕ ОСТАНОВИЛА гейт ✗ (код %d)"
                  % (tag, code))
            ok = False

    # Фикстура 5 (R81): БЛОК В КОНЦЕ СТРУКТУРЫ. Маркер переносится в самый конец
    # `Racer`, и гейт обязан извлечь ТОЛЬКО его поля — иначе он снова съест
    # соседние структуры, и «26 полей» станут числом без смысла.
    tail_fixture = []
    struct_end = None
    for index, line in enumerate(header_lines):
        if line.strip() == "};":
            struct_end = index
            break
    if struct_end is None:
        print("САМОПРОВЕРКА: конец структуры не найден — фикстуру 5 не построить")
        return 2
    tail_fixture = list(header_lines[:struct_end]) + [
        "    // === LOA-fix (RXX, фикстура): БЛОК В КОНЦЕ СТРУКТУРЫ =========",
        "    //! поле фикстуры",
        "    uint32_t fixtureTailField{};",
    ] + list(header_lines[struct_end:])
    found, error = one_block_fields(
        tail_fixture, "RXX", "LOA-fix (RXX, фикстура): БЛОК В КОНЦЕ СТРУКТУРЫ")
    if error is None and found == ["fixtureTailField"]:
        print("САМОПРОВЕРКА: фикстура «блок в конце структуры» — "
              "извлечено ровно своё ✓")
    else:
        print("САМОПРОВЕРКА: фикстура «блок в конце структуры» ПРОВАЛЕНА ✗ (%s)"
              % (found,))
        ok = False

    # Фикстура 6 (R81): ПОЛЕ С ХВОСТОВЫМ КОММЕНТАРИЕМ И ПОЛЕ С МНОГОСТРОЧНЫМ
    # ИНИЦИАЛИЗАТОРОМ. Обе формы гейт обязан ВИДЕТЬ — иначе расширение
    # регулярки декорация, а не проверка.
    form_fixture = list(header_lines[:struct_end]) + [
        "    // === LOA-fix (RYY, фикстура): ФОРМЫ ОБЪЯВЛЕНИЯ ================",
        "    //! хвостовой комментарий",
        "    uint32_t fixtureTrailing{};   // хвостовой комментарий",
        "    //! многострочный инициализатор",
        "    std::chrono::steady_clock::time_point fixtureMultiline{",
        "      std::chrono::steady_clock::time_point::max()};",
    ] + list(header_lines[struct_end:])
    found, error = one_block_fields(
        form_fixture, "RYY", "LOA-fix (RYY, фикстура): ФОРМЫ ОБЪЯВЛЕНИЯ")
    if error is None and found == ["fixtureTrailing", "fixtureMultiline"]:
        print("САМОПРОВЕРКА: фикстура «хвостовой комментарий / многострочный "
              "инициализатор» — ОБЕ ФОРМЫ ВИДНЫ ✓")
    else:
        print("САМОПРОВЕРКА: фикстура форм объявления ПРОВАЛЕНА ✗ (%s)" % (found,))
        ok = False

    # Фикстура 4 (слепота): переименована сама функция сброса.
    renamed = [line.replace(START_RACE_SIGNATURE,
                            "void RaceNetworkHandler::HandleStartRaceRenamed(")
               for line in handler_lines]
    _p, _n, code = check(header_lines, renamed)
    if code == 2:
        print("САМОПРОВЕРКА: фикстура «HandleStartRace переименована» — ОСТАНОВ (2) ✓")
    else:
        print("САМОПРОВЕРКА: фикстура слепоты 2 НЕ ОСТАНОВИЛА гейт ✗ (код %d)" % code)
        ok = False

    print("=== САМОПРОВЕРКА ПРОЙДЕНА ✓ ===" if ok
          else "=== САМОПРОВЕРКА ПРОВАЛЕНА ✗ гейту верить нельзя ===")
    return 0 if ok else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".",
                        help="корень репозитория (по умолчанию текущий каталог)")
    parser.add_argument("--self-test", action="store_true",
                        help="доказать, что гейт умеет упасть, и выйти")
    args = parser.parse_args()

    header_path = os.path.join(args.root, HEADER_PATH)
    handler_path = os.path.join(args.root, HANDLER_PATH)
    for path in (header_path, handler_path):
        if not os.path.exists(path):
            print("ОСТАНОВ: файл %s не найден" % path)
            return 2

    if args.self_test:
        return self_test(header_path, handler_path)

    problems, notes, code = check(read_lines(header_path), read_lines(handler_path))
    for note in notes:
        print("  " + note)
    if code == 2:
        print("=== ИТОГ: ОСТАНОВ ✗ гейт слеп ===")
    elif code == 1:
        print("=== ИТОГ: ПРОВАЛ ✗ инвариант журнала трассы нарушен ===")
    for problem in problems:
        print("  ✗ " + problem)
    if code == 0:
        print("=== ИТОГ: инварианты журнала трассы I4 и I6 держатся ✓ ===")
    return code


if __name__ == "__main__":
    sys.exit(main())
