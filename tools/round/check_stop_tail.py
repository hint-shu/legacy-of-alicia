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
  4. Сам блок обёрнут в `try { … } catch (…) { … } catch (…) { … }` — то есть
     ДВА внешних обработчика на месте.
     ★ЗАЧЕМ ОТДЕЛЬНО (находка ревью, итерация 5, W2). Комментарий у блока
     заявляет ДВА факта: «(а) блок сам не может бросить» и «(б) после блока нет
     операторов», и ссылается на этот гейт как на машинную проверку обоих. До
     этой правки гейт доказывал только (б): сними кто-нибудь ОБА внешних
     `catch`, оставив голый составной оператор `{ … }`, — хвост оказался бы
     пуст, гейт стал бы ЗЕЛЁНЫМ, а свойство (а) умерло бы молча, вернув форму
     дефекта #233 (бросок из пролога → повторный `Stop()` → повторная выплата
     2500 морковок). Проверка, чей вердикт никто не читает, — не проверка.

  ★ПРОВЕРКА ПОСИМВОЛЬНАЯ, А НЕ ПОСТРОЧНАЯ, И ЭТО ИСПРАВЛЕНИЕ ДЕФЕКТА ГЕЙТА.
  Первая версия шла по СТРОКАМ от `block_end + 1` до `end` (не включая `end`),
  из-за чего слепла на двух формах сразу, и обе прошли адверсариальный прогон:
      `}  DoSomething();`   — оператор В ОДНОЙ СТРОКЕ с концом блока
                              (строка `block_end` не осматривалась вовсе);
      `DoSomething();  }`   — оператор В ОДНОЙ СТРОКЕ с концом функции
                              (строка `end` не осматривалась вовсе).
  Теперь хвост собирается как поток СИМВОЛОВ кода от закрывающей скобки блока до
  закрывающей скобки функции, поэтому «в той же строке» перестало быть укрытием.
  Конец блока ищется не «последним возвратом глубины в ноль» (его сдвинул бы
  дописанный следом `if (x) { … }`), а ПЕРВЫМ возвратом в ноль, за которым не
  стоит `catch`.

САМОПРОВЕРКА (гейт сперва доказывает себя)
  `--self-test` строит семь фикстур из НАСТОЯЩЕГО файла и требует, чтобы гейт
  на каждой упал:
    фикстура 1 — после блока вставлен оператор `DoSomething();` отдельной строкой;
    фикстура 2 — маркер удалён вовсе («маркер не найден» не имеет права читаться
                 как «всё хорошо»);
    фикстура 3 — `DoSomething();` дописан В ТОЙ ЖЕ СТРОКЕ, где блок кончается;
    фикстура 4 — `DoSomething();` дописан В ТОЙ ЖЕ СТРОКЕ, где кончается функция;
    фикстура 7 — снят только последний `catch (...)`: `try` на месте, хвост
                 пуст, но обработчик один — падает счёт;
    фикстура 6 — у блока сняты ОБА внешних `catch` (остался голый составной
                 оператор): хвост пуст, но свойство «блок не бросает» мертво;
    фикстура 5 — после блока дописан `catch_label: { (void)0; }` — валидный
                 оператор, чей ПЕРВЫЙ ИДЕНТИФИКАТОР начинается на «catch»
                 (проверка на префикс его пропускала).
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

#: Начало настоящего обработчика исключения: ключевое слово `catch` целиком
#: (за ним НЕ может стоять символ идентификатора) и открывающая скобка списка.
CATCH_CLAUSE = re.compile(r"catch\s*\(")

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
            # ★СВОЙСТВО (а) — ОТДЕЛЬНОЙ ПРОВЕРКОЙ: пустой хвост сам по себе
            # ничего не говорит о том, обёрнут ли блок в перехваты.
            catch_problem = check_catches(lines, marker, end)
            if catch_problem:
                problems.append(catch_problem)
    return problems


