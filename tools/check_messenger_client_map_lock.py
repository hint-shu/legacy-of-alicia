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

★ЧЕГО ГЕЙТ НЕ УТВЕРЖДАЕТ, И ПОЧЕМУ ПРЕЖНЯЯ ФОРМУЛИРОВКА БЫЛА НЕВЕРНА.
Раньше здесь стояло, что незалоченные места безопасны, «потому что читатели — на
СВОЁМ потоке чата». После R78 это больше не довод (NIT ревю #3 №1): фаза 1
гашения — ПЕРВЫЙ чужепоточный писатель этих полей, поэтому незалоченные
чтения/записи `ClientContext` вне поднадзорных функций стали формально гоночными.
Порчи памяти из этого не следует — фаза 1 не вставляет и не удаляет, структурные
мутации остались на потоке чата, поля тривиальны, — но «безопасно» надо
обосновывать иначе, чем однопоточностью.

★ЧТО ИЗМЕНИЛ R82 (round82, director-clients-lock-hardening). До него гейт видел
только девять функций и ПРЯМО выводил из области видимости `GetClientContext`,
читателей buddy/letter/invite и две незалоченные ЗАПИСИ на потоке чата
(`clientContext.presence` в `HandleChatterUpdateState`, `isAuthenticated` в
`HandleChatterGuildLogin`) — закрытие класса целиком было работой размера
отдельного раунда, как для лобби им был R64-3/#215. R82 её сделал, и граница
гейта сдвинута вслед за кодом:
  * `GetClientContext` отдаёт КОПИЮ под `shared_lock` — он в `MUST_LOCK` и
    обязан объявить замок В СВОЁМ ТЕЛЕ (`MUST_DECLARE_LOCK`);
  * `MutateClientContext` — единственный путь записи значения, ему нужен
    ИСКЛЮЧИТЕЛЬНЫЙ замок в теле;
  * `GetClientContextLocked` — внутренний путь «вызывающий держит замок»: он
    НАМЕРЕННО вне `MUST_LOCK` (см. `GATE_EXCLUDED`), зато КАЖДЫЙ ЕГО ВЫЗОВ
    обязан стоять под замком;
  * шесть снимко-держателей (buddy add/add-reply/delete, letter send, chat
    invite, guild login) копировали/перебирали ВСЮ карту без замка — теперь они
    в `MUST_LOCK`;
  * запись `isAuthenticated` гильд-входа не «взята под замок», а УДАЛЕНА как
    избыточная: `GetClientContext(requireAuthentication=true)` бросил бы раньше,
    то есть она ставила true поверх гарантированного true.
Гейт по-прежнему не выдаёт себя за доказательство отсутствия гонок: он
структурный и стережёт форму, а не поток управления.

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
    # пишут значения записей (это и приводит сюда R78)
    "EvictOtherSessionsOfCharacter",
    "CloseSessionsOfCharacter",
    # LOA (R78-fix6, находка ревю W5): вход — ПИСАТЕЛЬ той же карты, и
    # предыдущая редакция гейта его не стерегла. Замок, взятый только
    # читателем, синхронизацией не является.
    "HandleChatterLogin",
    # LOA (R80-7, round80, backlog #235): раунд заводит в этом файле ДВЕ новые
    # функции, трогающие карту. Обе — писатели значений, обе обязаны стоять под
    # исключительным замком, и обе прошли бы мимо СПИСОЧНОГО гейта незамеченными.
    # `HandleClientActivity` пишет метку на КАЖДЫЙ входящий кадр (значит
    # совпадает с копированием карты с потоков ранча и заезда чаще всего
    # остального); `SweepChatSockets` обходит карту целиком и правит поля чужих
    # записей.
    "HandleClientActivity",
    "SweepChatSockets",
    # LOA (R82, round82, director-clients-lock-hardening): контракт R64-3 принят
    # целиком, и вместе с ним под надзор въезжают ДВА новых входа в карту и
    # ШЕСТЬ снимко-держателей, которые до раунда копировали/перебирали ВСЮ карту
    # без замка на потоке-владельце (`clientsSnapshot = _clients`,
    # незалоченный `find`, незалоченный `for`). Гейт R78 их прямо выводил из
    # области видимости (см. врезку выше) — R82 их туда вводит.
    "GetClientContext",
    "MutateClientContext",
    "HandleChatterBuddyAdd",
    "HandleChatterBuddyAddReply",
    "HandleChatterBuddyDelete",
    "HandleChatterLetterSend",
    "HandleChatterChatInvite",
    "HandleChatterGuildLogin",
}

#: ★`GetClientContextLocked` В `MUST_LOCK` НЕ ВНОСИТСЯ, И ЭТО НАМЕРЕННО.
#: Его контракт — «вызывающий держит замок»: незалоченное обращение к `_clients`
#: в теле и есть смысл метода (та же форма, что у лобби, R64-3). `judge` карает
#: только `func in MUST_LOCK and not protected`, поэтому исключение достигается
#: невнесением, а не особым случаем в коде. `FUNC_RE` его тело УЗНАЁТ (возврат
#: `ClientContext&`), так что атрибуция строк по функциям не сбивается.
#: Будущему раунду: внести его сюда — значит сделать гейт всегда-красным.
GATE_EXCLUDED = {"GetClientContextLocked"}

#: Функции, которые карту МЕНЯЮТ: им мало «какого-нибудь» замка, нужен
#: исключительный.
#: ★ЗАКРЫВАЕТ ОБХОД, НАЙДЕННЫЙ РЕВЮ (остаток 1): прежний `LOCK_RE` не различал
#: `shared_lock` и `unique_lock`, поэтому мутант, переводивший фазу 1 вытеснения
#: на РАЗДЕЛЯЕМЫЙ замок — то есть на запись под замком для чтения, — гейт
#: проходил насквозь.
MUST_LOCK_EXCLUSIVELY = {
    "HandleClientConnected",
    "HandleClientDisconnected",
    "EvictOtherSessionsOfCharacter",
    "CloseSessionsOfCharacter",
    "HandleChatterLogin",
    # LOA (R80-7): обе новые функции ПИШУТ значения записей. Разделяемого замка
    # им мало — «запись под замком для чтения» гейт обязан ловить, и ровно этот
    # мутант прежняя редакция пропускала.
    "HandleClientActivity",
    "SweepChatSockets",
    # LOA (R82): единственный путь ЗАПИСИ значения поля извне залоченного блока.
    # Разделяемого замка ему мало — «запись под замком для чтения» гейт обязан
    # ловить (тот же довод, что у R80-7 для `SweepChatSockets`).
    "MutateClientContext",
}

