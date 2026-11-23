# Validación de la continuación del gestor de mods

23/11/2026. Windows x64, MinGW 15.2, base `30f0a63`, rama `codex/gestor-mods`.

## Cambios y contratos

- Los paquetes son inmutables. La actualización cambia atómicamente la referencia de
  un perfil a la versión nueva y conserva la anterior. Mantiene orden, estado y opciones
  compatibles. Si la selección de Riivolution cambió, el perfil conserva la versión que
  usaba y la nueva queda disponible en Instalados.
- Opcionalmente, un ZIP o carpeta puede incluir `wii-launcher-mod.json` en su raíz.
  El manifiesto requiere el juego y permite declarar hasta 32 dependencias por ID y
  versión exacta. El perfil debe tenerlas activas para lanzarse. Por ejemplo:

  ```json
  {
    "schema": 1,
    "game_id": "mp9",
    "dependencies": [{"id": "local.pack-base", "version": "1.0.0"}]
  }
  ```

  Los paquetes anteriores siguen siendo importables. Las fuentes se resuelven usando
  los IDs del proveedor y los hashes de cada referencia.
- `ModReference` es la identidad portátil que comparte el catálogo local, el manifiesto
  de sala y la capa futura de cuenta. `export_profile_catalog`, `parse_profile_catalog`
  y `plan_catalog_restore` forman el contrato de integración. El catálogo JSON guarda ID
  de juego, disco base, cargador, nombre de perfil y referencias con versión, hashes,
  procedencia, orden, opciones, dependencias y activación. Una referencia con orden
  `-1` identifica un paquete instalado fuera de ese perfil. La restauración reutiliza
  copias locales verificadas y devuelve las referencias pendientes al llamador.
  La cuenta puede guardar este JSON por usuario, juego y perfil. El archivo se recupera
  desde su proveedor autorizado o se importa manualmente. El proyecto todavía carece
  de un cliente de autenticación.

## Resultados

| Tipo | Resultado |
|---|---|
| Automatizado (`wl_tests`) | 498/498. Incluye ZIP, actualización con 404 y reintento en el servicio de mods, conservación de opciones de Riivolution, dependencias, hashes, perfiles, restauración de metadatos, protocolo de sala y updater. |
| Port real (`REAL_1PC`) | Mario Party 9 cargó el reemplazo de textura de título al activar el mod y volvió al original al desactivar. El recurso original conservó su SHA-256. Un paquete Riivolution de solo archivos cargó el mismo reemplazo mediante el lanzamiento normal. |
| Loopback y proveedor real | Anfitrión con [`gamebanana.628724`](https://gamebanana.com/mods/628724), invitado con almacén vacío. El servicio detectó la diferencia, el anfitrión rechazó el HELLO inicial y el invitado quedó bloqueado antes de Listo. Tras resolver `1544590`, descargar desde GameBanana e instalar con verificación, el perfil temporal coincidió. El protocolo de Wii Party aceptó al invitado y confirmó Listo. Al salir volvió el perfil habitual y se desinstaló el paquete del almacén de prueba. |
| Interfaz de escritorio | Captura generada por el nuevo launcher de la pestaña Mods de Mario Party 9. La verificación de la sala y del estado Listo se ejecutó directamente mediante el servicio y los adaptadores por un aviso de firewall en la prueba visual. |
| Actualizador (`tests/e2e/staging-e2e.ps1`) | 9/9 en el subconjunto que cubre instalación firmada, descarga diferencial, conservación de datos, firma alterada, 404 y rollback. El sondeo de las primeras ejecuciones podía impedir temporalmente escribir el log o el diario. Se habilitó escritura compartida en el lector del script y se repitió la suite. |

Quedan por validar una partida en dos equipos físicos y una actualización entre versiones
publicadas del mismo mod. El archivo real que se usó en GameBanana tenía un único ZIP.
