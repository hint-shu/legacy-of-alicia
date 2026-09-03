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
    """Текст файла после тех же фаз трансляции, что применяют соседние гейты.

    ★ЧИТАЕТСЯ ВЫВОД, А НЕ КОД ВОЗВРАТА (правка ревью, итерация 11). Режим
    `--unsupported` ПЕЧАТАЕТ находки и ВСЕГДА возвращает ноль — он задуман как
    справка для вызывающего, а не как вердикт. Прежняя редакция смотрела ровно
    на код возврата, поэтому псевдоним `std`, склейка `##` и включение через
    макрос молча НОРМАЛИЗОВЫВАЛИСЬ вместо останова: гейт объявлял чистым текст,
    который прочитать не умеет ([[a-check-nobody-reads-is-not-a-check]]).
    Здесь останов даёт ЛЮБАЯ непустая находка, а ненулевой код — отдельно.
    """
    unsupported = subprocess.run(
        [sys.executable, SECRET_GATE, "--unsupported", path],
        capture_output=True, text=True)
    if unsupported.returncode != 0:
        sys.stdout.write(unsupported.stdout)
        sys.stderr.write(unsupported.stderr)
        die("%s: детектор неподдерживаемых форм отказал (код %d)"
            % (path, unsupported.returncode))
    findings = [line for line in unsupported.stdout.splitlines() if line.strip()]
    if findings:
        for line in findings:
            print("  ✗ неподдерживаемая форма: %s" % line, file=sys.stderr)
        die("%s: %d неподдерживаемая(ых) форма(ы) препроцессора — разобрать "
            "нельзя, «чисто» отсюда не следует" % (path, len(findings)))
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
    # ★ТЕЛО ПРОХОДА ЖИВЁТ В `TickNameIndexMaintenanceAt` (итерация 11): у
    # свойства «пол не опускается просьбой» иначе нет наблюдателя, а взять
    # верхнюю обёртку значило бы судить одну строку делегирования.
    entry = require_body(bodies, "TickNameIndexMaintenance", violations)
    if entry is not None and "TickNameIndexMaintenanceAt(" not in entry:
        violations.append((
            "ремонт-по-расписанию",
            "боевой плановый проход не делегирует TickNameIndexMaintenanceAt — "
            "у прохода снова два тела"))
    tick = require_body(bodies, "TickNameIndexMaintenanceAt", violations)
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


def write_sites(text, member):
    """Смещения всех ЗАПИСЕЙ в член `member` — перепись, а не список мест.

    ★КЛЮЧ — ДЕФЕКТ, А НЕ ФОРМА ОДНОГО МЕСТА. Ищутся ВСЕ вхождения имени, и
    каждое классифицируется: `.load(` — чтение, всё прочее (`.store(`,
    `.exchange(`, `.fetch_*(`, присваивание) — запись. Перечислять «известные
    места записи» значило бы завести список, который следующий автор пополнит
    молча ([[total-invariant-beats-list-of-sites]]).
    """
    sites = []
    start = 0
    while True:
        at = text.find(member, start)
        if at < 0:
            return sites
        start = at + len(member)
        tail = text[start:start + 24].lstrip()
        if tail.startswith(".load("):
            continue
        line_start = text.rfind("\n", 0, at) + 1
        if "std::atomic" in text[line_start:at]:
            continue                      # объявление члена
        sites.append(at)


def line_at(text, at):
    line_start = text.rfind("\n", 0, at) + 1
    line_end = text.find("\n", at)
    return text[line_start:line_end if line_end > 0 else len(text)].strip()


