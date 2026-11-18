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

## [0.1.0-clase] - 2026-11-18

- Versión repartida en clase en `WiiPorts-clase.zip`, sin actualizaciones automáticas.
