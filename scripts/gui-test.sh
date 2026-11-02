#!/bin/bash
# GUI test driver: runs one FreeCAD-driven test script under xvfb in an
# isolated configuration, with an external timeout, and judges it by the
# result file the script writes.
#
# Usage:
#   scripts/gui-test.sh <test.py> <outdir> [--timeout N] [--window]
#                       [--gl nvidia|radeon|sw]
#
# The script gets GT_OUT (the output directory) and GT_RESULT (the result
# file) in its environment and is expected to append PASS/FAIL lines and
# end with DONE; anything else -- no DONE, a FAIL line, an ABORT line --
# fails the test. The exit status of the process is reported but is not
# the verdict: the work the test does is what it writes, and a process
# that died after DONE is reported as that, on its own line.
#
# Before the test's script FreeCAD is handed scripts/gui-test-profile.py,
# which states what the tests start from where a default would otherwise
# decide it -- no multisampling, which the pictures they read were written
# against.
#
# Never the live desktop session: private XDG dirs + user.cfg, xvfb with
# WAYLAND_DISPLAY unset (WSLg's survives into xvfb-run's child otherwise
# and the window lands on the desktop), and `timeout` around the whole
# process group so a modal dialog or a hung teardown cannot leak a
# FreeCAD + Xvfb pair (timeout signals its process group, which is
# xvfb-run, Xvfb and FreeCAD together).
#
# --window is the exception, and only ever on request: the same isolated
# configuration and the same timeout, but in a window on the desktop's X
# server (GT_DISPLAY, :0 by default) and no Xvfb. For a measurement that
# has to be seen, or checked against what Xvfb says
# (docs/DocumentLoad.md sec 18.14). No ctest entry uses it.
#
# --gl picks the GL driver, under Xvfb or in a window: `sw` is what a
# plain run gets (llvmpipe), `radeon` and `nvidia` are Mesa's D3D12
# driver on WSL, on the default adapter and on the one named NVIDIA. A
# measurement names its display; the script should read GL_RENDERER back.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
RUN="$REPO/.conda/run.sh"
# The standard build, the one every test run uses (CLAUDE.md,
# docs/DevEnvironment.md); FC_BUILD repoints at another tree.
BUILD=${FC_BUILD:-"$REPO/build/conda-relwithdebinfo-801"}
FCBIN="$BUILD/bin/FreeCAD"
[ -x "$FCBIN" ] || {
    echo "no FreeCAD binary at $FCBIN (set FC_BUILD to another build tree)"
    exit 2
}

SCRIPT=${1:?usage: gui-test.sh <test.py> <outdir> [--timeout N]}
OUT=${2:?usage: gui-test.sh <test.py> <outdir> [--timeout N]}
shift 2
TIMEOUT=300
WINDOW=0
GL=()
while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT=$2; shift 2;;
        --window) WINDOW=1; shift;;
        --gl)
            case "$2" in
                sw) GL=();;
                radeon|nvidia)
                    GL=(LIBGL_ALWAYS_SOFTWARE=0 GALLIUM_DRIVER=d3d12
                        MESA_LOADER_DRIVER_OVERRIDE=d3d12
                        __GLX_VENDOR_LIBRARY_NAME=mesa)
                    [ "$2" = nvidia ] && GL+=(MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA);;
                *) echo "unknown GL driver: $2 (sw, radeon, nvidia)"; exit 2;;
            esac
            shift 2;;
        *) echo "unknown option: $1"; exit 2;;
    esac
done
[ -f "$SCRIPT" ] || { echo "no test script at $SCRIPT"; exit 2; }
SCRIPT=$(cd "$(dirname "$SCRIPT")" && pwd)/$(basename "$SCRIPT")

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
ISO="$OUT/.iso"
# Every run starts from a fresh profile: what an earlier run stored -- a
# theme, a language, a window's geometry in Thundereal.conf -- fails the
# next run of any test that reads a default. The cache is kept (Qt's and
# Mesa's shader caches), as it only makes the start faster -- but for the
# application's own: a run killed at its timeout leaves its documents'
# transient directories there, and the next run's start offers to recover
# them in a modal box that takes the active window (a lock file alone is
# not the trigger: a directory with no lock is offered too). Under the
# application's name, which is no longer FreeCAD.
rm -rf "$ISO/user.cfg" "$ISO/config"
rm -rf "$ISO/cache/FreeCAD/Cache" "$ISO/cache/Thundereal/Cache"
mkdir -p "$ISO/cache" "$ISO/config"
# What a browser test's driver wrote in an earlier run: appended to, so a
# second run read the first's phases and sampled the host at the wrong times.
rm -f "$OUT/phases.txt"

