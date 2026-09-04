#!/usr/bin/env bash
#
# Fixture delta gate for tools/round/check_tests_recipe.sh. NOT a gate over the server —
# it stands in for the real delta gates (tools/r75_*_gate.sh) inside a context that costs
# seconds instead of a server build.
#
# It copies their contract exactly, and only that contract:
#   * the base is `main` by default, as theirs is (`BASE="${BASE:-main}"`);
#   * a base that does not resolve, or a delta that is EMPTY, is BLINDNESS -> exit 2,
#     never "clean";
#   * a delta it could actually read -> exit 0.
#
# What the self-test proves with it: the recipe writes refs/heads/main from BASE_REF, so
# a real base gives a readable delta (green), and a wrong base — `--build-arg
# BASE_REF=HEAD`, i.e. the branch compared against ITSELF — makes this gate stop with
# code 2 and the build go red. Without the recipe's step there is no `main` here at all
# and the gate is equally blind, which is the defect being fixed.
#
# EXIT 0 delta readable · 2 blind (no base, or nothing to look at) · 3 usage
set -uo pipefail

FILE="delta_subject.txt"
BASE="${BASE:-main}"

cd "$(dirname "${BASH_SOURCE[0]}")" || { echo "ОСТАНОВ: не найден каталог фикстуры" >&2; exit 3; }

[ -f "$FILE" ] || { echo "ОСТАНОВ: нет $FILE — фикстура собрана неправильно" >&2; exit 3; }

git rev-parse --verify "$BASE^{commit}" >/dev/null 2>&1 || {
  echo "ОСТАНОВ: база '$BASE' не разрешается в коммит — гейт слеп" >&2; exit 2; }

diff="$(git diff "$BASE" -- "$FILE")" || {
  echo "ОСТАНОВ: git diff против '$BASE' не выполнился" >&2; exit 2; }

if [ -z "$diff" ]; then
  echo "ОСТАНОВ: дифф против '$BASE' ПУСТ — гейту нечего осматривать" >&2
  exit 2
fi

echo "  база : $BASE ($(git rev-parse --short "$BASE"))"
echo "  === ИТОГ: дельта против базы читается ✓ ==="
exit 0