def check_repair_floor_is_not_client_resettable(text, bodies, spans, violations):
    """B2 (итерация 11): просьба с провода НЕ умеет опустить пол ремонта.

    ★СЕМАНТИЧЕСКАЯ, А НЕ ПО ТОКЕНУ. Прежняя проверка спрашивала «зовёт ли поиск
    Reconcile» — и молчала о том, ЧТО ИМЕННО делал путь запроса: он ставил часы
    планового прохода в прошлое, то есть отменял шестидесятисекундный пол,
    оставаясь при этом «не зовущим Reconcile». Здесь переписываются ВСЕ записи
    в часы: писать их вправе только тело планового прохода.
    """
    clock = "_nameIndexMaintenanceLastRun"
    tick_span = spans.get("TickNameIndexMaintenanceAt")
    if tick_span is None:
        violations.append((
            "пол-ремонта",
            "TickNameIndexMaintenanceAt не найден — часы планового прохода "
            "проверить нечем"))
        return
    for at in write_sites(text, clock):
        if tick_span[0] <= at < tick_span[1]:
            continue
        violations.append((
            "пол-ремонта",
            "часы планового прохода пишутся вне его тела: %s — пол ремонта "
            "снова отменяем снаружи" % line_at(text, at)))

    request = require_body(bodies, "RequestScheduledNameIndexRepair", violations)
    if request is not None:
        if clock in request:
            violations.append((
                "пол-ремонта",
                "путь запроса трогает часы планового прохода — имя с провода "
                "снова покупает обход раньше пола"))
        if "_nameIndexRepairPending" not in request:
            violations.append((
                "пол-ремонта",
                "путь запроса не взводит флаг просьбы — просить стало нечем"))

    tick = text[tick_span[0]:tick_span[1]]
    floor = tick.find("kScheduledNameIndexRepairGap")
    store = tick.find(clock + ".store(")
    stop = tick.find("return;")
    if floor < 0 or store < 0 or stop < 0 or not floor < stop < store:
        violations.append((
            "пол-ремонта",
            "плановый проход переставляет часы, не выйдя раньше по полу "
            "kScheduledNameIndexRepairGap — пола больше нет"))
    if "_nameIndexRepairPending" not in tick:
        violations.append((
            "пол-ремонта",
            "плановый проход не смотрит на флаг просьбы — просьба потеряна"))


def check_stamp_has_one_locked_writer(text, bodies, spans, violations):
    """W6 (итерация 11): отметку индекса аккаунтов пишет ОДНА функция, под замком.

    ★СЕМАНТИЧЕСКАЯ, А НЕ ПО ПРИСУТСТВИЮ ТОКЕНА. Прежняя проверка требовала,
    чтобы рядом с публикацией ВСТРЕЧАЛОСЬ слово `_userIndexFailedGeneration`, —
    и это выполнялось при живой чередовке: путь отказа писал `false` и поднимал
    поколение ДВУМЯ записями без замка, а обход между ними публиковал `true`.
    Здесь проверяется само свойство: писатель ровно один, он под замком, и
    отметка о неудаче поднимает поколение в ТОЙ ЖЕ критической секции.
    """
    writer = "StoreUserIndexStampValidLocked"
    writer_span = spans.get(writer)
    if writer_span is None:
        violations.append((
            "отметка-под-замком",
            "функция %s не найдена — единственного писателя отметки нет"
            % writer))
        return
    sites = write_sites(text, "_userIndexStampValid")
    inside = [at for at in sites if writer_span[0] <= at < writer_span[1]]
    for at in sites:
        if at in inside:
            continue
        violations.append((
            "отметка-под-замком",
            "отметка индекса аккаунтов пишется вне %s: %s — правило "
            "публикации снова существует в двух экземплярах"
            % (writer, line_at(text, at))))
    if len(inside) != 1:
        violations.append((
            "отметка-под-замком",
            "единственный писатель отметки пишет её %d раз(а) — писателя "
            "снова нет" % len(inside)))
    body = text[writer_span[0]:writer_span[1]]
    if "_userIndexFailedGeneration" not in body:
        violations.append((
            "отметка-под-замком",
            "писатель отметки не смотрит на поколение неудач"))

    failure = require_body(bodies, "MarkUserIndexFailure", violations)
    if failure is not None:
        if "_userNameIndexMutex" not in failure:
            violations.append((
                "отметка-под-замком",
                "отметка о неудаче ставится БЕЗ замка индекса: между её "
                "половинами снова помещается опоздавший обход"))
        for needed in (writer + "(", "RaiseAtomicWatermark("):
            if needed not in failure:
                violations.append((
                    "отметка-под-замком",
                    "отметка о неудаче не делает %s — половины протокола "
                    "разъехались" % needed))
    indexer = require_body(bodies, "IndexUserName", violations)
    if indexer is not None and "MarkUserIndexFailure()" not in indexer:
        violations.append((
            "отметка-под-замком",
            "путь отказа регистрации имени не отмечает неудачу"))

    # Каждый ЗОВУЩИЙ писателя обязан держать замок к моменту зова.
    start = 0
    while True:
        at = text.find(writer + "(", start)
        if at < 0:
            break
        start = at + 1
        if writer_span[0] <= at < writer_span[1]:
            continue
        if text[max(0, at - 2):at] == "::":
            continue                      # это ОПРЕДЕЛЕНИЕ писателя, не зов
        holder = None
        for name, span in spans.items():
            if span and span[0] <= at < span[1]:
                if holder is None or span[0] > spans[holder][0]:
                    holder = name
        if holder is None:
            violations.append((
                "отметка-под-замком",
                "писателя отметки зовут из неизвестного места — под замком ли "
                "этот зов, сказать нечем: %s" % line_at(text, at)))
            continue
        if "_userNameIndexMutex" not in text[spans[holder][0]:at]:
            violations.append((
                "отметка-под-замком",
                "%s зовёт писателя отметки, не взяв замок индекса" % holder))


