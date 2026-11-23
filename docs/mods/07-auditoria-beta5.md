# Continuación del gestor de mods en beta.5

Auditoría del 23/11/2026. Punto de partida `30f0a63`, identificado en GitHub como `main`,
`release/beta5` y la etiqueta `v1.0.0-beta.5`. El PR #1 ya integró el gestor de mods. El
checkout `wii-launcher-mods` está en `e6195a5`, antecesor de beta.5. El directorio
`wiiparty-recomp-fix` recibido corresponde localmente a `wiiparty-launcher-fix`.
Los cambios locales detectados en los repositorios de los ports quedan en sus ramas.

## Línea base

- Windows x64, C++17, CMake/Ninja y MinGW. SDL3 e ImGui para la UI. El núcleo de mods
  reutiliza zlib, WinHTTP y hashes de CNG. La UI delega las operaciones del almacén y
  GameBanana a `ModsService` y lanza el juego por sus adaptadores.
- El actualizador de launcher comprueba manifiestos ECDSA firmados y SHA-256. Usa un
  aplicador con diario, copias y rollback. Los instaladores pequeños llevan otro
  manifiesto firmado. La compilación de UI y tests de mods no recompila los juegos.
- `wl_tests` desde la raíz, build `build-qol`: 431/431. Compilaron también `wlmods` y
  `wii-launcher`. Las pruebas incluyen el updater, el almacén, ZIP, Riivolution,
  GameBanana simulado y salas por loopback.
- Se revisó el catálogo de skills locales para C++, CMake, Windows, networking y testing.
  El entorno reproducible y las convenciones del repositorio están en `tools/entorno-mods.ps1`,
  los manifiestos y los documentos de integración. No se requiere otro framework.

## Estado observado

| Área | Evidencia y trabajo pendiente |
|---|---|
| Wii Party | Prueba A/B real en el historial. Reemplazos sobre la FST y Riivolution de archivos. La ROM y la NAND permanecen en su ubicación habitual. |
| Mario Party 9 | El port instalado `5a2b71d` y los dos checkouts DX/C++ del motor reciben el disco extraído en `argv[1]`. Requiere A/B en SSQE01 antes de habilitar la UI. |
| Mario Kart Wii y Brawl | Adaptador de Mario Kart sin overlay en tiempo de ejecución. Brawl figura como previsto. |
| Gestión local | ZIP/carpeta con previsualización, paquetes inmutables, perfiles persistentes, activar/desactivar, conflictos por archivo, opciones y desinstalación protegida por referencias. Faltan dependencias declarativas de paquetes y una actualización que conserve explícitamente la entrada anterior y sus preferencias. |
| Catálogo | GameBanana lista, busca y ofrece fichas, árbol del ZIP y descargas desde hosts autorizados. Sus formatos 7z y RAR requieren extracción manual. Se revisaron el [API de GameBanana](https://api.gamebanana.com/) y la [Game API de Game Jolt](https://gamejolt.com/game-api/doc). Esta última documenta trofeos, sesiones, datos y puntuaciones, sin operaciones públicas de catálogo o descarga de mods. |
| Salas por LAN/VPN | El HELLO existente comprueba revisión del port e ID de disco y añade la huella de mods. El canal TCP adicional compara perfiles y habilita una sesión temporal. La descarga asistida elige hoy el archivo más reciente y verifica el hash después de instalarlo. El manifiesto carece de la referencia de archivo exacta. |
| Cuenta | No hay login ni backend de cuenta en este checkout. Se puede reutilizar la identidad del paquete para persistir referencias y reconstruir perfiles manteniendo los archivos en origen. |

La validación continúa en Windows con los ports existentes. Las pruebas en loopback se
pueden repetir aquí. Una partida entre dos ordenadores requiere un segundo equipo.
