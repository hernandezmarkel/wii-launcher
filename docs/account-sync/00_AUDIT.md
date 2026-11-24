# Auditoría de cuentas y sincronización

24 de noviembre de 2026. Rama `codex/account-cloud-sync`, base `5683a97` (beta.7 publicada). Checkout original limpio al iniciar. El desarrollo usa un worktree independiente.

| Componente | Evidencia y estado |
|---|---|
| Aplicación | Windows x86_64, C++17, CMake, SDL3 e ImGui. `app/main.cpp` dirige vistas, montaje y espera de procesos. |
| Sesiones y perfiles de cuenta | Sin implementación previa, esquema SQL ni configuración de Supabase detectada. Preferencias ordinarias en `app/prefs.cpp`. |
| Red y JSON | WinHTTP en `updater/src/http_win.cpp`, originalmente solo GET. JSON acotado en `mods/src/json.cpp`. La política de hosts del updater es específica de las releases. |
| Mods | `profile_catalog.h` y `reference.h` exportan y reconstruyen referencias de paquetes verificados. `ModsService::verify_ready` sigue validando el modset efectivo de la sala. |
| Guardados | MP9 y Wii Party montan `games/mp9/nand` y `games/wiiparty/nand`. Ambos motores aceptan la raíz NAND como argumento. Falta verificar el rango exacto de archivos de cada título y su aislamiento. |
| Lanzamiento y aislamiento | `Game::running_pid`, `launch_modded` y `wait_exit`; el launcher ya permite separar `WL_LOCALAPPDATA` y `WL_APPDATA` en pruebas. |
| Infraestructura | GitHub CI y updater existentes. Docker Desktop instalado por usuario; el daemon no respondía al prevuelo. Staging remoto autorizado, sin identificador ni conexión disponible. |

El desarrollo parte de fixtures nuevos. Para cualquier migración de una instalación antigua se exige copia previa y consentimiento explícito. No se han leído contenidos de guardados personales.
