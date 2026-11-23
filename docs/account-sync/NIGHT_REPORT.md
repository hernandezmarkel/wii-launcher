# Informe de sesión

- Fecha: 24 de noviembre de 2026.
- Rama: `codex/account-cloud-sync`. Base: `5683a97`.
- Fase 0: autorización A–D recibida y worktree separado creado. Inventario en [00_AUDIT.md](00_AUDIT.md).
- Staging remoto: autorizado en el proyecto de pruebas que indique el usuario. Falta identificación y acceso delegado.
- Supabase local: Docker 29.8.1 y CLI 3.3.0 (checksum oficial), con Auth, PostgREST, Storage y SMTP de prueba. La CLI publicó puertos en todas las interfaces pese a la opción de la red. Se detuvo el stack antes de insertar cuentas y se reconstruyeron sus tres puertos de prueba con asignación explícita a 127.0.0.1. Verificado desde Docker y Windows.
- Fases 1–2: contrato, cliente de Auth, cuenta en los ajustes y renovación en segundo plano. DPAPI ligado al origen. El login y la renovación tienen prueba real. El correo de recuperación y una instalación física adicional siguen pendientes.
- Fase 3: migraciones de perfiles y guardados aplicadas al Postgres local. La prueba real de perfiles A/B pasó para lectura, actualización y borrado ajenos.
- Pruebas base: `ctest --test-dir build-account --output-on-failure` pasó los dos ejecutables. `wl_account_tests.exe --live` verificó registro A/B, login incorrecto, renovación, logout, DPAPI y RLS de perfil. Compiló `wii_launcher`.
- Checkpoint de cuentas: `4f3bbc3`.
- Fase 4: snapshot canónico verificado byte a byte, rechazo de SHA incorrecto y ruta ascendente, operación bloqueada con juego abierto simulado, y recuperación de journal tras interrumpir el cambio de directorio. Fixtures sintéticos.
- Fase 5: Supabase local real, guardado A→B restaurado con los mismos bytes, escrituras concurrentes desde una revisión común, ambas copias legibles y reintento de la misma revisión idempotente. Storage negó listado/descarga/subida/borrado cruzados. Pendiente: cola persistente y automatización al cerrar juego; entorno físico y cortes de una transferencia real.
- Datos: se usarán únicamente instalaciones aisladas y fixtures sintéticos. Producción, push, merges y releases quedan fuera del alcance.
- Siguiente tarea: guardar y leer catálogos de mods mediante el contrato existente, sin archivos de mods.