#: LOA (R82, round82, director-clients-lock-hardening): ФУНКЦИИ, КОТОРЫЕ ОБЯЗАНЫ
#: ОБЪЯВИТЬ ЗАМОК В СВОЁМ ТЕЛЕ.
#:
#: ★ЗАЧЕМ ОТДЕЛЬНОЕ ПРАВИЛО, ЕСЛИ ЕСТЬ `MUST_LOCK`. `MUST_LOCK` судит ОБРАЩЕНИЯ
#: к карте: «нашёл `_clients` — потребуй замок выше». После контракта-копии тело
#: `GetClientContext` не содержит токена `_clients` вовсе — поиск переехал в
#: `GetClientContextLocked`. Значит по правилу обращений `GetClientContext`
#: сторожить НЕЧЕГО: снимите из него `shared_lock`, и копия начнёт сниматься
#: незалоченной, а гейт промолчит — ровно «полнота по форме», от которой
#: [[gate-by-form-gives-false-completeness]]. Поэтому у двух методов контракта
#: требование прямое: замок обязан стоять В ТЕЛЕ.
MUST_DECLARE_LOCK = {
    "GetClientContext",
    "MutateClientContext",
}

#: LOA (R82): КАЖДЫЙ вызов `GetClientContextLocked(` обязан стоять ПОД замком.
#: Это вторая половина контракта «вызывающий держит замок»: без неё метод,
#: намеренно выведенный из-под `MUST_LOCK`, стал бы чёрным ходом к живой карте
#: без единой проверки. Единственные вызывающие сегодня — `GetClientContext`
#: (под своим `shared_lock`) и `HandleChatterLogin` (три места под своими
#: прямыми замками).
LOCKED_CALL_RE = re.compile(r"\bGetClientContextLocked\s*\(")

#: LOA (R82, round82, director-clients-lock-hardening): ВЫХОДЫ, ЗАПРЕЩЁННЫЕ
#: ВНУТРИ ЛЯМБДЫ `MutateClientContext`.
#:
#: ★ЗАЧЕМ ЭТО ЗДЕСЬ, А НЕ В `check_messenger_disconnect_outside_lock.py`.
#: Тот гейт ищет запретный вызов ПОД ТЕКСТОВЫМ ОБЪЯВЛЕНИЕМ ЗАМКА и до R82 видел
#: всё, потому что все замки объявлялись прямо в телах обработчиков. Раунд
#: инкапсулирует исключительный замок ВНУТРЬ `MutateClientContext`, и лямбда,
#: которая под этим замком исполняется, стоит в тексте вызывающего БЕЗ единого
#: объявления замка над собой. Проверено вживую на негативе `neg-callout-msgr`
#: (`BroadcastPresenceOfCharacter` перенесён внутрь лямбды): соседний гейт
#: остаётся ЗЕЛЁНЫМ — то есть контракт R82 вводит выход, который старая проверка
#: физически не умеет увидеть ([[gate-by-form-gives-false-completeness]]).
#: Правило заведено ЗДЕСЬ, потому что `MutateClientContext` — сущность этого
#: раунда, а соседний гейт раунд обязан оставить зелёным без правок (регрессия).
#:
#: ★ЦЕНА ПРОПУСКА — НЕ КОСМЕТИКА. `_clientsMutex` нерекурсивный:
#: `BroadcastPresenceOfCharacter` изнутри лямбды снова берёт его снимком, glibc
#: возвращает EDEADLK, `std::shared_lock` бросает `system_error` — и рассылка
#: присутствия обрывается на каждом обновлении статуса. Это класс R59/R78.
MUTATION_FORBIDDEN = (
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
    "GetClientContext",
    "MutateClientContext",
    "QueueCommand",
    "GetDataDirector",
    "GetCharacter",
)
MUTATION_FORBIDDEN_RE = re.compile(
    r"\b(" + "|".join(MUTATION_FORBIDDEN) + r")\s*(?:<[^<>;{}]*>)?\s*\(")

#: LOA (R82-2, round82, находка ревю NIT-1): ПРАВИЛО КЛЮЧИТСЯ НА СВОЙСТВО, А НЕ
#: НА ИМЯ `MutateClientContext`.
#:
#: ★ЧТО БЫЛО НЕ ТАК. Первая редакция правила искала литерал
#: `MutateClientContext(`. Сегодня ложно-зелёного это не давало — метод такой в
#: мессенджере ровно один, — но проверка стерегла ИМЯ, а опасен КЛАСС: метод,
#: который берёт `_clientsMutex` ИСКЛЮЧИТЕЛЬНО внутри себя и исполняет под ним
#: ЛЯМБДУ ВЫЗЫВАЮЩЕГО. Заведи следующий раунд второй такой хелпер под другим
#: именем — и выход в чужой код под исключительным замком снова стал бы
#: невидимым, молча ([[gate-by-form-gives-false-completeness]]).
#:
#: ★ЧТО СЧИТАЕТСЯ СВОЙСТВОМ. Метод `MessengerDirector::X`, у которого
#: ОДНОВРЕМЕННО: (а) в списке параметров есть вызываемое, переданное
#: вызывающим — `std::function<…> имя` или (при шаблонном определении) `T&& имя`,
#: где `T` объявлен в `template <…>` над определением; (б) в теле объявлен
#: замок `_clientsMutex` ЛЮБОГО рода — `shared_lock` или `unique_lock` (почему
#: не только исключительный — врезка в `mutation_encapsulators`); (в) это
#: вызываемое в теле ВЫЗЫВАЕТСЯ (`имя(`). Тело такого метода — замок, и текст
#: лямбды на стороне вызывающего стоит БЕЗ единого объявления замка над собой:
#: соседний `check_messenger_disconnect_outside_lock.py` его физически не видит.
#:
#: ★ПОРОГ СЛЕПОТЫ ДЕТЕКТОРА. Правило по свойству само может ослепнуть (сменилась
#: форма объявления — и «инкапсуляторов не найдено», ноль лямбд, зелено). Поэтому
#: у детектора есть ПОЛ: он ОБЯЗАН заново найти известного члена класса. Не нашёл
#: — гейт недействителен (exit 2), а не чист ([[a-gate-must-prove-itself-first]]).
MUTATION_ENCAPSULATOR_FLOOR = {"MutateClientContext"}

#: Определение метода класса: `<тип> MessengerDirector::<Имя>(`.
#: ★ТИП НЕ ПЕРЕЧИСЛЯЕТСЯ (в отличие от `FUNC_RE`): здесь важно НЕ пропустить
#: будущий хелпер с любым типом возврата — перечисление типов было бы ровно той
#: слепотой по форме, от которой это правило и заводится.
DEF_HEAD_RE = re.compile(r"\bMessengerDirector::(\w+)\s*\(")
#: Параметр-вызываемое, форма 1: `std::function<…> имя` / `const std::function<…>& имя`.
FUNCTION_PARAM_RE = re.compile(
    r"std::function\s*<.*?>\s*&{0,2}\s*(\w+)\s*(?:,|\)|$)", re.DOTALL)
#: Параметр-вызываемое, форма 2: `T&& имя` при шаблонном определении.
TEMPLATE_HEAD_RE = re.compile(r"template\s*<([^>]*)>")
TEMPLATE_NAME_RE = re.compile(r"(?:typename|class)\s+(\w+)")

