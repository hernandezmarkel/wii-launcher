# 06 — Pruebas y resultados reales

Tipos: `UNIT` (wl_tests), `INTEGRATION` (núcleo + archivos reales en carpetas temporales), `REAL_1PC` (port real
en el portátil), `SIMULATED_NETWORK`, `REAL_2PCS`. Un mock nunca cuenta como prueba real.

Entorno: portátil de Markel, Windows 11 Pro 26300, i5-1334U / Iris Xe / 16 GB. Wii Party SUPP01 (PAL),
exe c055018 sobre la instalación 6dab024. Compilador MinGW g++ 15.2 (toolchain de MP9), zlib 1.3.1.

## Pruebas automatizadas (`build-mods/wl_tests.exe`, desde la raíz del repo)

18/11/2026, commit de la fase 1-3: **331/331 comprobaciones** (incluye las 208 previas del launcher y updater).
Cubren, con archivos hostiles fabricados por la propia prueba:

| Matriz | Qué se comprueba | Resultado |
|---|---|---|
| D01 | ZIP con `../`, `..\`, `/abs`, `C:/`, `a/../../` | rechazado antes de extraer |
| D02 | enlace simbólico Unix y punto de reanálisis Windows en el ZIP; uniones en carpetas | rechazado |
| D03 | bomba por relación de compresión; entrada que descomprime más de lo declarado; entradas solapadas | rechazado / cortado y archivo parcial borrado |
| D04 | duplicados por mayúsculas, archivo y carpeta con el mismo nombre, `CON`, `COM1.bin` | rechazado |
| D05 | `.bat`/`.exe` dentro del paquete, imágenes de disco, Gecko `.gct` | rechazado con motivo |
| D07 | JSON enorme, profundo, con claves repetidas, decimales o sustitutos UTF-16 sueltos | rechazado sin excepción |
| D08 | paquete alterado tras instalar | `verify` falla y el disco virtual no se construye |
| D10 | staging a medias y disco virtual incompleto | se limpian al abrir el almacén |
| A03/A04/A05/A06 | Riivolution solo-archivos admitido; `<memory>`, `offset`, otra región, otro juego, DTD/entidades → rechazados | OK |
| A07 | dos mods sobre el mismo archivo | conflicto informado; gana el orden de carga |
| A10 | ZIP sin manifiesto (estructura `files/`, `DATA/files`, subcarpeta tipo MP9) | detección del prefijo contra la FST real |
| B01/B11/B14 | instalación atómica, sin duplicar, cancelación sin residuos | OK |
| B02 | perfiles default/custom, activo, borrado | OK |
| C06/C08/C09 | hash, activado/desactivado y orden cambian el fingerprint; es determinista | OK |

## Prueba A/B real en Wii Party (`REAL_1PC`) — gate de la fase 2

Herramientas: `tools/crear-mod-textura.py` (genera el mod desde el disco del propio usuario),
`build-mods/wlmods.exe` (mismo núcleo que la UI) y `tools/prueba-ab-wiiparty.ps1` (lanza el juego con
`WP_INPUT_SCRIPT` = A+B en el título y `WP_SAVE_FRAME` con reloj determinista).

Mod: `locale/sp_EU/menu/common.arc.lz` con la textura `copyright.tpl` (el «©2010 Nintendo» del título)
invertida, recomprimido en LZ11 solo con literales: **1 537 914 → 8 792 968 bytes**, así que la FST del
disco virtual recoloca el archivo al final del disco.

| Paso | Resultado observado | Evidencia |
|---|---|---|
| Sin mod (perfil vanilla) | «©2010 Nintendo» blanco con contorno oscuro | fotograma 13 de la ejecución vanilla |
| Mod activo (fingerprint `0cd86023…`) | el juego lee `vdisc/0cd86023…/files/locale/sp_EU/menu/common.arc.lz` (log `WP_LOG_DISC`) y el texto sale **oscuro sin contorno** | fotograma 24 |
| El juego sigue normal con el archivo recolocado y 5,7× más grande | llega al menú principal con A+B, sin errores | fotograma 30 |
| Mod desactivado (perfil vuelve a `vanilla`) | se lanza sin argumento de disco virtual; vuelve a leer `games/wiiparty/extracted/...`; el contorno original vuelve | fotograma 24 |
| ROM, disco extraído y guardados | el disco extraído solo se lee (enlaces duros de solo lectura); `nand/` no se pasa ni se toca | código + logs |

Capturas (no se suben al repo porque muestran el juego): `%LOCALAPPDATA%\WiiLauncher\mods-evidencias\`
(`ab-titulo-vanilla-vs-mod.png`, `ab-mod-vs-desactivado.png`, `comparacion-copyright.png`).

**Conclusión: Wii Party queda declarado compatible con mods de datos (sustitución de archivos y Riivolution
solo-archivos).** `game_support("wiiparty").proven = true`.

Nota honesta: un primer intento con un mod de texto (`mess.bin`) cargó el archivo modificado desde el
disco virtual (comprobado en el log), pero las frases cambiadas no aparecen en las pantallas visitadas
(los rótulos del menú son texturas). No se usó como evidencia.

Mario Party 9: mismo motor, **no declarado** hasta repetir esta prueba con su port (`proven = false`).

## Sala simulada en un PC (`SIMULATED_NETWORK`, 19/11/2026)

Dos launchers en el mismo portátil, por 127.0.0.1: el anfitrión con datos reales y el mod de textura activo
(huella `0cd86023…`), el cliente con `WL_LOCALAPPDATA` aparte (sus propios mods y perfiles) y el mismo juego.
Interfaz real, clicando (Windows-MCP). **No sustituye a la prueba con 2 PCs.**

| Matriz | Paso | Resultado |
|---|---|---|
| — | Anfitrión abre sala | escucha en 47630 (sala) y 47640 (manifiesto de mods) |
| C13 | Cliente sin mods pulsa Unirse | el anfitrión rechaza en el HELLO («otra versión»): no entra, no puede ponerse Listo |
| C04/C14 | Comparar mods | «El anfitrión usa 1 mods. Te faltan 1…», «Prueba de textura 1.0.0 — te falta y no hay descarga autorizada» (importado a mano) |
| C08 | Cliente importa el mismo ZIP sin activarlo y vuelve a comparar | «lo tienes desactivado», botón «Usar los mods de la sala» |
| C16/C17 | «Usar los mods de la sala» con «Unirme automáticamente» marcado | perfil de sesión activo, huella igual y unión automática: el anfitrión lo acepta (2 jugadores en la sala) |
| C20 | «Salir de la sala» | el cliente vuelve a su perfil `default`; `session.json` desaparece; el mod sigue instalado |

Tras la prueba: el perfil `default` de Markel vuelve a estar sin mods y los datos del cliente simulado se borraron.
Pruebas automáticas: 409/409 (incluye diff C01-C10, manifiestos hostiles D06/D07, canal TCP y perfil de sesión).

Pendiente real (2 PCs): C22, C23 y la partida completa con un mod de gameplay.
