#!/usr/bin/env bash
#
# check_tests_recipe.sh — prove that tools/round/Dockerfile.tests fails when ctest fails.
#
# WHY THIS EXISTS
#   The recipe's whole job is to turn "a test failed" into "the build failed", and for
#   three rounds it could not do that job: `ctest … | tee /ctest.log` under `/bin/sh`
#   returns tee's code, always 0 (measured 2026-09-04, R78 — three negatives with a
#   genuinely failing ctest all read green). A recipe that claims to be a gate has to
#   demonstrate a red before anyone may read its green, so this script makes the recipe
#   fail on demand and refuses to pass unless it does.
#
# WHAT IT DOES
#   Builds tools/round/Dockerfile.tests against tools/round/fixtures/tests-recipe — a
#   CMake project with no compiled code, only ctest entries — in four arms:
#
#     1 pass       one passing test                      expect docker build rc = 0
#     2 fail       one passing + one failing test        expect docker build rc != 0
#     3 notests    no tests registered at all            expect docker build rc != 0
#     4 control    arm 2's context, but the recipe with  expect docker build rc = 0
#                  its `SHELL [… pipefail …]` line cut
#
#   Arm 4 is the honest half of the proof. Without it, arm 2 going red only shows that
#   *something* in the file fails the build; arm 4 shows the failing test slipping
#   through the moment the pipefail line is removed and nothing else changes, which is
#   the defect this recipe was written against.
#
# USAGE
#   bash tools/round/check_tests_recipe.sh
#
# ENV
#   TAG_PREFIX  default: loa-tests-recipe-selftest   (images built and removed here)
#   KEEP_WORK   set to 1 to leave the temporary contexts and build logs behind
#
# EXIT CODES  0 all four arms behaved as stated · 1 any arm behaved otherwise, or setup
#             failed. Never exits 0 on an arm that did not run.
set -uo pipefail

die() { echo "ОСТАНОВ: $*" >&2; cleanup; exit 1; }
say() { printf '%s\n' "$*"; }

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SELF_DIR/../.." && pwd)"
RECIPE="$REPO/tools/round/Dockerfile.tests"
FIXTURE="$REPO/tools/round/fixtures/tests-recipe"
TAG_PREFIX="${TAG_PREFIX:-loa-tests-recipe-selftest}"
KEEP_WORK="${KEEP_WORK:-0}"
WORK=""
BUILT_TAGS=()

# Images built here are unreferenced by any container, which makes them fair game for a
# `docker image prune -a` running in another session mid-check. Each built image is
# therefore held by a stopped container for the duration and released in cleanup.
cleanup() {
  local t
  for t in "${BUILT_TAGS[@]:-}"; do
    [ -n "$t" ] || continue
    docker rm -f "keep-${t##*:}" >/dev/null 2>&1
    docker rmi "$t" >/dev/null 2>&1
  done
  if [ -n "$WORK" ] && [ "$KEEP_WORK" != "1" ]; then rm -rf "$WORK"; fi
}

command -v docker >/dev/null 2>&1 || die "нет docker"
[ -f "$RECIPE" ]  || die "нет $RECIPE"
[ -f "$FIXTURE/CMakeLists.txt" ] || die "нет фикстуры $FIXTURE/CMakeLists.txt"

# ---- 0. the recipe must have the shape this script claims to test -------------------
# A self-test that silently starts testing a different file is worse than none: it would
# keep printing "ПРОЙДЕНО" over a recipe whose fix somebody deleted.
SHELL_LINE="$(grep -n '^SHELL \[' "$RECIPE")"
[ -n "$SHELL_LINE" ] || die "в рецепте нет строки SHELL [...] — проверять нечего"
[ "$(grep -c '^SHELL \[' "$RECIPE")" -eq 1 ] || die "в рецепте больше одной строки SHELL"
grep -q '^SHELL \["/bin/bash", "-o", "pipefail", "-c"\]$' "$RECIPE" \
  || die "строка SHELL в рецепте не задаёт bash -o pipefail: $SHELL_LINE"
grep -q 'ctest .*| tee ' "$RECIPE" \
  || die "в рецепте нет конвейера ctest | tee — тогда это не тот дефект"