#: Ниже этого числа обращений файл заведомо не тот — проверка слепа.
#: ★ПОДНЯТ ПО ЗАМЕРУ, А НЕ НАУГАД (R80-7): на дереве R78 гейт находил 30
#: обращений при пороге 12 — то есть разбор мог ослепнуть больше чем вдвое и
#: по-прежнему печатать «чисто». Кандидат R80 даёт 39; порог ставится чуть ниже
#: замера, чтобы ловить именно слепоту, а не мелкую правку.
#: ★ПЕРЕИЗМЕРЕН R82. Форма изменилась: шесть «голых» `_clients` стали
#: лямбдами-снимками под замком, добавились `GetClientContext`,
#: `GetClientContextLocked`, `MutateClientContext` и их вызовы. Замер на
#: кандидате R82 — 44 обращения (на базе `a1171d0b` было 39). Порог ставится
#: чуть ниже НОВОГО замера. ★Читать этот гейт числом ОБРАЩЕНИЙ ВСЕГО, а не
#: числом «под замком»: порог гейтует именно первое.
MIN_ACCESSES = 40

#: ★ТИП ВОЗВРАТА ПЕРЕЧИСЛЯЕТСЯ, И ЭТО ЦЕНА ФОРМЫ: функция, чей тип здесь не
#: назван, для разбора НЕВИДИМА — её тело не приписывается никому, обращения из
#: него не считаются, и «0 нарушений» становится тише, чем должно быть.
#: R80 добавляет `chat::ReapThresholds MessengerDirector::GetReapThresholds()`.
#: ★R82 ДОБАВЛЯЕТ АЛЬТЕРНАТИВУ БЕЗ АМПЕРСАНДА, И ЭТО ПЕРВЫЙ ШАГ, БЕЗ КОТОРОГО
#: ВСЁ ОСТАЛЬНОЕ НЕДОСТИЖИМО. После перехода на контракт-копию
#: `GetClientContext` возвращает `MessengerDirector::ClientContext` БЕЗ `&`.
#: Прежний список типов знал только вариант с амперсандом — тело нового
#: `GetClientContext` стало бы для разбора невидимо, функция не попала бы в
#: `seen_functions`, и `missing = MUST_LOCK - seen` бросил бы `Invalid`, то есть
#: гейт дал бы exit 2 (недействителен) на ПОЧИНЕННОМ дереве.
#: ★ПОРЯДОК АЛЬТЕРНАТИВ ЗНАЧИМ: вариант с `&` стоит ПЕРВЫМ. Альтернация
#: проверяется слева направо, поэтому строка возврата-по-ссылке
#: (`GetClientContextLocked`) по-прежнему матчится вариантом с амперсандом, а
#: by-value — вариантом без него. Поставь их наоборот — и `ClientContext&`
#: разберётся как `ClientContext` с приклеенным `&`, а имя функции не найдётся.
FUNC_RE = re.compile(
    r"^(?:void|bool|std::optional<[^>]*>|MessengerDirector::ClientContext&|"
    r"MessengerDirector::ClientContext|"
    r"Config::Messenger&|chat::ReapThresholds)\s+MessengerDirector::(\w+)")
LOCK_RE = re.compile(r"std::(?:shared_lock|unique_lock)\s+lock\(\s*"
                     r"(?:director\.)?_clientsMutex\s*\)")
UNIQUE_LOCK_RE = re.compile(r"std::unique_lock\s+lock\(\s*"
                            r"(?:director\.)?_clientsMutex\s*\)")
ACCESS_RE = re.compile(r"(?<!_)_clients\b(?!Mutex)")
#: ЗАПИСЬ ЧЕРЕЗ ССЫЛКУ, полученную из `GetClientContext`. ★Гейт первой редакции
#: видел только текстовые обращения к `_clients` и такие записи ПРОПУСКАЛ — а
#: именно ими вход и портил карту, пока чужие потоки читали её под замком
#: (находка ревю W5). Ссылка указывает ВНУТРЬ карты, поэтому запись через неё —
#: то же обращение к карте, просто написанное иначе.
CONTEXT_WRITE_RE = re.compile(
    r"\bclientContext\.\w+\s*(?:=[^=]|\.emplace\(|\.reset\(\))")
#: ЧТЕНИЕ через ту же ссылку. ★Добавлено по NIT ревю #2 N2: гейт видел записи,
#! но не чтения, и новый гард повтора входа читал `isAuthenticated` и
#: `characterUid` без замка совершенно незаметно для него. Чтение поля, которое
#: чужой поток вправе переписать фазой 1 гашения, — та же гонка, что и запись.
CONTEXT_READ_RE = re.compile(r"\bclientContext\.\w+")


#: LOA (R82-2, round82, находка ревю WARN-1): ВЫХОДЫ В ЧУЖОЙ КОД, ПОСЛЕ КОТОРЫХ
#: СНИМОК КАРТЫ ПРОТУХ.
#:
#: ★ЗАЧЕМ ЭТО ОТДЕЛЬНОЕ ПРАВИЛО. Контракт-копия (`GetClientContext` отдаёт
#: значение) сделал БЕЗОПАСНЫМ то, что раньше было гонкой, и одновременно —
#: МЁРТВЫМ то, что раньше было гардом. Гард «а не сняли ли с сессии
#: аутентификацию, пока мы ходили в `DataDirector`» по замороженной копии
#: момента входа ВСЕГДА ложен: чужой поток фазы 1 гашения правит КАРТУ, а не
#: нашу копию. Ревю итерации 1 нашло ровно это в `HandleChatterGuildLogin`, и
#: правка на ЭТОМ месте без правила означала бы, что следующий такой же гард
#: умрёт молча ([[total-invariant-beats-list-of-sites]]).
#:
#: ★ПОЧЕМУ ИМЕННО `isAuthenticated`, А НЕ «любое поле». Это ЕДИНСТВЕННОЕ поле,
#: чья протухшая копия превращает ОТКАЗ в СОГЛАСИЕ: фаза 1 гашения снимает флаг,
#: и гард, читающий его по копии, пропускает погашенную сессию дальше.
#: `characterUid` после callout читают ЗАКОННО — как личность, уже проверенную
#: выше (`GetCharacter(clientContext.characterUid)`), и запрет на такое чтение
#: был бы ложным красным. Ограничение объявлено вслух, а не спрятано:
#: `characterUid` в гардах ОТКАЗА стережёт правило B ниже, поимённо.
CALLOUT_RE = re.compile(
    r"\b(GetDataDirector|GetCharacter|GetGuild|GetHorse|GetMail|GetItem|"
    r"Immutable|Mutable|QueueCommand|BroadcastPresenceOfCharacter|"
    r"DisconnectClient|SendStallionReward)\s*(?:<[^<>;{}]*>)?\s*\(")