def code_of_lines(lines, upto):
    """Код строк 0..upto включительно, без комментариев и строковых литералов.

    Состояние блочного комментария ведётся ОТ НАЧАЛА ФАЙЛА: начать разбор с
    середины значило бы принять код внутри `/* … */` за настоящий.
    """
    codes = []
    in_block_comment = False
    for index in range(0, upto + 1):
        code, in_block_comment = strip_comments_and_strings(lines[index], in_block_comment)
        codes.append(code)
    return codes


def locate_block(lines, marker, end):
    """Разбирает хвост Stop() посимвольно.

    @returns (chars, zeros, func_close, error), где
      chars      — [(строка, колонка, символ)] кода от маркера до конца функции;
      zeros      — индексы в chars тех `}`, что вернули глубину блока в ноль;
      func_close — индекс в chars закрывающей скобки самой функции.
    """
    codes = code_of_lines(lines, end)
    chars = [
        (index, column, ch)
        for index in range(marker, end + 1)
        for column, ch in enumerate(codes[index])]

    depth = 0
    zeros = []
    func_close = None
    for position, (_, _, ch) in enumerate(chars):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                zeros.append(position)
            elif depth < 0:
                func_close = position
                break
    if func_close is None:
        return None, None, None, "закрывающая скобка Stop() не найдена — файл разобран неверно"
    if not zeros:
        return None, None, None, "не нашёл конца блока достижений: скобки не сошлись"
    return chars, zeros, func_close, None


def check_tail(lines, marker, end):
    """После блока в Stop() не должно остаться ни одного оператора."""
    chars, zeros, func_close, error = locate_block(lines, marker, end)
    if error:
        return error

    for position in zeros:
        tail = chars[position + 1:func_close]
        rest = "".join(ch for (_, _, ch) in tail)
        # `try { … } catch (…) { … } catch (…) { … }` возвращает глубину в ноль
        # трижды; первые два раза за скобкой стоит `catch` — это ещё тот же блок.
        # ★ГРАММАТИКА, А НЕ ПРЕФИКС (находка ревью, итерация 2). Проверка
        # `startswith("catch")` принимала ЛЮБОЙ идентификатор, начинающийся на
        # «catch»: дописанный после блока валидный C++ `catch_label: { … }`
        # проходил гейт насквозь. Обработчик исключения — это ровно `catch`,
        # за которым (через пробелы) идёт `(`; идентификатор `catch_label`
        # этому не отвечает, потому что `\b` между `catch` и `_` не стоит.
        if CATCH_CLAUSE.match(rest.lstrip()):
            continue
        if not rest.strip():
            return None
        offender = next(
            (index, column) for (index, column, ch) in tail if not ch.isspace())
        return (
            "после блока достижений в Stop() стоит оператор — строка %d, колонка %d: %s"
            % (offender[0] + 1, offender[1] + 1, lines[offender[0]].strip()))
    return "конец блока достижений не опознан — за каждой закрывающей скобкой стоит catch"


