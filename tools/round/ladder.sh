#!/usr/bin/env bash
#
# ladder.sh CONTROL_TAG CAND_TAG [NEG_TAG...] --grow SYM [--grow SYM] \
#           --control-symbol SYM [--marker 'string'=DELTA ...] [--deep]
#
# WHY THIS EXISTS
#   A "quiet" round changes code inside existing functions: no new symbol, no new log
#   line, nothing a string search can see. Comparing the SET of symbols between the
#   production image and the candidate is then useless — it is equal even for a
#   deliberately broken build (measured: three defective negatives all had identical
#   symbol sets). What does fire is the SIZE of the symbols. So the ladder is built on
#   `nm -C -S` sizes, and every claim it makes is paired with an arm that must fail.
#
#   Three rules are baked in because breaking them produced false greens before:
#     * strings are counted with `strings -a -n6 | grep -cF`, NEVER `grep -ac` on the
#       binary — grep glues NUL-separated messages into one "line" and undercounts;
#     * a control symbol that must NOT change size is mandatory: without it a ladder
#       where everything moved (a toolchain change, say) still reads as success;
#     * every arm is checked for having been read at all (symbol and string counts
#       above a floor) — a failed extraction must not pass as "nothing changed".
#
# ARGUMENTS
#   CONTROL_TAG   the image the round is measured against — normally the image that is
#                 actually running in production, carried over, never rebuilt
#   CAND_TAG      the candidate image for this round
#   NEG_TAG...    zero or more negative images (one removed protection each). Each must
#                 differ from the candidate AND from the control: an arm equal to the
#                 control is the control rebuilt, it isolates nothing (see assertion 3).
#   --grow SYM            substring of a symbol that MUST change size in CAND vs CONTROL.
#                         Repeatable. Matching is a plain substring of the demangled name.
#   --control-symbol SYM  substring of a symbol that must NOT change size in ANY arm.
#   --marker 'STR'=DELTA  a runtime string whose count in CAND minus its count in
#                         CONTROL must equal DELTA (may be negative). Repeatable.
#   --deep                also run the normalized-disassembly and rootfs-manifest
#                         comparisons (slow, needs objdump and python3)
#
# ENV
#   IMAGE_REPO   default alicia-server-ru — used only when a tag has no colon
#   LADDER_WORK  default: a fresh mktemp -d, removed on exit. Point it somewhere
#                durable to keep the extracted binaries for review.
#   BIN_PATH     default /usr/local/bin/alicia-server
#   MIN_SYMBOLS  default 1000   blindness floor
#   MIN_STRINGS  default 1000   blindness floor
#
# EXIT CODES  0 every assertion held · 1 any assertion failed · 2 an arm could not be read
set -uo pipefail

die()  { echo "ОСТАНОВ: $*" >&2; exit 2; }
fail() { echo "  ✗ $*"; VIOLATIONS=$((VIOLATIONS + 1)); }
ok()   { echo "  ✓ $*"; }

IMAGE_REPO="${IMAGE_REPO:-alicia-server-ru}"
BIN_PATH="${BIN_PATH:-/usr/local/bin/alicia-server}"
MIN_SYMBOLS="${MIN_SYMBOLS:-1000}"
MIN_STRINGS="${MIN_STRINGS:-1000}"
VIOLATIONS=0
DEEP=0
TAGS=(); GROW=(); MARKERS=(); CONTROL_SYMBOL=""

# ---- argument parsing ------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --grow)           [ $# -ge 2 ] || die "--grow без значения";           GROW+=("$2"); shift 2 ;;
    --control-symbol) [ $# -ge 2 ] || die "--control-symbol без значения"; CONTROL_SYMBOL="$2"; shift 2 ;;
    --marker)         [ $# -ge 2 ] || die "--marker без значения";         MARKERS+=("$2"); shift 2 ;;
    --deep)           DEEP=1; shift ;;
    -*)               die "неизвестный флаг: $1" ;;
    *)                TAGS+=("$1"); shift ;;
  esac
done

