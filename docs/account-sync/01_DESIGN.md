# Diseño incremental

## Identidad y sesión

Cuenta opcional en Ajustes. Usar los endpoints Auth de Supabase para contraseña, confirmación por correo, renovación y recuperación. El correo solo identifica al entrar. El alias se guarda en `account_profiles` con unicidad ASCII sin distinguir mayúsculas. No se deduce del correo. OAuth queda fuera de este incremento.

El servicio de cuenta serializa los trabajos en segundo plano. El refresh token se guarda con DPAPI del usuario de Windows y entropía ligada al origen Supabase. El token de acceso queda en memoria. Ante corte se reintenta con espera creciente; el cierre local retira el estado de la cuenta aunque falle la revocación remota. WinHTTP mantiene las redirecciones deshabilitadas; la configuración admite HTTPS de un proyecto Supabase y HTTP local solo en 127.0.0.1. El cliente solo acepta claves publicables o JWT anon.

## Guardado

MP9 y Wii Party aplican `title/00010000/<id del título>/data` a la NAND configurada. Es la misma unidad que usa `engine/src/platform/save_backup.cpp` en ambos ports. El adaptador elige región y revisión a partir de los metadatos de instalación. Los guardados de otros títulos, la NAND compartida y RFL quedan fuera.

El bundle local tiene orden canónico, SHA-256, cotas de bytes y entradas, rutas ya normalizadas y verificación con dos lecturas. El diario de restauración conserva la copia anterior y permite recuperarla tras interrumpir el cambio de directorio. El bloqueo de nombre impide operaciones simultáneas sobre la misma ruta. Los usos automatizados solo operan sobre datos ficticios en esta rama.

Storage recibe objetos privados e inmutables por revisión. La función SQL comprueba existencia y tamaño antes de registrar, y decide el avance mediante comparación con el head esperado. Si hay conflicto, ambas copias quedan registradas. El cliente revalida el hash y el tamaño al descargar. La eliminación de objetos registrados queda fuera del cliente.

## Mods

El contrato `mods::ProfileCatalog` exporta referencias verificadas. Reutilizar su parser y `plan_catalog_restore`; la sincronización cloud no autoriza descargas ni cambia Ready.
