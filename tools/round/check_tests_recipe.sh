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
#   CMake project with no compiled code, only ctest entries — in eight arms. Every arm's
#   context is seeded as a git repository shaped like a round clone (a base commit, a
#   candidate commit, `refs/remotes/origin/main` on the base, detached HEAD, NO local
#   `main`), because half of what is being tested is what the recipe does about that.
#
#     1 pass       one passing test                      expect docker build rc = 0
#     2 fail       one passing + one failing test        expect docker build rc != 0
#     3 notests    no tests registered at all            expect docker build rc != 0
#     4 control    arm 2's context, but the recipe with  expect docker build rc = 0
#                  its `SHELL [… pipefail …]` line cut
#     5 delta      a delta gate + the default BASE_REF   expect docker build rc = 0
#     6 deltaself  arm 5, but --build-arg BASE_REF=HEAD  expect docker build rc != 0
#                  (the branch compared against ITSELF)
#     7 nobase     arm 5, but a BASE_REF that resolves   expect docker build rc != 0
#                  to nothing
#     8 nobasestep arm 5's context, but the recipe with   expect docker build rc != 0
#                  the whole base-ref step cut out
#
#   Arm 4 is the honest half of the pipefail proof. Without it, arm 2 going red only
#   shows that *something* in the file fails the build; arm 4 shows the failing test
#   slipping through the moment the pipefail line is removed and nothing else changes,
#   which is the defect this recipe was written against.
#
#   Arms 5-8 are the same idea for the base ref. Arm 5 alone would only show a green
#   build; arm 6 is the one that matters — it hands the recipe a base that IS the branch,
#   the delta collapses to nothing, and the gate that reads it must stop (code 2) instead
#   of reporting clean. Arm 7 shows the recipe refusing to build at all when the base
#   cannot be resolved, rather than letting that surface minutes later inside ctest.
#   Arm 8 is to the base-ref step what arm 4 is to the pipefail line: the canonical file
#   with that step and nothing else cut out, on arm 5's context — the delta gate then
#   finds no `main` at all and goes blind, which is exactly what every round clone did
#   before this step existed (measured on a clone of origin/main: ctest 32/34, the two
#   red being the R75 delta gates' self-tests). The stand-in gate is
#   tools/round/fixtures/tests-recipe/delta_gate.sh; it copies the contract of the
#   repository's real delta gates (tools/r75_*_gate.sh) and nothing else.
#
# USAGE
#   bash tools/round/check_tests_recipe.sh
#
# ENV
#   TAG_PREFIX  default: loa-tests-recipe-selftest   (images built and removed here)
#   KEEP_WORK   set to 1 to leave the temporary contexts and build logs behind
#
# EXIT CODES  0 all eight arms behaved as stated · 1 any arm behaved otherwise, or setup
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
command -v git    >/dev/null 2>&1 || die "нет git"
[ -f "$RECIPE" ]  || die "нет $RECIPE"
[ -f "$FIXTURE/CMakeLists.txt" ] || die "нет фикстуры $FIXTURE/CMakeLists.txt"
[ -f "$FIXTURE/delta_gate.sh" ]  || die "нет фикстурного дельта-гейта $FIXTURE/delta_gate.sh"

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
grep -q '^ARG BASE_REF=' "$RECIPE" \
  || die "в рецепте нет ARG BASE_REF — база дельта-гейтов снова отдана на волю контекста"
grep -q 'git update-ref refs/heads/main' "$RECIPE" \
  || die "рецепт не записывает refs/heads/main — дельта-гейты в клоне раунда снова слепы"
say "рецепт: $RECIPE"
say "  SHELL bash -o pipefail ✓ · ctest | tee ✓ · --no-tests=error ✓"
say "  ARG BASE_REF ✓ · update-ref refs/heads/main ✓"
say

WORK="$(mktemp -d)" || die "не создать временный каталог"

