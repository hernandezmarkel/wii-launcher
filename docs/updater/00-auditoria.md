# Fase 0: auditoría del launcher y salvaguardas

Fecha: 18/11/2026. Equipo: portátil de Markel (i5-1334U, Iris Xe, 16 GB, Windows 11 Pro 10.0.26300, x86_64).
Especificación: `PROMPT_MAESTRO_CLAUDE_CODE_LAUNCHER_AUTOUPDATE.md`, `WORKFLOW_LAUNCHER_GITHUB_AUTOUPDATE.md`,
`MATRIZ_PRUEBAS_AUTOUPDATE.md` y el `README.md` del paquete (en `Descargas\PAQUETE_CLAUDE_LAUNCHER_GITHUB_AUTOUPDATE.zip`).
Los cuatro se han leído completos.

Leyenda: **[H]** hecho observado, con su evidencia · **[Hip]** hipótesis · **[P]** decisión pendiente.

## 1. Stack y estructura

| Elemento | Valor | Evidencia |
|---|---|---|
| Lenguaje | C++17 **[H]** | `CMakeLists.txt` (`CMAKE_CXX_STANDARD 17`) |
| UI | SDL 3.4.16 (estático) + Dear ImGui 1.92.9b, renderer SDL **[H]** | `CMakeLists.txt`, `third_party/imgui`, `app/main.cpp` |
| Build | CMake ≥ 3.20 + Ninja, MinGW-w64 g++ 15.2.0, enlazado estático **[H]** | `build/CMakeCache.txt` (`CMAKE_CXX_COMPILER`, `CMAKE_GENERATOR=Ninja`) |
| Toolchain usado | el que instaló el launcher de MP9 en `%LOCALAPPDATA%\MarioParty9Online\build\deps\toolchain` (cmake 4.4.3, ninja, mingw) **[H]** | `CMakeCache.txt`. Solo se usa en lectura |
| SO / arquitectura | Windows x86_64 únicamente **[H]** | `ws2_32`, `WIN32`, `-static`, rutas `%LOCALAPPDATA%` |
| Red actual | sockets propios (salas LAN/Hamachi); **ninguna** petición HTTP **[H]** | `adapters/*/room/*.cpp`; no hay WinHTTP/curl |
| Versión del launcher | **no existe** ninguna constante de versión **[H]** | `grep version app/ core/`: solo `manifest_version`, `adapter_version` |
| Git | la carpeta **no era un repositorio** **[H]** | `git status` → `fatal: not a git repository` |
| CI / pipelines | ninguno **[H]** | no hay `.github/` en el launcher |

Módulos (todo en `wii-launcher/`):

- `core/` (`wl_core`): `manifest.cpp` (parser TOML propio y `safe_relative_path`), `catalog.cpp` (lee `manifests/*.toml`),
  `install.cpp` (`local_app_data()`, `install_root()`, `resolve_inside()`), `process.cpp` (`spawn`, `wait_exit`).
- `adapters/`: `mp9_adapter.cpp`, `wiiparty_adapter.cpp`, `mkwii_adapter.cpp` + código de sala copiado de los launchers originales.
  Contrato en `core/include/wl/adapter.h` (`GameAdapter`, `RoomSession`, `kAdapterContractVersion = 1`).
- `app/`: `main.cpp` (UI: `sidebar()`, `game_page()`, `room_page()`, bucle por eventos en `main()`), `prefs.cpp` (`Prefs`).
- `tests/tests.cpp`: 112 comprobaciones sin framework (`wl_tests`). `tools/wlaunch.cpp`: CLI de diagnóstico.

## 2. Juegos realmente integrados

