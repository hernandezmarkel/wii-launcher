# Estado y bloqueos

Actualizado: 19/11/2026.

## Hecho

- Fase 0: auditoría (`00-auditoria.md`).
- Fases 1-3: núcleo `wl_mods` con pruebas (331/331), CLI `wlmods`, página «Mods» en el launcher (importar ZIP/carpeta
  con revisión previa, arrastrar y soltar, perfiles, activar/desactivar, orden de carga, opciones de
  Riivolution, desinstalar con confirmación). Lanzamiento offline y online con el perfil activo.
- Fase 2: Wii Party verificado con prueba A/B real.
- Fases 4-5: Explorar GameBanana (comprobación previa sin descargar, descarga con consentimiento) y Actualizaciones.
- Fases 6-8: manifiesto de sala por canal aparte, comparación tipada, descargas de lo que falta, perfil de sesión,
  unión automática opcional y vuelta al perfil habitual. Probado con dos launchers en este PC (`06-pruebas_resultados.md`).
- Gate de sala: el fingerprint va en el token del HELLO, así que el anfitrión rechaza mods distintos.

## Bloqueos

| Bloqueo | Afecta a | Qué hace falta |
|---|---|---|
| Solo hay un PC aquí | C22, C23 y toda prueba `REAL_2PCS` | Otro PC con Wii Party instalado y el mismo disco |
| MP9 sin prueba A/B | mods de MP9 | Repetir `tools/prueba-ab-wiiparty.ps1` adaptado a MP9 (otro hilo trabaja su online: coordinar el turno) |
| MKWii sin overlays en runtime | mods de MKWii | Perfil compilado de WiiCompiled o cambio en su runtime (hilo de MKWii) |
| 7z/rar | importación directa | Biblioteca de extracción revisada (licencia y seguridad) o descomprimir a mano |
| Lobby sin estado «pendiente» | entrar como no listo | Cambio en el protocolo de sala (dueño del netcode); de momento: comparar antes de unirse |
| Sin permiso de descarga real | C23 y gate de la fase 4 | Markel pospuso la descarga de prueba de GameBanana |
| main avanzó (beta.3) | fusión futura | Rebasar la rama antes de abrir el PR |

## Datos de prueba que quedan en este PC

En `%LOCALAPPDATA%\WiiLauncher\mods\wiiparty\` hay 4 paquetes de prueba (texto v1-v3 y textura). El perfil
`default` está sin mods activos, así que el juego arranca como siempre. Se pueden desinstalar desde la página Mods.