# ---- 1. contexts ---------------------------------------------------------------------
# Each context is a git repository in the SHAPE OF A ROUND CLONE, and the shape is the
# point: `build_from_branch.sh` clones and then `checkout --detach`, which leaves the
# clone with the round's branch as its only local head — measured 2026-09-04, ~/rounds/
# r74cand2 and r78cand5 carry `refs/heads/<round branch>` and no `main` whatsoever. A
# fixture that quietly had a `main` would let a recipe with no base-ref step pass here
# while every real round clone still went red.
#
#   commit 1 "база"      delta_subject.txt = base       <- refs/remotes/origin/main
#   commit 2 "кандидат"  delta_subject.txt = candidate  <- HEAD (detached)
#   no local heads at all
seed_repo() { # dir
  local d="$1" base_sha
  git -C "$d" init -q -b arm-branch                      || return 1
  git -C "$d" config user.email "recipe-selftest@invalid" || return 1
  git -C "$d" config user.name  "tests recipe self-test"  || return 1
  printf 'base\n' > "$d/delta_subject.txt"               || return 1
  git -C "$d" add -A >/dev/null 2>&1                     || return 1
  git -C "$d" commit -q -m "база фикстуры"               || return 1
  base_sha="$(git -C "$d" rev-parse HEAD)"               || return 1
  git -C "$d" update-ref refs/remotes/origin/main "$base_sha" || return 1
  printf 'candidate\n' > "$d/delta_subject.txt"          || return 1
  git -C "$d" add -A >/dev/null 2>&1                     || return 1
  git -C "$d" commit -q -m "кандидат фикстуры"           || return 1
  git -C "$d" checkout -q --detach HEAD                  || return 1
  git -C "$d" branch -q -D arm-branch                    || return 1
  # A fixture that lied about its own shape would make every arm below meaningless.
  git -C "$d" rev-parse --verify main >/dev/null 2>&1 \
    && { echo "в контексте $d есть локальный main — форма клона раунда не воспроизведена" >&2; return 1; }
  [ -z "$(git -C "$d" for-each-ref --format='%(refname)' refs/heads)" ] || return 1
  git -C "$d" rev-parse --verify origin/main >/dev/null 2>&1 || return 1
  return 0
}

for arm in pass fail notests delta deltaself nobase; do
  mkdir -p "$WORK/$arm" || die "не создать $WORK/$arm"
  cp "$FIXTURE/CMakeLists.txt" "$FIXTURE/delta_gate.sh" "$WORK/$arm/" \
    || die "не скопировать фикстуру в $arm"
done
: > "$WORK/fail/ARM_FAIL"        || die "не создать маркер ARM_FAIL"
: > "$WORK/notests/ARM_NOTESTS"  || die "не создать маркер ARM_NOTESTS"
for arm in delta deltaself nobase; do
  : > "$WORK/$arm/ARM_DELTA"     || die "не создать маркер ARM_DELTA в $arm"
done
for arm in pass fail notests delta deltaself nobase; do
  seed_repo "$WORK/$arm" || die "не засеять git-репозиторий формы клона раунда в $arm"
done
say "контексты: 6 шт., каждый — git-репозиторий формы клона раунда"
say "  (origin/main на базовом коммите, HEAD отцеплен, локального main НЕТ) ✓"
say

# ---- 2. the control recipe: the canonical file MINUS exactly the pipefail line -------
CONTROL="$WORK/Dockerfile.tests.nopipefail"
grep -v '^SHELL \[' "$RECIPE" > "$CONTROL" || die "не собрать контрольный рецепт"
REMOVED="$(( $(wc -l < "$RECIPE") - $(wc -l < "$CONTROL") ))"
[ "$REMOVED" -eq 1 ] \
  || die "контрольный рецепт отличается на $REMOVED строк вместо одной — это уже не контроль"
say "контрольный рецепт: канонический минус одна строка ($SHELL_LINE) ✓"

# ---- 2b. the second control: the canonical file MINUS exactly the base-ref step ------
# Same reasoning as above, for the other half of the recipe. Arm 5 going green shows
# that a base ref reached the gate; only this control shows that it reached it FROM
# HERE, and that cutting the step out puts the gate back in the blind state the rounds
# were measured in. The excision is asserted, not assumed: the result must have lost the
# ARG and the update-ref, and must have kept the pipefail line and the ctest pipeline —
# otherwise it is testing something else.
CONTROL_NOBASE="$WORK/Dockerfile.tests.nobasestep"
awk '
  /^ARG BASE_REF=/ { cutting = 1; next }
  cutting && /^    fi$/ { cutting = 0; next }
  cutting { next }
  { print }
' "$RECIPE" > "$CONTROL_NOBASE" || die "не собрать контрольный рецепт без шага базы"
# Grep the INSTRUCTIONS, not the comments: the file's header explains BASE_REF at
# length, and a check that reads the comments would pass on a control that still
# carries the step.
grep -v '^#' "$CONTROL_NOBASE" | grep -q 'BASE_REF' \
  && die "в контроле без шага базы остался BASE_REF в инструкциях — вырезано не то"
