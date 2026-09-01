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
#   per file. Then it walks the HOST side too and reports files that exist there but
#   not in git. Exits 1 if ANY difference is not covered by the allowlists below.
#
# ALLOWLIST (deliberate, reviewed — see PLAN-MARATHON-2026-08-28 lead decision 3)
#   server/config.yaml     host-owned ADVERTISED addresses — and nothing else. The
#                          six waived fields are named one by one, by full YAML path:
#                            server.lobby.advertisement.ranch.address
#                            server.lobby.advertisement.race.address
#                            server.lobby.advertisement.messenger.address
#                            server.lobby.advertisement.all_chat.address
#                            server.lobby.advertisement.private_chat.address
#                            server.lobby.advertisement.udp_race_relay.address
#                          Those are the addresses the lobby hands to clients, so the
#                          host owns them (127.0.0.1 in git, the public IP on prod).
#                          A LISTEN/bind address (server.<svc>.listen.address, seven of
#                          them, all "0.0.0.0") is NOT waived: 0.0.0.0 -> 127.0.0.1
#                          would make the whole server unreachable and must stop a
#                          deploy. Neither is any port, name, flag or added/removed key.
#                          The check is structural, not a key-name regex: `address:`
#                          appears under BOTH `listen:` and `advertisement:`, so an
#                          `^ *address:` regex waives the bind address too (measured
#                          false green on fixture A6, verify-tools §1.4). Instead both
#                          sides are rewritten with the VALUE of exactly those six
#                          paths blanked out, and the rewritten files must be byte
#                          identical — that is literally "only these keys, only their
#                          values".
#   server/bullied.yaml    dead config; prod keeps two legacy nicknames. Fully waived.
#   game/achievements.yaml comment-only differences (lines whose first non-blank
#   game/care_skills.yaml  character is `#`). Any body change FAILS.
#
# EXPECTED JUNK ON THE HOST (files that are NOT in git and must not be)
#   *.bak-*      backup taken by a round before it overwrote a config
#   *.canon-*    a canon snapshot kept next to the file it was compared against
#   config.yaml.bak-wifi*  the address backup from the 2026-08-08 wifi move
#   Anything else that exists on the host but not in git is reported as DRIFT: it is
#   either a config the repo forgot, or a stray edit nobody wrote down.
#
# WHY THE `-a` FLAGS AND THE COUNT GUARDS (do not remove them)
#   A single NUL byte anywhere in a config used to make `diff`/`grep` declare the
#   pair "binary", print NOTHING, and the allowlist logic then saw zero mismatching
#   lines and said "allowed" — a real changed port slipped through with EXIT 0
#   (proven on fixture verify/neg7, R70-prep). Every diff/grep here is therefore
#   forced to text mode, the per-file line count must be positive whenever `cmp`
#   says the files differ, and the number of files actually walked is compared with
#   the number found. A check that can go blind is worse than no check.
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
#   DRIFT_MIN_FILES  default 22 — the gate refuses to run against fewer config files
#              than this. Bump it deliberately when the repo really gains configs;
#              never lower it to make a run go green.
#
# EXIT CODES
#   0 no drift outside the allowlists · 1 drift · 2 could not read a file, or the
#     scan could not prove it saw everything (undefined, never silently "equal")
set -uo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
HOST="${HOST:-myvps}"
HOST_ROOT="${HOST_ROOT:-/home/dev/alicia-server/config}"
DRIFT_MODE="${DRIFT_MODE:-auto}"
DRIFT_MIN_FILES="${DRIFT_MIN_FILES:-22}"

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

# list_host_files <destination file> -> 0 ok. Relative paths, LC_ALL=C sorted.
list_host_files() {
  local raw="$TMP/host.raw"
  if [ "$DRIFT_MODE" = local ]; then
    ( cd "$HOST_ROOT" && find . -type f ) > "$raw" 2>/dev/null || return 1
  else
    ssh -n -o BatchMode=yes "$HOST" "cd '$HOST_ROOT' && find . -type f" > "$raw" 2>/dev/null || return 1
  fi
  sed 's|^\./||' "$raw" | LC_ALL=C sort > "$1"
}

