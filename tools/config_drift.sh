#!/usr/bin/env bash
#
# config_drift.sh — gate: the git tree must reproduce the config the live server runs.
#
# WHY THIS EXISTS
#   Production bind-mounts its whole `config` directory from the host
#   (/home/dev/alicia-server/config -> /var/lib/alicia-server/config), so the copy
#   baked into the image is NEVER read there. That makes it possible for the host
#   copy and this repository to drift apart silently — and it already happened:
#   round R68 overwrote the host courses.yaml from the image and deleted a tutorial
#   block that only ever existed on the host (backlog #219). Since R70 this repo is
#   the source of truth for resources/config/**, so any difference that is not an
#   explicitly known one is an error and must stop a deploy.
#
# WHAT IT DOES
#   For every file under <repo>/resources/config/**, fetch the host counterpart
#   (`ssh $HOST cat $HOST_ROOT/<same relative path>`) and compare. Prints a verdict
#   per file. Exits 1 if ANY file differs in a way the allowlist below does not cover.
#
# ALLOWLIST (deliberate, reviewed — see PLAN-MARATHON-2026-08-28 lead decision 3)
#   server/config.yaml     host-owned advertised addresses. Only lines whose key is
#                          `address:` (or `host:`) may differ; a changed port, name
#                          or anything else FAILS.
#   server/bullied.yaml    dead config; prod keeps two legacy nicknames. Fully waived.
#   game/achievements.yaml comment-only differences (lines whose first non-blank
#   game/care_skills.yaml  character is `#`). Any body change FAILS.
#
# USAGE
#   bash tools/config_drift.sh
#   HOST_ROOT=/tmp/fake-host-config bash tools/config_drift.sh   # compare against a local tree
#
# ENV
#   REPO_ROOT  default: the repository this script lives in
#   HOST       default: myvps        (ssh destination)
#   HOST_ROOT  default: /home/dev/alicia-server/config
#   DRIFT_MODE auto|ssh|local  default auto — "local" when HOST_ROOT is a directory
#              that exists on THIS machine, "ssh" otherwise.
#
# EXIT CODES
#   0 no drift outside the allowlist · 1 drift · 2 could not read a file (undefined,
#   never silently "equal")
set -uo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
HOST="${HOST:-myvps}"
HOST_ROOT="${HOST_ROOT:-/home/dev/alicia-server/config}"
DRIFT_MODE="${DRIFT_MODE:-auto}"

CFG_ROOT="$REPO_ROOT/resources/config"
[ -d "$CFG_ROOT" ] || { echo "ОСТАНОВ: нет каталога $CFG_ROOT"; exit 2; }

if [ "$DRIFT_MODE" = auto ]; then
  if [ -d "$HOST_ROOT" ]; then DRIFT_MODE=local; else DRIFT_MODE=ssh; fi
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# fetch <relative path> <destination file>  -> 0 ok, non-zero if the file is unreadable
fetch() {
  if [ "$DRIFT_MODE" = local ]; then
    cat "$HOST_ROOT/$1" > "$2" 2>/dev/null
  else
    ssh -n -o BatchMode=yes "$HOST" "cat '$HOST_ROOT/$1'" > "$2" 2>/dev/null
  fi
}

# every changed line (both sides) must match the given extended regex
changed_lines_all_match() {
  local repo="$1" host="$2" re="$3" bad
  bad="$(diff -u "$repo" "$host" \
        | grep -E '^[-+]' \
        | grep -Ev '^(\+\+\+|---)' \
        | sed -E 's/^[-+]//' \
        | grep -Ecv "$re")"
  [ "$bad" -eq 0 ]
}

echo "=== config drift gate ==="
echo "repo : $CFG_ROOT"
if [ "$DRIFT_MODE" = ssh ]; then
  echo "host : $HOST:$HOST_ROOT  (mode=ssh)"
else
  echo "host : $HOST_ROOT  (mode=local)"
fi
echo

DRIFTED=0
UNREADABLE=0
CHECKED=0

# LC_ALL=C sort so the order of the report is stable across machines
while IFS= read -r rel; do
  CHECKED=$((CHECKED + 1))
  repo_file="$CFG_ROOT/$rel"
  host_file="$TMP/host.copy"

  if ! fetch "$rel" "$host_file"; then
    printf '  %-40s ОТСУТСТВУЕТ НА ХОСТЕ (или нечитаем)\n' "$rel"
    UNREADABLE=1
    continue
  fi

  if cmp -s "$repo_file" "$host_file"; then
    printf '  %-40s OK (идентичны)\n' "$rel"
    continue
  fi

  nlines="$(diff -u "$repo_file" "$host_file" | grep -Ec '^[-+]' || true)"
  nlines=$((nlines - 2))   # the ---/+++ header lines

  case "$rel" in
    server/config.yaml)
      # only advertised addresses may differ
      if changed_lines_all_match "$repo_file" "$host_file" '^[[:space:]]*(address|host):'; then
        printf '  %-40s РАЗРЕШЕНО: только advertised-адреса (%s строк)\n' "$rel" "$nlines"
      else
        printf '  %-40s ДРЕЙФ: изменены не только адреса\n' "$rel"
        diff -u "$repo_file" "$host_file" | grep -E '^[-+]' | grep -Ev '^(\+\+\+|---)' \
          | grep -Ev '^[-+][[:space:]]*(address|host):' | sed 's/^/      /'
        DRIFTED=1
      fi
      ;;
    server/bullied.yaml)
      printf '  %-40s РАЗРЕШЕНО: мёртвый конфиг, прод хранит свой список (%s строк)\n' "$rel" "$nlines"
      ;;
    game/achievements.yaml|game/care_skills.yaml)
      # comment-only header differences
      if changed_lines_all_match "$repo_file" "$host_file" '^[[:space:]]*#'; then
        printf '  %-40s РАЗРЕШЕНО: различие только в комментариях (%s строк)\n' "$rel" "$nlines"
      else
        printf '  %-40s ДРЕЙФ: изменены не только комментарии\n' "$rel"
        diff -u "$repo_file" "$host_file" | grep -E '^[-+]' | grep -Ev '^(\+\+\+|---)' \
          | grep -Ev '^[-+][[:space:]]*#' | sed 's/^/      /'
        DRIFTED=1
      fi
      ;;
    *)
      printf '  %-40s ДРЕЙФ (%s строк) — файла нет в allowlist\n' "$rel" "$nlines"
      diff -u "$repo_file" "$host_file" | sed -n '3,23p' | sed 's/^/      /'
      DRIFTED=1
      ;;
  esac
done < <(cd "$CFG_ROOT" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)

# undefined beats "differs": a file we could not read is never reported as equal
RC=0
[ "$DRIFTED" -eq 1 ] && RC=1
[ "$UNREADABLE" -eq 1 ] && RC=2

echo
echo "проверено файлов: $CHECKED"
case "$RC" in
  0) echo "=== ИТОГ: ДРЕЙФА НЕТ ✓ ===" ;;
  1) echo "=== ИТОГ: ДРЕЙФ ✗ — git и прод-хост разошлись вне allowlist ===" ;;
  2) echo "=== ИТОГ: ОСТАНОВ — величина не определена (файл не прочитан) ===" ;;
esac
exit "$RC"