def check_catches(lines, marker, end):
    """Блок обязан быть `try { … } catch (…) { … } catch (…) { … }`.

    Доказывает свойство (а) из комментария у блока: «блок сам не может бросить».
    Считаются ровно те возвраты глубины в ноль, за которыми стоит настоящий
    обработчик (грамматика CATCH_CLAUSE, а не префикс «catch»), — их обязано
    быть ДВА: `catch (const std::exception&)` и `catch (...)`.
    """
    chars, zeros, func_close, error = locate_block(lines, marker, end)
    if error:
        return error

    prologue = "".join(ch for (_, _, ch) in chars[:zeros[0]])
    head = prologue.split("{", 1)[0].strip()
    if head != "try":
        return (
            "блок достижений в Stop() не открыт `try` — перед первой скобкой "
            "стоит «%s»; свойство «блок не бросает» больше не доказано"
            % (head if head else "(ничего)"))

    catches = 0
    for position in zeros:
        rest = "".join(ch for (_, _, ch) in chars[position + 1:func_close])
        if CATCH_CLAUSE.match(rest.lstrip()):
            catches += 1
    if catches != 2:
        return (
            "у блока достижений в Stop() %d внешних `catch`, а обязано быть два "
            "(`const std::exception&` и `...`): бросок из пролога снова "
            "вылетел бы из Stop() и вызвал повторную выплату" % catches)
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

    marker_lines = [i for i, line in enumerate(lines) if MARKER in line]
    if len(marker_lines) != 1:
        print("САМОПРОВЕРКА: маркер найден %d раз(а) — фикстуры не построить"
              % len(marker_lines))
        return 2
    chars, zeros, _, error = locate_block(lines, marker_lines[0], end)
    if error:
        print("САМОПРОВЕРКА: %s" % error)
        return 2
    block_end_line, block_end_column, _ = chars[zeros[-1]]

    fixtures = []
    # Фикстура 1: оператор после блока, ПЕРЕД закрывающей скобкой функции.
    injected = lines[:end] + ["  DoSomething();"] + lines[end:]
    fixtures.append(("оператор после блока", "\n".join(injected) + "\n"))
    # Фикстура 2: маркера нет вовсе.
    fixtures.append(
        ("маркер удалён", original.replace(MARKER, "// === (маркер снят фикстурой)")))
    # Фикстура 3: `}  DoSomething();` — оператор в строке конца блока. Ровно эта
    # форма проходила первую версию гейта: строку block_end она не смотрела.
    same_line_block = list(lines)
    same_line_block[block_end_line] = (
        lines[block_end_line][:block_end_column + 1]
        + "  DoSomething();"
        + lines[block_end_line][block_end_column + 1:])
    fixtures.append(
        ("оператор в строке конца блока", "\n".join(same_line_block) + "\n"))
    # Фикстура 4: `DoSomething();  }` — оператор в строке конца функции. Вторая
    # форма, проходившая первую версию: строку end она тоже не смотрела.
    same_line_function = list(lines)
    same_line_function[end] = "  DoSomething();" + lines[end]
    fixtures.append(
        ("оператор в строке конца функции", "\n".join(same_line_function) + "\n"))
    # Фикстура 5: `catch_label: { (void)0; }` — валидный C++ (метка + составной
    # оператор), который проходил вторую версию гейта: та отсекала хвост по
    # префиксу «catch», а идентификатор метки с него начинается. Форма найдена
    # адверсариальным ревью (итерация 2) и им же проверена на компилируемость.
    catch_lookalike = lines[:end] + ["  catch_label: { (void)0; }"] + lines[end:]
    fixtures.append(
        ("хвост, начинающийся на catch", "\n".join(catch_lookalike) + "\n"))

    # Фикстура 6: у блока сняты ОБА внешних `catch` и открывающий `try` —
    # остаётся голый составной оператор `{ … }`. Хвост при этом ПУСТ, то есть
    # проверка (б) зелена; падать обязана проверка (а).
    block_end_line_last = chars[zeros[-1]][0]
    first_zero_line = chars[zeros[0]][0]
    stripped = lines[:first_zero_line + 1] + lines[block_end_line_last + 1:end + 1]
    try_line = next(
        (index for index in range(marker_lines[0], first_zero_line)
         if stripped[index].strip() == "try"), None)
    if try_line is None:
        print("САМОПРОВЕРКА: строка `try` блока не найдена — фикстуру 6 не построить")
        return 2
    stripped[try_line] = ""
    fixtures.append(("оба внешних catch сняты", "\n".join(stripped) + "\n"))

    # Фикстура 7: снят ТОЛЬКО последний `catch (...)`. `try` на месте, хвост
    # пуст — падать обязан именно СЧЁТ обработчиков. Без этой фикстуры путь
    # подсчёта не исполнялся бы самопроверкой ни разу.
    if len(zeros) >= 2:
        one_catch = lines[:chars[zeros[-2]][0] + 1] + lines[block_end_line_last + 1:end + 1]
        fixtures.append(("снят последний catch (...)", "\n".join(one_catch) + "\n"))

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
