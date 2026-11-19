# Fase 0 — Auditoría: cómo cargan mods de verdad los juegos del launcher

Fecha: 18/11/2026. Equipo: portátil de Markel (Windows 11 Pro 26300, i5-1334U, Iris Xe, 16 GB).
Rama: `feature/launcher-mods` (desde `main` = 65f5616, v1.0.0-beta.1), en un clon aparte
(`Documents\GitHub\wii-launcher-mods`) para no tocar la carpeta del updater.

Revisiones auditadas: `mp9-recomp` 01a05ad (instalada d755eb8), `wiiparty-recomp` c055018 (instalada
6dab024 + exe c055018), WiiCompiled de `MKWiiLAN\work` (perfil `base`, compilado el 18/11/2026).

## 1. Resumen

| Juego | Motor | ¿Carga mods hoy? | Clasificación | Piloto |
|---|---|---|---|---|
| Wii Party | wii_recomp (motor propio) | Sí, sustituyendo archivos del disco extraído vía un «disco virtual» que monta el launcher. Sin tocar el juego ni recompilar. | **Viable con trabajo (solo launcher)** → demostrar A/B | **Sí** |
| Mario Party 9 | wii_recomp (mismo `disc.cpp`, idéntico byte a byte) | Mismo mecanismo que Wii Party | Viable con trabajo; **no declarado compatible** hasta su propia prueba A/B | No |
| Mario Kart Wii | WiiCompiled (perfil `base`) | Su runtime tiene motor Riivolution, pero solo lo activa un perfil compilado con mods (`recomp.yml`) o el producto Retro Rewind. En el perfil `base` no se registra ninguna raíz de overlay en tiempo de ejecución. | Desconocido / requiere perfil o cambio en el juego (hilo de MKWii) | No |
| Super Smash Bros. Brawl | — | No hay port | No soportado (no existe) | No |

## 2. Launcher (lo que hay)

- C++17, SDL3 + Dear ImGui 1.92.9b, CMake + Ninja, MinGW g++ 15.2 (local) / MSYS2 UCRT64 (CI).
- `core/include/wl/adapter.h`: contrato launcher ↔ adaptador v1. `GameAdapter::offline()/online()` devuelven un
  `LaunchSpec {executable, working_dir, args, env}`; `compat_token()` es la cadena que la sala compara.
- `adapters/common.h` (`RecompAdapter`): MP9 y Wii Party se lanzan con `cwd = raíz de instalación` y **sin
  argumentos** → el juego usa su carpeta por defecto `games/<juego>/extracted`.
- Manifiestos de juego en `manifests/*.toml` (`game_id`, `disc_ids`, `install_folder`...).
- Preferencias: `%LOCALAPPDATA%\WiiLauncher\prefs.ini`. Juegos: `%LOCALAPPDATA%\<install_folder>`.
  `WL_LOCALAPPDATA` redirige ambos en pruebas.
- Updater (otro proyecto, no se toca): `updater/` + `app/update_service.*`. Reutilizo **solo** su API pública
  `sha256_hex`, `sha256_file_hex` y `HttpClient::get` (un salto, sin redirecciones); la lista de hosts y el
  seguimiento de redirecciones de los mods son propios.

## 3. Cómo leen el disco Wii Party y Mario Party 9 (trazabilidad)

1. `engine/src/app/main.cpp:93` — `extracted = argc > 1 ? argv[1] : description().data_directory`.
   **La carpeta del disco se puede elegir por argumento.**
2. `engine/src/core/boot.cpp:49` `init_boot_memory()` — lee `sys/boot.bin`, `sys/bi2.bin` y **`sys/fst.bin`** de esa
   carpeta y copia la FST al final de MEM1 (lo que ve el juego).
3. `engine/src/ios/disc.cpp:103` `mount()` — vuelve a leer `sys/fst.bin` y construye la tabla
   *desplazamiento de disco → archivo* (`files/<ruta FST>`). Tamaño = el de la FST.
4. `disc.cpp:123` `read()` — cada lectura DVD del juego abre el archivo que cubre ese desplazamiento.
5. `engine/src/core/modules.cpp` — los REL (`files/rel/*.rel.lz` en Wii Party, `modules` en MP9) se identifican
   por un hash de sus secciones y se enlazan con **código recompilado estático**. `main.dol` también es código
   recompilado. Cambiarlos no cambia el comportamiento y puede colgar el juego.
6. `engine/src/gpu/gx_render.cpp:1152` — la caché de shaders va junto a la carpeta por defecto, no junto al
   argumento; un disco virtual no la duplica.