#: Чтение `<переменная>.isAuthenticated` (не запись).
AUTH_READ_RE = re.compile(r"\b(\w+)\.isAuthenticated\b(?!\s*=[^=])")
#: Определение локальной переменной: `const auto X =`, `auto& X =`, `bool X =`,
#: `X =` — годится любое, важна СТРОКА, на которой значение получено.
def _def_re(name: str) -> re.Pattern:
    return re.compile(r"(?:^|[^\w.>])" + re.escape(name) + r"\s*=[^=]")

#: Правило B (поимённое): гарды ОТКАЗА гильдийного входа. Оба обязаны судить по
#: значению, взятому ПОСЛЕ последнего выхода в чужой код и ПОД замком карты.
#: ★ПОИМЁННОЕ — ПОТОМУ ЧТО ЭТО ФИКС ЭТОГО РАУНДА, и запись раунда обязана уметь
#: покраснеть именно на его откате, а не только на «похожем классе».
GUILD_LOGIN_FUNC = "HandleChatterGuildLogin"
GUILD_LOGIN_REFUSALS = (
    "GuildLoginClientNotAuthenticated",
    "CommandCharacterIsNotClientCharacter",
)


class Invalid(Exception):
    """Проверка недействительна (exit 2)."""


def _strip_comment(line: str) -> str:
    """Убрать `//`-комментарий и СТРОКОВЫЕ ЛИТЕРАЛЫ.

    ★ЛИТЕРАЛЫ УБИРАЮТСЯ РАДИ УЧЁТА СКОБОК. Формат-строки вроде
    `"... character {}"` несут фигурные скобки, и без вырезания они попадают в
    подсчёт глубины блока наравне с настоящими. Здесь такие пары оказывались
    сбалансированными и вреда не делали, но одна непарная скобка в тексте
    сообщения сдвинула бы всю глубину — то есть вердикт гейта зависел бы от
    прозы в логах.
    """
    idx = line.find("//")
    line = line if idx < 0 else line[:idx]
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def _function_body(text: str, name: str) -> str | None:
    """Тело функции `MessengerDirector::<name>` как текст, или None."""
    lines = text.splitlines()
    start = None
    for number, raw in enumerate(lines):
        match = FUNC_RE.match(raw)
        if match and match.group(1) == name:
            start = number
            break
    if start is None:
        return None
    depth = 0
    opened = False
    body = []
    for raw in lines[start:]:
        code = _strip_comment(raw)
        body.append(raw)
        depth += code.count("{")
        if depth > 0:
            opened = True
        depth -= code.count("}")
        if opened and depth <= 0:
            break
    return "\n".join(body)


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

        # LOA (R82): вызов внутреннего пути «вызывающий держит замок» —
        # обращение к карте по определению, и оно обязано стоять под замком.
        # Определение самого метода из учёта исключается: `FUNC_RE` уже назвал
        # текущую функцию, и `GetClientContextLocked` внутри себя не зовёт.
        if (LOCKED_CALL_RE.search(code)
                and func is not None
                and func not in GATE_EXCLUDED):
            protected_locked = bool(lock_depths)
            accesses.append((number, func, protected_locked))
            if not protected_locked:
                violations.append(
                    (number, func,
                     "GetClientContextLocked вызван БЕЗ замка: " + raw.strip()))

        is_access = bool(ACCESS_RE.search(code))
        # Запись и чтение через ссылку считаются одинаково: и то и другое —
        # обращение к элементу карты, просто записанное не через `_clients`.
        is_context_touch = (func in MUST_LOCK_EXCLUSIVELY
                            and (bool(CONTEXT_WRITE_RE.search(code))
                                 or bool(CONTEXT_READ_RE.search(code))))
        if (is_access or is_context_touch) and func is not None:
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
            # ★СРАВНЕНИЕ НЕСТРОГОЕ, И ЭТО ИСПРАВЛЕНИЕ ОШИБКИ НА ЕДИНИЦУ.
            # Замок, объявленный на глубине D, действует на ВСЕХ операторах
            # глубины D и выходит из области только когда блок закроется, то
            # есть когда глубина станет D-1. Прежнее `d < depth` снимало его
            # уже при возврате к D — достаточно было вложенного `if {…}`
            # ВНУТРИ залоченного блока, чтобы гейт объявил следующие строки
            # незалоченными. Пойман ложным красным на собственном дереве:
            # три записи входа стоят под `unique_lock`, а гейт назвал их
            # нарушителями после того, как между замком и ними появился
            # вложенный блок ранней проверки ключа.
            lock_depths = [d for d in lock_depths if d <= depth]
            if func is not None and depth <= func_depth:
                func = None
                lock_depths = []

    return accesses, violations, seen_functions


def mutation_encapsulators(text: str) -> dict[str, int]:
    """Найти методы, ИНКАПСУЛИРУЮЩИЕ исключительный `_clientsMutex` и
    исполняющие под ним лямбду ВЫЗЫВАЮЩЕГО. Ключ — имя, значение — строка
    определения.

    ★ИМЯ НЕ УЧАСТВУЕТ В РЕШЕНИИ. Решают три признака (см. врезку у
    `MUTATION_ENCAPSULATOR_FLOOR`): параметр-вызываемое, `unique_lock` на
    `_clientsMutex` в теле и вызов этого параметра в теле.
    """
    lines = text.splitlines()
    found: dict[str, int] = {}
    for index, raw in enumerate(lines):
        code = _strip_comment(raw)
        head = DEF_HEAD_RE.search(code)
        if not head:
            continue
        name = head.group(1)

        # Список параметров — по балансу круглых скобок от найденной `(`.
        depth = 0
        opened = False
        params: list[str] = []
        cursor = index
        offset = head.end() - 1
        while cursor < len(lines):
            chunk = _strip_comment(lines[cursor])[offset:]
            for char in chunk:
                if char == "(":
                    depth += 1
                    opened = True
                    if depth == 1:
                        continue
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        break
                if opened and depth >= 1:
                    params.append(char)
            offset = 0
            if opened and depth == 0:
                break
            params.append("\n")
            cursor += 1
        if not opened or depth != 0:
            continue
        param_text = "".join(params)

        # ★ЭТО ОБЪЯВЛЕНИЕ, А НЕ ОПРЕДЕЛЕНИЕ? Тела нет — свойства не проверить.
        tail = _strip_comment(lines[cursor])
        rest = tail[tail.rfind(")") + 1:]
        if ";" in rest and "{" not in rest:
            continue

        # Параметры-вызываемые: `std::function<…> имя` …
        callables = set(FUNCTION_PARAM_RE.findall(param_text))
        # … и `T&& имя`, если `T` объявлен в `template <…>` над определением.
        template_names: set[str] = set()
        for back in range(max(0, index - 3), index):
            head_match = TEMPLATE_HEAD_RE.search(_strip_comment(lines[back]))
            if head_match:
                template_names |= set(TEMPLATE_NAME_RE.findall(head_match.group(1)))
        for template_name in template_names:
            callables |= set(re.findall(
                r"\b" + re.escape(template_name) + r"\s*&&\s*(\w+)", param_text))
        if not callables:
            continue

        # ★ТЕЛО БЕРЁТСЯ ПО СКОБКАМ ОТ ЭТОГО ЖЕ ОПРЕДЕЛЕНИЯ, А НЕ ЧЕРЕЗ
        # `_function_body`: тот ищет функцию по `FUNC_RE`, а `FUNC_RE`
        # ПЕРЕЧИСЛЯЕТ типы возврата. Хелпер с типом вне списка стал бы для
        # детектора невидим — то есть правило по свойству снова ослепло бы по
        # форме, ровно от чего оно и заводится.
        body_depth = 0
        body_opened = False
        body_lines: list[str] = []
        for raw_body in lines[cursor:]:
            body_code = _strip_comment(raw_body)
            body_lines.append(raw_body)
            body_depth += body_code.count("{")
            if body_depth > 0:
                body_opened = True
            body_depth -= body_code.count("}")
            if body_opened and body_depth <= 0:
                break
        if not body_opened:
            continue
        body = "\n".join(body_lines)
        # ★ЗАМОК ЛЮБОГО РОДА, А НЕ ТОЛЬКО ИСКЛЮЧИТЕЛЬНЫЙ. Требуй тут
        # `unique_lock` — и мутант, переведший хелпер на `shared_lock`, выпал бы
        # из НАДЗОРА (детектор перестал бы его видеть) ровно тогда, когда он
        # опаснее всего. `shared_mutex` нерекурсивен в обе стороны: выход в
        # `BroadcastPresenceOfCharacter` из-под разделяемого замка — тот же
        # самозахват. «Исключительный он или нет» судит `MUST_LOCK_EXCLUSIVELY`,
        # и это ОТДЕЛЬНОЕ правило с отдельным вердиктом.
        if not LOCK_RE.search(body):
            continue
        if not any(re.search(r"\b" + re.escape(c) + r"\s*\(", body)
                   for c in callables):
            continue
        found[name] = index + 1
    return found


