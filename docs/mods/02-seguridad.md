# 02 — Seguridad (modelo de amenazas y defensas)

Principio: **por defecto se rechaza**. Solo se instala lo que el análisis demuestra que es un mod de datos que
este port carga; todo lo demás se explica y se descarta.

## Adversario A — anfitrión malicioso

- El manifiesto de sala solo sirve para comparar (fase 6). Nunca lleva URLs, rutas ni órdenes: el cliente busca
  los mods en sus propios proveedores permitidos, con consentimiento.
- El anfitrión no sirve archivos (ni P2P): no puede colar contenido ni redistribuir mods con licencia NC-ND.
- El JSON del anfitrión pasa por `parse_json` con límites (1 MiB, profundidad 16, 20 000 elementos, solo enteros,
  sin claves repetidas). Pruebas D07.
- Gate en el anfitrión sin tocar su netcode: el fingerprint va en el token del HELLO; el anfitrión rechaza a quien
  no coincida. Un cliente que manipule su UI no puede «forzar Ready» porque ni siquiera entra (C13).

## Adversario B — paquete malicioso

| Ataque | Defensa | Prueba |
|---|---|---|
| `../`, `..\`, absolutas, `C:`, ADS `a:b` | `normalize_relative` antes de extraer nada; `join_inside` + `is_within` al escribir | D01 |
| enlaces simbólicos, uniones, puntos de reanálisis | atributos del ZIP; `FILE_ATTRIBUTE_REPARSE_POINT` al importar carpetas | D02 |
| bomba ZIP | relación ≤ 200 para > 1 MiB, ≤ 256 MiB por archivo, ≤ 1 GiB total, ≤ 20 000 entradas, corte al superar el tamaño declarado, entradas solapadas | D03 |
| duplicados / mayúsculas / archivo y carpeta homónimos | `fold_key` | D04 |
| nombres reservados, puntos y espacios finales, no ASCII | rechazo | D04 |
| ejecutables, scripts, instaladores | rechazo del paquete entero | D05 |
| Gecko, `<memory>`, REL/DOL | rechazo: es código y el port es recompilación estática | A04 |
| imagen de disco completa | rechazo (no es un mod; contenido de Nintendo) | D14 |
| ZIP cifrado, ZIP64, multivolumen, métodos raros | rechazo | — |
| CRC o tamaño falsos | verificación al extraer | — |
| XML con DTD/entidades | rechazo (sin XXE) | — |

## Adversario C — proveedor o archivo alterado

- Cada archivo instalado tiene su SHA-256 en `mod.json`. `verify()` y la construcción del disco virtual lo
  comprueban: un paquete alterado no se lanza (D08).
- El MD5 de GameBanana solo detecta corrupción; no se presenta como prueba de autenticidad.

## Persistencia y fallos

- Instalación: staging → validación → `rename` atómico. Cancelación o corte → al abrir se borra staging (D10).
- Perfiles y `active.txt`: escritura a temporal + `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`.
- Candado exclusivo entre procesos (dos launchers).
- Disco virtual: se construye en `.tmp` y solo se da por bueno con `.complete`.

## Lo que el gestor nunca hace

Ejecutar nada del paquete, escribir en la ROM / disco extraído / guardados, descargar por orden del anfitrión,
pedir credenciales, enviar rutas privadas.
