# Puntos de extensión para próximas mejoras QoL

El updater es una pieza cerrada: añadir una mejora QoL normal no lo toca. Basta con cambiar el código, subir la versión
en `CMakeLists.txt`, escribir sus novedades en `CHANGELOG.md` y publicar (`docs/updater/04-publicacion.md`).

## Dónde va cada tipo de mejora

| Mejora | Dónde | Notas |
|---|---|---|
| Interfaz, menús, estados | `app/main.cpp` (páginas `Page::Game`, `Page::Room`, `Page::Launcher`) | El enum `Page` admite páginas nuevas; la barra lateral tiene un pie con accesos |
| Ajustes del launcher | `Prefs` (`app/prefs.*`), claves `clave=valor` en `%LOCALAPPDATA%\WiiLauncher\prefs.ini` | Prefijar las claves por módulo (`update.*`, `online_*`, ...). Solo desde el hilo de la UI |
| Juego nuevo | `manifests/<id>.toml` + adaptador en `adapters/` (contrato `wl/adapter.h`) | Los manifests viajan en las actualizaciones; un juego retirado se quita solo |
| Salas y online | `RoomSession` del adaptador | Sin cambios en el updater |
| Notificaciones | patrón de `update_toast()`: ventana superpuesta, no modal, con «Más tarde» | No bloquear la navegación con mando |
| Trabajo en segundo plano | patrón de `UpdateService`: hilo desprendido + `snapshot()` + `wake()` | Nunca bloquear el hilo de la UI |
| Archivos nuevos del launcher | añadirlos a `allowed_update_path()` (`updater/src/update_manifest.cpp`) y a `tools/empaquetar.sh` | Lista cerrada a propósito: es una decisión de seguridad |

## Compatibilidad del updater

- `format = 1` del manifiesto: si algún día hace falta otro formato, se publica primero una versión que entienda los dos.
- `min_from`: obliga a pasar por una versión intermedia si una migración lo requiere.
- Canal beta: listo para probar mejoras con unos pocos amigos antes de la estable.

## Futuro (no implementado)

- **Actualizaciones por juego**: otro proyecto. Cada juego tendría su propio manifiesto versionado y su propia fuente;
  el launcher solo mostraría el estado (`InstallStatus.revision` ya existe). No mezclar con este updater: el launcher no
  debe descargar ni redistribuir builds de juegos.
- Noticias, perfiles y estado por juego: páginas nuevas en `Page`, datos en `Prefs` o en archivos propios bajo
  `%LOCALAPPDATA%\WiiLauncher`.
- Firma Authenticode del exe (SignPath Foundation) cuando se consiga certificado.
