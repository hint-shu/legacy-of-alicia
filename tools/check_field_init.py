#!/usr/bin/env python3
"""check_field_init.py — build gate: every scalar field of the registries has a value.

WHY THIS EXISTS
  Registry structs are filled field by field from YAML at load time. A scalar member
  without an initialiser holds whatever was on the heap until some code path happens
  to assign it, and a path that forgets is silent: we have already paid for that twice
  (a 13-year incubation from `hatchDuration`, a free incubator from `incubatorSlots`).
  R63 closed "the last field without an initialiser" by counting field names; the count
  was wrong, because it keyed on the NAMES OF BUILT-IN TYPES and both survivors hide
  behind an ALIAS (`DeckId` -> uint32_t, `data::Tid` -> uint32_t), one of them defined
  outside the scanned directory entirely. A list of sites goes stale; this gate states
  the property instead: in include/libserver/registry/*.hpp, the number of scalar
  fields declared without an initialiser must be ZERO.

WHY A SCRIPT AND NOT A static_assert
  Considered and rejected: `std::is_trivially_default_constructible_v<T>` cannot see
  this defect — ONE member of class type (a `protocol::Vector3` with NSDMI, a
  std::string, a std::vector) makes the whole struct non-trivial and MASKS every
  uninitialised scalar inside it. That is exactly the situation in
  `Course::MapBlockInfo::DeckItemInstance`. A consteval read of each member does catch
  it, but only if the members are listed by name — which is the list of sites again.
  A `static_assert` on aggregate-ness would also require touching ~300 protocol structs
  whose `GetCommand()` is not constexpr. So: a property gate over the source.

SCOPE
  include/libserver/registry/*.hpp only — precisely the domain of the #174 claim.
  Extending it to include/libserver/data/ or to the protocol structs is deliberately
  NOT part of this round.

THE THREE OUTCOMES PER FIELD (and silence is not one of them)
  ok         — has an initialiser, or its type initialises itself
  offender   — no initialiser and the type resolves to a scalar (built-in, alias chain
               to a built-in, enum, pointer, or std::array of those)
  STOP (2)   — the type could not be classified at all. An unknown type name must never
               coast through as "probably fine": that is how `data::Tid` was missed.

USAGE
  python3 tools/check_field_init.py [DIR]     # default: <repo>/include/libserver/registry
  python3 tools/check_field_init.py --selftest

EXIT CODES
  0 clean · 1 offenders found (printed with file:line) · 2 the check is invalid
    (missing directory, coverage floor not met, or an unclassifiable type)
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Coverage floors. MEASURED on the tree at 2026-09-02 (round R72):
#   files scanned .................. 12
#   field declarations in total .... see below (printed on every run)
#   of them without an initialiser . see below
# A zero-offender verdict is only good news when the scan is known to have been
# complete. Raise these deliberately; NEVER lower one to make a run go green.
MIN_FILES = 10
MIN_FIELD_DECLS = 300
MIN_WITHOUT_INIT = 50

# Where alias chains are resolved from, on top of the scanned directory itself.
# `data::Tid` lives here and not in the registries — that is precisely why the
# previous recount could not see it as a scalar.
EXTRA_ALIAS_SOURCES = (
    "include/libserver/data/DataDefinitions.hpp",
    "include/libserver/registry/RegistryDefinitions.hpp",
)

BUILTIN_SCALARS = {
    "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t",
    "char16_t", "char32_t", "short", "unsigned short", "int", "unsigned",
    "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
    "float", "double", "long double", "size_t", "std::size_t", "ssize_t",
    "ptrdiff_t", "std::ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "std::int8_t", "std::int16_t", "std::int32_t", "std::int64_t",
    "std::uint8_t", "std::uint16_t", "std::uint32_t", "std::uint64_t",
}

# The small, explicit table of types that live outside the scanned set. Every row
# carries its reason; a row without a reason is a guess, and a guess here is the
# blindness this gate exists to remove.
EXTERNAL_TYPES = {
    # CommonStructureDefinitions.hpp: all three floats carry an NSDMI.
    "protocol::Vector3": "self",
    "Vector3": "self",
    # DataDefinitions.hpp: both are aliases of uint32_t.
    "data::Tid": "scalar",
    "data::Uid": "scalar",
    # DataDefinitions.hpp: std::chrono::system_clock::duration. The default
    # constructor of std::chrono::duration is `= default`, i.e. TRIVIAL — the
    # value is NOT zeroed.
    "data::Clock::duration": "scalar",
    "data::Clock::time_point": "scalar",
    # std::chrono, same reasoning as above.
    "std::chrono::milliseconds": "scalar",
    "std::chrono::seconds": "scalar",
    "std::chrono::minutes": "scalar",
    "std::chrono::hours": "scalar",
}

DECL_SKIP_PREFIXES = (
    "using", "typedef", "friend", "static_assert", "return", "template",
    "public", "private", "protected", "namespace", "struct", "class", "enum",
    "union", "constexpr", "static", "inline", "virtual", "explicit", "extern",
)


def strip_comments_and_strings(text):
    """Replace comments and string/char literals with spaces, KEEPING newlines.

    ★NOT COSMETIC. A gate that names the wrong file:line sends the reader off to
    fix somebody else's line. Cutting comments out (rather than blanking them)
    shifts every following number by the height of the 17-line licence header —
    that is how a prototype of this gate reported CourseRegistry.hpp:83 for a
    field that lives on :100.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if two == "/*":
            while i < n and text[i:i + 2] != "*/":
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        if c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append("  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


class Statement:
    def __init__(self, text, line):
        self.text = text
        self.line = line


def collect_statements(clean_text):
    """Yield (statement_text, line, scope_kind_of_enclosing_scope).

    Scope kinds: 'agg' (struct/class/union body), 'enum', 'ns', 'func', 'file'.
    A `{` that is a braced INITIALISER is kept inside the statement rather than
    opening a scope, so `data::Clock::duration d{0};` reads as one declaration
    that HAS an initialiser.
    """
    statements = []
    scopes = ["file"]
    buf = []
    buf_line = None
    line = 1
    init_depth = 0

    def buf_text():
        return re.sub(r"\s+", " ", "".join(buf)).strip()

    def note_char(ch):
        nonlocal buf_line
        if buf_line is None and not ch.isspace():
            buf_line = line
        buf.append(ch)

    i = 0
    n = len(clean_text)
    while i < n:
        c = clean_text[i]
        if c == "\n":
            line += 1
            note_char(" ") if buf else None
            i += 1
            continue

        if init_depth > 0:
            note_char(c)
            if c == "{":
                init_depth += 1
            elif c == "}":
                init_depth -= 1
            i += 1
            continue

        if c == "{":
            head = buf_text()
            kind = None
            if re.search(r"\benum\b", head):
                kind = "enum"
            elif re.search(r"\bnamespace\b", head):
                kind = "ns"
            elif re.search(r"\b(struct|class|union)\b", head) and "=" not in head:
                kind = "agg"
            elif "(" in head or ")" in head:
                kind = "func"
            elif head == "" or head.endswith(("else", "try", "do")):
                kind = "func"
            if kind is None:
                # A braced initialiser inside a declaration.
                init_depth = 1
                note_char(c)
                i += 1
                continue
            scopes.append(kind)
            buf.clear()
            buf_line = None
            i += 1
            continue

        if c == "}":
            if len(scopes) > 1:
                scopes.pop()
            buf.clear()
            buf_line = None
            i += 1
            continue

        if c == ";":
            text = buf_text()
            if text:
                statements.append((text, buf_line, scopes[-1]))
            buf.clear()
            buf_line = None
            i += 1
            continue

        note_char(c)
        i += 1

    return statements


def build_alias_table(paths):
    """name -> aliased type, from `using X = Y;` (both short and qualified keys)."""
    aliases = {}
    enums = set()
    aggregates = set()
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            clean = strip_comments_and_strings(handle.read())
        for match in re.finditer(r"\busing\s+(\w+)\s*=\s*([^;]+);", clean):
            aliases[match.group(1)] = re.sub(r"\s+", " ", match.group(2)).strip()
        for match in re.finditer(r"\benum\s+(?:class\s+|struct\s+)?(\w+)", clean):
            enums.add(match.group(1))
        for match in re.finditer(r"\b(?:struct|class)\s+(\w+)", clean):
            aggregates.add(match.group(1))
    return aliases, enums, aggregates


def normalise_type(raw):
    t = raw.strip()
    t = re.sub(r"\bconst\b|\bvolatile\b|\bmutable\b|\bstatic\b|\binline\b", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def classify(type_name, aliases, enums, aggregates, steps=0):
    """Return 'scalar' | 'self' | None (unclassifiable)."""
    t = normalise_type(type_name)
    if not t:
        return None
    if t.endswith("&"):
        return "self"       # references cannot be uninitialised in an aggregate
    if t.endswith("*"):
        return "scalar"
    if t in EXTERNAL_TYPES:
        return EXTERNAL_TYPES[t]
    if t in BUILTIN_SCALARS:
        return "scalar"

    # std::array<T, N> is classified BY ITS ELEMENT TYPE: an array of scalars is
    # exactly as uninitialised as one scalar, only N times over.
    array_match = re.match(r"^std::array\s*<(.+),[^,>]+>$", t)
    if array_match:
        return classify(array_match.group(1), aliases, enums, aggregates, steps + 1)

    if "<" in t:
        return "self"       # std::vector / map / optional / ... default-construct
    if t.startswith("std::"):
        return "self"

    short = t.split("::")[-1]
    if t in enums or short in enums:
        return "scalar"
    if t in aggregates or short in aggregates:
        return "self"       # its own fields are judged by this same gate

    if steps < 5:
        for key in (t, short):
            if key in aliases:
                return classify(aliases[key], aliases, enums, aggregates, steps + 1)
    return None


FIELD_NAME_RE = re.compile(r"^(?P<type>.+?)\s(?P<name>\w+)\s*(?P<arr>(\[[^\]]*\]\s*)*)$")

# A pointer-to-function (or pointer-to-member-function) DATA MEMBER: the declarator
# name sits inside parentheses behind a `*`. `void (*callback)();` is a scalar that
# holds whatever was on the heap — exactly the defect this gate exists to find — and
# the first version of this script threw it away together with method declarations,
# because both merely "contain a parenthesis".
FUNCTION_POINTER_RE = re.compile(r"\(\s*[\*&][^()]*\)\s*\(")

# A member FUNCTION declaration: the declarator ends with the parameter list, possibly
# followed by cv/ref/exception specifiers. A data member never ends that way — even
# one whose type carries parentheses (`std::function<void(int)> callback`) ends with
# its own name. Checked AFTER the function-pointer form, which also ends with `)`.
METHOD_TAIL_RE = re.compile(
    r"\)\s*(?:const|volatile|noexcept|override|final|&{1,2}|\s)*$")

# A bit-field: `uint32_t flags : 3;`. The `(?<!:):(?!:)` pair keeps `data::Tid` out of
# it. A bit-field without an initialiser is uninitialised exactly like any other
# scalar; the first version skipped the whole statement and counted nothing.
BITFIELD_RE = re.compile(r"^(?P<decl>.+?)\s*(?<!:):(?!:)\s*(?P<width>[^:]+)$")


def classify_statement_with_parens(decl):
    """'funcptr' | 'method' | None (unclassifiable — the caller must STOP)."""
    if FUNCTION_POINTER_RE.search(decl):
        return "funcptr"
    if METHOD_TAIL_RE.search(decl):
        return "method"
    return None


def scan_file(path, aliases, enums, aggregates, report):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        clean = strip_comments_and_strings(handle.read())
    for text, line, scope in collect_statements(clean):
        if scope != "agg":
            continue
        # An access specifier can sit on the same statement as the first field
        # (`public: uint32_t a;`) — strip it, do not lose the field with it.
        text = re.sub(r"^(public|private|protected)\s*:\s*", "", text)
        head = text.split(" ")[0].rstrip(":")
        if head in DECL_SKIP_PREFIXES:
            continue
        # `operator==(...)`, `operator=(...)`: the `=` belongs to the NAME, so the
        # initialiser split below would tear the declaration apart and leave
        # `bool operator` looking like an uninitialised scalar field.
        if re.search(r"\boperator\b", text):
            report["methods"] += 1
            continue

        has_init = "{" in text or "=" in text
        decl = text.split("=")[0].split("{")[0].strip()

        # ★PARENTHESES ARE NO LONGER A SILENT SKIP (R72-fix-5, Codex finding 5).
        # Three outcomes, and silence is not one of them: a function-pointer member
        # is a FIELD (and a scalar one), a method declaration is counted as a method,
        # and anything else with a parenthesis STOPS the gate. Note the test is on
        # `decl` — the part BEFORE any initialiser — so `uint32_t x = foo();` is
        # judged as the initialised field it is, not thrown away as "a functional
        # cast".
        if "(" in decl or ")" in decl:
            kind = classify_statement_with_parens(decl)
            if kind == "method":
                report["methods"] += 1
                continue
            if kind is None:
                report["unparsed"].append((path, line, text))
                continue
            report["fields"] += 1
            if has_init:
                continue
            report["without_init"] += 1
            report["offenders"].append((path, line, text, "указатель на функцию"))
            continue

        # ★BIT-FIELDS ARE JUDGED, NOT SKIPPED (R72-fix-5, Codex finding 5). The width
        # is cut off and the declaration underneath is classified like any other.
        bitfield = BITFIELD_RE.match(decl)
        if bitfield is not None:
            report["bitfields"] += 1
            decl = bitfield.group("decl").strip()

        match = FIELD_NAME_RE.match(decl)
        if not match:
            continue
        report["fields"] += 1
        if has_init:
            continue
        report["without_init"] += 1
        verdict = classify(match.group("type"), aliases, enums, aggregates)
        if verdict is None:
            report["unknown"].append((path, line, text, match.group("type")))
        elif verdict == "scalar":
            report["offenders"].append((path, line, text, normalise_type(match.group("type"))))


def analyse(directory, repo_root):
    headers = sorted(
        os.path.join(directory, name)
        for name in os.listdir(directory)
        if name.endswith(".hpp")
    )
    alias_sources = list(headers)
    for rel in EXTRA_ALIAS_SOURCES:
        candidate = os.path.join(repo_root, rel)
        if os.path.isfile(candidate):
            alias_sources.append(candidate)
    aliases, enums, aggregates = build_alias_table(alias_sources)
    report = {"files": len(headers), "fields": 0, "without_init": 0,
              "methods": 0, "bitfields": 0,
              "offenders": [], "unknown": [], "unparsed": []}
    for header in headers:
        scan_file(header, aliases, enums, aggregates, report)
    return report


def relpath(path, root):
    try:
        return os.path.relpath(path, root)
    except ValueError:
        return path


def run_tree(directory, repo_root):
    if not os.path.isdir(directory):
        print(f"ОСТАНОВ: нет каталога {directory} — считать нечего")
        return 2
    report = analyse(directory, repo_root)

    print("=== field-init gate ===")
    print(f"каталог                            : {relpath(directory, repo_root)}")
    print(f"файлов просканировано              : {report['files']}   (порог: >= {MIN_FILES})")
    print(f"объявлений полей ВСЕГО             : {report['fields']}   (порог: >= {MIN_FIELD_DECLS})")
    print(f"из них без инициализатора          : {report['without_init']}   (порог: >= {MIN_WITHOUT_INIT})")
    print(f"объявлений методов (пропущено)     : {report['methods']}")
    print(f"из них битовых полей               : {report['bitfields']}")

    invalid = False
    if report["files"] < MIN_FILES:
        print("ОСТАНОВ: файлов меньше порога — ноль нарушителей на неполном наборе это слепота")
        invalid = True
    if report["fields"] < MIN_FIELD_DECLS:
        print("ОСТАНОВ: объявлений полей меньше порога — разбор поехал, ноль читать нельзя")
        invalid = True
    if report["without_init"] < MIN_WITHOUT_INIT:
        print("ОСТАНОВ: полей без инициализатора меньше порога — разбор поехал")
        invalid = True
    if report["unparsed"]:
        print(f"ОСТАНОВ: объявлений со скобками, которые разбор не смог отнести "
              f"ни к методу, ни к полю: {len(report['unparsed'])}")
        for path, line, text in report["unparsed"]:
            print(f"  {relpath(path, repo_root)}:{line}: {text};")
        print("  молча пропустить такую форму нельзя — именно так поле уходит из-под гарда")
        invalid = True
    if report["unknown"]:
        print(f"ОСТАНОВ: типов, которые не удалось классифицировать: {len(report['unknown'])}")
        for path, line, text, type_name in report["unknown"]:
            print(f"  {relpath(path, repo_root)}:{line}: {text};   [тип {type_name}]")
        print("  расширь таблицу EXTERNAL_TYPES с обоснованием ИЛИ дай полю инициализатор")
        invalid = True
    if invalid:
        return 2

    if report["offenders"]:
        print(f"скалярных полей без значения       : {len(report['offenders'])} (ожидалось 0)")
        for path, line, text, type_name in report["offenders"]:
            print(f"  {relpath(path, repo_root)}:{line}: {text};   [тип {type_name}]")
        print("=== ИТОГ: ПРОВАЛ ✗ — поле реестра без значения ===")
        return 1

    print("скалярных полей без значения       : 0 (ожидалось 0)")
    print("=== ИТОГ: ЧИСТО ✓ ===")
    return 0


# ---------------------------------------------------------------------------
# Selftest. ★A GATE MUST PROVE IT CAN FAIL BEFORE IT IS ALLOWED TO JUDGE.
# Each fixture states its expectation, INCLUDING the line number where an
# offender must be reported — the line number is the half a broken comment
# stripper gets wrong while still reporting "one offender".
SELFTEST_EXPECTATIONS = {
    "top_scalar_bad.hpp":          {"code": 1, "offenders": [("a", 18)]},
    "nested_scalar_bad.hpp":       {"code": 1, "offenders": [("inner", 18)]},
    "double_nested_bad.hpp":       {"code": 1, "offenders": [("deep", 18)]},
    "alias_bad.hpp":               {"code": 1, "offenders": [("id", 18)]},
    "enum_bad.hpp":                {"code": 1, "offenders": [("region", 21)]},
    "initialised_ok.hpp":          {"code": 0, "offenders": []},
    "class_members_ok.hpp":        {"code": 0, "offenders": []},
    "methods_ok.hpp":              {"code": 0, "offenders": []},
    "comments_and_strings_ok.hpp": {"code": 0, "offenders": [], "line_probe": ("probe", 39)},
    "unknown_type_stop.hpp":       {"code": 2, "offenders": []},
    # R72-fix-5 (Codex finding 5): the two forms the first version threw away.
    "func_pointer_bad.hpp":        {"code": 1, "offenders": [("callback", 18)]},
    "bitfield_bad.hpp":            {"code": 1, "offenders": [("flags", 18)]},
    "bitfield_init_ok.hpp":        {"code": 0, "offenders": []},
    "trailing_return_stop.hpp":    {"code": 2, "offenders": []},
}


def offender_field_name(text):
    """The declared NAME of an offending field, whatever shape it was declared in.

    Exists for the selftest table: a plain scalar, a function pointer and a bit-field
    put their name in three different places, and `text.split()[-1]` names only the
    first of the three (it would call `void (*callback)()` "(*callback)()" and
    `uint32_t flags : 3` "3").
    """
    decl = text.split("=")[0].split("{")[0].strip()
    pointer = re.search(r"\(\s*\*+\s*(\w+)\s*\)", decl)
    if pointer:
        return pointer.group(1)
    bitfield = BITFIELD_RE.match(decl)
    if bitfield is not None:
        decl = bitfield.group("decl").strip()
    match = FIELD_NAME_RE.match(decl)
    if match:
        return match.group("name")
    return decl.split()[-1] if decl.split() else decl


def selftest(repo_root):
    fixtures_dir = os.path.join(repo_root, "tools", "fixtures", "field_init")
    if not os.path.isdir(fixtures_dir):
        print(f"ОСТАНОВ: нет каталога фикстур {fixtures_dir}")
        return 2
    present = sorted(f for f in os.listdir(fixtures_dir) if f.endswith(".hpp"))
    if sorted(SELFTEST_EXPECTATIONS) != present:
        print("ОСТАНОВ: набор фикстур не совпадает с таблицей ожиданий")
        print(f"  на диске : {present}")
        print(f"  в таблице: {sorted(SELFTEST_EXPECTATIONS)}")
        return 2

    print("=== field-init gate: SELFTEST ===")
    failures = 0
    for name in present:
        expected = SELFTEST_EXPECTATIONS[name]
        path = os.path.join(fixtures_dir, name)
        aliases, enums, aggregates = build_alias_table([path])
        report = {"files": 1, "fields": 0, "without_init": 0,
                  "methods": 0, "bitfields": 0,
                  "offenders": [], "unknown": [], "unparsed": []}
        scan_file(path, aliases, enums, aggregates, report)

        if report["unknown"] or report["unparsed"]:
            code = 2
        elif report["offenders"]:
            code = 1
        else:
            code = 0

        problems = []
        if code != expected["code"]:
            problems.append(f"код {code}, ожидался {expected['code']}")
        got = [(offender_field_name(text), line) for _, line, text, _ in report["offenders"]]
        want = expected["offenders"]
        if sorted(got) != sorted(want):
            problems.append(f"нарушители {got}, ожидались {want}")
        probe = expected.get("line_probe")
        if probe is not None:
            # The fixture is clean, so the offenders list cannot carry the proof
            # that line numbers did not shift. Re-scan and look the field up by
            # name among ALL declarations.
            found = None
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                clean = strip_comments_and_strings(handle.read())
            for text, line, scope in collect_statements(clean):
                if scope == "agg" and text.split("=")[0].split("{")[0].strip().endswith(probe[0]):
                    found = line
            if found != probe[1]:
                problems.append(f"строка поля {probe[0]}: {found}, ожидалась {probe[1]}")

        status = "✓" if not problems else "✗"
        print(f"  {status} {name}: код {code}, нарушители {got}")
        for problem in problems:
            print(f"      {problem}")
            failures += 1

    print()
    if failures:
        print(f"=== ИТОГ SELFTEST: ПРОВАЛ ✗ ({failures} расхождений) ===")
        return 2
    print(f"=== ИТОГ SELFTEST: гард доказал, что видит и умеет провалиться ✓ ({len(present)} фикстур) ===")
    return 0


def main(argv):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest(repo_root)
    directory = argv[1] if len(argv) > 1 else os.path.join(
        repo_root, "include", "libserver", "registry")
    return run_tree(os.path.abspath(directory), repo_root)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