Consecuencia: si el launcher pasa como `argv[1]` una carpeta con la misma estructura (`sys/` + `files/`) donde
algunos archivos son los del mod y `sys/fst.bin` está regenerada con los tamaños y desplazamientos nuevos,
el juego carga los recursos del mod **sin modificar la ROM, el disco extraído ni el ejecutable**. Los archivos
sin cambios se enlazan con *hard links* (mismo volumen, sin copiar 1 GB). La FST en memoria y la del montaje
salen del mismo `fst.bin`, así que son coherentes.

Comprobado en ejecución (18/11/2026, Wii Party 6dab024/c055018): `wiiparty.exe games/wiiparty/extracted 35`
arranca igual que sin argumento; con `WP_LOG_DISC=1` el log lista cada archivo leído (incluido
`files/locale/sp_EU/mess.bin`, la tabla de textos que se ve en la pantalla de título). Captura de referencia
sin mod: «Pulsa Ⓐ y Ⓑ al mismo tiempo.» (`WP_SAVE_FRAME`, fotograma 13).

## 4. Mario Kart Wii (WiiCompiled)

- `runtime/src/recomp_mod_loader.cpp:273` `RegisterDvdOverlayRoot()` y `:297` `RegisterRiivolutionXml()` solo los
  llama código generado por un perfil con mods (`recomp.yml`), no la configuración.
- `runtime/src/hle/storage/riivolution.cpp:136-150`: raíces = Retro Rewind (si el producto lo es) + las del manifiesto
  de mods compilado. Con el perfil `base` instalado (`juego/local-build.json`: `Profile: base`) no hay ninguna.
- `UserData/Config.toml [paths] dvd_root` apunta a un disco extraído (`disco/sys`, `disco/files`). Un disco virtual
  como el de §3 podría funcionar cambiando `dvd_root`, pero: (a) no he verificado cómo lee WiiCompiled la FST,
  (b) la configuración y la LAN de MKWii son de otro hilo, (c) los mods típicos de MKWii (pistas SZS con CT-Code,
  Pulsar, Retro Rewind) **son de código** y requieren un perfil compilado. Queda **desconocido / fuera del MVP**.

## 5. Formatos de mod y soporte

| Tipo | Wii Party / MP9 | MKWii | Motivo |
|---|---|---|---|
| Sustitución de archivos del disco (`files/...`, carpeta `DATA/files`) | **Soportado tras A/B** | Desconocido | Disco virtual §3 |
| Riivolution XML con solo `<file>`/`<folder>` | **Viable**: se traduce a sustituciones | Requiere perfil | El launcher interpreta el XML; nada se ejecuta |
| Riivolution `<memory>` (parches de RAM) | No soportado | Requiere perfil | Es código: el ejecutable es recompilado estático |
| Gecko / Ocarina (`.gct`, `codes/*.txt`) | No soportado | No soportado | Código PowerPC |
| REL / DOL modificados | No soportado («requiere recompilación») | Ídem | Código recompilado estático (§3.5) |
| Pistas SZS, CT-Code, Pulsar, Kamek | — | Requiere perfil compilado | Código + datos |
| Imagen de disco completa (ISO/WBFS/RVZ) | **Rechazado** | Rechazado | Contenido de Nintendo / no es un mod |
| Ejecutables, scripts, instaladores | **Rechazado** | Rechazado | Código del anfitrión/autor |
| Texturas «HD» de Dolphin (`Load/Textures`) | No soportado | No soportado | El motor no tiene cargador de texturas |

## 6. GameBanana y otros proveedores

- API v11 (`https://gamebanana.com/apiv11/...`) pública, sin clave, JSON. Comprobado en vivo:
  `Util/Search/Results` (buscar juego), `Mod/Index?_aFilters[Generic_Game]=<id>` (listado paginado),
  `Mod/<id>/ProfilePage` y `Mod/<id>/DownloadPage` (archivos con tamaño, MD5, resultado de antivirus y
  `_sDownloadUrl = https://gamebanana.com/dl/<id>`), licencia en `_sLicense`.
- API «oficial» clásica (`https://api.gamebanana.com/Core/Item/Data`) devuelve el **árbol del archivo sin
  descargarlo** (`fields=Metadata().aArchiveFilesList()`): permite clasificar un mod antes de bajar nada.
- IDs de juego: Wii Party **6370** (39 mods), Mario Party 9 **6248** (92), Mario Kart Wii **5896** (2257),
  Brawl **5678** (2881).
- Sondeo de los 39 mods de Wii Party por árbol de archivos (sin descargar): 11 son solo datos
  (Riivolution `<file>/<folder>` o carpeta `files`), 13 incluyen REL (código), 13 incluyen Gecko, 2 traen
  imágenes de disco de 0,7–1,2 GB (rechazar) y 1 es un `.exe` («ISO Patcher»). Formatos de archivo: zip, 7z y rar.