def mutation_lambdas(text: str, encapsulators: set[str] | None = None):
    """Вернуть (вызовов инкапсуляторов замка, нарушений внутри их лямбд).

    ★РАЗБОР ПО БАЛАНСУ КРУГЛЫХ СКОБОК, а не по строкам: аргумент-лямбда занимает
    несколько строк, и «следующие N строк» было бы догадкой. Область вызова —
    от строки с `MutateClientContext(` до строки, на которой баланс `(`/`)`
    вернулся к нулю. Первое вхождение самого имени в счёт не идёт, иначе вызов
    объявлял бы нарушителем сам себя.
    """
    names = sorted(encapsulators if encapsulators is not None
                   else mutation_encapsulators(text))
    if not names:
        return 0, []
    alternation = "|".join(re.escape(n) for n in names)
    call_re = re.compile(r"\b(?:" + alternation + r")\s*\(")
    #: Строка ОПРЕДЕЛЕНИЯ метода — не вызов; её из учёта исключаем.
    def_re = re.compile(r"MessengerDirector::(?:" + alternation + r")\s*\(")
    #: ★САМ ИНКАПСУЛЯТОР ВНУТРИ ЧУЖОЙ ЛЯМБДЫ — ТОЖЕ ВЫХОД: он берёт тот же
    #: нерекурсивный замок. Поэтому обнаруженные имена добавляются к запрету
    #: динамически, а не переписываются руками в `MUTATION_FORBIDDEN`.
    forbidden_re = re.compile(
        r"\b(" + "|".join(list(MUTATION_FORBIDDEN) + names)
        + r")\s*(?:<[^<>;{}]*>)?\s*\(")

    lines = text.splitlines()
    calls = 0
    violations = []
    index = 0
    while index < len(lines):
        code = _strip_comment(lines[index])
        if not call_re.search(code) or def_re.search(code):
            index += 1
            continue

        calls += 1
        start = index
        depth = 0
        opened = False
        region = []
        while index < len(lines):
            code = _strip_comment(lines[index])
            region.append((index + 1, lines[index], code))
            depth += code.count("(")
            if depth > 0:
                opened = True
            depth -= code.count(")")
            index += 1
            if opened and depth <= 0:
                break

        for number, raw, code in region:
            for match in forbidden_re.finditer(code):
                # Имя самого инкапсулятора на открывающей строке — это
                # разбираемый вызов, а не выход из-под замка.
                if number == start + 1 and match.group(1) in names:
                    continue
                violations.append((number, match.group(1), raw.strip()))

    return calls, violations


def _function_span(text: str, name: str) -> tuple[int, int] | None:
    """Границы тела `MessengerDirector::<name>` в номерах строк (1-based, вкл.)."""
    lines = text.splitlines()
    start = None
    depth = 0
    func_depth = 0
    for number, raw in enumerate(lines, 1):
        code = _strip_comment(raw)
        match = FUNC_RE.match(raw)
        if match and start is None and match.group(1) == name:
            start = number
            func_depth = depth
        opened = code.count("{")
        closed = code.count("}")
        depth += opened
        if closed:
            depth -= closed
            if start is not None and depth <= func_depth:
                return start, number
    return None


def _locked_lines(text: str) -> set[int]:
    """Номера строк, стоящих ПОД объявленным замком `_clientsMutex`.

    Та же механика областей, что у `analyse` (глубина фигурных скобок по коду с
    вырезанными комментариями), вынесенная сюда, чтобы правила уровня `judge`
    могли спросить «а эта строка вообще под замком?».
    """
    lines = text.splitlines()
    depth = 0
    lock_depths: list[int] = []
    locked: set[int] = set()
    for number, raw in enumerate(lines, 1):
        code = _strip_comment(raw)
        if LOCK_RE.search(code):
            lock_depths.append(depth)
        if lock_depths:
            locked.add(number)
        opened = code.count("{")
        closed = code.count("}")
        depth += opened
        if closed:
            depth -= closed
            lock_depths = [d for d in lock_depths if d <= depth]
    return locked


