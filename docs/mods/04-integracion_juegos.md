# 04 — Integración por juego

| Juego | Estado | Formatos | Cómo se aplica | Evidencia |
|---|---|---|---|---|
| Wii Party (SUPP01/SUPE01) | **Compatible (verificado)** | archivos del disco; Riivolution `<file>/<folder>` | disco virtual por `argv[1]` | A/B real 18/11/2026 (`06-pruebas_resultados.md`) |
| Mario Party 9 (SSQE01) | **Compatible (verificado)** | archivos del disco; Riivolution `<file>/<folder>` | disco virtual por `argv[1]` | A/B real 23/11/2026 (`06-pruebas_resultados.md`) |
| Mario Kart Wii (WiiCompiled) | No soportado en el launcher | — | requiere perfil compilado o cambio en su runtime | auditoría §4 |
| Brawl | No existe el port | — | — | — |

## Wii Party / Mario Party 9 (motor wii_recomp)

- Se admiten: sustitución de archivos existentes del disco (cualquier tamaño < 4 GiB), Riivolution de solo
  archivos con opciones (las elegidas cuentan para el online).
- No se admiten: crear archivos nuevos en el disco (`create="true"`), parches parciales (`offset`), REL/DOL,
  Gecko, `<memory>`, `<savegame>`, texturas de Dolphin (`tex1_*.png`), 7z/rar sin descomprimir.
- Región: un archivo que no existe en tu disco (p. ej. `en_US` en un disco PAL) se ignora y se informa; si no
  queda ninguno, se rechaza el paquete. `<id game>` y `<region>` de Riivolution se comprueban.
- Reinicio: los mods se aplican al lanzar. Con el juego abierto, los cambios esperan al siguiente arranque
  (sin falsa recarga en caliente, A08).

## Guía para modders (resumen)

Un ZIP con la estructura del disco extraído (`files/...`, o `DATA/files/...`, o una carpeta del disco como
`common/...` en MP9) o un pack de Riivolution (`riivolution/*.xml` + carpeta). Sin ejecutables ni códigos.
