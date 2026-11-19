# Cambios del gestor de mods

Separado del CHANGELOG del launcher: los mods no cambian la versión del launcher ni de los juegos.

## Sin publicar (rama feature/launcher-mods)

- Auditoría de cómo cargan mods los ports (fase 0).
- Núcleo `wl_mods`: ZIP defensivo, rutas seguras, JSON acotado, Riivolution solo-archivos, almacén
  transaccional, perfiles, fingerprint canónico y disco virtual.
- Wii Party: mods de datos verificados en el port real (prueba A/B).
- Launcher: página «Mods», lanzamiento con el perfil activo, token de sala con fingerprint.
- Herramientas: `wlmods`, `crear-mod-textura.py`, `prueba-ab-wiiparty.ps1`.