def stale_auth_reads(text: str):
    """ПРАВИЛО A. Чтение `X.isAuthenticated`, отделённое от определения `X`
    выходом в чужой код, — протухший гард.

    ★ЧТО ИМЕННО ЛОВИТСЯ. Копия, снятая ДО `GetCharacter(...).Immutable(...)`, к
    моменту чтения уже не отвечает на вопрос «сессия ещё аутентифицирована?»:
    флаг снимает ЧУЖОЙ поток в КАРТЕ. Значит чтение обязано быть свежим
    относительно ВСЕХ выходов между определением и собой.
    """
    lines = text.splitlines()
    violations = []
    func = None
    func_depth = 0
    depth = 0
    body_start = 0
    for number, raw in enumerate(lines, 1):
        code = _strip_comment(raw)
        match = FUNC_RE.match(raw)
        if match and func is None:
            func = match.group(1)
            func_depth = depth
            body_start = number

        if func is not None:
            for read in AUTH_READ_RE.finditer(code):
                holder = read.group(1)
                pattern = _def_re(holder)
                def_line = None
                for back in range(number - 1, body_start - 1, -1):
                    back_code = _strip_comment(lines[back - 1])
                    if pattern.search(back_code):
                        def_line = back
                        break
                    # Разбор структурной привязки: `for (auto& [id, X] : _clients)`
                    if re.search(r"\[\s*\w+\s*,\s*" + re.escape(holder)
                                 + r"\s*\]", back_code):
                        def_line = back
                        break
                if def_line is None:
                    # Держатель объявлен не в этой функции (параметр, поле) —
                    # правило о нём ничего не утверждает и молчит вслух.
                    continue
                between = [
                    n for n in range(def_line + 1, number)
                    if CALLOUT_RE.search(_strip_comment(lines[n - 1]))]
                if between:
                    violations.append((
                        number, func,
                        f"чтение {holder}.isAuthenticated по снимку строки "
                        f"{def_line}, между ними выход в чужой код (строка "
                        f"{between[0]}): гард судит ДОcallout-состояние"))

        opened = code.count("{")
        closed = code.count("}")
        depth += opened
        if closed:
            depth -= closed
            if func is not None and depth <= func_depth:
                func = None
    return violations


def guild_login_guard_violations(text: str):
    """ПРАВИЛО B. Оба гарда ОТКАЗА гильдийного входа обязаны судить по значению,
    взятому ПОСЛЕ последнего выхода в чужой код и ПОД замком карты.

    ★ЗАЧЕМ ПОИМЁННО, ЕСЛИ ЕСТЬ ПРАВИЛО A. Правило A стережёт КЛАСС и ключится на
    `isAuthenticated`; сверку личности (`characterUid`) оно намеренно не трогает,
    иначе ложно краснело бы на законных чтениях личности после callout. Откат
    ЭТОГО фикса — это откат ОБЕИХ веток сразу, и запись раунда обязана уметь
    покраснеть на нём целиком.
    """
    span = _function_span(text, GUILD_LOGIN_FUNC)
    if span is None:
        raise Invalid(f"функции {GUILD_LOGIN_FUNC} в файле нет — правило B "
                      "стерегло бы пустоту")
    start, end = span
    lines = text.splitlines()
    locked = _locked_lines(text)

    callouts = [n for n in range(start, end + 1)
                if CALLOUT_RE.search(_strip_comment(lines[n - 1]))]
    if not callouts:
        raise Invalid(f"в {GUILD_LOGIN_FUNC} не найдено ни одного выхода в чужой "
                      "код — разбор не сработал, правило B слепо")

    violations = []
    for refusal in GUILD_LOGIN_REFUSALS:
        sites = [n for n in range(start, end + 1) if refusal in lines[n - 1]
                 and "emplace" in _strip_comment(lines[n - 1])]
        if not sites:
            raise Invalid(f"ветка {refusal} в {GUILD_LOGIN_FUNC} не найдена — "
                          "правило B стерегло бы пустоту")
        site = sites[0]
        # Ближайшее `if (`/`else if (` ВЫШЕ ветки — её условие.
        guard_line = None
        for back in range(site - 1, start - 1, -1):
            if re.match(r"\s*(?:else\s+)?if\s*\(", _strip_comment(lines[back - 1])):
                guard_line = back
                break
        if guard_line is None:
            violations.append((site, GUILD_LOGIN_FUNC,
                               f"у ветки {refusal} не нашлось условия"))
            continue
        condition = _strip_comment(lines[guard_line - 1])
        # Выход в чужой код, стоящий ПЕРЕД гардом: снимок обязан быть свежее его.
        last_callout = max((n for n in callouts if n < guard_line), default=None)
        if last_callout is None:
            violations.append((guard_line, GUILD_LOGIN_FUNC,
                               f"гард {refusal} стоит ДО единственного выхода — "
                               "разбор не сработал"))
            continue
        identifiers = [i for i in re.findall(r"\b([A-Za-z_]\w*)\b", condition)
                       if i not in {"if", "else", "not", "and", "or", "command",
                                    "true", "false"}]
        fresh = False
        for name in identifiers:
            pattern = _def_re(name)
            for back in range(guard_line - 1, start - 1, -1):
                if pattern.search(_strip_comment(lines[back - 1])):
                    if back > last_callout and back in locked:
                        fresh = True
                    break
            if fresh:
                break
        if not fresh:
            violations.append((
                guard_line, GUILD_LOGIN_FUNC,
                f"гард {refusal} судит по значению, взятому ДО выхода в чужой "
                f"код (строка {last_callout}) или вне замка карты: {condition.strip()}"))
    return violations


def _owning_function(text: str, line_number: int) -> str | None:
    """Имя функции `MessengerDirector::<name>`, чьё ТЕЛО содержит эту строку.

    ★ЗАЧЕМ. Самопроверка печатала «снят замок со строки N» — номер строки не
    говорит читателю, ЧТО именно доказано. R80 добавляет в поднадзорный файл две
    функции, и требование к нему сформулировано ПОИМЁННО: канарейка обязана
    называть `SweepChatSockets` и `HandleClientActivity`, иначе «12 канареек
    поймано» не отличает их от двенадцати чужих.
    """
    lines = text.splitlines()
    func = None
    func_depth = 0
    depth = 0
    for number, raw in enumerate(lines, 1):
        code = _strip_comment(raw)
        match = FUNC_RE.match(raw)
        if match and func is None:
            func = match.group(1)
            func_depth = depth
        if number == line_number:
            return func
        opened = code.count("{")
        closed = code.count("}")
        depth += opened
        if closed:
            depth -= closed
            if func is not None and depth <= func_depth:
                func = None
    return None


