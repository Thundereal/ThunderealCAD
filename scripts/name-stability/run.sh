#!/bin/bash
# run.sh [out-prefix] [case-name-part] : the name stability corpus, counted.
# Writes <out>.md (the tables), <out>.json and <out>.log, and runs in a user
# home of its own (<out>.home) so the real user.cfg is neither read nor written.
# NAMES_FREEZE=0 runs with ImmutableShapeValues off; the default leaves it unset.
# docs/TopoNamingEnhance.md sec 9.6.
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=${FC_BUILD:-$ROOT/build/conda-relwithdebinfo-801}
OUT=${1:-$HOME/.cache/name-audit/score}
mkdir -p "$(dirname "$OUT")"
rm -rf "$OUT.progress" "$OUT.md" "$OUT.json" "$OUT.home"
mkdir -p "$OUT.home"
if [ "$NAMES_FREEZE" = 0 ]; then
    printf '<?xml version="1.0" encoding="UTF-8" standalone="no" ?>\n<FCParameters><FCParamGroup Name="Root"><FCParamGroup Name="BaseApp"><FCParamGroup Name="Preferences"><FCParamGroup Name="Mod"><FCParamGroup Name="Part"><FCBool Name="ImmutableShapeValues" Value="0"/></FCParamGroup></FCParamGroup></FCParamGroup></FCParamGroup></FCParamGroup></FCParameters>\n' > "$OUT.home/user.cfg"
fi
cd "$B" || exit 1
FREECAD_USER_HOME=$OUT.home NAMES_OUT=$OUT NAMES_ONLY=$2 QT_QPA_PLATFORM=offscreen \
    "$ROOT/.conda/run.sh" ./bin/FreeCADCmd "$HERE/run.py" > "$OUT.log" 2>&1
echo "exit $?"
cat "$OUT.md" 2>/dev/null || tail -20 "$OUT.log"