grep -v '^#' "$CONTROL_NOBASE" | grep -q 'update-ref refs/heads/main' \
  && die "в контроле без шага базы осталась запись refs/heads/main — вырезано не то"
grep -q '^SHELL \["/bin/bash", "-o", "pipefail", "-c"\]$' "$CONTROL_NOBASE" \
  || die "контроль без шага базы потерял строку pipefail — это уже другой контроль"
grep -q 'ctest .*--no-tests=error.*| tee ' "$CONTROL_NOBASE" \
  || die "контроль без шага базы потерял прогон ctest — проверять нечего"
CUT="$(( $(wc -l < "$RECIPE") - $(wc -l < "$CONTROL_NOBASE") ))"
[ "$CUT" -ge 5 ] \
  || die "контроль без шага базы отличается всего на $CUT строк — блок вырезан не целиком"
say "контрольный рецепт 2: канонический минус шаг подачи базы ($CUT строк) ✓"
say

# ---- 3. arms -------------------------------------------------------------------------
# rc is read from `docker build` itself — the thing a round actually looks at.
run_arm() { # name recipe context expectation("zero"|"nonzero") why [extra docker args…]
  local name="$1" recipe="$2" ctx="$3" expect="$4" why="$5"
  shift 5
  local tag="$TAG_PREFIX:$name" log="$WORK/$name.log" rc
  say "--- арм '$name': $why"
  [ $# -gt 0 ] && say "    доп. аргументы сборки: $*"
  docker build --progress=plain "$@" -f "$recipe" -t "$tag" "$ctx" > "$log" 2>&1
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
  grep -E '^#[0-9]+ [0-9.]+ +(Test *#|[0-9]+% tests|Errors while|No tests were|The following tests FAILED|BASE_REF=|ОСТАНОВ|ВНИМАНИЕ)' "$log" \
    | sed -E 's/^#[0-9]+ [0-9.]+ +/      | /' | head -14
  if [ "$verdict" != "ОК" ]; then
    say "    --- хвост лога $log ---"
    tail -25 "$log" | sed 's/^/      /'
    die "арм '$name' повёл себя не так, как заявлено"
  fi
  say
}

say "=== check_tests_recipe: 8 армов ==="
say
run_arm pass    "$RECIPE"  "$WORK/pass"    zero    "один проходящий тест — сборка должна пройти"
run_arm fail    "$RECIPE"  "$WORK/fail"    nonzero "добавлен падающий тест — сборка ОБЯЗАНА упасть"
run_arm notests "$RECIPE"  "$WORK/notests" nonzero "тестов нет вовсе — сборка ОБЯЗАНА упасть"
run_arm control "$CONTROL" "$WORK/fail"    zero    "тот же падающий тест, но рецепт без pipefail — дефект виден: сборка проходит"
run_arm delta   "$RECIPE"  "$WORK/delta"   zero    "дельта-гейт и база по умолчанию (origin/main) — рецепт обязан подать базу, гейт видит дельту"
run_arm deltaself "$RECIPE" "$WORK/deltaself" nonzero \
        "тот же гейт, но база = сама ветка (BASE_REF=HEAD) — дельта пуста, гейт ОБЯЗАН остановиться, а не отчитаться чисто" \
        --build-arg BASE_REF=HEAD
run_arm nobase  "$RECIPE"  "$WORK/nobase"  nonzero \
        "база не разрешается вовсе — сборка ОБЯЗАНА остановиться на месте, а не отдать это ctest'у" \
        --build-arg BASE_REF=refs/heads/net-takoi-bazy
run_arm nobasestep "$CONTROL_NOBASE" "$WORK/nobase" nonzero \
        "тот же дельта-гейт, но рецепт БЕЗ шага подачи базы — гейт не находит main и слепнет: сборка краснеет"

cleanup
say "=== ИТОГ: ПРОЙДЕНО ✓ рецепт краснеет на падающем ctest и на пустом наборе;"
say "    без строки pipefail тот же падающий ctest читается зелёным;"
say "    рецепт подаёт дельта-гейтам базу — на базе-самой-себе, на неразрешимой базе"
say "    и на рецепте без шага подачи базы сборка краснеет, а не проходит ==="
exit 0
