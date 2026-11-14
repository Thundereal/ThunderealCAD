#!/bin/bash
# The Python unit tests, one FreeCADCmd per test module, several at a time.
#
# Usage:
#   scripts/pytest-split.sh [--build DIR] [--jobs N] [--out DIR] [--cap KB]
#                           [--timeout SEC] [module ...]
#
# `FreeCADCmd -t 0` runs every module in one process: about six minutes on
# this box, and an address space that grows to 16.8 GB though it uses 1.6 --
# every thread pool is sized to the cores and each thread reserves a malloc
# arena, V8 reserves 4.8 GB for the expression sandbox, OpenBLAS a buffer per
# thread (docs/Testing.md, "Python, split by module"). Under an address-space
# limit below that the run does not fail a test, it dies near its end.
#
# Split, each module is a process of its own with its own profile and its own
# temporary directory -- tests save under fixed names -- and the whole suite
# takes two and a half minutes six at a time, no process past 8 GB of address
# space. The totals are those of `-t 0`.
#
#   --build DIR    the build tree (default build/conda-relwithdebinfo-801)
#   --jobs N       modules at a time (default 6)
#   --out DIR      logs, profiles, summary.txt (default a new temporary
#                  directory, named on the last line)
#   --cap KB       address-space limit per process, 0 for none (default
#                  8000000; not on macOS, whose ulimit has no -v)
#   --timeout SEC  per module (default 1800)
#   module ...     only these (default: all of FreeCAD.__unit_test__)
#
# env RUN: the wrapper that puts a command in the build's environment
# (default ~/works/sw/fcad/.conda/run.sh when it exists, else none).
#
# Exit status 0 when every module ran and reported OK.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$REPO/build/conda-relwithdebinfo-801
JOBS=6
OUT=
CAP=8000000
TIMEOUT=1800
ONE=
MODULES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD=$2; shift 2 ;;
        --jobs) JOBS=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --cap) CAP=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --one) ONE=$2; shift 2 ;;  # internal: run this one module
        -h|--help) sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) MODULES+=("$1"); shift ;;
    esac
done
if [ -z "${RUN+x}" ]; then
    RUN=
    [ -x "$HOME/works/sw/fcad/.conda/run.sh" ] && RUN=$HOME/works/sw/fcad/.conda/run.sh
fi
CMD=$BUILD/bin/FreeCADCmd
[ -x "$CMD" ] || { echo "no FreeCADCmd in $BUILD/bin" >&2; exit 2; }
MAC=
[ "$(uname)" = Darwin ] && MAC=1

# FreeCADCmd needs a pty for -t (a CAM test dies on a bad file descriptor
# without one); GNU and BSD script take their arguments differently.
in_pty() {  # in_pty <log> <command line>
    if [ -n "$MAC" ]; then
        script -q /dev/null /bin/bash -c "$2" > "$1" 2>&1
    else
        script -qec "$2" /dev/null > "$1" 2>&1
    fi
}

if [ -n "$ONE" ]; then
    m=$ONE
    mkdir -p "$OUT/home/$m" "$OUT/tmp/$m"
    # The expression sandbox's guest lives in the profile; without it its
    # tests skip ("no sandbox guest in this build").
    guest=${XDG_DATA_HOME:-$HOME/.local/share}/Thundereal/Pyodide
    [ -e "$OUT/home/$m/Pyodide" ] || { [ -e "$guest" ] && ln -s "$guest" "$OUT/home/$m/Pyodide"; }
    limit=
    [ -z "$MAC" ] && [ "$CAP" != 0 ] && limit="ulimit -v $CAP;"
    start=$(date +%s)
    in_pty "$OUT/$m.log" "$limit cd '$BUILD' && QT_QPA_PLATFORM=offscreen MALLOC_ARENA_MAX=8 \
OPENBLAS_NUM_THREADS=1 FREECAD_USER_HOME='$OUT/home/$m' TMPDIR='$OUT/tmp/$m' \
$RUN timeout $TIMEOUT ./bin/FreeCADCmd -t $m"
    verdict=$(tr '\r' '\n' < "$OUT/$m.log" | grep -a '^Ran \|^OK\|^FAILED\|terminate called' | tr '\n' ' ')
    echo "$m $(($(date +%s) - start))s $verdict" >> "$OUT/summary.txt"
    exit 0
fi

[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/pytest-split.XXXXXX")
mkdir -p "$OUT/home/_list"
: > "$OUT/summary.txt"
if [ ${#MODULES[@]} -eq 0 ]; then
    # asked of FreeCADCmd through a file: a script's stdout does not reach us
    cat > "$OUT/list.py" <<EOF
import FreeCAD
open(r"$OUT/modules.txt", "w").write("\n".join(FreeCAD.__unit_test__) + "\n")
EOF
    in_pty "$OUT/list.log" "cd '$BUILD' && QT_QPA_PLATFORM=offscreen \
FREECAD_USER_HOME='$OUT/home/_list' $RUN ./bin/FreeCADCmd '$OUT/list.py'"
    [ -s "$OUT/modules.txt" ] || { echo "could not list the test modules, see $OUT/list.log" >&2; exit 2; }
else
    printf '%s\n' "${MODULES[@]}" > "$OUT/modules.txt"
fi

export RUN
xargs -P "$JOBS" -n 1 "$0" --build "$BUILD" --out "$OUT" --cap "$CAP" --timeout "$TIMEOUT" --one \
    < "$OUT/modules.txt"

sort "$OUT/summary.txt"
total=$(awk '{for (i = 1; i <= NF; i++) if ($i == "Ran") n += $(i + 1)} END {print n + 0}' "$OUT/summary.txt")
modules=$(grep -c . "$OUT/modules.txt")
ok=$(grep -c ' OK' "$OUT/summary.txt")
echo "$total tests in $modules modules, $ok OK; logs in $OUT"
[ "$ok" -eq "$modules" ]
