# 01 — Arquitectura del gestor de mods

El gestor de mods es un subsistema aparte (`wl_mods`) que no toca el updater ni el netcode. Del updater solo
usa su API pública de SHA-256 (CNG) y su `HttpClient` de un salto; la lista de hosts permitidos y las
redirecciones de los mods son propias.

```
app/main.cpp ─ página «Mods», botón Jugar, token de sala
app/mods_service.* ─ hilo de trabajo: importar, instalar, perfiles (la UI solo lee instantáneas)
mods/  (biblioteca wl_mods, sin UI)
  json        JSON acotado y canónico (manifiestos, perfiles, salas)
  safe_path   rutas relativas seguras (OWASP + particularidades de Windows)
  zip         lector ZIP defensivo (zlib) — ModPackageValidator, parte 1
  xml         XML mínimo para Riivolution (sin DTD ni entidades externas)
  fst         FST del disco de Wii y su regeneración
  package     PackageSource (ZIP/carpeta) + análisis contra el disco real + GameModSupport por juego
  store       ModStore: instalador transaccional, perfiles, conjunto efectivo, fingerprint y disco virtual
  launch      prepare_launch (argv[1] = disco virtual) y token de sala con fingerprint
tools/wlmods  CLI con el mismo núcleo (pruebas reproducibles)
```

Correspondencia con los componentes exigidos:

| Componente | Dónde |
|---|---|
| `ModProvider` / `LocalProvider` | `package.cpp` (`open_source`: ZIP o carpeta); `GameBananaProvider` en la fase 4 |
| `ModCatalog` | `ModStore::installed()` + mod.json; catálogo remoto en la fase 4 |
| `GameModAdapter` | `GameModSupport` (formato, rutas de código bloqueadas, `proven`) + disco virtual `wl-vdisc-1` |
| `ModPackageValidator` | `zip.cpp` + `analyze_package` (tipos, rutas, región, código, tamaños) |
| `ModInstaller` | `ModStore::install` (staging → hash → rename atómico; dedupe por clave) |
| `ModProfileManager` | perfiles JSON `default`, `custom-*`, `session-*`; `active.txt`; `epoch` monotónico |
| `ModUpdateService` | instalar otra versión con el mismo `id` = actualización; la anterior queda instalada para volver |
| `SessionModNegotiator` / `ReadyGate` | fases 6-7 (`docs/mods/05-protocolo_salas.md`) |
| Telemetría | mensajes tipados en español; logs del juego con `WP_LOG_DISC`; sin rutas privadas en la red |

## Disco virtual (`wl-vdisc-1`)

Para un perfil con mods se construye `%LOCALAPPDATA%/WiiLauncher/mods/<juego>/vdisc/<fp16>/`:

- `sys/boot.bin`, `bi2.bin`, `apploader.img`, `main.dol`: enlaces duros a los originales.
- `sys/fst.bin`: regenerada. Un archivo que cabe en su hueco conserva su desplazamiento; si no, se recoloca al
  final del disco (alineado a 32 KiB). Mismo tamaño de FST: `boot.bin` no cambia.
- `files/...`: enlaces duros a todo el disco extraído, salvo los archivos de los mods (enlaces a su paquete).
- `.complete` con el fingerprint, escrito al final. Se reutiliza si coincide y los hashes de los archivos de mod
  siguen siendo los esperados; si no, se reconstruye. Solo se guarda uno por juego.

El juego recibe la carpeta como `argv[1]` (el motor ya lo admite). Con perfil vanilla la `LaunchSpec` es
idéntica a la de siempre. Si la ruta tiene caracteres no ASCII se pasa su nombre corto 8.3 (el motor usa `fopen`).

## Fingerprint

SHA-256 del JSON canónico (claves ordenadas, sin espacios, `fingerprint_schema: 1`) con: `game_id`,
`base` (`<disc_id>:<16 hex de sha256(fst.bin)>`), `loader`, mods en orden (`id`, `version`,
`effective_content_sha256`, `load_order`, `config_sha256` de las opciones) y la lista de bytes que se aplican
de verdad (`[ruta, sha256]` por archivo efectivo, tras resolver el orden de carga). Sin mods: `vanilla`.

## Almacenamiento

```
%LOCALAPPDATA%/WiiLauncher/mods/<game_id>/
  packages/<clave>/mod.json + content/...   inmutables
  staging/<op>/                              temporal; se borra al abrir (recuperación tras apagón)
  profiles/*.json, active.txt
  vdisc/<fp16>/
  locks/store.lock                           exclusivo entre procesos
```

Fuera de la carpeta del launcher (el updater la reemplaza sin tocar esto) y fuera de la del juego (su
«reparar instalación» no lo borra). Nunca se escribe en `extracted/`, en la ROM ni en `nand/`.