- Sin firma del proveedor: el MD5 de GameBanana solo detecta corrupción; el SHA-256 lo calcula el launcher.
- Límites/términos: no encontré documentación de *rate limit*; uso educado (caché, 1 petición cada ≥300 ms,
  backoff ante 429/5xx), User-Agent identificable y respeto de licencias (muchas son CC BY-NC-ND: se permite
  descargar para uso propio, **no** redistribuir; por eso el anfitrión nunca sirve archivos).
- Game Jolt: su Game API es de trofeos/puntuaciones/sesiones; no ofrece catálogo de mods → no se integra.
- Nexus Mods: no hay páginas de estos juegos de Wii y exige API key → fuera del MVP.

## 7. Online (dónde va la negociación)

- Wii Party (`adapters/wiiparty/room/lobby.cpp`, protocolo copiado del launcher 2611-029): el HELLO lleva
  `build` y `content`; **el anfitrión rechaza** al que no coincide (`lobby.cpp:469`). `content` sale de
  `compat_token()` (`wiiparty_adapter.cpp:121`).
- Mario Party 9 (`adapters/mp9/room/online.cpp`, de otro hilo): el HELLO lleva `version`; el anfitrión rechaza
  si no coincide (`online.cpp:285`).
- Ready lo controla el anfitrión en ambas salas, pero el protocolo no tiene estado «pendiente de mods» ni sitio
  para un manifiesto.
- Plan sin tocar el netcode: (1) con mods activos, el adaptador añade el fingerprint efectivo al
  `compat_token` → el anfitrión antiguo **ya** rechaza a quien no coincida (gate del lado del anfitrión, falla
  cerrado); con perfil vanilla el token no cambia (compatibilidad E03). (2) Un servicio aparte del launcher
  (`launcher-lobby-mods`, TCP en el puerto de la sala + 1) publica el manifiesto de sesión para que el cliente
  vea las diferencias **antes** de unirse. Así no hace falta modo espectador en un lobby que no lo tiene.

## 8. Skills

Revisadas las instaladas: ninguna cubre C++/ImGui, extracción segura de ZIP o loaders de recompilaciones
de Wii (las hay de Roblox, Blender, diseño, `security-review`, `code-review`, `simplify`). La búsqueda de
skills del catálogo para «c++, imgui, mod manager, zip security, path traversal, cmake» no dio resultados.
No instalo nada; aplicaré `security-review`/`code-review` al final de las fases de seguridad.

## 9. Piloto y MVP propuesto

- **Juego piloto: Wii Party** (estado `playable`, el más maduro; su sala ya compara contenido).
- **Formato piloto:** sustitución de archivos de datos (`files/...`) + Riivolution XML de solo archivos.
- **Mod de prueba propio** (no propietario): un parche de texto que cambia «al mismo tiempo.» por otra frase de
  igual longitud en `locale/sp_EU/mess.bin`, generado a partir del disco del propio usuario (el paquete de
  prueba no se distribuye; se crea en local). Prueba A/B: captura sin mod / con mod / desactivado.
- Prueba extra: un archivo de distinto tamaño para comprobar la reubicación en la FST.

## 10. Riesgos

| Riesgo | Mitigación |
|---|---|
| Hard links imposibles (otro volumen, FAT32) | Comprobar volumen; si falla, no lanzar con mods (nunca copiar 1 GB a ciegas) y explicar |
| Alguien abre un hard link para escribir y cambia el original | El launcher solo crea enlaces; el juego abre `rb`; el disco virtual se borra y se recrea |
| Mod pensado para otra región (US vs EU) | Comprobar `disc_id` del XML/paquete y que las rutas existen en la FST del usuario |
| Mod reemplaza un REL/DOL | Lista de rutas prohibidas por juego → «requiere recompilación» |
| Desync online por un mod «cosmético» | Política estricta: todo mod activo cuenta en el fingerprint |
| Cambios de netcode de otros hilos | Solo se toca `compat_token()` cuando hay mods; con perfil vanilla, idéntico |

## 11. Backlog priorizado

1. Núcleo `wl_mods`: JSON acotado, rutas seguras, lector ZIP (zlib), validador, almacén por hash, perfiles,
   disco virtual y fingerprint, con pruebas unitarias (Fase 1–3).
2. Integración en el adaptador de Wii Party (`argv[1]` = disco virtual) y prueba A/B real (Fase 2).
3. UI «Mods» (Instalados/Perfiles, Importar) en ImGui con tareas en segundo plano (Fase 3).
4. `GameBananaProvider` (listado, ficha, árbol, descarga con lista de hosts) y Explorar (Fase 4–5).
5. Manifiesto de sesión, diff tipado, servicio `launcher-lobby-mods` y token con fingerprint (Fase 6–7).
6. Sincronización asistida con perfil de sesión y reingreso opcional (Fase 8). Pruebas reales con 2 PCs:
   bloqueadas hasta tener el segundo equipo (ver `estado_y_bloqueos.md`).
