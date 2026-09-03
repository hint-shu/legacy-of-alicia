#!/usr/bin/env bash
#
# check_potential_roll.sh — source gate: the BREEDING potential-type roll is
# weighted by the foal's coat star tier, and is not a uniform draw.
#
# WHY THIS EXISTS (R77-fix-2, Codex finding 6 / WARN)
#   R77's whole product change is that `Genetics::CalculateFoalPotential` picks the
#   potential TYPE with weights taken from the foal's coat tier instead of rolling a
#   uniform integer over all 14 types. The round proved that with a negative branch
#   (neg-d) that puts `std::uniform_int_distribution` back — and NO unit test could
#   see it: `Genetics` lives in the alicia-server EXECUTABLE, needs a whole
#   ServerInstance, and cannot be linked into a test binary. neg-d was therefore
#   proved only by the stand and the binary ladder, i.e. by evidence that exists for
#   ONE SHA and is not re-run by anybody afterwards. A property that only a round
#   checks is green by default from the next round on.
#
#   This gate is the durable half: it reads the shipped source and states the
#   property as text, so `ctest` goes red the day the roll goes back to uniform —
#   in this round on neg-d, and in every round after this one on a regression.
#
# WHAT IT PROVES, AND WHAT IT DELIBERATELY DOES NOT
#   It proves the CALL: that the tier is mapped to a column, that the candidates
#   come from BuildPotentialTypeChoices, that the pick is PickWeighted, and that no
#   uniform distribution or bare rand() survives inside that one function. It does
#   NOT prove the numbers — those are checked by `ranch_test_genetics` against the
#   shipped potential.yaml, and by the stand's chi-square. Two different checks on
#   two different properties, on purpose.
#
# ★AND IT PROVES ITSELF FIRST (house rule: a gate must prove itself before its
#   verdict counts). Before reading the repository it parses two fixtures written
#   below — one shaped like the shipped function, one shaped like neg-d — and stops
#   unless it accepts the first and refuses the second. A checker that cannot fail
#   is not a checker.
#
# EXIT CODES  0 property holds · 1 property broken · 2 cannot certify (parse failed)
set -uo pipefail

ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TARGET="$ROOT/src/server/ranch/Genetics.cpp"
SIGNATURE='Genetics::PotentialResult Genetics::CalculateFoalPotential('

die()  { echo "ОСТАНОВ: $*" >&2; exit 2; }
fail() { echo "ПРОВАЛ: $*" >&2; FAILED=1; }

# Extracts the body of the function whose definition starts with $SIGNATURE:
# from that line to the first line that is exactly "}" at column 0.
extract_body() {
  local file="$1"
  awk -v sig="$SIGNATURE" '
    index($0, sig) == 1 { inside = 1 }
    inside { print }
    inside && $0 == "}" { exit }
  ' "$file"
}

# Judges one body. Prints nothing on success; returns 1 on a broken property.
judge_body() {
  local body="$1" where="$2" rc=0
  [ -n "$body" ] && [ "$(printf '%s' "$body" | wc -l)" -ge 5 ] \
    || { echo "  $where: тело функции не извлечено" >&2; return 2; }

  printf '%s\n' "$body" | grep -q 'CoatTierToOddsIndex' \
    || { echo "  $where: нет CoatTierToOddsIndex — масть не участвует в броске" >&2; rc=1; }
  printf '%s\n' "$body" | grep -q 'BuildPotentialTypeChoices' \
    || { echo "  $where: нет BuildPotentialTypeChoices — кандидаты не взвешены" >&2; rc=1; }
  printf '%s\n' "$body" | grep -q 'PickWeighted' \
    || { echo "  $where: нет PickWeighted — выбор не по весам" >&2; rc=1; }
  if printf '%s\n' "$body" | grep -qE 'uniform_int_distribution|uniform_real_distribution|\brand\(\)'; then
    echo "  $where: равномерный бросок вернулся в выбор типа способности" >&2
    rc=1
  fi
  return $rc
}

# ---- 0. self-check on fixtures --------------------------------------------------
FIX="$(mktemp -d)"
trap 'rm -rf "$FIX"' EXIT

cat > "$FIX/good.cpp" <<'FIXTURE'
Genetics::PotentialResult Genetics::CalculateFoalPotential(
  const data::Uid mareUid)
{
  PotentialResult result{};
  const auto tierIndex = registry::CoatTierToOddsIndex(tier);
  const auto choices = registry::BuildPotentialTypeChoices(registry, *tierIndex);
  result.type = static_cast<uint8_t>(PickWeighted(
    server::util::GetRandomEngine(), choices.values, choices.weights, uint32_t{0}));
  return result;
}
FIXTURE

cat > "$FIX/uniform.cpp" <<'FIXTURE'
Genetics::PotentialResult Genetics::CalculateFoalPotential(
  const data::Uid mareUid)
{
  PotentialResult result{};
  const auto& potentialTypes = registry.GetPotentialTypes();
  std::uniform_int_distribution<size_t> typeDist(0, potentialTypes.size() - 1);
  result.type = static_cast<uint8_t>(potentialTypes[typeDist(engine)]);
  return result;
}
FIXTURE

judge_body "$(extract_body "$FIX/good.cpp")" "fixture-good" >/dev/null 2>&1 \
  || die "самопроверка: гейт отверг ПРАВИЛЬНУЮ форму — его вердикт ничего не значит"
if judge_body "$(extract_body "$FIX/uniform.cpp")" "fixture-uniform" >/dev/null 2>&1; then
  die "самопроверка: гейт ПРИНЯЛ равномерный бросок — он не умеет провалиться"
fi
echo "самопроверка гейта: правильная форма принята, равномерная отвергнута ✓"

# ---- 1. the repository ----------------------------------------------------------
[ -f "$TARGET" ] || die "нет файла $TARGET"
BODY="$(extract_body "$TARGET")"
[ -n "$BODY" ] || die "не нашёл определение '$SIGNATURE' в $TARGET"
LINES="$(printf '%s\n' "$BODY" | wc -l)"

FAILED=0
judge_body "$BODY" "Genetics.cpp" || FAILED=1

if [ "$FAILED" -ne 0 ]; then
  echo "check_potential_roll: ПРОВАЛ — бросок типа способности не взвешен по масти" >&2
  exit 1
fi

echo "check_potential_roll: CalculateFoalPotential ($LINES строк) взвешен по масти ✓"
exit 0
