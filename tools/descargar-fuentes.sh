#!/usr/bin/env bash
# Descarga Inter 4.1 (OFL) con hash fijo y deja Inter-Regular.ttf e Inter-SemiBold.ttf en <carpeta>.
# Mismo archivo y hash que usa mp9-recomp. Uso: descargar-fuentes.sh <carpeta>
set -euo pipefail
out="$1"
mkdir -p "$out"
zip="$out/Inter-4.1.zip"
curl -fsSL --proto '=https' --tlsv1.2 -o "$zip" https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
echo "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e  $zip" | sha256sum -c -
unzip -j -o "$zip" "extras/ttf/Inter-Regular.ttf" "extras/ttf/Inter-SemiBold.ttf" -d "$out"
rm -f "$zip"
