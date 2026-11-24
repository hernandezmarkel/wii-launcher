# Reanudación de cuentas y sincronización

1. Abrir el worktree `wii-launcher-account-sync` y comprobar `git status --short --branch` y `git log -5 --oneline`.
2. Cargar el toolchain con `. .\tools\entorno-mods.ps1`.
3. Configurar núcleo con `cmake -S . -B build-account -G Ninja "-DWL_BUILD_APP=OFF" "-DCMAKE_BUILD_TYPE=Release"`, compilar con `cmake --build build-account -j 4` y ejecutar `ctest --test-dir build-account --output-on-failure`.
4. Para las pruebas vivas, aplica las migraciones `supabase/migrations` en Supabase de desarrollo y verifica que los puertos publicados estén ligados a 127.0.0.1. Configura `WL_SUPABASE_URL` y `WL_SUPABASE_PUBLISHABLE_KEY` para ese proceso y ejecuta `./tests/e2e/account-local.ps1`. El script solo admite loopback.
5. La interfaz está en **Ajustes del launcher → Cuenta**. Compila el objetivo `wii_launcher` con `WL_BUILD_APP=ON`. Usa raíces aisladas `WL_LOCALAPPDATA` y `WL_APPDATA` para los ensayos de lanzamiento.
6. La recuperación por código requiere que la plantilla de correo de recuperación incluya `{{ .Token }}`. Para repetir la prueba viva, define `WL_AUTH_TEST_MAIL=http://127.0.0.1:54324` con el buzón Mailpit local y usa una plantilla que contenga `Wii Ports recovery code: {{ .Token }}`. El script se conecta a buzones sintéticos.
7. Siguiente incremento: crear una NAND separada por cuenta mediante opt-in, enlazar la ruta al lanzamiento y activar el procesamiento de la cola al cerrar. El cliente cloud, los respaldos y la cola ya tienen pruebas vivas aisladas. La ruta de progreso local compartida no se importará automáticamente.
8. Configura el staging remoto únicamente cuando esté identificado. Verifica Auth y RLS/Storage con dos usuarios sintéticos, sin copiar sesiones ni datos reales. El origen y la clave publicable se inyectan localmente.
