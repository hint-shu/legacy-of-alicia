#!/usr/bin/env bash
#
# build_from_branch.sh REF TAG [WORK] — build a round candidate image from git.
#
# WHY THIS EXISTS
#   Round candidates used to be produced by replaying a 36k-line patch canon over a
#   pinned upstream clone. The repository is the source of truth now, so a round is
#   "a branch" and building it must be one command that either produces a named image
#   or stops loudly. Everything below is a real guard (`[ … ] || die`), never a bare
#   grep whose output nobody reads: the failure this script exists to prevent is a
#   build that silently produced something other than the tree you asked for.
#
#   Two of those guards were written against observed behaviour, not theory:
#     * `git submodule update --init --recursive` EXITS 0 AND DOES NOTHING when the
#       clone has no .gitmodules wiring — so the count of populated submodules is
#       compared with the count declared in .gitmodules.
#     * `docker build -q` prints an id, but the id that matters is the one docker
#       reports for the tag afterwards; both are captured and must agree.
#
# WHAT IT DOES
#   clone REPO at REF into WORK → submodules → quiet-logging gate on the CLONE →
#   docker build -t alicia-server-ru:TAG → WORK/manifest.json
#
# USAGE
#   bash tools/round/build_from_branch.sh r70-topic r70cand
#   bash tools/round/build_from_branch.sh main r70tools-check /tmp/scratch-build
#
# ENV
#   REPO        default: the repository this script lives in
#   IMAGE_REPO  default: alicia-server-ru   (image name before the colon)
#
# EXIT CODES  0 built and verified · 1 any guard failed
set -uo pipefail

