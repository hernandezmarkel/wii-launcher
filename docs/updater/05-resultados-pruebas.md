# Resultados de la matriz de pruebas (MATRIZ_PRUEBAS_AUTOUPDATE.md)

Fecha: 18/11/2026. Entorno: portátil de desarrollo (i5-1334U, 16 GB, Windows 11 Pro 26300, x86_64), un solo PC.
**Todo es simulación local**: feed de staging en `http://127.0.0.1` (`tests/e2e/feed_server.py`) con exes compilados con
`-DWL_UPDATER_TESTING=ON` y una clave de pruebas. **No se ha probado en PCs de amigos ni contra GitHub Releases reales**
(no hay repo remoto todavía). Instalación: portable (la única que existe).

- Unitarias: `build/wl_tests.exe` → **208/208** (112 del launcher original + 96 del updater con HTTP falso).
- E2E: `tests/e2e/staging-e2e.ps1` → **25/25 PASS** (ejecución completa en 183 s; NET-07 repetida tras un arreglo, ver abajo).
- Commit probado: rama `feature/launcher-autoupdate` (ver `git log`).

## Experiencia y compatibilidad

| ID | Estado | Evidencia |
|---|---|---|
| UPD-01 | PASS | e2e: 1.0.0 detecta 1.0.1 y muestra aviso con novedades (captura `aviso`); unit: feed estable `Available` |
| UPD-02 | PASS | e2e: descarga, verifica, instala, reinicia en 1.0.1 en 6,5 s; hash del exe = 1.0.1; diario `health = ok`, `state = done` |
| UPD-03 | PASS (código, sin clic real) | «Más tarde» solo oculta el aviso en la sesión (`app.update_later`); nada se descarga sin pulsar Actualizar |
| UPD-04 | PASS | unit: `UpToDate` sin pedir la firma; e2e UPD-15/UPD-06: sin aviso |
| UPD-05 | PASS | «Buscar actualizaciones» con 30 s mínimos entre pulsaciones; descarga solo con «Actualizar»; un solo hilo a la vez (`busy`) |
| UPD-06 | PASS | e2e: reabrir 1.0.1 contra el mismo feed → `UpToDate`, sin bucle |
| UPD-07 | PASS | e2e: `prefs.ini`, partida simulada en `%LOCALAPPDATA%`, `instaladores/` y `servidor-mkw/` intactos |
| UPD-08 | PASS (sin abrir el juego) | `wlaunch probe` con el código nuevo: MP9, Wii Party y MKWii «LISTO»; `dry-run wiiparty` da el mismo exe/cwd/env. Adaptadores sin cambios; 112 tests de contrato en verde. No se ha abierto un juego para no tocar instalaciones reales |
| UPD-09 | PASS (código) | «Reiniciar y actualizar» desactivado con juego en marcha (`any_game_running`), con explicación; no automatizado en e2e |
| UPD-10 | PASS | e2e: con otra ventana abierta se pospone con aviso y el exe no cambia; unit: `UpdateLock` exclusivo |
| UPD-11 | PASS | e2e: carpeta sin permiso de escritura → no descarga ni pide administrador; mensaje para moverla |
| UPD-12 | PASS | e2e: exe de clase (sin updater) → ZIP puente 1.0.0 encima → actualización automática a 1.0.1 en 6,4 s |
| UPD-13 | PASS | build del launcher no tiene objetivos de juegos; paquete solo con exe, manifests, fuentes y textos (`empaquetar.sh`) |
| UPD-14 | PASS | e2e y unit: manifiesto beta rechazado en canal estable |
| UPD-15 | PASS | e2e y unit: versión más vieja → no se ofrece |

## Red y fallos

| ID | Estado | Evidencia |
|---|---|---|
| NET-01 | PASS | e2e: feed inalcanzable → error en 2,1 s en hilo aparte; la ventana sigue abierta |
| NET-02 | PASS (real) | contra github.com: repo inexistente → 404 «sin versiones» en 377 ms. DNS/TLS: mensajes propios en `net_error()`; no simulado |
| NET-03 | PASS | e2e 404 en el exe; unit 403 y 404 |
| NET-04 | PASS | e2e y unit: 429 → `RateLimited`, espera mínima 15 min guardada en `update.next_allowed` |
| NET-05 | PASS | e2e: 500 → error no fatal; descarga: 3 reintentos (1, 4, 10 s) |
| NET-06 | PASS | e2e: corte a 3 MB → «se cortó a medias», nada instalado, `.part` borrado |
| NET-07 | PASS | e2e: servidor entrega 5 MB más de lo firmado → se rechaza por `Content-Length` sin escribir nada. *Primera ejecución: se rechazaba por hash (seguro, mensaje confuso); arreglado y repetido* |
| NET-08 | PASS | e2e: descarga de ~11 s; la ventana respondió en 23/23 muestras |
| NET-09 | PASS | e2e y unit: redirección a otro dominio rechazada sin llegar a pedirla |
| INST-01 | PASS | e2e (fallo simulado de disco lleno): no toca nada, reabre la versión vieja con aviso |
| INST-02 | PASS (parcial) | sin permisos: ver UPD-11. Antivirus que bloquea un archivo: reintentos de 5 s y rollback (código); no simulado |
| INST-03 | PASS | e2e: aplicador matado tras colocar el exe nuevo → al reabrir, recupera 1.0.0 solo |
| INST-04 | PASS | e2e: versión nueva sin señal de salud → rollback en 12 s y reabre 1.0.0 |
| INST-05 | PASS | unit: descarga cancelada no deja archivos; botón Cancelar en la UI |

