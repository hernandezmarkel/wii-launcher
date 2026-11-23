# Cambios del launcher

Formato: una sección `## [x.y.z]` por versión, con viñetas `- ` (son las «Novedades» que ve el usuario en el
launcher; `tools/notas-version.sh` las copia al manifiesto firmado). Versionado SemVer. Un commit no es una versión:
solo una etiqueta `vX.Y.Z` aprobada publica algo.

## [Sin publicar]

## [1.0.0-beta.6] - 2026-11-23

- Mario Party 9 admite mods de datos mediante archivos de reemplazo y paquetes Riivolution de solo archivos. Puedes importarlos como ZIP o carpeta y gestionarlos con perfiles.
- El catálogo de GameBanana indica qué mods tienes instalados. Al actualizar uno, se conserva la versión anterior y se mantienen el orden, la activación y las opciones compatibles del perfil.
- El launcher compara los mods al unirte a una sala. Puede descargar los paquetes exactos que te falten desde GameBanana, con tu permiso. Verifica los archivos antes de marcar Listo y restaura tu perfil al salir.
- Los paquetes pueden declarar dependencias. El launcher muestra los requisitos e identifica qué mod y versión debes activar.

## [1.0.0-beta.5] - 2026-11-19

- Online: al empezar la partida desde la sala, la ventana del juego pasa al frente (o se minimiza el launcher).
- Mario Party 9 y Wii Party: el launcher usa la carpeta de instalación que eligieras en el instalador de cada juego.
- Instaladores de Mario Party 9 y Wii Party: comprueban que puedes escribir en la carpeta elegida antes de empezar. No hace falta recompilar los juegos y las salas siguen siendo compatibles.
- Servidor de Mario Kart Wii LAN: si queda en marcha una copia anterior del servidor, se apaga antes de arrancar la nueva, y un primer arranque interrumpido se repara solo.

## [1.0.0] - pendiente de aprobación

- El launcher se actualiza solo: comprueba al abrir (como mucho cada 6 horas) y te avisa sin interrumpir.
- Nueva sección «Ajustes del launcher»: versión instalada, buscar actualizaciones, novedades y canal beta opcional.
- Las actualizaciones van firmadas y se verifican antes de instalarse; si algo falla, vuelve sola la versión anterior.
- Solo se actualiza el launcher: juegos, partidas, salas y ajustes no se tocan.

## [1.0.0-beta.4] - 2026-11-19

- Nueva página «Mods» en Wii Party: importa mods (ZIP o carpeta, también arrastrándolos), revísalos antes de instalar, actívalos o desactívalos, ordénalos y guarda perfiles. Tu disco y tus partidas no se tocan.
- «Explorar» busca mods de Wii Party en GameBanana y te dice si funcionan con tu disco antes de descargar nada; los que llevan código, ejecutables o discos enteros se rechazan con el motivo.
- «Actualizaciones» de mods bajo demanda: la versión anterior queda instalada para volver a ella.
- Online con mods: solo entran en una sala quienes tengan exactamente los mismos mods. «Comparar mods» explica qué te falta y ofrece usar un perfil temporal igual al del anfitrión o descargar lo que falte (con tu permiso). Al salir vuelve tu perfil de siempre.
- Mario Party 9: parche de estabilidad del online. El launcher descarga solo el instalador nuevo; después pulsa «Instalar o actualizar» en Mario Party 9. Todos los jugadores de una sala deben tenerlo.
- El launcher ya puede actualizar solo los instaladores de Mario Party 9 y Wii Party.
- Descarga más ligera: el paquete completo pasa de 401 a unos 290 MB y cada actualización de unos 9 a unos 6 MB.

## [1.0.0-beta.3] - 2026-11-19 (no publicada: sustituida por la beta.4)

- Descarga más ligera: el paquete completo pasa de 401 a unos 285 MB y cada actualización de unos 9 a unos 6 MB.
- El instalador de Mario Kart Wii ocupa 400 MB menos en disco: solo lleva el compilador para PCs de 64 bits.

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
