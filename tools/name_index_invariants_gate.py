#!/usr/bin/env python3
"""name_index_invariants_gate.py — build gate (LOA-fix R73, ревью итерация 9).

ЗАЧЕМ ЭТОТ ГЕЙТ СУЩЕСТВУЕТ
  Итерация 9 закрыла четыре дефекта индексов имён, и ни один из них НЕ ВИДЕН
  снаружи детерминированно: три из четырёх — окна между двумя инструкциями под
  замком, а четвёртый (ремонт по расписанию) виден только через минуту работы
  живого шарда. Проверка, которой нет, — это не «свойство держится», это
  «никто не смотрел» ([[a-check-nobody-reads-is-not-a-check]]).

  Поэтому свойства записаны СТРУКТУРНО, как форма исходника, а не как поведение
  под нагрузкой. Такой гейт не доказывает отсутствие гонки; он доказывает, что
  форма, в которой гонка БЫЛА, не вернулась — и делает это детерминированно, на
  каждой сборке, для КАЖДОГО будущего места, а не для списка починенных
  ([[total-invariant-beats-list-of-sites]]).

ЧЕСТНЫЙ РАДИУС (не продавать этот гейт дороже, чем он стоит)
  Смотрит РОВНО два файла: src/libserver/data/file/FileDataSource.cpp и
  src/libserver/data/DataDirector.cpp. Индексы имён живут только там. Гонку в
  третьем файле он не увидит и не обещает.

КАК ОН ДОКАЗЫВАЕТ, ЧТО УМЕЕТ ПОКРАСНЕТЬ
  `--selftest` прогоняет КАЖДУЮ проверку по канарейке, в которой ровно это
  свойство нарушено, и требует от неё нарушения; плюс по чистой копии, от
  которой требует тишины. Проверка, чья канарейка молчит, — ОСТАНОВ.

ЧТО ОН ЧИТАЕТ
  Не сырой текст, а НОРМАЛИЗОВАННЫЙ КОД (`secret_write_gate.py --normalize-code`):
  склейка строк, снятие комментариев, ЗАЧИЩЕННОЕ содержимое литералов. Иначе
  ★-комментарий, цитирующий починенную форму, читался бы как сама форма
  ([[a-blind-checker-says-clean]]). Перед разбором зовётся общий детектор
  неподдерживаемых форм (`--unsupported`): псевдоним std, склейка `##`,
  включение через макрос — ОСТАНОВ, а не «чисто».

КОДЫ ВОЗВРАТА  0 чисто · 1 инвариант нарушен · 2 проверка НЕДЕЙСТВИТЕЛЬНА
"""
import os
import subprocess
import sys
import tempfile

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
TREE = os.environ.get("TREE", os.path.dirname(SELF_DIR))
SECRET_GATE = os.path.join(SELF_DIR, "secret_write_gate.py")

SCANNED = [
    "src/libserver/data/file/FileDataSource.cpp",
    "src/libserver/data/DataDirector.cpp",
]


def die(message):
    print("ОСТАНОВ: %s" % message, file=sys.stderr)
    print("=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ===")
    sys.exit(2)