# The full YAML paths whose VALUE the production host is allowed to own.
# One line per path. Adding a path here is a reviewed decision, not a fix for a red run.
ADVERTISED_PATHS='server.lobby.advertisement.ranch.address
server.lobby.advertisement.race.address
server.lobby.advertisement.messenger.address
server.lobby.advertisement.all_chat.address
server.lobby.advertisement.private_chat.address
server.lobby.advertisement.udp_race_relay.address'

# waive_advertised <file>  -> the same file on stdout, with the value of each path in
# ADVERTISED_PATHS replaced by <WAIVED>. Every other byte, including indentation, key
# order, comments and blank lines, is passed through untouched.
#
# The path is derived from the indentation stack, not from the key name, because the
# key name alone cannot tell `listen.address` from `advertisement.ranch.address`.
# Line count is preserved exactly, so the caller can prove the rewriter ran at all.
waive_advertised() {
  awk -v allow="$ADVERTISED_PATHS" '
    BEGIN { n = split(allow, A, "\n"); for (i = 1; i <= n; i++) if (A[i] != "") ALLOW[A[i]] = 1; sp = 0 }
    function parents(   i, p) { p = ""; for (i = 1; i <= sp; i++) p = (p == "" ? K[i] : p "." K[i]); return p }
    {
      line = $0
      body = line; sub(/^[ \t]*/, "", body)
      ind = length(line) - length(body)
      if (body == "" || substr(body, 1, 1) == "#") { print line; next }
      ci = index(body, ":")
      if (ci == 0) { print line; next }
      key = substr(body, 1, ci - 1)
      if (key !~ /^[A-Za-z0-9_.-]+$/) { print line; next }
      val = substr(body, ci + 1); sub(/^[ \t]+/, "", val)
      while (sp > 0 && I[sp] >= ind) sp--
      if (val == "") { sp++; I[sp] = ind; K[sp] = key; print line; next }   # mapping node
      p = parents(); p = (p == "" ? key : p "." key)                        # scalar leaf
      if (p in ALLOW) { printf "%s%s: <WAIVED>\n", substr(line, 1, ind), key; next }
      print line
    }
  ' "$1"
}

