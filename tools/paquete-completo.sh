#!/usr/bin/env bash
# Paquete completo para quien empieza de cero: launcher + instaladores de los juegos + servidor de Mario Kart LAN.
# Los instaladores NO están en el repo: se publican aparte (release «instaladores-AAAA-MM-DD», ver
# packaging/instaladores.lock) y aquí se comprueba su SHA-256 antes de usarlos. El updater nunca los descarga ni los
# actualiza: solo viajan en este ZIP de primera instalación.
# Uso: paquete-completo.sh <salida-de-empaquetar.sh> <instaladores.zip>
set -euo pipefail
out="$1"; inst_zip="$2"
ver=$(ls -d "$out"/WiiLauncher-*/ | head -1 | sed 's#.*/WiiLauncher-\(.*\)/#\1#')
want=$(sed -n 's/^sha256=//p' packaging/instaladores.lock)
echo "$want  $inst_zip" | sha256sum -c -
full="$out/WiiPorts-$ver"
rm -rf "$full"
cp -r "$out/WiiLauncher-$ver" "$full"
unzip -q "$inst_zip" -d "$full"
# Nada privado ni datos del juego (el servidor los crea en el primer arranque).
for bad in servidor-mkw/pgdata servidor-mkw/pgpass.local servidor-mkw/pg.log servidor-mkw/run servidor-mkw/wfc-server/config.xml; do
    [ ! -e "$full/$bad" ] || { echo "ERROR: $bad no debe ir en el paquete"; exit 1; }
done
if find "$full" -type f \( -iname '*.iso' -o -iname '*.wbfs' -o -iname '*.rvz' -o -iname '*.wia' -o -iname '*.dol' -o -iname '*.rel' \) | grep -q .; then
    echo "ERROR: hay archivos de disco o del juego en el paquete"; exit 1
fi
if command -v zip > /dev/null; then
    (cd "$out" && rm -f "WiiPorts-$ver.zip" && zip -qr -9 "WiiPorts-$ver.zip" "WiiPorts-$ver")
else
    (cd "$out" && rm -f "WiiPorts-$ver.zip" && powershell.exe -NoProfile -Command "Compress-Archive -Path 'WiiPorts-$ver' -DestinationPath 'WiiPorts-$ver.zip'")
fi
rm -rf "$full"
echo "paquete completo: $out/WiiPorts-$ver.zip ($(wc -c < "$out/WiiPorts-$ver.zip") bytes)"
