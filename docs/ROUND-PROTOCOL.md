# Round protocol

A **round** is one shippable change to the dedicated server: a branch, an image built
from it, a stand run, a binary ladder, a review, a deploy, and an acceptance done by
someone who did not build it. This document is the procedure. It is deliberately
boring; every step exists because skipping it once produced a false green.

The tools live in `tools/` and `tools/round/`. They take image tags and git refs and
nothing else — no host names, no paths outside the repository — so they run the same
way on a laptop and on the build machine.

Throughout, **the production host** means the machine that runs the live server. Its
address is not written down here; keep it in your ssh config under a name of your
choosing and substitute that name for `$PROD_HOST` below.

---

## 0. Vocabulary

| Term | Meaning |
|---|---|
| **control** | The image that is running in production right now. Carried over from the production host, never rebuilt — a rebuilt "control" stops being what is actually deployed. |
| **candidate** | The image built from the round branch. What you intend to ship. |
| **negative** | An image built from the candidate branch with exactly one protection removed. It exists to prove the checks can fail. |
| **marker** | A runtime string that a round adds or removes; counted in the binary as evidence the code arrived. |
| **ladder** | The comparison control ↔ candidate ↔ negatives that answers "did the code arrive", as distinct from "does it work". |

---

## 1. Branch

```
git switch -c r<N>-<topic> main
```

Work in ordinary commits. When review asks for a change, amend or fixup it into the
commit it belongs to, so the branch always reproduces the *current* candidate rather
than a history of states.

Before building, the tree gates must be green:

```
bash tools/check_quiet_logging.sh          # no raw spdlog:: outside the wrapper header
bash tools/config_drift.sh                 # repo config == the config the live server runs
```

Both print a verdict and a file count. **Read the count, not just the verdict**: these
gates are written so that "0 findings" is only meaningful next to "and I scanned all
N files". If a count looks low, the scan was incomplete and the verdict means nothing.

`config_drift.sh` needs to reach the production host (read-only, `ssh … cat`). Point it
somewhere else with `HOST=… HOST_ROOT=…`, or at a local directory for a dry run.

---

## 2. Build the candidate

```
bash tools/round/build_from_branch.sh r<N>-<topic> r<N>cand
```

It clones the repository at that ref into `~/rounds/r<N>cand` (override with a third
argument), initialises submodules, runs the quiet-logging gate **against the clone**,
builds `alicia-server-ru:r<N>cand`, and writes `manifest.json` with the ref, commit,
image id, build time and base-image digest.

Why a clone and not a worktree: the Dockerfile copies the whole context and then runs
`git init` and `git submodule update` *inside* the image. A worktree's `.git` is a file
holding an absolute host path, which does not exist in the image.

Two of the script's guards are there because the underlying commands lie:

* `git submodule update --init --recursive` exits **0** and does nothing when the clone
  has no submodule wiring. The script counts populated submodules against `.gitmodules`.
* the base image is required to be pinned by digest in the Dockerfile. An unpinned
  `FROM gcc:15` lets Docker Hub hand you a rebuilt toolchain, and a rebuilt toolchain
  can shift code generation across the entire binary — the ladder then differs
  everywhere and the cause is not your code.

---

## 3. Negatives

One branch per removed protection, one commit each:

```
git switch -c r<N>-neg-a r<N>-<topic>     # remove exactly one guard, commit
bash tools/round/negative_shape.sh r<N>-<topic> r<N>-neg-a 1 1
bash tools/round/build_from_branch.sh r<N>-neg-a r<N>negA
```

`negative_shape.sh` asserts the diff touches exactly the expected number of files and
(optionally) hunks. A negative that quietly grew to three files no longer isolates
anything; a negative whose edit never applied is byte-identical to the candidate and
would make the ladder "pass" while proving nothing. Both have happened.

---

## 4. Stand

Bring the candidate up on a disposable compose project with a free port block, on an
empty data directory or a read-only copy of a production snapshot. Check the boot log
for the things a health check counts — listening ports, registry sizes, zero errors —
and then run the scenario for the round's actual behaviour.

Two rules that keep stand results honest:

* **A stand on an empty data directory logs one error about missing uid metadata.**
  That is the empty directory, not the build. Compare arms against each other, not
  against an absolute "zero errors".
* Negative arms write to their **own results directory**. A negative run that
  overwrites the positive run's artefacts destroys the evidence you are about to be
  asked for.

---

## 5. Ladder