def judge(tree: Path) -> int:
    path = tree / SOURCE
    if not path.is_file():
        raise Invalid(f"нет файла {path}")
    text = path.read_text(encoding="utf-8")

    accesses, violations, seen = analyse(text)

    exclusive_missing = []
    for name in sorted(MUST_LOCK_EXCLUSIVELY):
        body = _function_body(text, name)
        if body is None:
            raise Invalid(f"функции {name} в файле нет — гейт стерёг бы пустоту")
        if not UNIQUE_LOCK_RE.search(body):
            exclusive_missing.append(name)

    # LOA (R82): замок обязан стоять В ТЕЛЕ (см. MUST_DECLARE_LOCK).
    declare_missing = []
    for name in sorted(MUST_DECLARE_LOCK):
        body = _function_body(text, name)
        if body is None:
            raise Invalid(f"функции {name} в файле нет — гейт стерёг бы пустоту")
        if not LOCK_RE.search(body):
            declare_missing.append(name)

    if len(accesses) < MIN_ACCESSES:
        raise Invalid(
            f"обращений к карте найдено {len(accesses)}, минимум {MIN_ACCESSES} — "
            "разбор не сработал, «ноль нарушений» здесь ничего не значит")
    missing = sorted(MUST_LOCK - seen)
    if missing:
        raise Invalid(
            f"функции {missing} в файле нет — гейт стерёг бы то, чего не существует")

    guarded = [a for a in accesses if a[1] in MUST_LOCK]
    for name in exclusive_missing:
        violations.append((0, name, "меняет карту, но исключительного замка в "
                                    "функции нет (есть только разделяемый)"))
    for name in declare_missing:
        violations.append((0, name, "обязана объявить замок в СВОЁМ теле "
                                    "(контракт R82), а объявления нет"))
    # LOA (R82-2, находка ревю NIT-1): набор поднадзорных методов вычисляется
    # ПО СВОЙСТВУ, а пол ловит слепоту самого детектора.
    encapsulators = mutation_encapsulators(text)
    missing_floor = sorted(MUTATION_ENCAPSULATOR_FLOOR - set(encapsulators))
    if missing_floor:
        raise Invalid(
            f"детектор инкапсуляторов замка не нашёл {missing_floor} — правило "
            "по свойству ослепло, «ноль выходов под замком» здесь ничего не значит")
    mutation_calls, mutation_violations = mutation_lambdas(
        text, set(encapsulators))

    # LOA (R82-2, находка ревю WARN-1): гард, судящий ДОcallout-состояние.
    stale_violations = stale_auth_reads(text)
    for number, name, why in stale_violations:
        violations.append((number, name, why))
    guild_violations = guild_login_guard_violations(text)
    for number, name, why in guild_violations:
        violations.append((number, name, why))
    for number, name, code in mutation_violations:
        violations.append((number, "лямбда под инкапсулированным замком",
                           f"выход {name} ПОД исключительным замком: {code}"))
    print("=== gate: замок над картой клиентов мессенджера ===")
    print(f"дерево            : {tree}")
    print(f"обращений к карте : {len(accesses)} (минимум {MIN_ACCESSES}) — "
          f"включая записи через ссылку из GetClientContext")
    print(f"обязаны быть под замком : {len(guarded)} в {len(MUST_LOCK)} функциях")
    print(f"замок в СВОЁМ теле обязан : {len(MUST_DECLARE_LOCK) - len(declare_missing)}"
          f" из {len(MUST_DECLARE_LOCK)} (R82)")
    print(f"инкапсуляторов замка (по СВОЙСТВУ) : {len(encapsulators)} "
          f"[{', '.join(f'{n}:{ln}' for n, ln in sorted(encapsulators.items()))}]")
    print(f"лямбд под ними    : {mutation_calls}, выходов в них: "
          f"{len(mutation_violations)} (ожидалось 0)")
    print(f"протухших гардов isAuthenticated : {len(stale_violations)} "
          f"(правило A, ожидалось 0)")
    print(f"гардов отказа GuildLogin не по свежему снимку : "
          f"{len(guild_violations)} (правило B, ожидалось 0)")
    print(f"исключительный замок в теле : "
          f"{len(MUST_LOCK_EXCLUSIVELY) - len(exclusive_missing)} "
          f"из {len(MUST_LOCK_EXCLUSIVELY)}")
    print(f"нарушений         : {len(violations)} (ожидалось 0)")
    if violations:
        print("\nнарушители:")
        for number, func, code in violations:
            print(f"  {SOURCE}:{number}  {func}: {code[:80]}")
        print("\n=== ИТОГ: НАРУШЕНО ✗ ===")
        return 1
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


