#!/usr/bin/env python3
"""Compare appearance.yaml against the stock DNA tables BY ShapeID.

Why this exists (R77): the coat/mane/tail numbers were imported by walking the
YAML blocks top to bottom and handing block N the numbers of TSV row N. Our YAML
does not list shapes in ShapeID order, so five shapes ended up carrying another
shape's inheritance rate, grade limit and rarity.

The fix for one such class of defect is not a list of the rows somebody noticed.
It is a total invariant over every row, checked by ShapeID, which is what this
oracle is. It prints file:line for each mismatch so the finding is actionable.

Note: minGrade for a coat is compared against LimitGrade AS SHIPPED. This oracle
knows nothing about childGradeLimit and must not: whether a coat is reachable by
breeding is a question for the game, not for the data integrity check.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_YAML = os.path.join(REPO, "resources", "config", "game", "horses", "appearance.yaml")
DEFAULT_STOCK = os.path.join(REPO, "tools", "stock")


def read_tsv(path):
    """Return {ShapeID: {column: value}} for a stock DNA table."""
    with open(path, encoding="utf-8") as handle:
        rows = [line.rstrip("\n").split("\t") for line in handle if line.strip()]
    header = rows[0]
    out = {}
    for row in rows[1:]:
        record = dict(zip(header, row))
        out[int(record["ShapeID"])] = record
    return out


def parse_appearance(path):
    """Return {section: [entry]} where entry = {key: (value, line_number)}.

    Deliberately a line-oriented parser rather than a YAML load: the whole point
    is to report the LINE a wrong value sits on, and a YAML loader throws that
    away.
    """
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")

    sections = {}
    section = None
    entry = None
    for index, line in enumerate(lines, start=1):
        top = re.match(r"^([a-zA-Z]+):\s*$", line)
        if top:
            section = top.group(1)
            sections.setdefault(section, [])
            entry = None
            continue
        if section is None:
            continue
        start = re.match(r"^  - (\w+): (\S+)\s*$", line)
        if start:
            entry = {start.group(1): (start.group(2), index)}
            sections[section].append(entry)
            continue
        cont = re.match(r"^    (\w+): (\S+)\s*$", line)
        if cont and entry is not None:
            entry[cont.group(1)] = (cont.group(2), index)
    return sections


def as_number(text):
    return float(text.strip("'\""))


def check(yaml_path, stock_dir):
    """Return a list of human-readable findings."""
    sections = parse_appearance(yaml_path)
    findings = []
    name = os.path.basename(yaml_path)

    def compare(where, key, got, want, shape):
        value, line = got
        if abs(as_number(value) - want) > 0.0005:
            findings.append(
                "%s:%d: %s shape %s: %s is %s, stock says %g"
                % (name, line, where, shape, key, value, want))

    # Coats are keyed by tid, which IS the ShapeID of DNA_SkinInfo.
    skins = read_tsv(os.path.join(stock_dir, "DNA_SkinInfo.tsv"))
    for entry in sections.get("coats", []):
        shape = int(entry["tid"][0])
        row = skins.get(shape)
        if row is None:
            findings.append("%s:%d: coats: tid %d has no stock row"
                            % (name, entry["tid"][1], shape))
            continue
        compare("coats", "inheritanceRate", entry["inheritanceRate"],
                float(row["InheritanceRate"]), shape)
        compare("coats", "minGrade", entry["minGrade"], float(row["LimitGrade"]), shape)
        compare("coats", "tier", entry["tier"], float(row["Rare"]), shape)

    # Manes and tails carry an explicit shape field; several tids share a shape.
    for section, table in (("manes", "DNA_ManeInfo.tsv"), ("tails", "DNA_TailInfo.tsv")):
        stock = read_tsv(os.path.join(stock_dir, table))
        for entry in sections.get(section, []):
            shape = int(entry["shape"][0])
            row = stock.get(shape)
            if row is None:
                findings.append("%s:%d: %s: shape %d has no stock row"
                                % (name, entry["shape"][1], section, shape))
                continue
            compare(section, "inheritanceRate", entry["inheritanceRate"],
                    float(row["InheritanceRate"]), shape)
            compare(section, "minGrade", entry["minGrade"], float(row["LimitGrade"]), shape)
            compare(section, "tier", entry["tier"], float(row["Rare"]), shape)

    scanned = sum(len(sections.get(s, [])) for s in ("coats", "manes", "tails"))
    return findings, scanned


def selftest(stock_dir):
    """A gate must prove itself before its verdict means anything.

    Corrupts exactly one value in a copy of the real file and requires the oracle
    to report exactly one finding, on the right line.
    """
    import tempfile

    with open(DEFAULT_YAML, encoding="utf-8") as handle:
        lines = handle.read().split("\n")

    target = None
    for index, line in enumerate(lines):
        if re.match(r"^    inheritanceRate: \S+\s*$", line):
            target = index
            break
    assert target is not None, "selftest found no inheritanceRate to corrupt"
    lines[target] = "    inheritanceRate: 99.0"

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "appearance.yaml")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines))
        findings, scanned = check(path, stock_dir)

    expected_line = target + 1
    ok = len(findings) == 1 and (":%d:" % expected_line) in findings[0]
    print("selftest: corrupted line %d, oracle reported %d finding(s), scanned %d entries"
          % (expected_line, len(findings), scanned))
    for finding in findings:
        print("  " + finding)
    print("selftest: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--yaml", default=DEFAULT_YAML)
    parser.add_argument("--stock", default=DEFAULT_STOCK)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--expect", type=int, default=0,
                        help="required number of findings; a negative branch that "
                             "'passes' is not a negative")
    args = parser.parse_args()

    if args.selftest:
        return selftest(args.stock)

    findings, scanned = check(args.yaml, args.stock)
    for finding in findings:
        print(finding)
    # Read the count, not just the verdict: zero findings out of zero entries is
    # a scan that never happened.
    print("scanned %d entries (coats + manes + tails), %d finding(s), expected %d"
          % (scanned, len(findings), args.expect))
    if scanned < 80:
        print("STOP: scanned too few entries, the parse is incomplete")
        return 3
    return 0 if len(findings) == args.expect else 1


if __name__ == "__main__":
    sys.exit(main())
