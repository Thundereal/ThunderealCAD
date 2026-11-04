#!/bin/sh
# Header-inclusion audit for a built Ninja tree. Read-only on the tree; writes into OUT.
#   sh run_audit.sh [BUILD] [OUT]
# Needs: the fork's conda python (never the Store `python` stub), ninja via .conda/run.cmd.
set -e
FCAD=D:/works/sw/fcad
BUILD=${1:-$FCAD/build/win-relwithdebinfo-801}
# results go beside the build, never into the source tree
OUT=${2:-$BUILD/header-audit}
PY=$FCAD/.conda/freecad/python.exe
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 1 ] && shift
[ $# -ge 1 ] && shift
mkdir -p "$OUT"

# 1. every object's real header set, as the compiler reported it
cmd //c "D:\\works\\sw\\fcad\\.conda\\run.cmd ninja -C $(cygpath -w "$BUILD") -t deps" > "$OUT/deps.txt"

# 2. how often each header was edited in the last 12 months
since=$(date -d '12 months ago' +%Y-%m-%d 2>/dev/null || echo 2025-10-22)
git -C "$FCAD" log --since="$since" --no-merges --name-only --format= HEAD -- 'src/*.h' 'src/*.hpp' 'src/*.inl' \
  | grep -v '^$' | sort | uniq -c | sort -rn | awk '{c=$1; $1=""; sub(/^ /,""); print c"\t"$0}' > "$OUT/churn.tsv"

# 3. ranking (pass --baseline old/ranking.csv to flag headers whose reach grew)
"$PY" "$HERE/header_cost.py" "$BUILD" "$FCAD" "$OUT/deps.txt" --top 60 \
  --csv "$OUT/ranking.csv" --churn "$OUT/churn.tsv" "$@" > "$OUT/ranking.txt"

# 4. why: parents, dominators (umbrella headers) and Qt/Coin/OCCT/Python gateways
head -250 "$OUT/ranking.csv" | tail -n +2 | cut -d, -f2 > "$OUT/targets.txt"
"$PY" "$HERE/include_graph.py" analyze "$BUILD" "$FCAD" "$OUT/deps.txt" \
  --targets "$OUT/targets.txt" --out "$OUT/analyze.json"

# 5. what-if (edit proposals.spec first)
# "$PY" "$HERE/include_graph.py" whatif "$BUILD" "$FCAD" "$OUT/deps.txt" \
#   --spec "$HERE/proposals.spec" --churn "$OUT/churn.tsv" --out "$OUT/whatif.json"
echo "wrote $OUT/ranking.txt $OUT/ranking.csv $OUT/analyze.json"