# every changed line (both sides) must match the given extended regex.
# -a everywhere: a NUL byte must not be able to turn this into "nothing changed".
changed_lines_all_match() {
  local repo="$1" host="$2" re="$3" bad
  bad="$(diff -a -u "$repo" "$host" \
        | grep -aE '^[-+]' \
        | grep -aEv '^(\+\+\+|---)' \
        | sed -E 's/^[-+]//' \
        | grep -aEcv "$re")"
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

# Blindness guard #1: know up front how many files we are supposed to walk, so a
# truncated find (or a wrong REPO_ROOT) cannot pass as "checked everything".
# LC_ALL=C sort so the order of the report is stable across machines
REL_LIST=()
while IFS= read -r rel; do
  [ -n "$rel" ] && REL_LIST+=("$rel")
done < <(cd "$CFG_ROOT" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
TOTAL=${#REL_LIST[@]}
if [ "$TOTAL" -lt "$DRIFT_MIN_FILES" ]; then
  echo "ОСТАНОВ: под $CFG_ROOT найдено $TOTAL файлов, ожидалось не меньше $DRIFT_MIN_FILES"
  echo "         ноль различий на неполном списке — это не «чисто», это слепота."
  exit 2
fi

DRIFTED=0
UNREADABLE=0
CHECKED=0

for rel in "${REL_LIST[@]}"; do
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

  nlines="$(diff -a -u "$repo_file" "$host_file" | grep -aEc '^[-+]' || true)"
  nlines=$((nlines - 2))   # the ---/+++ header lines

  # Blindness guard #2: cmp says they differ, so diff owes us at least one line.
  # Zero (or the negative count that the old binary-mode bug produced) means the
  # comparison said nothing at all — undefined, not "allowed".
  if [ "$nlines" -le 0 ]; then
    printf '  %-40s ОСТАНОВ: cmp говорит «различаются», а diff не дал ни одной строки (%s)\n' "$rel" "$nlines"
    UNREADABLE=1
    continue
  fi

  case "$rel" in
    server/config.yaml)
      # Only the six ADVERTISED address values may differ. Blank those six values on
      # both sides; whatever still differs is drift — a port, a listen/bind address,
      # a renamed or removed key, anything.
      wrepo="$TMP/waived.repo"; whost="$TMP/waived.host"
      waive_advertised "$repo_file" > "$wrepo"
      waive_advertised "$host_file" > "$whost"
      # Blindness guard #4: the rewriter must have produced a line for every input
      # line. An awk that died would emit nothing on BOTH sides, the two empty files
      # would compare equal, and the waiver would swallow the entire file.
      lr="$(grep -ac '' "$wrepo")"; lh="$(grep -ac '' "$whost")"
      orr="$(grep -ac '' "$repo_file")"; ohh="$(grep -ac '' "$host_file")"
      if [ "$lr" -ne "$orr" ] || [ "$lh" -ne "$ohh" ]; then
        printf '  %-40s ОСТАНОВ: маскировка advertised-адресов дала %s/%s строк вместо %s/%s\n' \
          "$rel" "$lr" "$lh" "$orr" "$ohh"
        UNREADABLE=1
      elif cmp -s "$wrepo" "$whost"; then
        nadv="$(grep -ac '<WAIVED>' "$wrepo")"
        printf '  %-40s РАЗРЕШЕНО: различаются только advertised-адреса (%s строк, %s замаскированных полей)\n' \
          "$rel" "$nlines" "$nadv"
      else
        printf '  %-40s ДРЕЙФ: изменено не только значение advertised-адреса\n' "$rel"
        diff -a -u "$wrepo" "$whost" | grep -aE '^[-+]' | grep -aEv '^(\+\+\+|---)' | sed 's/^/      /'
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
        diff -a -u "$repo_file" "$host_file" | grep -aE '^[-+]' | grep -aEv '^(\+\+\+|---)' \
          | grep -aEv '^[-+][[:space:]]*#' | sed 's/^/      /'
        DRIFTED=1
      fi
      ;;
    *)
      printf '  %-40s ДРЕЙФ (%s строк) — файла нет в allowlist\n' "$rel" "$nlines"
      diff -a -u "$repo_file" "$host_file" | sed -n '3,23p' | sed 's/^/      /'
      DRIFTED=1
      ;;
  esac
done

# ---- the other direction: what the host has and git does not --------------------
# Round backups are expected sediment; anything else means the repo is not the whole
# source of truth after all, and a deploy would ship an incomplete config set.
echo
HOST_FILES=0
EXTRA_OK=0
EXTRA_BAD=0
host_list="$TMP/host.list"
if ! list_host_files "$host_list"; then
  echo "  ОСТАНОВ: не удалось перечислить файлы на стороне хоста"
  UNREADABLE=1
else
  while IFS= read -r hrel; do
    [ -n "$hrel" ] || continue
    HOST_FILES=$((HOST_FILES + 1))
    [ -f "$CFG_ROOT/$hrel" ] && continue
    case "$hrel" in
      *.bak-*|*.canon-*|*config.yaml.bak-wifi*)
        printf '  %-40s только на хосте — ОЖИДАЕМЫЙ МУСОР (бэкап раунда)\n' "$hrel"
        EXTRA_OK=$((EXTRA_OK + 1))
        ;;
      *)
        printf '  %-40s ДРЕЙФ: есть на хосте, НЕТ в git, и это не бэкап\n' "$hrel"
        EXTRA_BAD=$((EXTRA_BAD + 1))
        DRIFTED=1
        ;;
    esac
  done < "$host_list"
fi

# undefined beats "differs": a file we could not read is never reported as equal
RC=0
[ "$DRIFTED" -eq 1 ] && RC=1
[ "$UNREADABLE" -eq 1 ] && RC=2

# Blindness guard #3: the loop must have visited every file we found.
if [ "$CHECKED" -ne "$TOTAL" ]; then
  echo "ОСТАНОВ: пройдено $CHECKED из $TOTAL файлов — обход оборвался"
  RC=2
fi

echo
echo "проверено файлов: $CHECKED (из $TOTAL найденных, минимум $DRIFT_MIN_FILES)"
echo "файлов на хосте : $HOST_FILES · лишних ожидаемых (бэкапы): $EXTRA_OK · лишних НЕожиданных: $EXTRA_BAD"
case "$RC" in
  0) echo "=== ИТОГ: ДРЕЙФА НЕТ ✓ ===" ;;
  1) echo "=== ИТОГ: ДРЕЙФ ✗ — git и прод-хост разошлись вне allowlist ===" ;;
  2) echo "=== ИТОГ: ОСТАНОВ — величина не определена (файл не прочитан или обход неполон) ===" ;;
esac
exit "$RC"
