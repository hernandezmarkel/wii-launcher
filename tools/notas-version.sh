#!/usr/bin/env bash
# Imprime las viñetas de CHANGELOG.md de la versión pedida (sección "## [x.y.z]"). Falla si no existe.
set -euo pipefail
ver="$1"
awk -v v="$ver" '
    /^## \[/ { inside = ($0 ~ "^## \\[" v "\\]") ; if (inside) found = 1 ; next }
    inside && /^- / { print }
    END { if (!found) exit 3 }
' CHANGELOG.md || { echo "CHANGELOG.md no tiene la sección [$ver]" >&2; exit 3; }
