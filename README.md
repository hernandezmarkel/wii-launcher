# Wii Ports · launcher unificado

Launcher de escritorio (Windows x86_64) para los ports nativos de Wii a PC: biblioteca de juegos, instalación con el
instalador de cada port, salas online por LAN/VPN y **actualización automática y firmada del propio launcher**.

Este repositorio **no contiene juegos, discos, ROMs ni material de Nintendo**. Cada port se compila en el PC de cada
jugador desde su propia copia del disco, con su instalador (otros repos). Una actualización del launcher nunca
recompila ni descarga juegos.

| Juego | Estado |
|---|---|
| Wii Party | jugable, online por LAN/VPN |
| Mario Party 9 | en desarrollo, online por LAN/VPN |
| Mario Kart Wii | beta, online con servidor WFC en la LAN |
| Super Smash Bros. Brawl | previsto |

## Estructura

```
core/       catálogo, manifests, procesos (sin UI)
adapters/   un adaptador por juego (contrato en core/include/wl/adapter.h)
updater/    autoactualización: verificación, red, descarga, aplicador (sin UI)
app/        interfaz SDL3 + Dear ImGui
manifests/  un .toml por juego
tools/      wlaunch (diagnóstico), wl_release (claves, empaquetado, firma), scripts de CI
tests/      wl_tests (unitarios) y e2e/ (feed de staging local)
docs/       arquitectura, updater (auditoría, ADR, publicación, seguridad, guía) y extensiones
```

## Compilar

Requisitos: CMake ≥ 3.20, Ninja y MinGW-w64 g++ (probado con GCC 15.2). En CI se usa MSYS2 MINGW64.

```bash
bash tools/descargar-fuentes.sh _fonts
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWL_INTER_DIR=$PWD/_fonts
cmake --build build
./build/wl_tests.exe
bash tools/empaquetar.sh build dist
```

Para no descargar SDL: `-DFETCHCONTENT_SOURCE_DIR_SDL3=<carpeta con SDL 3.4.16>`. Solo núcleo y tests:
`-DWL_BUILD_APP=OFF`. Tiempos medidos en el portátil de desarrollo: configurar ~2-4 min (comprobaciones de SDL),
compilar todo ~1,7 min, incremental de la UI ~5-15 s, tests ~10 s.

## Versiones y publicación

- La versión está en `CMakeLists.txt` y en `CHANGELOG.md`. SemVer: `vX.Y.Z` estable, `vX.Y.Z-beta.N` beta.
- **Un commit no publica nada.** CI (`.github/workflows/ci.yml`) solo compila y prueba.
- Publicar: etiqueta aprobada → `release.yml` → borrador → el propietario lo publica. Ver
  [docs/updater/04-publicacion.md](docs/updater/04-publicacion.md).

## Documentación

- [Auditoría inicial](docs/updater/00-auditoria.md) · [Repositorio](docs/updater/01-repositorio.md) ·
  [ADR del updater](docs/updater/02-adr-arquitectura.md) · [Publicación](docs/updater/04-publicacion.md) ·
  [Resultados de pruebas](docs/updater/05-resultados-pruebas.md) · [Seguridad y recuperación](docs/updater/seguridad-y-recuperacion.md) ·
  [Guía para jugadores](docs/updater/guia-usuario.md) · [Extensiones](docs/launcher/extensions.md)

## Licencias

Dependencias: SDL 3 (zlib), Dear ImGui (MIT, en `third_party/imgui`), fuente Inter (OFL 1.1, se descarga al compilar).
Detalle en `packaging/LICENCIAS.txt`. **La licencia del código propio del launcher está pendiente de que la elija el
propietario.**
