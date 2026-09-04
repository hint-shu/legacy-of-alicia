#!/usr/bin/env python3
"""check_bounded_lists.py — гейт ограниченных списков (LOA R74, backlog #170).

ЧТО ОН СТЕРЕЖЁТ
    Свойство: «ни один сериализатор протокола не пишет длину списка сырым
    сужающим кастом и не гоняет тело динамического контейнера циклом мимо
    `server::util::WriteBoundedList*`».

    Почему свойство, а не форма. Дефект убивал персонажа насмерть двумя
    способами: счётчик `static_cast<uint8_t>(c.size())` при `size()==264`
    уезжал как 8 (кадр рассинхронивался), а тело сверх буфера команды бросало
    `std::overflow_error` из поставщика записи — и `Client::WriteLoop` закрывал
    соединение НА КАЖДОМ входе.

КЛЮЧ ПОИСКА — «ФУНКЦИЯ, СРЕДИ ПАРАМЕТРОВ КОТОРОЙ ЕСТЬ `SinkStream&`»
    Не «функция, которая называется ::Write». Первая формулировка ловит и
    методы, и СВОБОДНЫЕ функции-сериализаторы (`WritePlayerRacer`,
    `WriteRacer`, `WriteRoomDescription`), и лямбды-писатели элементов. Вторая
    не ловила свободные функции — и площадка `WritePlayerRacer.equipment`,
    чей счётчик уезжает каждому входящему в комнату, пережила бы раунд.

ПОЧЕМУ ПОИСК МНОГОСТРОЧНЫЙ
    Перепись раунда делалась `grep`'ом, то есть ПОСТРОЧНО, и не увидела две
    площадки, у которых `static_cast<...>` и `.size()` стоят на разных строках
    (`AcCmdCRStartRaceNotify::ActiveSkillSet::Write`, `AcCmdGameRaceP2PResult::Write`;
    вторая — настоящий `std::vector` без потолка). Здесь всё сканирование идёт
    по ТЕКСТУ ЦЕЛИКОМ, с `re.S`, потому что построчный сканер этого класса
    слеп по построению.

ТРИ НЕЗАВИСИМЫХ ПРЕДИКАТА (ни один по отдельности не достаточен)
    P1  сырой узкий счётчик:      static_cast<uintN_t>( ... .size() ... )
    P2  неохваченное тело:        цикл `for (x : EXPR)` или булк `Write(E.data(), …)`
                                  по ДИНАМИЧЕСКОМУ контейнеру. После свипа таких
                                  циклов не остаётся вовсе: элементы пишет сам
                                  хелпер. Фиксированные `std::array` и C-массивы
                                  пропускаются ПО ТИПУ, разрешённому из
                                  объявления, а не по имени.
    P3  кламп после каста:        std::min(static_cast<uintN_t>(…), …) — форма,
                                  дающая 0 при size()==256.

КОДЫ ВОЗВРАТА
    0  чисто
    2  есть находки
    3  НЕ РАЗОБРАЛСЯ (тип не разрешился, скан подозрительно мал, самопроверка
       не прошла). «Не смог проверить» — это ОСТАНОВ, а не «чисто».
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

# --------------------------------------------------------------------------- #
# Что сканируем
# --------------------------------------------------------------------------- #

SOURCE_GLOBS = (
    "src/libserver/network/command/proto/*.cpp",
    "src/libserver/network/chatter/proto/*.cpp",
)
HEADER_GLOBS = (
    "include/libserver/network/command/proto/*.hpp",
    "include/libserver/network/chatter/proto/*.hpp",
    "include/libserver/util/*.hpp",
)

ALLOWLIST_PATH = "tools/bounded_lists_allowlist.txt"
FIXTURE_DIR = "tools/bounded_lists_fixtures"

#! Ниже этого числа просмотренных функций вердикт «0 находок» не значит ничего:
#! он значит, что скан не дошёл. Контроль слепоты по объёму.
MIN_FUNCTIONS_SCANNED = 200

HELPERS = ("WriteBoundedList", "WriteBoundedListInto", "WriteBoundedBytes")

DYNAMIC_TEMPLATES = (
    "std::vector", "std::map", "std::unordered_map", "std::deque",
    "std::set", "std::unordered_set", "std::list", "std::multimap",
)
FIXED_TEMPLATES = ("std::array",)

# --------------------------------------------------------------------------- #
# Мелкий разбор C++ (текстовый, но не построчный)
# --------------------------------------------------------------------------- #

COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def strip_comments(text: str) -> str:
    """Гасит комментарии, СОХРАНЯЯ переводы строк, чтобы номера строк не поехали."""
    def repl(m: re.Match) -> str:
        return "\n" * m.group(0).count("\n")
    return COMMENT_RE.sub(repl, text)


def match_brace(text: str, open_index: int) -> int:
    """Индекс парной '}' к '{' на позиции open_index, либо -1."""
    depth = 0
    i = open_index
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def match_paren(text: str, open_index: int) -> int:
    depth = 0
    i = open_index
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


# --------------------------------------------------------------------------- #
# Индекс типов: члены структур из заголовков
# --------------------------------------------------------------------------- #

#! ★АНОНИМНАЯ ВЛОЖЕННАЯ СТРУКТУРА ТОЖЕ ТИП. `struct { … } unk9{};` встречается
#! в протоколе не раз, и без неё член `command.unk9.unk2` не разрешался вовсе —
#! то есть гейт объявлял себя слепым на живой площадке свипа.
STRUCT_RE = re.compile(r'\b(?:struct|class)\s+([A-Za-z_]\w*)?\s*(?::[^{;=]*)?\{')
ANON_PREFIX = "__anon_"
#! `TYPE NAME{…};` / `TYPE NAME;` / `TYPE NAME = …;`
MEMBER_RE = re.compile(
    r'(?P<type>(?:const\s+)?[A-Za-z_][\w:]*(?:\s*<[^;{}]*?>)?(?:\s*\*)?)\s+'
    r'(?P<name>[A-Za-z_]\w*)\s*(?:\{[^{}()]*\}|=\s*[^;{}]*)?\s*;')
#! `} rewards;` — член, чей тип объявлен только что закрывшейся структурой.
TRAILING_MEMBER_RE = re.compile(r'\}\s*([A-Za-z_]\w*)\s*(?:\{[^{}]*\})?\s*;')


class TypeIndex:
    """{простое имя структуры: {имя члена: строка типа}}."""

    def __init__(self) -> None:
        self.members: dict[str, dict[str, str]] = {}
        self.conflicts: set[tuple[str, str]] = set()

    def add(self, struct: str, member: str, typ: str) -> None:
        table = self.members.setdefault(struct, {})
        typ = " ".join(typ.split())
        if member in table and table[member] != typ:
            # Одноимённые структуры в разных заголовках с РАЗНЫМ типом члена:
            # разрешить нельзя, и молчать об этом нельзя.
            self.conflicts.add((struct, member))
        table[member] = typ

    def lookup(self, struct: str, member: str) -> str | None:
        if (struct, member) in self.conflicts:
            return None
        return self.members.get(struct, {}).get(member)


def index_headers(root: str, header_globs=HEADER_GLOBS) -> TypeIndex:
    index = TypeIndex()
    for pattern in header_globs:
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            _index_scope(text, 0, len(text), index)
    return index


def _index_scope(text: str, start: int, end: int, index: TypeIndex) -> None:
    """Рекурсивно индексирует структуры в [start, end)."""
    pos = start
    while True:
        m = STRUCT_RE.search(text, pos, end)
        if not m:
            return
        open_brace = m.end() - 1
        close_brace = match_brace(text, open_brace)
        if close_brace < 0:
            return
        name = m.group(1)
        if not name:
            # Анонимная: имя синтезируется по члену, которым она объявлена.
            tail = re.match(r'\s*([A-Za-z_]\w*)', text[close_brace + 1:])
            if not tail:
                pos = close_brace + 1
                continue
            name = ANON_PREFIX + tail.group(1)
        body = text[open_brace + 1:close_brace]

        # Вложенные структуры индексируются первыми и вычищаются из тела, чтобы
        # их члены не приписались владельцу.
        _index_scope(text, open_brace + 1, close_brace, index)
        flat = _blank_nested(body)

        for mm in MEMBER_RE.finditer(flat):
            typ = mm.group("type").strip()
            member = mm.group("name")
            last = typ.split()[-1]
            if last in ("return", "case", "else", "struct", "class", "enum", "union"):
                # `struct   unk9;` — остаток замазанной анонимной структуры;
                # её настоящий тип проставит блок TRAILING_MEMBER_RE ниже.
                continue
            index.add(name, member, typ)

        # `struct Inner {...} member;`
        for tm in TRAILING_MEMBER_RE.finditer(body):
            inner = _struct_name_before(body, tm.start())
            index.add(name, tm.group(1), inner if inner else ANON_PREFIX + tm.group(1))

        pos = close_brace + 1


def _blank_nested(body: str) -> str:
    """Заменяет тела вложенных {...} пробелами (сохраняя длину и переводы строк)."""
    out = list(body)
    depth = 0
    for i, c in enumerate(body):
        if c == "{":
            depth += 1
            if depth >= 1:
                out[i] = " "
            continue
        if c == "}":
            if depth >= 1:
                out[i] = " "
            depth -= 1
            continue
        if depth >= 1 and c != "\n":
            out[i] = " "
    return "".join(out)


def _struct_name_before(body: str, close_index: int) -> str | None:
    """Имя структуры, чья '}' стоит на close_index."""
    depth = 0
    i = close_index
    while i >= 0:
        if body[i] == "}":
            depth += 1
        elif body[i] == "{":
            depth -= 1
            if depth == 0:
                m = None
                for m in STRUCT_RE.finditer(body, 0, i + 1):
                    pass
                if m and m.end() - 1 == i:
                    return m.group(1)
                return None
        i -= 1
    return None


# --------------------------------------------------------------------------- #
# Функции, среди параметров которых есть SinkStream&
# --------------------------------------------------------------------------- #

FUNC_HEAD_RE = re.compile(r'\(')


class Scope:
    def __init__(self, name: str, params: dict[str, str], start: int, end: int):
        self.name = name
        self.vars: dict[str, str] = dict(params)
        self.start = start
        self.end = end


PARAM_RE = re.compile(
    r'(?P<type>(?:const\s+)?[A-Za-z_][\w:]*(?:\s*<[^<>]*(?:<[^<>]*>)?[^<>]*>)?)\s*'
    r'(?P<ref>[&*]?)\s*(?P<name>[A-Za-z_]\w*)\s*$')


def parse_params(params: str) -> dict[str, str]:
    out: dict[str, str] = {}
    depth = 0
    current = ""
    parts = []
    for c in params:
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += c
    parts.append(current)
    for part in parts:
        part = " ".join(part.split())
        if not part:
            continue
        m = PARAM_RE.match(part)
        if m:
            typ = m.group("type").strip()
            if typ.startswith("const "):
                typ = typ[len("const "):].strip()
            out[m.group("name")] = typ
    return out


NAME_BEFORE_PAREN_RE = re.compile(r'([A-Za-z_]\w*)\s*$')


def find_sink_functions(text: str) -> list[tuple[str, int, int, dict[str, str], int]]:
    """Все функции/лямбды с `SinkStream&` среди параметров.

    Возвращает (имя, начало тела, конец тела, параметры, позиция '(' списка).
    """
    out = []
    for m in FUNC_HEAD_RE.finditer(text):
        open_paren = m.start()
        close_paren = match_paren(text, open_paren)
        if close_paren < 0:
            continue
        params = text[open_paren + 1:close_paren]
        if "SinkStream" not in params:
            continue
        if "&" not in params:
            continue
        # За списком параметров обязана идти '{' (через возможные квалификаторы).
        tail = text[close_paren + 1:close_paren + 80]
        tm = re.match(r'\s*(?:const\s*|noexcept\s*|mutable\s*|->\s*[\w:<>&*\s]+?)*\{', tail)
        if not tm:
            continue
        open_brace = close_paren + 1 + tm.end() - 1
        close_brace = match_brace(text, open_brace)
        if close_brace < 0:
            continue
        head = text[max(0, open_paren - 120):open_paren]
        nm = NAME_BEFORE_PAREN_RE.search(head)
        name = nm.group(1) if nm else "<lambda>"
        if head.rstrip().endswith("]"):
            name = "<lambda>"
        out.append((name, open_brace, close_brace, parse_params(params), open_paren))
    return out


QUALIFIER_RE = re.compile(r'((?:[A-Za-z_]\w*::)+)([A-Za-z_]\w*)\s*$')


def qualified_name(text: str, open_paren: int, simple: str) -> str:
    """Полное имя функции: `AcCmdCRGuildMemberListOK::Write` вместо `Write`.

    ★ЯКОРЬ — САМ СПИСОК ПАРАМЕТРОВ, а не поиск «где-то в 400 символах назад».
    Поиск назад цеплял ПРЕДЫДУЩУЮ функцию файла, и ключ allowlist указывал бы
    не туда — то есть исключение молча покрывало бы чужую площадку."""
    if simple == "<lambda>":
        return simple
    head = text[max(0, open_paren - 200):open_paren]
    m = QUALIFIER_RE.search(head)
    if m and m.group(2) == simple:
        qualifier = m.group(1)
        # `server::protocol::X::Write` -> `X::Write` (пространства имён не ключ)
        parts = [p for p in qualifier.split("::") if p]
        while parts and parts[0] in ("server", "protocol", "util", "data"):
            parts.pop(0)
        return "::".join(parts + [simple]) if parts else simple
    return simple


# --------------------------------------------------------------------------- #
# Разрешение типа итерируемого выражения
# --------------------------------------------------------------------------- #

LOCAL_DECL_RE = re.compile(
    r'\b(?:const\s+)?(?P<type>auto|[A-Za-z_][\w:]*(?:\s*<[^;{}]*?>)?)\s*&?\s*'
    r'(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<init>[^;{}]+);')

ELEMENT_OF_RE = re.compile(r'^(std::(?:vector|deque|list|set|unordered_set|array))\s*<\s*(.+?)\s*(?:,[^<>]*)?>$')


def base_type(typ: str) -> str:
    typ = typ.strip()
    typ = re.sub(r'^const\s+', "", typ)
    typ = typ.rstrip("&* ")
    return typ.strip()


def element_type(typ: str) -> str | None:
    typ = base_type(typ)
    m = ELEMENT_OF_RE.match(typ)
    if not m:
        return None
    inner = m.group(2).strip()
    # `std::array<Egg, 3>` → внутреннее уже без хвоста, но `std::vector<std::pair<a,b>>`
    # оставим как есть: нам важно только простое имя структуры.
    return base_type(inner)


def simple_name(typ: str) -> str:
    typ = base_type(typ)
    typ = re.sub(r'<.*$', "", typ)
    return typ.split("::")[-1]


class Unresolved(Exception):
    pass


def resolve_expression(expr: str, scope_vars: dict[str, str], index: TypeIndex) -> str:
    """Тип выражения вида `root.member.member` (плюс `.value()` у optional)."""
    expr = expr.strip()
    expr = re.sub(r'\s+', "", expr)
    if not expr:
        raise Unresolved("пустое выражение")

    parts = expr.split(".")
    root = parts[0]
    if root not in scope_vars:
        raise Unresolved(f"корень '{root}' не объявлен в области видимости")
    typ = scope_vars[root]
    if typ == "auto":
        raise Unresolved(f"корень '{root}' объявлен через auto и не связан")

    for part in parts[1:]:
        if part in ("value()", "operator*()"):
            m = re.match(r'std::optional\s*<\s*(.+?)\s*>$', base_type(typ))
            if not m:
                raise Unresolved(f"'.value()' на неоптионале '{typ}'")
            typ = base_type(m.group(1))
            continue
        member = re.sub(r'\(\)$', "", part)
        if member in ("data", "size", "begin", "end", "at", "front", "back"):
            continue
        owner = simple_name(typ)
        found = index.lookup(owner, member)
        if found is None:
            raise Unresolved(f"член '{member}' не найден в типе '{owner}'")
        typ = found
    return typ


def classify(typ: str) -> str:
    """'dynamic' | 'fixed' | 'scalar'."""
    t = base_type(typ)
    for tmpl in DYNAMIC_TEMPLATES:
        if t.startswith(tmpl + "<") or t == tmpl:
            return "dynamic"
    if t == "std::string" or t.startswith("std::string"):
        return "dynamic"
    for tmpl in FIXED_TEMPLATES:
        if t.startswith(tmpl + "<"):
            return "fixed"
    if re.search(r'\[\s*\d+\s*\]$', t):
        return "fixed"
    return "scalar"


# --------------------------------------------------------------------------- #
# Находки
# --------------------------------------------------------------------------- #

class Finding:
    def __init__(self, predicate: str, path: str, line: int, function: str,
                 expression: str, detail: str):
        self.predicate = predicate
        self.path = path
        self.line = line
        self.function = function
        self.expression = expression
        self.detail = detail

    def key(self) -> tuple[str, str, str]:
        return (self.path, self.function, self.expression)

    def __str__(self) -> str:
        return (f"{self.path}:{self.line} [{self.predicate}] {self.function} "
                f"— {self.expression} ({self.detail})")


P1_RE = re.compile(r'static_cast\s*<\s*(u?int(?:8|16|32)_t)\s*>\s*\(\s*(?P<expr>[^;]*?)\.size\(\)', re.S)
P3_RE = re.compile(r'std::min\s*\(\s*static_cast\s*<\s*u?int(?:8|16|32)_t\s*>', re.S)
#! ★ДВОЕТОЧИЕ РАЗДЕЛИТЕЛЯ — НЕ ЧАСТЬ `::`. Без этого `for (std::size_t idx = 0;
#! …)` разбирался как range-for по выражению «:size_t idx = 0; …», и индексный
#! цикл — та самая форма, которой написаны три площадки входа на ранчо, —
#! уходил в «не разобрался» вместо того, чтобы быть проверенным.
RANGE_FOR_RE = re.compile(
    r'\bfor\s*\(\s*(?P<decl>[^;()=]*?)\s*(?<!:):(?!:)\s*'
    r'(?P<expr>[^();]*?(?:\([^()]*\))?[^();]*?)\s*\)', re.S)
INDEX_FOR_RE = re.compile(r'\bfor\s*\((?P<init>[^;]*);(?P<cond>[^;]*);(?P<step>[^)]*)\)', re.S)
BULK_RE = re.compile(r'\bWrite\s*\(\s*(?P<expr>[A-Za-z_][\w.]*(?:\.value\(\))?)\.data\(\)\s*,', re.S)
LAMBDA_BIND_RE = re.compile(
    r'(?P<helper>WriteBoundedList|WriteBoundedListInto|WriteBoundedBytes)'
    r'\s*<[^;>]*>\s*\(', re.S)


def loop_body_span(text: str, after_head: int) -> tuple[int, int]:
    """[начало, конец) тела цикла, чья голова кончилась на after_head."""
    i = after_head
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    if i < len(text) and text[i] == "{":
        close = match_brace(text, i)
        return (i, close + 1 if close > 0 else len(text))
    semi = text.find(";", i)
    return (i, semi + 1 if semi > 0 else len(text))


class Scanner:
    def __init__(self, root: str, index: TypeIndex, allowlist: set[tuple[str, str, str]]):
        self.root = root
        self.index = index
        self.allowlist = allowlist
        self.findings: list[Finding] = []
        self.blind: list[str] = []
        self.functions_scanned = 0
        self.helper_calls = 0
        self.fixed_skipped = 0
        self.allowlisted_hits: set[tuple[str, str, str]] = set()

    # -- дерево областей видимости ----------------------------------------- #
    #
    # ★ЛЯМБДЫ-ПИСАТЕЛИ — ОТДЕЛЬНЫЕ ОБЛАСТИ, И ИХ ПАРАМЕТРЫ ОБЪЯВЛЕНЫ ЧЕРЕЗ
    # `const auto&`. Разрешить такой корень «по объявлению» нельзя вовсе —
    # объявление не называет типа. Поэтому область лямбды связывается с
    # контейнером ТОГО вызова хелпера, в аргументах которого она стоит:
    # элемент контейнера и есть тип параметра. Без этого шага гейт объявил бы
    # себя слепым ровно там, куда свип и перенёс тела списков.
    def scan_file(self, path: str) -> None:
        raw = open(path, encoding="utf-8", errors="replace").read()
        text = strip_comments(raw)
        rel = os.path.relpath(path, self.root)

        scopes = find_sink_functions(text)
        scopes.sort(key=lambda s: (s[1], -(s[2])))
        # дерево по вложенности
        children: dict[int, list[int]] = {}
        parent: dict[int, int | None] = {}
        stack: list[int] = []
        for i, (_n, bs, be, _p, _op) in enumerate(scopes):
            while stack and scopes[stack[-1]][2] < bs:
                stack.pop()
            parent[i] = stack[-1] if stack else None
            children.setdefault(parent[i] if parent[i] is not None else -1, []).append(i)
            stack.append(i)

        for i in children.get(-1, []):
            self._visit(rel, text, scopes, children, i, {})

    def _visit(self, rel, text, scopes, children, i, inherited):
        name, body_start, body_end, params, open_paren = scopes[i]
        self.functions_scanned += 1
        body = text[body_start:body_end + 1]
        fq = qualified_name(text, open_paren, name)
        if fq == "<lambda>" and inherited.get("__fq__"):
            fq = inherited["__fq__"] + ".<lambda>"

        scope_vars = dict(inherited)
        for pname, ptype in params.items():
            if ptype != "auto":
                scope_vars[pname] = ptype

        # `const auto&` параметры лямбды: тип = элемент контейнера того вызова
        # хелпера, в аргументах которого лямбда стоит.
        auto_params = [n for n, t in params.items() if t == "auto"]
        if auto_params:
            elem = self._element_of_enclosing_helper(text, scopes, i, inherited)
            for pname in auto_params:
                if elem:
                    scope_vars[pname] = elem
                else:
                    scope_vars.pop(pname, None)

        # текст области БЕЗ тел вложенных областей: каждая конструкция судится
        # ровно один раз, в своей внутренней области
        own = self._own_text(text, scopes, children, i)

        for lm in LOCAL_DECL_RE.finditer(own):
            typ = lm.group("type").strip()
            if typ == "auto":
                try:
                    typ = resolve_expression(lm.group("init"), scope_vars, self.index)
                except Unresolved:
                    typ = "auto"
            scope_vars[lm.group("name")] = typ

        # ★ПЕРЕМЕННАЯ range-for СВЯЗЫВАЕТСЯ ПО СВОЕМУ ЦИКЛУ, А НЕ ПО ФУНКЦИИ.
        # Соседние циклы одной функции сплошь и рядом называют переменную
        # одинаково (`element` в `AcCmdCRStartRaceNotify::Write` — дважды, над
        # разными контейнерами). Связка «первая победила» приписала бы второму
        # циклу ЧУЖОЙ тип элемента — и если бы тот оказался фиксированным
        # массивом, гейт пропустил бы настоящий динамический цикл. Это путь к
        # ЛОЖНО-ЗЕЛЁНОМУ, а не к слепоте, поэтому связка позиционная.
        loop_bindings: list[tuple[int, int, str, str]] = []
        for fm in RANGE_FOR_RE.finditer(own):
            decl = fm.group("decl").strip()
            dm = re.search(r'([A-Za-z_]\w*)\s*$', decl)
            if not dm:
                continue
            try:
                ctype = resolve_expression(fm.group("expr"), scope_vars, self.index)
                elem = element_type(ctype)
            except Unresolved:
                elem = None
            if not elem:
                continue
            lo, hi = loop_body_span(own, fm.end())
            loop_bindings.append((lo, hi, dm.group(1), elem))

        for hm in LAMBDA_BIND_RE.finditer(own):
            self.helper_calls += 1

        self.check_p1(rel, text, own, body_start, fq)
        self.check_p3(rel, text, own, body_start, fq)
        self.check_p2(rel, text, own, body_start, fq, scope_vars, loop_bindings)

        scope_vars["__fq__"] = fq
        for child in children.get(i, []):
            self._visit(rel, text, scopes, children, child, scope_vars)

    def _own_text(self, text, scopes, children, i) -> str:
        """Тело области с вырезанными (замазанными) телами вложенных областей."""
        _n, bs, be, _p, _op = scopes[i]
        buf = list(text[bs:be + 1])
        for child in children.get(i, []):
            _cn, cbs, cbe, _cp, _cop = scopes[child]
            for k in range(cbs - bs, cbe - bs + 1):
                if buf[k] != "\n":
                    buf[k] = " "
        return "".join(buf)

    def _element_of_enclosing_helper(self, text, scopes, i, outer_vars) -> str | None:
        _n, bs, _be, _p, _op = scopes[i]
        # ближайший слева вызов хелпера, чьи скобки накрывают начало лямбды
        best = None
        for m in LAMBDA_BIND_RE.finditer(text, 0, bs):
            open_paren = m.end() - 1
            close_paren = match_paren(text, open_paren)
            if close_paren > bs:
                best = (m, open_paren, close_paren)
        if not best:
            return None
        m, open_paren, close_paren = best
        args = split_args(text[open_paren + 1:close_paren])
        if len(args) < 2:
            return None
        container_arg = args[1].strip()
        if m.group("helper") == "WriteBoundedListInto" and len(args) >= 3:
            container_arg = args[2].strip()
        if container_arg.startswith("std::span"):
            inner = container_arg[container_arg.find("{") + 1:container_arg.rfind("}")]
            container_arg = inner.strip()
        try:
            container_type = resolve_expression(container_arg, outer_vars, self.index)
        except Unresolved:
            return None
        return element_type(container_type)

    # -- P1 ----------------------------------------------------------------- #
    def check_p1(self, rel, text, body, body_start, fq):
        for m in P1_RE.finditer(body):
            expr = " ".join(m.group("expr").split())
            expr = expr.lstrip("(").strip()
            self.record(Finding("P1", rel, line_of(text, body_start + m.start()),
                                fq, expr, "raw narrowing list counter"))

    # -- P3 ----------------------------------------------------------------- #
    def check_p3(self, rel, text, body, body_start, fq):
        for m in P3_RE.finditer(body):
            self.record(Finding("P3", rel, line_of(text, body_start + m.start()),
                                fq, "std::min(static_cast<…>)",
                                "clamp applied AFTER the narrowing cast"))

    # -- P2 ----------------------------------------------------------------- #
    def check_p2(self, rel, text, body, body_start, fq, scope_vars, loop_bindings):
        def vars_at(pos: int) -> dict[str, str]:
            merged = dict(scope_vars)
            for lo, hi, name, elem in loop_bindings:
                if lo <= pos < hi:
                    merged[name] = elem
            return merged

        for m in RANGE_FOR_RE.finditer(body):
            expr = " ".join(m.group("expr").split())
            self._judge_container(rel, text, body_start + m.start(), fq, expr,
                                  vars_at(m.start()), "loop over")
        for m in BULK_RE.finditer(body):
            expr = " ".join(m.group("expr").split())
            self._judge_container(rel, text, body_start + m.start(), fq, expr,
                                  vars_at(m.start()), "bulk write of")
        for m in INDEX_FOR_RE.finditer(body):
            # Индексный цикл: контейнер ищем в теле по `NAME[idx]`.
            im = re.search(r'([A-Za-z_]\w*)\s*=\s*0', m.group("init"))
            if not im:
                continue
            idx = im.group(1)
            lo, hi = loop_body_span(body, m.end())
            for sub in re.finditer(
                    r'([A-Za-z_][\w.]*)\s*\[\s*' + re.escape(idx) + r'\s*\]', body[lo:hi]):
                self._judge_container(rel, text, body_start + m.start(), fq,
                                      sub.group(1), vars_at(m.start()), "indexed loop over")

    def _judge_container(self, rel, text, index_in_text, fq, expr, scope_vars, verb):
        if not expr or expr.startswith("//"):
            return
        if re.match(r'^[A-Za-z_][\w.]*(\.value\(\))?$', expr) is None:
            # Выражение не является цепочкой `root.member` — например
            # `command.iceWallProperties.value().member1`. Нормализуем.
            normalised = expr.replace("value()", "value()")
            if re.match(r'^[A-Za-z_][\w.]*(\(\))?([\w.]*)$', normalised) is None:
                self.blind.append(
                    f"{rel}:{line_of(text, index_in_text)} {fq}: выражение "
                    f"'{expr}' не разобрано")
                return
        key = (rel, fq, expr)
        try:
            typ = resolve_expression(expr, scope_vars, self.index)
        except Unresolved as e:
            if key in self.allowlist:
                self.allowlisted_hits.add(key)
                return
            self.blind.append(f"{rel}:{line_of(text, index_in_text)} {fq}: {e}")
            return
        kind = classify(typ)
        if kind == "fixed":
            self.fixed_skipped += 1
            return
        if kind == "scalar":
            return
        if key in self.allowlist:
            self.allowlisted_hits.add(key)
            return
        self.record(Finding("P2", rel, line_of(text, index_in_text), fq, expr,
                            f"{verb} a dynamic container ({base_type(typ)}) outside the helper"))

    def record(self, finding: Finding) -> None:
        if finding.key() in self.allowlist:
            self.allowlisted_hits.add(finding.key())
            return
        self.findings.append(finding)


def split_args(text: str) -> list[str]:
    out, depth, cur = [], 0, ""
    i = 0
    while i < len(text):
        c = text[i]
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    out.append(cur)
    return out


# --------------------------------------------------------------------------- #
# Allowlist
# --------------------------------------------------------------------------- #

def load_allowlist(root: str) -> set[tuple[str, str, str]]:
    path = os.path.join(root, ALLOWLIST_PATH)
    entries: set[tuple[str, str, str]] = set()
    if not os.path.exists(path):
        return entries
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) < 4:
            print(f"ОСТАНОВ: строка allowlist не в формате "
                  f"<путь>|<функция>|<выражение>|<причина>: {raw.strip()}", file=sys.stderr)
            sys.exit(3)
        entries.add((parts[0], parts[1], parts[2]))
    return entries


# --------------------------------------------------------------------------- #
# Самопроверка на фикстурах
# --------------------------------------------------------------------------- #

FIXTURES = {
    "f1.cpp": ("P1", 1, "счётчик u8 в методе X::Write"),
    "f2.cpp": ("P1", 1, "счётчик в СВОБОДНОЙ функции (ловушка ключа '::Write(')"),
    "f3.cpp": ("P3", 1, "кламп ПОСЛЕ сужающего каста"),
    "f4.cpp": ("P2", 1, "цикл по std::vector мимо хелпера"),
    "f5.cpp": ("P2", 1, "булк Write(v.data(), …) мимо WriteBoundedBytes"),
    "f6.cpp": (None, 0, "std::array — находок быть НЕ должно"),
    "f7.cpp": ("P2", 1, "контейнер через локальную const auto&"),
}


def run_self_test(root: str) -> int:
    fixture_dir = os.path.join(root, FIXTURE_DIR)
    if not os.path.isdir(fixture_dir):
        print(f"ОСТАНОВ: нет каталога фикстур {fixture_dir}", file=sys.stderr)
        return 3

    index = index_headers(root)
    # Фикстуры описывают свои типы сами.
    fixture_index = index_headers(fixture_dir, ("*.hpp",))
    for struct, table in fixture_index.members.items():
        for member, typ in table.items():
            index.add(struct, member, typ)

    ok = True
    for name, (predicate, expected, what) in sorted(FIXTURES.items()):
        path = os.path.join(fixture_dir, name)
        if not os.path.exists(path):
            print(f"ОСТАНОВ: фикстура {name} отсутствует", file=sys.stderr)
            return 3
        scanner = Scanner(fixture_dir, index, set())
        scanner.scan_file(path)
        got = len(scanner.findings)
        kinds = sorted({f.predicate for f in scanner.findings})
        status = "OK"
        if got != expected:
            status = "ПРОВАЛ"
            ok = False
        elif predicate and kinds != [predicate]:
            status = "ПРОВАЛ"
            ok = False
        if scanner.blind and name != "f6.cpp":
            status = "ПРОВАЛ (слепота)"
            ok = False
        print(f"  {name:8s} ожидалось {expected} {predicate or '-':3s} · получено "
              f"{got} {','.join(kinds) or '-'} · {status} — {what}")
        for b in scanner.blind:
            print(f"      слепота: {b}")

    if not ok:
        print("ОСТАНОВ: гейт не доказал, что умеет краснеть — судить дерево нельзя.",
              file=sys.stderr)
        return 3
    print("самопроверка: 7/7 фикстур ✓ (гейт умеет краснеть на каждом своём предикате)")
    return 0


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".", help="корень репозитория")
    parser.add_argument("--self-test", action="store_true",
                        help="прогнать фикстуры и выйти")
    parser.add_argument("--allow-small-scan", action="store_true",
                        help="снять контроль слепоты по объёму (только для фикстур)")
    args = parser.parse_args()
    root = os.path.abspath(args.root)

    if args.self_test:
        return run_self_test(root)

    sources: list[str] = []
    for pattern in SOURCE_GLOBS:
        sources.extend(sorted(glob.glob(os.path.join(root, pattern))))
    if not sources:
        print(f"ОСТАНОВ: под {root} не нашлось ни одного файла сериализаторов",
              file=sys.stderr)
        return 3

    index = index_headers(root)
    if not index.members:
        print("ОСТАНОВ: индекс типов пуст — заголовки не разобраны", file=sys.stderr)
        return 3

    allowlist = load_allowlist(root)
    scanner = Scanner(root, index, allowlist)
    for path in sources:
        scanner.scan_file(path)

    for finding in sorted(scanner.findings, key=lambda f: (f.path, f.line)):
        print(finding)

    stale = allowlist - scanner.allowlisted_hits
    for entry in sorted(stale):
        print(f"ОСТАНОВ: запись allowlist ни на что не указывает: {entry}", file=sys.stderr)

    print(f"sites={len(scanner.findings)} helped={scanner.helper_calls} "
          f"array={scanner.fixed_skipped} allowlisted={len(scanner.allowlisted_hits)} "
          f"functions_scanned={scanner.functions_scanned}")

    if scanner.blind:
        for b in scanner.blind:
            print(f"НЕ РАЗОБРАЛСЯ: {b}", file=sys.stderr)
        print(f"ОСТАНОВ: {len(scanner.blind)} мест не разобрано — «не смог проверить» "
              f"не равно «чисто».", file=sys.stderr)
        return 3

    if stale:
        return 3

    if not args.allow_small_scan and scanner.functions_scanned < MIN_FUNCTIONS_SCANNED:
        print(f"ОСТАНОВ: просмотрено всего {scanner.functions_scanned} функций "
              f"(порог {MIN_FUNCTIONS_SCANNED}) — «0 находок» ничего не значит, "
              f"скан не дошёл.", file=sys.stderr)
        return 3

    if scanner.findings:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