def _move_reread_above_callout(source: str) -> str:
    """Перенести блок пост-callout перечитывания ВЫШЕ выхода в `DataDirector`.

    Мутация текстовая и якорная: не нашла якорь — вернула исходник без правок, и
    самопроверка объявит фикстуру невнедрённой (провал), а не тихо зачтёт её.
    """
    block = (
        "  bool currentIsAuthenticated{false};\n"
        "  data::Uid currentCharacterUid{data::InvalidUid};\n"
        "  {\n"
        "    const std::shared_lock lock(_clientsMutex);\n"
        "    const auto currentIter = _clients.find(clientId);\n"
        "    if (currentIter != _clients.end())\n"
        "    {\n"
        "      currentIsAuthenticated = currentIter->second.isAuthenticated;\n"
        "      currentCharacterUid = currentIter->second.characterUid;\n"
        "    }\n"
        "  }\n\n")
    callout = ("  // Check if client belongs to the guild in the command\n"
               "  data::Uid characterGuildUid{data::InvalidUid};\n")
    if source.count(block) != 1 or source.count(callout) != 1:
        return source
    return source.replace(block, "", 1).replace(callout, block + callout, 1)


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

    named = set()
    for number in lock_lines:
        lines = original.splitlines()
        # Канарейка: замок снят (строка выброшена), всё остальное на месте.
        canary = "\n".join(lines[:number - 1] + lines[number:])
        _, canary_violations, _ = analyse(canary)
        owner = _owning_function(original, number) or "?"
        if canary_violations:
            named.add(owner)
            print(f"  ✓ канарейка «снят замок в {owner} (строка {number})» "
                  f"поймана ({len(canary_violations)} нарушений)")
        else:
            # Замок в функции, за которой гейт не следит, — это ЗАЯВЛЕННЫЙ предел.
            print(f"  · строка {number} ({owner}): замок не в поднадзорной "
                  "функции — гейт молчит, как и заявлено")

    # ★ИМЕННОЕ ТРЕБОВАНИЕ R82: раунд вводит в надзор шесть снимко-держателей и
    # два метода контракта. Без поимённого требования «N канареек поймано» не
    # отличает их от N чужих — ровно та же причина, по которой R80 назвал свои
    # две функции.
    required_r82 = {
        "HandleChatterBuddyAdd",
        "HandleChatterBuddyAddReply",
        "HandleChatterBuddyDelete",
        "HandleChatterLetterSend",
        "HandleChatterChatInvite",
        "HandleChatterGuildLogin",
    }

    # ★ИМЕННОЕ ТРЕБОВАНИЕ R80: без него «12 канареек» не доказывает, что гейт
    # видит именно ДВЕ НОВЫЕ функции раунда.
    required = {"HandleClientActivity", "SweepChatSockets"}
    if not required.issubset(named):
        print(f"САМОПРОВЕРКА ПРОВАЛЕНА: не названы поимённо {sorted(required - named)}")
        failures += 1
    if not required_r82.issubset(named):
        print("САМОПРОВЕРКА ПРОВАЛЕНА: не названы поимённо (R82) "
              f"{sorted(required_r82 - named)}")
        failures += 1

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

    # ★ФИКСТУРЫ УРОВНЯ `judge` (R82). Три правила раунда живут не в `analyse`,
    # а в `judge` (замок в СВОЁМ теле, исключительный замок, вызов `*Locked` под
    # замком), и снятием строки замка их не проверить. Каждая фикстура
    # впрыскивает РОВНО ОДНО нарушение в текст и требует красного.
    # ★ЭЛЕМЕНТ = (подпись, мутация, ОЖИДАЕМЫЙ КОД). Код указывается явно,
    # потому что часть канареек обязана дать не «красный» (1), а «зелёный» (0)
    # или «недействителен» (2), и «поймана» для них значит РАЗНОЕ.
    fixtures = (
        (1, "GetClientContext без замка в теле",
         lambda s: s.replace(
             "  const std::shared_lock lock(_clientsMutex);\n"
             "  return GetClientContextLocked(clientId, requireAuthentication);",
             "  return GetClientContextLocked(clientId, requireAuthentication);",
             1)),
        (1, "MutateClientContext под shared_lock вместо unique_lock",
         lambda s: s.replace(
             "  const std::unique_lock lock(_clientsMutex);\n\n"
             "  const auto clientContextIter = _clients.find(clientId);",
             "  const std::shared_lock lock(_clientsMutex);\n\n"
             "  const auto clientContextIter = _clients.find(clientId);",
             1)),
        (1, "снимок карты возвращён к незалоченному = _clients",
         lambda s: s.replace(
             "  const auto clientsSnapshot = [this]\n"
             "  {\n"
             "    const std::shared_lock lock(_clientsMutex);\n"
             "    return _clients;\n"
             "  }();",
             "  const auto clientsSnapshot = _clients;",
             1)),
        (1, "выход в чужой код внутри лямбды MutateClientContext",
         lambda s: s.replace(
             "      mutableClientContext.presence = command.presence;",
             "      mutableClientContext.presence = command.presence;\n"
             "      BroadcastPresenceOfCharacter(\n"
             "        mutableClientContext.characterUid, command.presence, "
             "clientId, nullptr);",
             1)),
        (1, "GetClientContextLocked вызван вне замка",
         lambda s: s.replace(
             "    const std::shared_lock lock(_clientsMutex);\n"
             "    const auto& clientContext = GetClientContextLocked(clientId, false);",
             "    const auto& clientContext = GetClientContextLocked(clientId, false);",
             1)),
    )

    # ★КАНАРЕЙКИ ПРАВИЛА ПО СВОЙСТВУ (R82-2, находка ревю NIT-1). Правило больше
    # не ключится на имя `MutateClientContext`, и доказать это можно только
    # ДРУГИМ ИМЕНЕМ: фикстура вводит хелпер `MutateClientState` — ту же форму
    # (исключительный `_clientsMutex` внутри + лямбда вызывающего), но чужое имя.
    # Троица обязательна целиком: красная (выход в лямбде ловится), ЗЕЛЁНАЯ (без
    # выхода гейт молчит — иначе «красный» доказывал бы лишь то, что появилась
    # новая функция) и НЕДЕЙСТВИТЕЛЬНАЯ (детектор ослеп → exit 2, а не «чисто»).
    encapsulator_helper = """bool MessengerDirector::MutateClientState(
  const network::ClientId clientId,
  const std::function<void(ClientContext&)>& stateMutation)
{
  const std::unique_lock lock(_clientsMutex);

  const auto stateIter = _clients.find(clientId);
  if (stateIter == _clients.end())
    return false;

  stateMutation(stateIter->second);
  return true;
}

void MessengerDirector::CanaryUseMutateClientState(const network::ClientId clientId)
{
  MutateClientState(
    clientId,
    [this, clientId](ClientContext& mutableClientContext)
    {
      mutableClientContext.presence = protocol::Presence{};
%(callout)s    });
}

"""
    callout_line = ("      BroadcastPresenceOfCharacter(\n"
                    "        mutableClientContext.characterUid, "
                    "protocol::Presence{}, clientId, nullptr);\n")
    namespace_end = "} // namespace server"

    def _inject(callout: str):
        block = encapsulator_helper % {"callout": callout}
        return lambda s: s.replace(namespace_end, block + namespace_end, 1)

    fixtures = fixtures + (
        (1, "ЧУЖОЕ ИМЯ: хелпер MutateClientState с выходом в лямбде",
         _inject(callout_line)),
        (0, "ЧУЖОЕ ИМЯ: тот же хелпер БЕЗ выхода — гейт обязан молчать",
         _inject("")),
        (2, "детектор свойства ослеп (параметр-вызываемое не распознан)",
         lambda s: s.replace(
             "  const std::function<void(ClientContext&)>& mutation)",
             "  const CallerMutation& mutation)", 1)),
        # ★КАНАРЕЙКИ ФИКСА WARN-1 (R82-2). Обе — ровно те два способа откатить
        # восстановленный гард, и обе обязаны краснить.
        (1, "гард GuildLogin возвращён к копии момента входа (откат WARN-1)",
         lambda s: s.replace(
             "  if (not currentIsAuthenticated)",
             "  if (not clientContext.isAuthenticated)", 1).replace(
             "  else if (command.characterUid != currentCharacterUid)",
             "  else if (command.characterUid != clientContext.characterUid)", 1)),
        (1, "с пост-callout перечитывания снят замок карты",
         lambda s: s.replace(
             "    const std::shared_lock lock(_clientsMutex);\n"
             "    const auto currentIter = _clients.find(clientId);",
             "    const auto currentIter = _clients.find(clientId);", 1)),
        # ★ТРЕТИЙ СПОСОБ ОТКАТА: блок перечитывания цел и под замком, но
        # ПЕРЕЕХАЛ ВЫШЕ выхода в `DataDirector` — и снова судит доcallout-состояние.
        (1, "пост-callout перечитывание переехало ВЫШЕ выхода в чужой код",
         _move_reread_above_callout),
    )

    import tempfile
    for expected, label, mutate in fixtures:
        mutated = mutate(original)
        if mutated == original:
            print(f"  ✗ фикстуру «{label}» не удалось впрыснуть (якорь не найден)")
            failures += 1
            continue
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp)
            (fake / SOURCE).parent.mkdir(parents=True, exist_ok=True)
            (fake / SOURCE).write_text(mutated, encoding="utf-8")
            try:
                code = judge(fake)
            except Invalid as exc:
                code = 2
                if expected != 2:
                    print(f"    (фикстура дала «недействительна»: {exc})")
        if code == expected:
            print(f"  ✓ фикстура «{label}» дала ожидаемый код {expected}")
        else:
            print(f"  ✗ фикстура «{label}»: код {code}, ожидался {expected}")
            failures += 1

    if failures:
        print("=== ИТОГ САМОПРОВЕРКИ: ПРОВАЛ ✗ ===")
        return 2
    print(f"=== ИТОГ САМОПРОВЕРКИ: ЧИСТО ✓ (поймано {caught} канареек "
          f"+ {len(fixtures)} фикстур R82) ===")
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