```
bash tools/round/ladder.sh r<N-1>cand r<N>cand r<N>negA r<N>negB \
  --grow 'Server::HandleThing' \
  --control-symbol 'Server::HandleSomethingElse' \
  --marker 'a string this round adds'=1 \
  --marker 'a string from an earlier round'=0
```

The ladder extracts `/usr/local/bin/alicia-server` from each image (`docker create` →
`docker cp` → `docker rm`; no container is ever started) and compares **symbol sizes**
from `nm -C -S`. It asserts:

* every `--grow` symbol changed size in the candidate versus the control — a symbol
  that cannot be found at all is a failure, not a pass;
* the `--control-symbol` did **not** change size in any arm;
* every negative arm really differs from the candidate;
* each `--marker` string's count changed by exactly the stated delta.

Three details are not negotiable:

1. **A "quiet" round changes nothing a name search can see.** Comparing the *set* of
   symbols is useless for it — deliberately broken builds have identical symbol sets.
   Sizes are what move.
2. **Strings are counted with `strings -a -n6 | grep -cF`, never `grep -ac` on the
   binary.** grep glues NUL-separated messages into one "line" and undercounts, which
   reads as a clean revert.
3. **The non-moving control symbol is mandatory.** Without it, a ladder in which
   *everything* shifted — a toolchain change, a relocated string literal — still reads
   as success.

Expect and name out loud any delta that is not the round's work (a changed banner
literal shifts nearby symbols by a few bytes). An unexplained difference read as "the
round did something else" costs a day; a difference explained in advance costs a line.

`--deep` additionally compares normalized disassembly and a full manifest of every file
in the image root filesystem. It is slower and its output is evidence for review rather
than a verdict: masking addresses could in principle hide a reference that now points
at a different object, which is exactly why the unmasked symbol sizes carry the verdict.

---

## 6. Review

Send the diff for independent review and iterate until it approves. Fix what it finds
rather than arguing scope; if you genuinely disagree after three rounds on the same
finding, escalate instead of looping. Keep the verdicts verbatim next to the round's
records.

---

## 7. Stage the image on the production host

The production host has no compiler image, so the candidate is built here and shipped:

```
set -o pipefail
docker save alicia-server-ru:r<N>cand | ssh "$PROD_HOST" docker load
echo "transfer rc=$?"          # with pipefail this is the FIRST failing stage
```

Without `pipefail` the exit code you read is `docker load`'s, and a `docker save` that
died halfway reads as success.

Then compare the id **on both ends** before touching anything:

```
docker image inspect alicia-server-ru:r<N>cand --format '{{.Id}}'
ssh "$PROD_HOST" "docker image inspect alicia-server-ru:r<N>cand --format '{{.Id}}'"
```

They must be equal. Loading an image that already exists under a different tag is a
no-op that prints reassuring output, so the id is the only proof the bytes arrived.

---

## 8. Deploy

1. **Check the window is empty.** Established connections on the game ports, and the
   login log, decide this — not the wall clock.
2. **Back up what you are about to replace**, and prove the backup by count, not by the
   absence of an error: a data snapshot has a known file count and fingerprint; take it
   with enough privilege to read every file, then verify the number.
3. **Tag the currently running image** as the rollback point before it stops being the
   running image.
4. If the round touches `resources/config/**`, copy the config out of the image, back up
   the host copy alongside it, and verify the arrived config by number (entry counts,
   not "looks right"). Run `tools/config_drift.sh` first: shipping a config from an
   image over a host file that had host-only edits has silently deleted content before.
5. Restart the service, then read the health of the **running container** — its image
   digest, restart count, listening ports, registry sizes, error lines.

The digest that goes into the round record is the one the running container reports,
not the one `docker build -q` printed.

---

## 9. Acceptance

Acceptance is done by someone who did not build the round, from the durable copies of
the tools rather than from a scratch directory, and it is written down the same day.
It answers three separate questions:

* **Did the code arrive?** — the ladder.
* **Does it behave?** — the stand scenario and a live look with human eyes.
* **Did anything else move?** — data fingerprint and file count before and after,
  rollback ladder intact, config drift gate green again.

Write the record immediately after the deploy, including the section that says what
was deployed and what was checked. A record written later is a record that says the
round is still a candidate long after it is live — that has happened twice.

---

## 10. Rollback

The previous round's image stays tagged as the rollback point, and the data snapshots
taken in step 8 stay next to it. Rolling back is retagging and restarting, plus
restoring the data snapshot **only** if the round migrated data. Decide which of those
two it is before you need it, not during.