| Juego | Estado en manifest | Lanza | Online | Evidencia |
|---|---|---|---|---|
| Mario Party 9 | `in_development` | sí | sala propia LAN/VPN | `manifests/mp9.toml`, `adapters/mp9_adapter.cpp` |
| Wii Party | `playable` | sí | sala propia LAN/VPN | `manifests/wiiparty.toml` |
| Mario Kart Wii | `beta` | sí | servidor WFC en la LAN (`lan_wfc`) | `manifests/zz_mkwii.toml` |
| Smash Bros. Brawl | `planned` | no | no | `manifests/zz_brawl.toml` (adaptador `none`) |

No se ha tocado ningún adaptador, juego ni netcode.

## 3. Distribución actual y datos de usuario

**Distribución [H]:** ZIP portable sin instalador, `WiiPorts-clase.zip` (423,6 MB, versión buena en `Descargas`; la copia de
`dist/` está truncada). Contenido de `dist/WiiPorts-clase/`:

| Ruta | Tamaño | ¿Es del launcher? |
|---|---|---|
| `wii-launcher.exe` | 9,0 MB | **sí** |
| `manifests/*.toml` (4) | < 4 KB | **sí** |
| `fonts/Inter-*.ttf` (2, OFL) | 0,8 MB | **sí** |
| `LEEME.txt`, `LICENCIAS.txt` | < 4 KB | **sí** |
| `instaladores/` (mp9, wiiparty, mkwii) | 1.161 MB | no: instaladores de cada juego (otros hilos) |
| `servidor-mkw/` | 204 MB | no: servidor WFC de MKWii (otro hilo). Contiene `pgpass.local` y `pgdata/` |

Conclusión: **lo que cambia cuando cambia el launcher pesa ~10 MB** (exe + manifests + fuentes + textos). El updater solo debe
tocar esos archivos. Los instaladores y el servidor son de otros hilos y no los actualiza este sistema.

**Datos de usuario [H]** (todos fuera de la carpeta del launcher, así que reemplazar la carpeta no los toca):

- Preferencias del launcher: `%LOCALAPPDATA%\WiiLauncher\prefs.ini` (`app/prefs.cpp`, escritura atómica con `.tmp` + rename).
- Métrica de arranque: `%LOCALAPPDATA%\WiiLauncher\metricas.txt` (`main()`, primer fotograma).
- Juegos instalados, sus ajustes y partidas: `%LOCALAPPDATA%\<install_folder>` (`install_root()`), p. ej. `MarioParty9Online`,
  `WiiPartyOnline`, `MKWiiLAN` (con `UserData/`, identidad de consola de MKWii).
- Rutas a launchers antiguos elegidos por el usuario: `prefs.ini` (`legacy_launcher.<id>`).
- `WL_LOCALAPPDATA` redirige `local_app_data()` para pruebas (`core/src/install.cpp`).

**Usuarios portable vs instalador [H]:** solo existe el ZIP portable. No hay instalador, entradas de registro ni accesos
directos. No hace falta administrador. **[Hip]** Alguien podría haberlo descomprimido en `Program Files`: el updater debe
detectar carpeta sin escritura y no pedir elevación (ver ADR).

**Versiones instaladas por usuarios [H/Hip]:** una sola, la repartida en clase el 18/11/2026 (exe de 9.401.977 bytes). La he
etiquetado `v0.1.0-clase`. Esa versión **no tiene updater**: necesitará una única actualización manual (migración, fase 8).

## 4. Baseline medido (antes de cambios)

Build en carpeta aparte `build-qol/` (no se toca `build/`), SDL desde la caché local de `mp9-recomp` (`FETCHCONTENT_SOURCE_DIR_SDL3`).