## Seguridad

| ID | Estado | Evidencia |
|---|---|---|
| SEC-01 | PASS | e2e y unit: un byte cambiado → rechazo |
| SEC-02 | PASS | e2e y unit: manifiesto alterado → firma inválida |
| SEC-03 | PASS | unit: replay de versión firmada anterior a la mayor vista → rechazo |
| SEC-04 | PASS | unit: `platform = windows-arm64` firmado → rechazo |
| SEC-05 | PASS | unit: 11 rutas peligrosas (`../`, absolutas, `\`, instaladores, servidor, `prefs.ini`...) rechazadas aunque estén firmadas |
| SEC-06 | PASS | unit: tamaño > 64 MB rechazado; sin archivos comprimidos que extraer |
| SEC-07 | PASS | unit: clave `postinstall` → rechazo; el formato no tiene comandos |
| SEC-08 | PASS | e2e y unit: firmado con otra clave → rechazo |
| SEC-09 | PASS | unit: `redact()` sustituye `%USERPROFILE%`; el registro no guarda nombre de usuario |
| SEC-10 | N/A | recomendación de repo público; el cliente no lleva ningún token (comprobado por `comprobar-publicable.sh`) |

## CI, repositorio y releases

| ID | Estado | Evidencia |
|---|---|---|
| REP-01 | PASS | `tools/comprobar-publicable.sh`: 0 problemas en los archivos rastreados |
| REP-02 | PASS | repo nuevo (historial desde el 18/11/2026); `.gitignore` revisado |
| CI-01 | PASS (GitHub real) | runs 44361208172 y 44362072198 en `hernandezmarkel/wii-launcher`: verde en ~4 min (208/208 tests, ZIP de CI 3,5 MB); ninguna release creada |
| CI-02 | PASS | el CI compila solo el launcher (sin objetivos de juegos); no hubo PR de prueba, se probó con push |
| CI-03 | PASS (GitHub real) | rama temporal `prueba/ci-test-roto` con `UCHECK(false)`: run 44362072030 en rojo («208/209», paso Tests). Rama borrada después |
| CI-04 | PASS (configuración) | `permissions: contents: read`, `persist-credentials: false`, sin secretos en CI, solo `pull_request` (nunca `pull_request_target`); el secreto solo existe en el entorno `release` |
| REL-01..06 | BLOCKED | `release.yml` escrito; probado en local: empaquetado (3,6 MB), firma y verificación con clave de pruebas, rechazo con la clave equivocada, comprobación de clave anclada |

## Rendimiento

| Métrica | Antes (v0.1.0-clase) | Después (v1.0.0) |
|---|---|---|
| Primer fotograma, 9 arranques | p50 273 ms, máx. 347 ms | p50 242 ms, máx. 259 ms (ruido; el updater va en otro hilo) |
| RAM privada en reposo | 54,0 MB | 55,5 MB |
| CPU en reposo (10 s) | 0,000 s | 0,000 s |
| Tamaño del exe | 9,40 MB | 9,77 MB |
| Comprobar actualización | — | 45-110 ms (feed local), 377 ms (github.com real) |
| Descarga típica | 424 MB (ZIP de clase completo) | ~9,3 MB (solo el exe; diferencial por archivo) |
| Build limpio del launcher | configurar 216 s + compilar 103 s | igual; incremental de la UI 5-15 s. Ningún juego se compila |

## Actualización real contra GitHub (18/11/2026, 23:01, portátil sin cargador)

| Prueba | Estado | Evidencia |
|---|---|---|
| beta.1 publicada → beta.2 publicada | PASS (GitHub real) | beta.1 bajada de la Release, canal beta, datos aparte (`WL_LOCALAPPDATA`). Detecta beta.2 al abrir en 1,3 s (API con ETag); «Actualizar» baja 2 archivos (9,46 MB: exe y CHANGELOG; fuentes y manifests no cambian); firma y hashes verificados; aplicador en 1 s; el exe instalado tiene el SHA-256 de la beta.2 publicada; `online_name` conservado; copia de la beta.1 en `.wl-backup`; al reabrir, «al día» en 227 ms |
| Paquete completo `WiiPorts-1.0.0-beta.2.zip` | PASS (contenido) | 31.601 archivos, 401 MB: launcher, instaladores de los tres juegos y servidor LAN; sin `pgdata`, `pgpass.local`, logs, `config.xml` ni `obj` |

## Pendiente de validar en PCs reales (cuando haya release beta autorizada)

1. PC limpio: descargar el ZIP de la beta, abrir, ver «Launcher v1.0.0».
2. PC con `WiiPorts-clase`: copiar el ZIP puente encima; comprobar que los juegos siguen «Listo para jugar» y el nombre de sala.
3. Publicar una 1.0.1-beta.1 de prueba y ver que ambos PCs la ofrecen solo con «Recibir versiones beta» marcado.
4. Actualizar en los dos, reabrir, lanzar Wii Party y crear/unirse a una sala entre ellos.
5. Repetir con un PC sin Internet y con uno detrás de proxy/antivirus del instituto.
6. Recoger `%LOCALAPPDATA%\WiiLauncher\updater.log` de cada uno.