# The font is stated too, the one the conda env brings. The box's default
# otherwise decides what a test sees: a panel's minimum widths, the room left
# for a view in a 1280 px window, a sketch's label size. Noto Sans, Ubuntu's
# default, is 3 px taller than DejaVu Sans at the same size, and that alone
# failed four tests on 26.04 (docs/Testing.md, the fifth trap).
FONT_ENV=()
CONDA_FONTS="$REPO/.conda/freecad/fonts"
if [ -z "${FONTCONFIG_FILE:-}" ] && [ -f "$CONDA_FONTS/DejaVuSans.ttf" ]; then
    {
        echo '<?xml version="1.0"?>'
        echo '<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">'
        echo '<fontconfig>'
        echo "  <include ignore_missing=\"yes\">$REPO/.conda/freecad/etc/fonts/fonts.conf</include>"
        echo "  <dir>$CONDA_FONTS</dir>"
        for family in "sans-serif" "Sans Serif" "Helvetica" "Arial" "Liberation Sans" \
                      "Noto Sans"; do
            echo "  <match target=\"pattern\"><test name=\"family\" qual=\"any\">"
            echo "    <string>$family</string></test>"
            echo "    <edit name=\"family\" mode=\"prepend\" binding=\"strong\">"
            echo "    <string>DejaVu Sans</string></edit></match>"
        done
        echo '</fontconfig>'
    } > "$ISO/fonts.conf"
    FONT_ENV=(FONTCONFIG_FILE="$ISO/fonts.conf")
fi

# Mesa's shader cache is shared by every run instead of living in $ISO:
# what llvmpipe compiles depends on the driver and the shader, not on the
# test, and a work directory's first run otherwise compiles every bgfx
# program from nothing -- an empty test took 11.5-13.6 s in a new
# directory and 5.1-5.3 s in a used one (2026-10-25). Mesa keeps the
# cache safe for processes sharing it. GT_MESA_CACHE points elsewhere;
# an empty value keeps the per-directory cache.
MESA_CACHE=${GT_MESA_CACHE-"${XDG_CACHE_HOME:-$HOME/.cache}/thundereal-gui-test"}
MESA_ENV=()
if [ -n "$MESA_CACHE" ]; then
    mkdir -p "$MESA_CACHE"
    MESA_ENV=(MESA_SHADER_CACHE_DIR="$MESA_CACHE")
fi
RESULT="$OUT/result.txt"
: > "$RESULT"
LOG="$OUT/run.log"

echo "GUI test $(basename "$SCRIPT") (log: $LOG)"
if [ "$WINDOW" = 1 ]; then
    env -u WAYLAND_DISPLAY DISPLAY="${GT_DISPLAY:-:0}" "${GL[@]}" "${MESA_ENV[@]}" "${FONT_ENV[@]}" \
        XDG_CACHE_HOME="$ISO/cache" XDG_CONFIG_HOME="$ISO/config" \
        GT_OUT="$OUT" GT_RESULT="$RESULT" \
        QT_QPA_PLATFORM=xcb \
        timeout -k 15 "$TIMEOUT" \
        "$RUN" "$FCBIN" --user-cfg "$ISO/user.cfg" \
        "$REPO/scripts/gui-test-profile.py" "$SCRIPT" \
        > "$LOG" 2>&1 </dev/null
else
    env -u WAYLAND_DISPLAY "${GL[@]}" "${MESA_ENV[@]}" "${FONT_ENV[@]}" \
        XDG_CACHE_HOME="$ISO/cache" XDG_CONFIG_HOME="$ISO/config" \
        GT_OUT="$OUT" GT_RESULT="$RESULT" \
        QT_QPA_PLATFORM=xcb \
        timeout -k 15 "$TIMEOUT" \
        xvfb-run -a -s "-screen 0 1280x1024x24" \
        "$RUN" "$FCBIN" --user-cfg "$ISO/user.cfg" \
        "$REPO/scripts/gui-test-profile.py" "$SCRIPT" \
        > "$LOG" 2>&1 </dev/null
fi
status=$?

echo "---- $RESULT"
cat "$RESULT"
echo "---- exit status $status"

if ! grep -q "^DONE$" "$RESULT"; then
    if [ "$status" = 124 ] || [ "$status" = 137 ]; then
        echo "GUI TEST FAILED: no DONE within ${TIMEOUT}s"
    else
        echo "GUI TEST FAILED: no DONE (exit status $status)"
    fi
    echo "---- tail of $LOG"
    tail -n 40 "$LOG"
    exit 1
fi
if grep -q "^FAIL\|^ABORT" "$RESULT"; then
    echo "GUI TEST HAD FAILURES"
    exit 1
fi
if [ "$status" != 0 ]; then
    echo "GUI TEST FAILED: the process wrote DONE but exited with status $status"
    echo "---- tail of $LOG"
    tail -n 40 "$LOG"
    exit 1
fi
echo "GUI TEST OK"