[ ${#TAGS[@]} -ge 2 ] || die "нужны минимум CONTROL_TAG и CAND_TAG"
[ ${#GROW[@]} -ge 1 ] || die "нужен хотя бы один --grow: лесенка без ожидаемого эффекта ничего не доказывает"
[ -n "$CONTROL_SYMBOL" ] || die "нужен --control-symbol: без НЕменяющейся опоры «всё изменилось» читается как успех"

qualify() { case "$1" in *:*) printf '%s' "$1" ;; *) printf '%s:%s' "$IMAGE_REPO" "$1" ;; esac; }

CONTROL_TAG="$(qualify "${TAGS[0]}")"
CAND_TAG="$(qualify "${TAGS[1]}")"
NEG_TAGS=()
for ((i=2; i<${#TAGS[@]}; i++)); do NEG_TAGS+=("$(qualify "${TAGS[$i]}")"); done

command -v docker  >/dev/null 2>&1 || die "нет docker"
command -v nm      >/dev/null 2>&1 || die "нет nm (binutils)"
command -v strings >/dev/null 2>&1 || die "нет strings (binutils)"

if [ -n "${LADDER_WORK:-}" ]; then
  W="$LADDER_WORK"; mkdir -p "$W" || die "не создать $W"; KEEP=1
else
  W="$(mktemp -d)"; KEEP=0
  trap 'rm -rf "$W"' EXIT
fi

ARM_NAMES=(control cand); ARM_TAGS=("$CONTROL_TAG" "$CAND_TAG")
n=0
for t in "${NEG_TAGS[@]}"; do n=$((n + 1)); ARM_NAMES+=("neg$n"); ARM_TAGS+=("$t"); done

echo "=== ladder ==="
for ((i=0; i<${#ARM_NAMES[@]}; i++)); do printf '  %-8s %s\n' "${ARM_NAMES[$i]}" "${ARM_TAGS[$i]}"; done
echo "  бинарь : $BIN_PATH"
echo "  работа : $W"
echo

# ---- extraction: docker create / cp / rm — no container is ever started ----------
for ((i=0; i<${#ARM_NAMES[@]}; i++)); do
  a="${ARM_NAMES[$i]}"; t="${ARM_TAGS[$i]}"
  docker image inspect "$t" >/dev/null 2>&1 || die "образа $t нет на этой машине"
  cid="$(docker create "$t" 2>/dev/null)"
  [ -n "$cid" ] || die "docker create не дал контейнера для $t"
  docker cp "$cid:$BIN_PATH" "$W/$a.bin" >/dev/null 2>&1
  rc=$?
  docker rm -f "$cid" >/dev/null 2>&1
  [ "$rc" -eq 0 ] || die "не извлечь $BIN_PATH из $t"
  [ -s "$W/$a.bin" ] || die "извлечённый бинарь $a пуст"
done

# ---- per-arm facts + blindness floors --------------------------------------------
printf '%-8s %-14s %12s  %8s %8s  %s\n' arm image_id size symbols strings md5
for ((i=0; i<${#ARM_NAMES[@]}; i++)); do
  a="${ARM_NAMES[$i]}"; t="${ARM_TAGS[$i]}"
  id="$(docker image inspect "$t" --format '{{.Id}}' | cut -c8-19)"
  # "name<TAB>size", LC_ALL=C sorted. Compared later with diff, NOT join: demangled
  # names repeat (D0/D1/D2 destructor clones all print the same text), and join would
  # produce the cartesian product of every repeated name and report thousands of
  # phantom "size changes". diff treats the two files as multisets and does not.
  nm -C -S --defined-only "$W/$a.bin" 2>/dev/null \
    | awk 'NF>=4 && $2 ~ /^[0-9a-fA-F]+$/ && $3 ~ /^[a-zA-Z]$/ { name=$4; for (k=5;k<=NF;k++) name=name" "$k; print name "\t" $2 }' \
    | LC_ALL=C sort > "$W/$a.sizes"
  strings -a -n 6 "$W/$a.bin" > "$W/$a.strings"
  sy="$(grep -c . "$W/$a.sizes")"
  st="$(grep -c . "$W/$a.strings")"
  sz="$(stat -c%s "$W/$a.bin")"
  md5="$(md5sum "$W/$a.bin" | cut -d' ' -f1)"
  printf '%-8s %-14s %12s  %8s %8s  %s\n' "$a" "$id" "$sz" "$sy" "$st" "$md5"
  [ "$sy" -ge "$MIN_SYMBOLS" ] || die "у арки $a всего $sy символов (< $MIN_SYMBOLS) — проверка слепа"
  [ "$st" -ge "$MIN_STRINGS" ] || die "у арки $a всего $st строк (< $MIN_STRINGS) — проверка слепа"
  echo "$md5" > "$W/$a.md5"
  echo "$st"  > "$W/$a.stcount"
done
echo

# ---- which symbols changed size, per arm, against the control --------------------
echo "--- изменения размеров символов относительно control ---"
for ((i=1; i<${#ARM_NAMES[@]}; i++)); do
  a="${ARM_NAMES[$i]}"
  diff "$W/control.sizes" "$W/$a.sizes" > "$W/$a.sizedelta"
  grep '^<' "$W/$a.sizedelta" | sed 's/^< //' | cut -f1 | LC_ALL=C sort -u > "$W/$a.lnames"
  grep '^>' "$W/$a.sizedelta" | sed 's/^> //' | cut -f1 | LC_ALL=C sort -u > "$W/$a.rnames"
  # a name on BOTH sides changed size; a name on one side only appeared or vanished
  comm -12 "$W/$a.lnames" "$W/$a.rnames" > "$W/$a.changed"
  comm -23 "$W/$a.lnames" "$W/$a.rnames" > "$W/$a.gone"
  comm -13 "$W/$a.lnames" "$W/$a.rnames" > "$W/$a.new"
  ch="$(grep -c . "$W/$a.changed")"; gone="$(grep -c . "$W/$a.gone")"; new="$(grep -c . "$W/$a.new")"
  printf '  %-8s изменили размер: %-5s пропало: %-5s появилось: %s\n' "$a" "$ch" "$gone" "$new"
  while IFS= read -r nm_name; do
    [ -n "$nm_name" ] || continue
    from="$(grep -F "$nm_name"$'\t' "$W/control.sizes" | cut -f2 | tr '\n' ',' | sed 's/,$//')"
    to="$(grep -F "$nm_name"$'\t' "$W/$a.sizes" | cut -f2 | tr '\n' ',' | sed 's/,$//')"
    printf '      0x%s -> 0x%s  %s\n' "$from" "$to" "$(printf '%.110s' "$nm_name")"
  done < <(head -12 "$W/$a.changed")
  [ "$ch" -gt 12 ] && echo "      … ещё $((ch - 12))"
done
echo

# ---- assertion 1: every --grow symbol changed in cand vs control -----------------
echo "--- ожидаемый эффект раунда (--grow) ---"
for g in "${GROW[@]}"; do
  in_control="$(grep -cF -- "$g" "$W/control.sizes")"
  in_cand="$(grep -cF -- "$g" "$W/cand.sizes")"
  changed="$(grep -cF -- "$g" "$W/cand.changed")"
  if [ "$in_control" -eq 0 ] || [ "$in_cand" -eq 0 ]; then
    fail "'$g': символ не найден (control=$in_control cand=$in_cand) — опечатка или не тот бинарь"
  elif [ "$changed" -eq 0 ]; then
    fail "'$g': найден ($in_control/$in_cand), но размер НЕ изменился — работа раунда не доехала"
  else
    ok "'$g': изменил размер в $changed символах (найден $in_control/$in_cand)"
    grep -F -- "$g" "$W/cand.changed" | sed 's/^/        /' | cut -c1-120
  fi
done
echo

# ---- assertion 2: the control symbol did not move in ANY arm ---------------------
echo "--- контрольный символ (не должен меняться нигде): $CONTROL_SYMBOL ---"
cs_in_control="$(grep -cF -- "$CONTROL_SYMBOL" "$W/control.sizes")"
if [ "$cs_in_control" -eq 0 ]; then
  fail "контрольный символ не найден в control — опора не существует, лесенка без опоры"
else
  for ((i=1; i<${#ARM_NAMES[@]}; i++)); do
    a="${ARM_NAMES[$i]}"
    moved="$(command cat "$W/$a.changed" "$W/$a.gone" "$W/$a.new" | grep -cF -- "$CONTROL_SYMBOL")"
    present="$(grep -cF -- "$CONTROL_SYMBOL" "$W/$a.sizes")"
    if [ "$present" -eq 0 ]; then
      fail "$a: контрольный символ пропал"
    elif [ "$moved" -ne 0 ]; then
      fail "$a: контрольный символ изменил размер в $moved местах — сдвинулось то, что не должно"
      command cat "$W/$a.changed" "$W/$a.gone" "$W/$a.new" | grep -F -- "$CONTROL_SYMBOL" | sed 's/^/        /' | cut -c1-120
    else
      ok "$a: контрольный символ на месте и не изменился ($present вхождений)"
    fi
  done
fi
echo

# ---- assertion 3: every negative arm differs from the candidate AND from the -----
#      control.
#      WHY BOTH. The candidate comparison catches "the negative's edit never got
#      applied". It does NOT catch the opposite mistake: a negative that equals the
#      CONTROL. That happens when the round's work never reached the negative at all
#      — built from `main` instead of the round branch, or branched off the base
#      instead of off the candidate. Such an arm is the control rebuilt a second time:
#      it isolates nothing, yet the earlier version of this check compared it only
#      with the candidate, found a difference (of course it did — the control differs
#      from the candidate) and printed "ЛЕСЕНКА ЧИСТА ✓". Measured false green.
#      Byte-equality is the loud form. The quiet form is the same tree rebuilt with a
#      different `git describe` string: md5 differs, but not one symbol size moved and
#      the string count is the control's to the last entry. Both are refused here.
if [ ${#NEG_TAGS[@]} -gt 0 ]; then
  echo "--- негативы обязаны отличаться и от кандидата, и от контроля ---"
  cand_md5="$(command cat "$W/cand.md5")"
  control_md5="$(command cat "$W/control.md5")"
  control_st="$(command cat "$W/control.stcount")"
  csym="$(grep -c . "$W/cand.changed")"
  for ((i=2; i<${#ARM_NAMES[@]}; i++)); do
    a="${ARM_NAMES[$i]}"
    amd5="$(command cat "$W/$a.md5")"
    ast="$(command cat "$W/$a.stcount")"
    nsym="$(grep -c . "$W/$a.changed")"
    ngone="$(grep -c . "$W/$a.gone")"
    nnew="$(grep -c . "$W/$a.new")"
    bad=0
    if [ "$amd5" = "$cand_md5" ]; then
      fail "$a: бинарь БАЙТ-В-БАЙТ равен КАНДИДАТУ — правка негатива не применилась, он ничего не проверяет"
      bad=1
    fi
    if [ "$amd5" = "$control_md5" ]; then
      fail "$a: бинарь БАЙТ-В-БАЙТ равен КОНТРОЛЮ — работа раунда до негатива не доехала (собран не с той ветки / ответвлён от базы, а не от кандидата); такая арка и есть контроль, она ничего не изолирует"
      bad=1
    elif [ "$nsym" -eq 0 ] && [ "$ngone" -eq 0 ] && [ "$nnew" -eq 0 ] && [ "$ast" -eq "$control_st" ]; then
      fail "$a: по существу равен КОНТРОЛЮ — ни один символ не сдвинулся (изменили 0 · пропало 0 · появилось 0) и строк ровно столько же ($ast): это пересобранный контроль, а не негатив"
      bad=1
    fi
    if [ "$bad" -eq 0 ]; then
      ok "$a: отличается от кандидата (md5 ${amd5:0:12} ≠ ${cand_md5:0:12}) и от контроля (${control_md5:0:12}; символов сдвинуто $nsym против $csym у кандидата)"
    fi
  done
  echo
fi

# ---- assertion 4: runtime string markers ----------------------------------------
if [ ${#MARKERS[@]} -gt 0 ]; then
  echo "--- строковые маркеры (strings -a -n6 | grep -cF, НИКОГДА grep -ac) ---"
  for m in "${MARKERS[@]}"; do
    lit="${m%=*}"; want="${m##*=}"
    case "$want" in ''|-|*[!0-9-]*) fail "маркер '$m' без числовой дельты (нужно 'строка'=DELTA)"; continue ;; esac
    line="  "
    for ((i=0; i<${#ARM_NAMES[@]}; i++)); do
      a="${ARM_NAMES[$i]}"
      c="$(grep -cF -- "$lit" "$W/$a.strings")"
      eval "cnt_$a=$c"
      line="$line$a=$c "
    done
    got=$(( cnt_cand - cnt_control ))
    if [ "$got" -eq "$want" ]; then
      ok "'$lit': $line· дельта cand-control = $got (ожидалось $want)"
    else
      fail "'$lit': $line· дельта cand-control = $got, ожидалось $want"
    fi
  done
  echo
fi

# ---- optional deep mode ----------------------------------------------------------
if [ "$DEEP" -eq 1 ]; then
  echo "--- --deep: нормализованный дизассемблер + опись корневой ФС ---"
  if ! command -v objdump >/dev/null 2>&1; then
    fail "--deep запрошен, но objdump не установлен"
  else
    for ((i=0; i<${#ARM_NAMES[@]}; i++)); do
      a="${ARM_NAMES[$i]}"
      # Normalization, in this order: drop the address column, drop the raw instruction
      # bytes (they encode the very offsets we are masking), mask absolute addresses
      # and mask `+0x…>` offsets inside the `<symbol+off>` comments. What survives is
      # the mnemonic stream, which is what "did the code change" actually means.
      objdump -d -j .text "$W/$a.bin" 2>/dev/null \
        | sed -E 's/^[[:space:]]*[0-9a-f]+:\t[^\t]*\t/:\t/; s/^[[:space:]]*[0-9a-f]+:/:/; s/\b0x[0-9a-f]+\b/HEX/g; s/\b[0-9a-f]{4,}\b/ADDR/g; s/\+0x[0-9a-f]+>/+OFF>/g' \
        > "$W/$a.disasm.norm"
      [ -s "$W/$a.disasm.norm" ] || fail "$a: нормализованный дизассемблер пуст — objdump ничего не дал"
    done
    for ((i=1; i<${#ARM_NAMES[@]}; i++)); do
      a="${ARM_NAMES[$i]}"
      d="$(diff "$W/control.disasm.norm" "$W/$a.disasm.norm" | grep -c '^[<>]')"
      printf '  %-8s различающихся строк дизассемблера против control: %s\n' "$a" "$d"
    done
    # Normalization can in principle hide "the reference now points at a DIFFERENT
    # object", so it never carries a verdict alone — it is reported next to the
    # unmasked symbol sizes above.
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    fail "--deep запрошен, но python3 не установлен"
  else
    for ((i=0; i<${#ARM_NAMES[@]}; i++)); do
      a="${ARM_NAMES[$i]}"; t="${ARM_TAGS[$i]}"
      cid="$(docker create "$t" 2>/dev/null)"
      if [ -z "$cid" ]; then fail "$a: docker create для описи ФС не удался"; continue; fi
      docker export "$cid" 2>/dev/null | python3 "$(dirname "${BASH_SOURCE[0]}")/rootfs_manifest.py" "$W/$a.rootfs" > "$W/$a.rootfs.count"
      docker rm -f "$cid" >/dev/null 2>&1
      [ -s "$W/$a.rootfs" ] || fail "$a: опись корневой ФС пуста"
      LC_ALL=C sort -o "$W/$a.rootfs" "$W/$a.rootfs"
    done
    for ((i=1; i<${#ARM_NAMES[@]}; i++)); do
      a="${ARM_NAMES[$i]}"
      if [ -s "$W/control.rootfs" ] && [ -s "$W/$a.rootfs" ]; then
        d="$(diff "$W/control.rootfs" "$W/$a.rootfs" | grep -c '^[<>]')"
        printf '  %-8s различающихся записей ФС против control: %s (всего %s)\n' \
          "$a" "$d" "$(grep -c . "$W/$a.rootfs")"
      fi
    done
  fi
  echo
fi

# ---- verdict ---------------------------------------------------------------------
[ "$KEEP" -eq 1 ] && echo "артефакты оставлены в $W"
if [ "$VIOLATIONS" -eq 0 ]; then
  echo "=== ИТОГ: ЛЕСЕНКА ЧИСТА ✓ — все утверждения выдержаны ==="
  exit 0
fi
echo "=== ИТОГ: ОСТАНОВ ✗ — нарушений: $VIOLATIONS ==="
exit 1