def normalized(path):
    """Текст файла после тех же фаз трансляции, что применяют соседние гейты."""
    unsupported = subprocess.run(
        [sys.executable, SECRET_GATE, "--unsupported", path],
        capture_output=True, text=True)
    if unsupported.returncode != 0:
        sys.stdout.write(unsupported.stdout)
        sys.stderr.write(unsupported.stderr)
        die("%s: неподдерживаемая форма препроцессора — разобрать нельзя" % path)
    result = subprocess.run(
        [sys.executable, SECRET_GATE, "--normalize-code", path],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        die("нормализатор отказал на %s" % path)
    return result.stdout


def body_span(text, name):
    """(начало, конец) тела ОПРЕДЕЛЕНИЯ функции `name`, или None.

    ★ОПРЕДЕЛЕНИЕ ОТЛИЧАЕТСЯ ОТ ВЫЗОВА ПО ФОРМЕ, А НЕ ПО ПОРЯДКУ В ФАЙЛЕ: за
    закрывающей скобкой определения (с точностью до `noexcept`/`const`) стоит
    `{`. Брать «первое вхождение имени» значило бы разобрать вызов вместо тела
    и молча судить не тот текст.
    """
    search = 0
    while True:
        at = text.find(name + "(", search)
        if at < 0:
            return None
        search = at + 1
        before = text[at - 1] if at > 0 else " "
        if before.isalnum() or before in "_.>":
            continue
        depth = 0
        close = -1
        for position in range(at + len(name), len(text)):
            if text[position] == "(":
                depth += 1
            elif text[position] == ")":
                depth -= 1
                if depth == 0:
                    close = position
                    break
        if close < 0:
            continue
        tail = text[close + 1:close + 40]
        stripped = tail
        for word in ("noexcept", "const", "\n", " ", "\t"):
            stripped = stripped.replace(word, "", 1) if stripped.startswith(word) else stripped
        stripped = stripped.lstrip()
        for word in ("noexcept", "const"):
            if stripped.startswith(word):
                stripped = stripped[len(word):].lstrip()
        if not stripped.startswith("{"):
            continue
        opening = text.index("{", close)
        depth = 0
        for position in range(opening, len(text)):
            if text[position] == "{":
                depth += 1
            elif text[position] == "}":
                depth -= 1
                if depth == 0:
                    return (opening, position + 1)
        return None


def body_of(text, name):
    span = body_span(text, name)
    return text[span[0]:span[1]] if span else None


def require_body(bodies, name, violations):
    body = bodies.get(name)
    if body is None:
        violations.append(("структура", "функция '%s' не найдена — проверка о "
                                        "ней ничего не может сказать" % name))
    return body


FLAGS = ("_characterNameIndexComplete", "_guildNameIndexComplete")


def check_one_snapshot(text, bodies, spans, violations):
    """B1: полнота индекса читается для ОТВЕТА ровно в одном месте.

    ★РАЗРЕШЁННЫЕ МЕСТА НАЗВАНЫ ПОИМЁННО, А НЕ ВЫВЕДЕНЫ. Флаг законно пишут
    перестройка и `Mark*NameIndexBroken`, законно читает короткое замыкание
    `Reconcile*NameIndexIfBroken` («чинить ли вообще») и обмен в `Index*Name`.
    Всё остальное чтение — это ОТВЕТ НА ВОПРОС ОБ ИМЕНИ, а он обязан приходить
    одним снимком вместе с содержимым карты.
    """
    allowed = []
    for name in ("ReadNameIndexAnswer", "ReconcileCharacterNameIndexIfBroken",
                 "ReconcileGuildNameIndexIfBroken", "RebuildCharacterNameIndex",
                 "RebuildGuildNameIndex", "MarkCharacterNameIndexBroken",
                 "MarkGuildNameIndexBroken", "IndexCharacterName",
                 "IndexGuildName"):
        span = spans.get(name)
        if span:
            allowed.append(span)
    if spans.get("ReadNameIndexAnswer") is None:
        violations.append(("один-снимок",
                           "ReadNameIndexAnswer не найден — читать полноту "
                           "стало опять негде и везде"))
        return
    for flag in FLAGS:
        start = 0
        while True:
            at = text.find(flag, start)
            if at < 0:
                break
            start = at + len(flag)
            if any(begin <= at < end for begin, end in allowed):
                continue
            # Передача помощнику — не чтение ответа: он и есть тот, кто читает.
            if "ReadNameIndexAnswer(" in text[max(0, at - 200):at]:
                continue
            line_start = text.rfind("\n", 0, at) + 1
            line_end = text.find("\n", at)
            line = text[line_start:line_end if line_end > 0 else len(text)]
            if "std::atomic_bool" in line:
                continue            # объявление члена
            violations.append((
                "один-снимок",
                "'%s' читается вне ReadNameIndexAnswer: %s"
                % (flag, line.strip())))


def check_repair_is_scheduled(text, bodies, violations):
    """B2: ремонт индекса не покупается поиском, а зовётся по расписанию."""
    for name in ("IsCharacterNameUnique", "IsGuildNameUnique",
                 "RetrieveCharacterUidByName"):
        body = require_body(bodies, name, violations)
        if body is None:
            continue
        if "Reconcile" in body:
            violations.append((
                "ремонт-по-расписанию",
                "'%s' зовёт перестройку индекса — имя с провода покупает "
                "обход каталога" % name))
    tick = require_body(bodies, "TickNameIndexMaintenance", violations)
    if tick is not None:
        for callee in ("ReconcileCharacterNameIndexIfBroken",
                       "ReconcileGuildNameIndexIfBroken"):
            if callee not in tick:
                violations.append((
                    "ремонт-по-расписанию",
                    "плановый проход не зовёт '%s' — этот индекс не чинится "
                    "ничем" % callee))


def check_tick_is_wired(director_text, violations):
    """B2 (вторая половина): плановый проход кто-то реально зовёт.

    ★СМОТРИМ ТЕЛО `DataDirector::Tick`, А НЕ ВЕСЬ ТЕКСТ. Искать имя во всём
    дереве значило бы находить его собственное ОПРЕДЕЛЕНИЕ и всегда говорить
    «проводка на месте» — ложно-зелёное по построению.
    """
    tick = body_of(director_text, "Tick")
    if tick is None:
        violations.append((
            "ремонт-по-расписанию",
            "DataDirector::Tick не найден — проводку планового прохода "
            "проверить нечем"))
        return
    if "TickNameIndexMaintenance()" not in tick:
        violations.append((
            "ремонт-по-расписанию",
            "DataDirector::Tick не зовёт TickNameIndexMaintenance — повод "
            "ремонта снова только клиентский"))


def check_invalidate_before_mutate(bodies, violations):
    """B4: полнота снимается ДО первой мутации карт, под тем же замком."""
    for name in ("IndexCharacterName", "IndexGuildName"):
        body = require_body(bodies, name, violations)
        if body is None:
            continue
        clears = body.find(".exchange(")
        mutations = [body.find("DetachNameKey("), body.find("AttachNameKey(")]
        mutations = [position for position in mutations if position >= 0]
        if clears < 0:
            violations.append((
                "снять-полноту-до-мутации",
                "'%s' не снимает полноту индекса перед мутацией" % name))
            continue
        if not mutations:
            violations.append((
                "снять-полноту-до-мутации",
                "'%s' не мутирует карты — проверка потеряла свой предмет" % name))
            continue
        if clears > min(mutations):
            violations.append((
                "снять-полноту-до-мутации",
                "'%s' снимает полноту ПОСЛЕ первой мутации: читатель успеет "
                "увидеть полукарту при полноте 'да'" % name))


def check_guard_split(bodies, violations):
    """B3: поиск и создание судятся РАЗНЫМИ гейтами, и не наоборот."""
    expectations = (
        ("RetrieveCharacterUidByName", "IsLookupKeyShaped", "IsStorableNameShaped"),
        ("IsCharacterNameUnique", "IsStorableNameShaped", "IsLookupKeyShaped"),
        ("IsGuildNameUnique", "IsStorableNameShaped", "IsLookupKeyShaped"),
    )
    for name, wanted, forbidden in expectations:
        body = require_body(bodies, name, violations)
        if body is None:
            continue
        if wanted not in body:
            violations.append((
                "гейт-поиска-отдельно",
                "'%s' не зовёт '%s'" % (name, wanted)))
        if forbidden in body:
            violations.append((
                "гейт-поиска-отдельно",
                "'%s' судит имя гейтом '%s' — это гейт другого направления"
                % (name, forbidden)))


def check_canonical_identity(bodies, violations):
    """W5: личность записи — каноническое имя; пол счётчика — любое числовое."""
    parser = require_body(bodies, "ParseRecordUid", violations)
    # ★Литерал '0' здесь НЕ ищется: нормализатор зачищает содержимое литералов,
    # и проверка по нему была бы слепой по построению. Ищется ФОРМА условия.
    if parser is not None and "stem.size() > 1 && stem.front() ==" not in parser:
        violations.append((
            "каноническая-личность",
            "ParseRecordUid не отвергает ведущий ноль: '007.json' снова "
            "делит личность с '7.json'"))
    floor = require_body(bodies, "HighestUidInDirectory", violations)
    if floor is not None:
        if "ParseUidLikeStem(" not in floor:
            violations.append((
                "каноническая-личность",
                "пол счётчика uid не зовёт снисходительный разбор — алиасы "
                "перестали резервироваться и uid можно выдать занятым"))
        if "ParseRecordUid(" in floor:
            violations.append((
                "каноническая-личность",
                "пол счётчика uid судит имена строгим разбором — пол опустится "
                "ровно на тех именах, ради которых заведён"))
    for name in ("RebuildCharacterNameIndex", "RebuildGuildNameIndex"):
        body = require_body(bodies, name, violations)
        if body is not None and "ParseRecordUid(" not in body:
            violations.append((
                "каноническая-личность",
                "'%s' не берёт личность записи строгим разбором" % name))


def check_scan_generation(bodies, violations):
    """W6: публикация обхода учитывает поколение своих неудач."""
    publisher = require_body(bodies, "RebuildUserNameIndexUnderRebuildGuard",
                             violations)
    if publisher is not None:
        at = publisher.find("_userIndexStampValid.store(")
        window = publisher[at:at + 400] if at >= 0 else ""
        if "_userIndexFailedGeneration" not in window:
            violations.append((
                "поколение-обхода",
                "публикация отпечатка не смотрит на поколение своих неудач: "
                "устаревший обход снова перезапишет 'false' на 'true'"))
    indexer = require_body(bodies, "IndexUserName", violations)
    if indexer is not None and "_userIndexFailedGeneration" not in indexer:
        violations.append((
            "поколение-обхода",
            "неудача регистрации имени не отмечает своё поколение"))


CHECKS = ("один-снимок", "ремонт-по-расписанию", "снять-полноту-до-мутации",
          "гейт-поиска-отдельно", "каноническая-личность", "поколение-обхода")


def analyse(tree):
    violations = []
    joined = ""
    director_text = ""
    bodies = {}
    for relative in SCANNED:
        path = os.path.join(tree, relative)
        if not os.path.isfile(path):
            die("нет файла %s — смотреть нечего" % path)
        text = normalized(path)
        if relative.endswith("DataDirector.cpp"):
            director_text = text
        joined += text
    spans = {}
    for name in ("ReadNameIndexAnswer", "IsCharacterNameUnique",
                 "IsGuildNameUnique", "RetrieveCharacterUidByName",
                 "TickNameIndexMaintenance", "IndexCharacterName",
                 "IndexGuildName", "ParseRecordUid", "HighestUidInDirectory",
                 "RebuildCharacterNameIndex", "RebuildGuildNameIndex",
                 "RebuildUserNameIndexUnderRebuildGuard", "IndexUserName",
                 "ReconcileCharacterNameIndexIfBroken",
                 "ReconcileGuildNameIndexIfBroken",
                 "MarkCharacterNameIndexBroken", "MarkGuildNameIndexBroken"):
        spans[name] = body_span(joined, name)
        bodies[name] = body_of(joined, name)

    check_one_snapshot(joined, bodies, spans, violations)
    check_repair_is_scheduled(joined, bodies, violations)
    check_tick_is_wired(director_text, violations)
    check_invalidate_before_mutate(bodies, violations)
    check_guard_split(bodies, violations)
    check_canonical_identity(bodies, violations)
    check_scan_generation(bodies, violations)
    return violations


def selftest():
    """Каждая проверка обязана покраснеть на своей канарейке."""
    source = os.path.join(TREE, SCANNED[0])
    original = open(source, encoding="utf-8").read()
    director = os.path.join(TREE, SCANNED[1])
    original_director = open(director, encoding="utf-8").read()

    canaries = [
        ("один-снимок",
         "  if (answer.uid != data::InvalidUid)\n    return false;\n"
         "  if (answer.complete)\n    return true;\n\n"
         "  // \u2605\u041f\u0420\u041e\u041c\u0410\u0425",
         "  if (answer.uid != data::InvalidUid)\n    return false;\n"
         "  if (_characterNameIndexComplete.load(std::memory_order::relaxed))\n"
         "    return true;\n\n"
         "  // \u2605\u041f\u0420\u041e\u041c\u0410\u0425"),
        ("ремонт-по-расписанию",
         "  RequestScheduledNameIndexRepair();\n  return false;\n}\n\nvoid server::FileDataSource::CreateHorse",
         "  ReconcileCharacterNameIndexIfBroken();\n  return false;\n}\n\nvoid server::FileDataSource::CreateHorse"),
        ("снять-полноту-до-мутации",
         "    const bool wasComplete = _characterNameIndexComplete.exchange(\n"
         "      false, std::memory_order::relaxed);\n\n    if (previous != _characterUidToName.end())",
         "    if (previous != _characterUidToName.end())"),
        ("гейт-поиска-отдельно",
         "  if (not server::util::IsLookupKeyShaped(",
         "  if (not server::util::IsStorableNameShaped("),
        ("каноническая-личность",
         "  if (stem.size() > 1 && stem.front() == '0')\n    return std::nullopt;\n",
         ""),
        ("поколение-обхода",
         "        && _userIndexFailedGeneration.load(std::memory_order::relaxed)\n"
         "             < generation,",
         "        ,"),
    ]

    failures = []
    print("=== самопроверка гейта инвариантов индекса имён ===")
    clean = analyse(TREE)
    if clean:
        for reason, detail in clean:
            print("  ✗ %s: %s" % (reason, detail))
        failures.append("чистое дерево уже нарушает инварианты")
    else:
        print("  ✓ чистое дерево: 0 нарушений")

    for reason, needle, replacement in canaries:
        if original.count(needle) != 1:
            failures.append("канарейка '%s': якорь не найден ровно один раз "
                            "(найдено %d)" % (reason, original.count(needle)))
            continue
        open(source, "w", encoding="utf-8").write(
            original.replace(needle, replacement))
        try:
            found = analyse(TREE)
        finally:
            open(source, "w", encoding="utf-8").write(original)
        reasons = {item[0] for item in found}
        if reason in reasons:
            print("  ✓ канарейка '%s' поймана" % reason)
        else:
            print("  ✗ канарейка '%s' НЕ поймана (найдено: %s)"
                  % (reason, sorted(reasons) or "ничего"))
            failures.append("проверка '%s' не умеет покраснеть" % reason)

    # Отдельная канарейка на проводку планового прохода — она в другом файле.
    needle = "    _fileDataSource->TickNameIndexMaintenance();"
    if original_director.count(needle) != 1:
        failures.append("канарейка проводки: якорь не найден ровно один раз")
    else:
        open(director, "w", encoding="utf-8").write(
            original_director.replace(needle, "    (void)_fileDataSource;"))
        try:
            found = analyse(TREE)
        finally:
            open(director, "w", encoding="utf-8").write(original_director)
        if any("плановый проход" in detail or "Tick не зовёт" in detail
               for _, detail in found):
            print("  ✓ канарейка 'проводка планового прохода' поймана")
        else:
            print("  ✗ канарейка 'проводка планового прохода' НЕ поймана")
            failures.append("проводка планового прохода не проверяется")

    if failures:
        for failure in failures:
            print("  ОСТАНОВ: %s" % failure)
        print("=== ИТОГ: ПРОВЕРКА НЕДЕЙСТВИТЕЛЬНА ===")
        return 2
    print("=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ ===")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    violations = analyse(TREE)
    print("=== gate: инварианты индексов имён ===")
    print("дерево          : %s" % TREE)
    print("файлов прочитано: %d" % len(SCANNED))
    print("проверок        : %d (%s)" % (len(CHECKS), ", ".join(CHECKS)))
    print("нарушений       : %d (ожидалось 0)" % len(violations))
    for reason, detail in violations:
        print("  ✗ %s: %s" % (reason, detail))
    if violations:
        print("=== ИТОГ: НАРУШЕНО ===")
        return 1
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