die() { echo "ОСТАНОВ: $*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

[ $# -ge 2 ] || die "usage: build_from_branch.sh REF TAG [WORK]"
REF="$1"; TAG="$2"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$SELF_DIR/../.." && pwd)}"
WORK="${3:-$HOME/rounds/$TAG}"
IMAGE_REPO="${IMAGE_REPO:-alicia-server-ru}"
IMAGE="$IMAGE_REPO:$TAG"

command -v docker >/dev/null 2>&1 || die "нет docker"
command -v git    >/dev/null 2>&1 || die "нет git"

git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "$REPO — не git-репозиторий"
COMMIT="$(git -C "$REPO" rev-parse --verify "$REF^{commit}" 2>/dev/null)" \
  || die "ref '$REF' не разрешается в коммит в $REPO"
[ -n "$COMMIT" ] || die "пустой коммит для '$REF'"

# A pre-existing work tree is never reused: it could hold another round's checkout and
# the build would then be of something nobody asked for.
if [ -e "$WORK" ]; then
  [ -d "$WORK" ] || die "$WORK существует и это не каталог"
  [ -z "$(ls -A "$WORK" 2>/dev/null)" ] || die "$WORK непуст — удалите его явным путём и повторите"
fi

say "=== build_from_branch ==="
say "repo   : $REPO"
say "ref    : $REF -> $COMMIT"
say "work   : $WORK"
say "image  : $IMAGE"
say

# ---- 1. clone (CLONE, not worktree: the Dockerfile runs `git init` + submodule
#         update INSIDE the image over the copied .git, and a worktree's .git is a
#         file pointing at an absolute host path that does not exist in the image)
mkdir -p "$(dirname "$WORK")" || die "не создать $(dirname "$WORK")"
git clone -q "$REPO" "$WORK" || die "клон не удался"
git -C "$WORK" checkout -q --detach "$COMMIT" || die "checkout $COMMIT не удался"
GOT="$(git -C "$WORK" rev-parse HEAD)"
[ "$GOT" = "$COMMIT" ] || die "клон стоит на $GOT, а просили $COMMIT"
[ -z "$(git -C "$WORK" status --porcelain)" ] || die "клон грязный сразу после checkout"
say "  клон на $COMMIT, дерево чистое ✓"

# ---- 2. submodules, with a count guard (the command lies by exiting 0) ----------
if [ -f "$WORK/.gitmodules" ]; then
  git -C "$WORK" submodule update --init --recursive -q || die "submodule update провалился"
  WANT_SM="$(grep -cE '^[[:space:]]*path[[:space:]]*=' "$WORK/.gitmodules")"
  HAVE_SM=0
  while IFS= read -r sp; do
    [ -n "$sp" ] || continue
    [ -n "$(ls -A "$WORK/$sp" 2>/dev/null)" ] && HAVE_SM=$((HAVE_SM + 1))
  done < <(sed -nE 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$WORK/.gitmodules")
  [ "$HAVE_SM" -eq "$WANT_SM" ] \
    || die "субмодулей заполнено $HAVE_SM из $WANT_SM — сборка взяла бы пустые 3rd-party"
  say "  субмодули: $HAVE_SM/$WANT_SM заполнены ✓"
else
  say "  субмодулей нет (.gitmodules отсутствует)"
fi

# ---- 3. tree gates, run against the CLONE (not against the repo you are sitting in)
QUIET_GATE="$WORK/tools/check_quiet_logging.sh"
[ -f "$QUIET_GATE" ] || die "в клоне нет $QUIET_GATE"
ROOT="$WORK" bash "$QUIET_GATE" > "$WORK/.gate-quiet.log" 2>&1
QRC=$?
sed 's/^/    /' "$WORK/.gate-quiet.log"
[ "$QRC" -eq 0 ] || die "quiet-logging gate вернул $QRC — сборка не начата"
say "  quiet-logging gate: EXIT=0 ✓"

# ---- 4. the base image must be pinned by digest ---------------------------------
BASE_DIGEST="$(sed -nE 's/^FROM[[:space:]]+[^[:space:]@]+@(sha256:[0-9a-f]{64}).*/\1/p' \
                "$WORK/Dockerfile" | head -1)"
[ -n "$BASE_DIGEST" ] \
  || die "Dockerfile не пинит базовый образ по digest — лесенка станет нечитаемой при обновлении базы"
say "  база пинована: $BASE_DIGEST ✓"

# ---- 5. build --------------------------------------------------------------------
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "  docker build … (контекст $WORK)"
BUILD_OUT="$(cd "$WORK" && docker build -q -t "$IMAGE" . 2>"$WORK/.build.err")"
BRC=$?
[ "$BRC" -eq 0 ] || { sed 's/^/    /' "$WORK/.build.err" >&2; die "docker build вернул $BRC"; }

IMAGE_ID="$(docker image inspect "$IMAGE" --format '{{.Id}}' 2>/dev/null)"
case "$IMAGE_ID" in sha256:*) : ;; *) die "не получен image id для $IMAGE" ;; esac
# `docker build -q` and the tag lookup must agree; if they do not, the tag was moved
# by someone else between the two calls and the manifest would name the wrong image.
if [ -n "$BUILD_OUT" ] && [ "$BUILD_OUT" != "$IMAGE_ID" ]; then
  die "docker build напечатал $BUILD_OUT, а тег $IMAGE указывает на $IMAGE_ID"
fi

# ---- 6. the image must actually carry the server binary -------------------------
CID="$(docker create "$IMAGE" 2>/dev/null)"
[ -n "$CID" ] || die "docker create не дал контейнера для $IMAGE"
BIN_TMP="$(mktemp -d)"
docker cp "$CID:/usr/local/bin/alicia-server" "$BIN_TMP/alicia-server" >/dev/null 2>&1
CPRC=$?
docker rm -f "$CID" >/dev/null 2>&1
if [ "$CPRC" -ne 0 ]; then rm -rf "$BIN_TMP"; die "в образе нет /usr/local/bin/alicia-server"; fi
BIN_SIZE="$(stat -c%s "$BIN_TMP/alicia-server")"
BIN_MD5="$(md5sum "$BIN_TMP/alicia-server" | cut -d' ' -f1)"
rm -rf "$BIN_TMP"
[ "$BIN_SIZE" -gt 1000000 ] || die "бинарь подозрительно мал ($BIN_SIZE Б)"
say "  бинарь: $BIN_SIZE Б, md5 $BIN_MD5 ✓"

# ---- 7. manifest -----------------------------------------------------------------
MANIFEST="$WORK/manifest.json"
command cat > "$MANIFEST" <<JSON
{
  "ref": "$REF",
  "commit": "$COMMIT",
  "tag": "$IMAGE",
  "image_id": "$IMAGE_ID",
  "built_at": "$BUILT_AT",
  "base_digest": "$BASE_DIGEST",
  "binary_size": $BIN_SIZE,
  "binary_md5": "$BIN_MD5"
}
JSON
[ -s "$MANIFEST" ] || die "manifest.json не записан"
say
sed 's/^/  /' "$MANIFEST"
say
say "=== ИТОГ: ОБРАЗ СОБРАН ✓  $IMAGE = $IMAGE_ID ==="
exit 0