| Métrica | Valor | Cómo |
|---|---|---|
| Configurar CMake (limpio) | 215,6 s | `cmake -S . -B build-qol -G Ninja ...` (la mayor parte son las comprobaciones de SDL) |
| Compilar todo (limpio, `-j 8`) | 102,6 s | `cmake --build build-qol -j 8` (286 pasos) |
| Tests | 112/112 OK en 4,9 s | `build-qol/wl_tests.exe` |
| Tamaño `wii-launcher.exe` | 9.401.977 bytes | idéntico al repartido |
| Primer fotograma (7 arranques) | 162 / 182 / 186 / 201 / 235 / 238 / 276 ms → p50 ≈ 201 ms | `metricas.txt`, `--screenshot` |
| Proceso completo con captura | 1,02 s (caliente), 2,3 s (frío) | `Start-Process -Wait`; incluye 4 fotogramas con vsync |
| RAM en reposo | 54,0 MB privada, 58,6 MB working set | a los 6 s |
| CPU en reposo | 0,000 s en 10 s | bucle `SDL_WaitEventTimeout` |
| Comprobar actualización | no existe | — |

Los ~40 min por port no intervienen: el build del launcher no compila ningún juego (CMake solo tiene `wl_core`, `wlaunch`,
`wl_tests`, `wl_imgui`, `wii_launcher`).

## 5. Inventario de riesgos antes del primer `git add`

| Elemento | Riesgo | Decisión |
|---|---|---|
| `dist/` (2,1 GB: instaladores, servidor, ZIPs, BMP de capturas) | binarios ajenos, `pgpass.local`, datos de PostgreSQL | ignorado en `.gitignore`. **Nunca** en el repo ni en Releases |
| `build/`, `build-qol/` | binarios | ignorados |
| `build-tools/` (`ccache.exe`, `ccache-key.asc.gh`, logs) | binario de terceros y clave pública de ccache | ignorado; es del trabajo de builds de juegos |
| `parches/` (fuentes del launcher de MP9 y parches ccache) | código de otro proyecto | ignorado; pertenece a los repos de cada juego |
| `third_party_imgui.tgz`, `wlaunch` (vacío) | restos | ignorados |
| `third_party/imgui/` | MIT, con `LICENSE.txt` | se versiona |
| Fuentes Inter | OFL 1.1 | no se versionan (vienen de la caché de mp9-recomp); el release las descarga con hash fijo |
| Material de Nintendo | ninguno en el código: el launcher dibuja fichas con iniciales y colores (`tile_color`) | revisado `grep` de discos, claves, IPs y rutas personales: solo IPs de ejemplo en tests y textos |

`git add -n .` tras el `.gitignore` deja 48 archivos, todos código propio, ImGui o manifests. Commit inicial `d5ca087`
(etiqueta `v0.1.0-clase`). Rollback local: `git checkout v0.1.0-clase` y el ZIP bueno de `Descargas`.

## 6. Skills revisadas

- Instaladas y útiles aquí: `security-review` y `code-review` (revisión del updater), `simplify`, `run` (arrancar la app),
  `game-ui-ux`/`game-ui-design` (estados y accesibilidad de la UI del updater).
- Buscadas en el catálogo (GitHub Actions, CI/CD, auto update, firma de código, C++ escritorio, testing): **ninguna** disponible.
  No he instalado skills de la comunidad por `npx`: añadirían código remoto sin revisar para algo que se cubre con la
  documentación oficial de GitHub enlazada en la especificación.

## 7. Decisiones pendientes del propietario

- **[P1]** Crear el repositorio remoto, su visibilidad (recomendado: público) y la cuenta (`gh` tiene sesión como `MarkelHernandez`).
- **[P2]** Guardar la clave privada de firma como secreto del repositorio (`WL_UPDATE_SIGNING_KEY`) y una copia de seguridad offline.
- **[P3]** Autorizar el push de la rama y la primera release (beta y después stable).
- **[P5]** Licencia del código propio del launcher (no la elijo yo; sin ella el repo público es «todos los derechos reservados»).
- **[P4]** Firma Authenticode: no hay certificado. Opción gratuita para código abierto: SignPath Foundation (requiere solicitud).

## 8. Gate de la fase 0

- El launcher actual compila y arranca: **sí** (medido arriba).
- Copia recuperable: **sí** (etiqueta `v0.1.0-clase` + `Descargas\WiiPorts-clase.zip`).
- Stack y formato de distribución identificados: **sí**.
