#!/usr/bin/env bash
# Empaqueta SOLO el launcher (exe, manifests, fuentes, textos). Nunca instaladores de juegos ni servidor-mkw.
# Uso: empaquetar.sh <carpeta-build> <salida> [canal]
#   <salida>/WiiLauncher-<versión>/              carpeta lista para copiar encima de una instalación
#   <salida>/WiiLauncher-<versión>-actualizacion.zip   ZIP puente / instalación manual
#   <salida>/assets/                             archivos sueltos + update.txt (sin firmar) para la Release
set -euo pipefail
build="$1"; out="$2"; channel="${3:-}"
ver=$(cat "$build/version.txt")
if [ -z "$channel" ]; then case "$ver" in *-*) channel=beta ;; *) channel=stable ;; esac; fi
pkg="$out/WiiLauncher-$ver"
rm -rf "$pkg" "$out/assets"
mkdir -p "$pkg/manifests" "$pkg/fonts" "$out/assets"
cp "$build/wii-launcher.exe" "$pkg/"
cp manifests/*.toml "$pkg/manifests/"
cp "$build"/fonts/Inter-Regular.ttf "$build"/fonts/Inter-SemiBold.ttf "$pkg/fonts/"
cp packaging/LEEME.txt packaging/LICENCIAS.txt CHANGELOG.md "$pkg/"
bash tools/notas-version.sh "${ver}" > "$out/notas.txt"
"$build/wl_release.exe" pack "$pkg" "$out/assets" "$ver" "$channel" "$out/notas.txt"
if command -v zip > /dev/null; then
    (cd "$out" && rm -f "WiiLauncher-$ver-actualizacion.zip" && zip -qr "WiiLauncher-$ver-actualizacion.zip" "WiiLauncher-$ver")
else  # Git Bash local sin zip
    (cd "$out" && rm -f "WiiLauncher-$ver-actualizacion.zip" && powershell.exe -NoProfile -Command "Compress-Archive -Path 'WiiLauncher-$ver' -DestinationPath 'WiiLauncher-$ver-actualizacion.zip'")
fi
echo "paquete: $out/WiiLauncher-$ver-actualizacion.zip ($(wc -c < "$out/WiiLauncher-$ver-actualizacion.zip") bytes)"