CHECKS = ("один-снимок", "ремонт-по-расписанию", "снять-полноту-до-мутации",
          "гейт-поиска-отдельно", "каноническая-личность",
          "пол-ремонта", "отметка-под-замком")


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
                 "TickNameIndexMaintenanceAt",
                 "RequestScheduledNameIndexRepair",
                 "StoreUserIndexStampValidLocked", "MarkUserIndexFailure",
                 "TryPublishStaleUserIndexStampForTest",
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
    check_repair_floor_is_not_client_resettable(
        joined, bodies, spans, violations)
    check_stamp_has_one_locked_writer(joined, bodies, spans, violations)
    return violations


UNSUPPORTED_FIXTURES = (
    ("псевдоним-std", "namespace alias = std;\nint value = 0;\n"),
    ("макрос-склейка", "#define JOIN(a, b) a ## b\nint JOIN(x, y) = 0;\n"),
    ("включение-через-макрос", "#define HEADER <vector>\n#include HEADER\n"),
)


def selftest_unsupported():
    """Непрочитываемая форма обязана ОСТАНАВЛИВАТЬ, а не нормализоваться.

    ★ЭТО САМА НАХОДКА ИТЕРАЦИИ 10 (№3), А НЕ ЕЁ ПЕРЕСКАЗ. Режим `--unsupported`
    печатает находки и ВСЕГДА возвращает ноль; гейт смотрел на код возврата и
    потому объявлял чистым текст, который прочитать не умеет. Здесь каждая из
    трёх форм подаётся на вход настоящему `normalized()`, и от него требуется
    ОСТАНОВ с кодом 2 — а от чистого файла требуется тишина. Проверка, чья
    канарейка молчит, — ОСТАНОВ ([[a-gate-must-prove-itself-first]]).
    """
    failures = []
    with tempfile.TemporaryDirectory() as sandbox:
        clean = os.path.join(sandbox, "clean.cpp")
        with open(clean, "w", encoding="utf-8") as handle:
            handle.write("#include <vector>\nint value = 0;\n")
        try:
            normalized(clean)
            print("  ✓ читаемый файл проходит нормализацию")
        except SystemExit:
            failures.append("нормализатор останавливается на ЧИТАЕМОМ файле")

        for name, payload in UNSUPPORTED_FIXTURES:
            fixture = os.path.join(sandbox, "fixture.cpp")
            with open(fixture, "w", encoding="utf-8") as handle:
                handle.write(payload)
            try:
                normalized(fixture)
            except SystemExit as stop:
                if stop.code == 2:
                    print("  ✓ форма %s даёт ОСТАНОВ" % name)
                    continue
                failures.append("форма %s остановила гейт кодом %s (ждали 2)"
                                % (name, stop.code))
                continue
            failures.append("форма %s молча НОРМАЛИЗОВАНА — гейт снова читает "
                            "код возврата вместо находок" % name)
    return failures


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
        # ★КАНАРЕЙКИ ИТЕРАЦИИ 11: ровно те две формы, в которых дефект БЫЛ.
        ("пол-ремонта",
         "  _nameIndexRepairPending.store(true, std::memory_order::relaxed);",
         "  _nameIndexMaintenanceLastRun.store(\n"
         "    std::chrono::steady_clock::time_point{}, std::memory_order::relaxed);"),
        ("отметка-под-замком",
         "  const std::unique_lock indexLock(_userNameIndexMutex);\n"
         "  // Порядок половин внутри секции безразличен",
         "  // Порядок половин внутри секции безразличен"),
    ]

    failures = []
    print("=== самопроверка гейта инвариантов индекса имён ===")
    failures.extend(selftest_unsupported())
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
