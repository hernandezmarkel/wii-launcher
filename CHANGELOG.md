# Cambios del launcher

Formato: una sección `## [x.y.z]` por versión, con viñetas `- ` (son las «Novedades» que ve el usuario en el
launcher; `tools/notas-version.sh` las copia al manifiesto firmado). Versionado SemVer. Un commit no es una versión:
solo una etiqueta `vX.Y.Z` aprobada publica algo.

## [Sin publicar]

## [1.0.0] - pendiente de aprobación

- El launcher se actualiza solo: comprueba al abrir (como mucho cada 6 horas) y te avisa sin interrumpir.
- Nueva sección «Ajustes del launcher»: versión instalada, buscar actualizaciones, novedades y canal beta opcional.
- Las actualizaciones van firmadas y se verifican antes de instalarse; si algo falla, vuelve sola la versión anterior.
- Solo se actualiza el launcher: juegos, partidas, salas y ajustes no se tocan.

## [1.0.0-beta.2] - 2026-11-18

- Nueva descarga completa WiiPorts-<versión>.zip: trae los instaladores de Wii Party, Mario Party 9 y Mario Kart Wii (beta) y el servidor LAN de Mario Kart, para instalar desde tu WBFS sin bajar nada más.
- «Instalar» ya no pide un .exe si falta el instalador: explica qué descargar.
- Mario Kart Wii LAN: dos PCs reales entraron en el servidor y se añadieron como amigos; una carrera completa aún no está confirmada.

## [1.0.0-beta.1] - 2026-11-18

- Primera beta con actualizaciones automáticas: para probar en dos o tres PCs antes de la 1.0.0.

- El launcher se actualiza solo: comprueba al abrir (como mucho cada 6 horas) y te avisa sin interrumpir.
- Nueva sección «Ajustes del launcher»: versión instalada, buscar actualizaciones, novedades y canal beta opcional.
- Las actualizaciones van firmadas y se verifican antes de instalarse; si algo falla, vuelve sola la versión anterior.
- Solo se actualiza el launcher: juegos, partidas, salas y ajustes no se tocan.

## [0.1.0-clase] - 2026-11-18

- Versión repartida en clase en `WiiPorts-clase.zip`, sin actualizaciones automáticas.