grep -q 'ctest .*--no-tests=error' "$RECIPE" \
  || die "в рецепте нет --no-tests=error — пустой набор тестов даст ложную зелень"
say "рецепт: $RECIPE"
say "  SHELL bash -o pipefail ✓ · ctest | tee ✓ · --no-tests=error ✓"
say

WORK="$(mktemp -d)" || die "не создать временный каталог"

# ---- 1. contexts ---------------------------------------------------------------------
for arm in pass fail notests; do
  mkdir -p "$WORK/$arm" || die "не создать $WORK/$arm"
  cp "$FIXTURE/CMakeLists.txt" "$WORK/$arm/" || die "не скопировать фикстуру в $arm"
done
: > "$WORK/fail/ARM_FAIL"       || die "не создать маркер ARM_FAIL"
: > "$WORK/notests/ARM_NOTESTS" || die "не создать маркер ARM_NOTESTS"

# ---- 2. the control recipe: the canonical file MINUS exactly the pipefail line -------
CONTROL="$WORK/Dockerfile.tests.nopipefail"
grep -v '^SHELL \[' "$RECIPE" > "$CONTROL" || die "не собрать контрольный рецепт"
REMOVED="$(( $(wc -l < "$RECIPE") - $(wc -l < "$CONTROL") ))"
[ "$REMOVED" -eq 1 ] \
  || die "контрольный рецепт отличается на $REMOVED строк вместо одной — это уже не контроль"
say "контрольный рецепт: канонический минус одна строка ($SHELL_LINE) ✓"
say

# ---- 3. arms -------------------------------------------------------------------------
# rc is read from `docker build` itself — the thing a round actually looks at.
run_arm() { # name recipe context expectation("zero"|"nonzero") why
  local name="$1" recipe="$2" ctx="$3" expect="$4" why="$5"
  local tag="$TAG_PREFIX:$name" log="$WORK/$name.log" rc
  say "--- арм '$name': $why"
  docker build --progress=plain -f "$recipe" -t "$tag" "$ctx" > "$log" 2>&1
  rc=$?
  # Only a build that succeeded leaves an image; pin that one against a concurrent prune.
  if [ "$rc" -eq 0 ]; then
    BUILT_TAGS+=("$tag")
    docker create --name "keep-$name" "$tag" true >/dev/null 2>&1 \
      || say "    (предупреждение: не удалось пиновать $tag контейнером keep-$name)"
  fi
  local verdict="НЕОЖИДАННО"
  case "$expect" in
    zero)    [ "$rc" -eq 0 ] && verdict="ОК" ;;
    nonzero) [ "$rc" -ne 0 ] && verdict="ОК" ;;
    *) die "внутренняя ошибка: неизвестное ожидание '$expect'" ;;
  esac
  say "    docker build rc=$rc, ожидалось $expect -> $verdict"
  grep -E '^#[0-9]+ [0-9.]+ (Test *#|[0-9]+% tests|Errors while|No tests were|The following tests FAILED)' "$log" \
    | sed -E 's/^#[0-9]+ [0-9.]+ /      ctest| /' | head -12
  if [ "$verdict" != "ОК" ]; then
    say "    --- хвост лога $log ---"
    tail -25 "$log" | sed 's/^/      /'
    die "арм '$name' повёл себя не так, как заявлено"
  fi
  say
}

say "=== check_tests_recipe: 4 арма ==="
say
run_arm pass    "$RECIPE"  "$WORK/pass"    zero    "один проходящий тест — сборка должна пройти"
run_arm fail    "$RECIPE"  "$WORK/fail"    nonzero "добавлен падающий тест — сборка ОБЯЗАНА упасть"
run_arm notests "$RECIPE"  "$WORK/notests" nonzero "тестов нет вовсе — сборка ОБЯЗАНА упасть"
run_arm control "$CONTROL" "$WORK/fail"    zero    "тот же падающий тест, но рецепт без pipefail — дефект виден: сборка проходит"

cleanup
say "=== ИТОГ: ПРОЙДЕНО ✓ рецепт краснеет на падающем ctest и на пустом наборе;"
say "    без строки pipefail тот же падающий ctest читается зелёным ==="
exit 0
