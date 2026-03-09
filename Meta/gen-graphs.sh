#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCS_DIR="${ROOT_DIR}/Docs"
OUT_PNG="${DOCS_DIR}/components.png"

mkdir -p "${DOCS_DIR}"

count_lines() {
  local dir="$1"
  if [ ! -d "${ROOT_DIR}/${dir}" ]; then
    echo 0
    return
  fi
  find "${ROOT_DIR}/${dir}" -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) -print0 \
    | xargs -0 cat 2>/dev/null \
    | wc -l | awk '{print $1}'
}

kernel_lines=$(count_lines "Kernel")
userland_all_lines=$(count_lines "Userland")
micropython_lines=$(count_lines "Userland/Apps/MicroPython")

# Userland natif = tout Userland moins le port MicroPython (3rd‑party),
# qui est ignoré dans le graphe.
userland_native_lines=$((userland_all_lines - micropython_lines))
if [ "${userland_native_lines}" -lt 0 ]; then
  userland_native_lines=0
fi

total=$((kernel_lines + userland_native_lines))
if [ "${total}" -eq 0 ]; then
  echo "[gen-graphs] no source lines found, skipping graph generation"
  exit 0
fi

echo "[gen-graphs] LOC: kernel=${kernel_lines} userland_native=${userland_native_lines} micropython_ignored=${micropython_lines}"

# Convert to percentage of total LOC (one decimal place).
kernel_pct=$(LC_NUMERIC=C awk -v k="${kernel_lines}" -v t="${total}" 'BEGIN { if (t>0) printf "%.1f", (k*100.0/t); else print "0.0"; }')
userland_native_pct=$(LC_NUMERIC=C awk -v k="${userland_native_lines}" -v t="${total}" 'BEGIN { if (t>0) printf "%.1f", (k*100.0/t); else print "0.0"; }')

echo "[gen-graphs] PCT: kernel=${kernel_pct}% userland_native=${userland_native_pct}% (MicroPython ignored)"

chart_json=$(cat <<EOF
{
  "type": "doughnut",
  "data": {
    "labels": [
      "Kernel (${kernel_pct}%)",
      "Userland (${userland_native_pct}%)"
    ],
    "datasets": [{
      "data": [${kernel_pct}, ${userland_native_pct}]
    }]
  },
  "options": {
    "plugins": {
      "legend": {
        "position": "bottom"
      }
    }
  }
}
EOF
)

if ! command -v curl >/dev/null 2>&1; then
  echo "[gen-graphs] curl not found, cannot contact quickchart.io"
  exit 0
fi

echo "[gen-graphs] generating ${OUT_PNG} via quickchart.io"
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d "{\"chart\":${chart_json},\"format\":\"png\",\"width\":600,\"height\":600,\"backgroundColor\":\"transparent\"}" \
  https://quickchart.io/chart \
  -o "${OUT_PNG}"

echo "[gen-graphs] done -> ${OUT_PNG}"

exit 0

