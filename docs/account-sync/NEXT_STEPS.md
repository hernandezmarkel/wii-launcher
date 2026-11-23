# Reanudación de cuentas y sincronización

1. Abrir el worktree `wii-launcher-account-sync` y comprobar `git status --short --branch` y `git log -5 --oneline`.
2. Cargar el toolchain con `. .\tools\entorno-mods.ps1`.
3. Configurar núcleo con `cmake -S . -B build-account -G Ninja "-DWL_BUILD_APP=OFF" "-DCMAKE_BUILD_TYPE=Release"`, compilar con `cmake --build build-account -j 4` y ejecutar `ctest --test-dir build-account --output-on-failure`.
4. Para las pruebas vivas, aplica las migraciones `supabase/migrations` en Supabase de desarrollo y verifica que los puertos publicados estén ligados a 127.0.0.1. Configura `WL_SUPABASE_URL` y `WL_SUPABASE_PUBLISHABLE_KEY` para ese proceso y ejecuta `./tests/e2e/account-local.ps1`. El script solo admite loopback.
5. La interfaz está en **Ajustes del launcher → Cuenta**. Compila el objetivo `wii_launcher` con `WL_BUILD_APP=ON`. Usa raíces aisladas `WL_LOCALAPPDATA` y `WL_APPDATA` para los ensayos de lanzamiento.
6. La recuperación por código requiere que la plantilla de correo de recuperación incluya `{{ .Token }}`. Falta validar el correo y el cambio de contraseña end-to-end.
7. Siguiente incremento: sincronización de `ProfileCatalog` por cuenta. Los guardados cloud están accesibles desde el contrato y las pruebas; el launcher aún requiere asociación opt-in y cola por dispositivo antes de habilitar operaciones sobre instalaciones.
