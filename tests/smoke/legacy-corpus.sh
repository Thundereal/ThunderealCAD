#!/bin/bash
# The legacy corpus loop (docs/SmokeTesting.md sec 3, step 2): each document
# of a list through tests/smoke/legacy-corpus.py, one FreeCAD each, in the
# isolated profile scripts/gui-test.sh makes, and after each the checks of
# the end (sec 2.2, check 8): the exit code, a crash log, what is left in
# the transient folder against an empty session's.
#
# Usage:
#   tests/smoke/legacy-corpus.sh <install or build tree> <list> <outdir> [timeout]
#
# <list> has a document per line; a line "path<TAB>skip-twin" leaves the
# worker twin out for it. <outdir>/summary.tsv: one line a document --
# findings, exit status, crash log, leftovers, seconds -- and
# <outdir>/findings.txt, every FINDING line with its document.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TREE=${1:?usage: legacy-corpus.sh <tree> <list> <outdir> [timeout]}
LIST=${2:?list of documents}
OUT=${3:?output directory}
TIMEOUT=${4:-1200}
SCRIPT="$REPO/tests/smoke/legacy-corpus.py"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# What a session leaves in the cache with no document: the baseline. Caches
# kept on purpose are not leftovers: Qt's shaders, matplotlib's font list,
# the compiled user shaders of the renderer
leftovers() {
    (cd "$1/.iso/cache" 2>/dev/null && find . -mindepth 1 \
        ! -path './qtshadercache*' ! -path './matplotlib*' \
        ! -path './Thundereal/BGFXUserShaders*' ! -path '.' 2>/dev/null | sort)
}
crashlog() {
    find "$1/.iso/data" -name 'crash*.log' -size +0 2>/dev/null | head -1
}

base="$OUT/_empty"
FC_BUILD="$TREE" GT_DOC="" "$REPO/scripts/gui-test.sh" "$SCRIPT" "$base" \
    --timeout 300 > "$OUT/_empty.out" 2>&1
leftovers "$base" > "$OUT/_empty.left"

printf "doc\tfindings\texit\tcrash\tleft\tseconds\n" > "$OUT/summary.tsv"
: > "$OUT/findings.txt"
n=0
while IFS=$'\t' read -r doc flag; do
    [ -z "$doc" ] && continue
    case "$doc" in \#*) continue;; esac
    n=$((n + 1))
    dir="$OUT/$(printf '%03d' "$n")"
    skip=0
    [ "${flag:-}" = "skip-twin" ] && skip=1
    t0=$(date +%s)
    FC_BUILD="$TREE" GT_DOC="$doc" GT_SMOKE_SKIP_TWIN=$skip \
        GT_SMOKE_WAIT=$((TIMEOUT - 120)) \
        "$REPO/scripts/gui-test.sh" "$SCRIPT" "$dir" --timeout "$TIMEOUT" > "$dir.out" 2>&1
    t1=$(date +%s)
    status=$(sed -n 's/^---- exit status //p' "$dir.out" | tail -1)
    done_line=$(grep -c '^DONE$' "$dir/result.txt" 2>/dev/null)
    crash=$(crashlog "$dir")
    left=$(comm -13 "$OUT/_empty.left" <(leftovers "$dir") | grep -v '^\./[^/]*$' | wc -l)
    leftnames=$(comm -13 "$OUT/_empty.left" <(leftovers "$dir") | head -5 | tr '\n' ' ')
    found=$(grep -c '^FINDING' "$dir/result.txt" 2>/dev/null)
    {
        grep '^FINDING' "$dir/result.txt" 2>/dev/null | sed "s|^|$n\t|"
        [ "${status:-x}" != 0 ] && printf "%s\tFINDING end exit: status %s\n" "$n" "${status:-none}"
        [ "${done_line:-0}" = 0 ] && printf "%s\tFINDING end exit: no DONE (killed or crashed)\n" "$n"
        [ -n "$crash" ] && printf "%s\tFINDING end crash log: %s\n" "$n" \
            "$(head -c 600 "$crash" | tr '\n' ' ')"
        [ "$left" -gt 0 ] && printf "%s\tFINDING end left in the transient folder: %s\n" "$n" "$leftnames"
        grep '^ABORT' "$dir/result.txt" 2>/dev/null | cut -c1-600 | sed "s|^|$n\t|"
    } >> "$OUT/findings.txt"
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$doc" "$found" "${status:-none}" \
        "$([ -n "$crash" ] && echo yes || echo no)" "$left" "$((t1 - t0))" >> "$OUT/summary.tsv"
    echo "[$n] $(basename "$doc"): $found findings, exit ${status:-none}, $((t1 - t0)) s"
    # the document's copies are not kept: the corpus is large
    rm -rf "$dir/work"
done < "$LIST"
echo "done: $n documents, $(grep -c FINDING "$OUT/findings.txt") findings in $OUT/findings.txt"
