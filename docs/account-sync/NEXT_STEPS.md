# Reanudación de cuentas y sincronización

1. Abrir el worktree `wii-launcher-account-sync` y comprobar `git status --short --branch` y `git log -5 --oneline`.
2. Cargar el toolchain con `. .\tools\entorno-mods.ps1`.
3. Configurar núcleo con `cmake -S . -B build-account -G Ninja "-DWL_BUILD_APP=OFF" "-DCMAKE_BUILD_TYPE=Release"`, compilar con `cmake --build build-account -j 4` y ejecutar `ctest --test-dir build-account --output-on-failure`.
4. Para las pruebas vivas, aplica las migraciones `supabase/migrations` en Supabase de desarrollo y verifica que los puertos publicados estén ligados a 127.0.0.1. Configura `WL_SUPABASE_URL` y `WL_SUPABASE_PUBLISHABLE_KEY` para ese proceso y ejecuta `./tests/e2e/account-local.ps1`. El script solo admite loopback.
5. La interfaz está en **Ajustes del launcher → Cuenta**. Compila el objetivo `wii_launcher` con `WL_BUILD_APP=ON`. Usa raíces aisladas `WL_LOCALAPPDATA` y `WL_APPDATA` para los ensayos de lanzamiento.
6. La recuperación por código requiere que la plantilla de correo de recuperación incluya `{{ .Token }}`. Para repetir la prueba viva, define `WL_AUTH_TEST_MAIL=http://127.0.0.1:54324` con el buzón Mailpit local y usa una plantilla que contenga `Wii Ports recovery code: {{ .Token }}`. El script se conecta a buzones sintéticos.
7. En Cuenta, selecciona Wii Party o Mario Party 9, activa **Guardado de cuenta** y confirma. El botón **Jugar** usa la NAND de esa cuenta. Para comprobar el port con un fixture temporal, usa `./tests/e2e/account-port-local.ps1`. Los respaldos locales se conservan al restaurar. Falta migración opt-in desde la NAND compartida.
8. Configura el staging remoto únicamente cuando esté identificado. Verifica Auth y RLS/Storage con dos usuarios sintéticos, sin copiar sesiones ni datos reales. El origen y la clave publicable se inyectan localmente.
9. **Ver historial** lista las dos ramas de una divergencia. Selecciona una copia y confirma **Restaurar y conservar respaldo**. Una copia histórica se restaura en el equipo; el head remoto conserva su historial. **Exportar copia** crea un `.wlsave` con SHA-256 en la carpeta privada de la cuenta.
10. Si se interrumpe una subida, la cola conserva el snapshot. El launcher vuelve a comprobar el head antes de subir y reanuda la cola al abrir con la cuenta correcta. Si se cierra el launcher antes de terminar el juego, abre Cuenta y sincroniza manualmente tras cerrar el port.
11. Para la validación remota, prueba el correo de confirmación, el cambio entre dos cuentas de staging y el arranque de dos PCs físicos con los mismos disco y revisión. Continúa con la migración consentida de guardados antiguos, la instalación asistida de referencias y el contrato de Mii según la matriz.

## Repetir el backend local de esta sesión

Supabase CLI y su proyecto de prueba están bajo `build-account/supabase-local`. El daemon asignó los puertos 54321, 54322 y 54324 explícitamente a 127.0.0.1. Comprueba esas asignaciones antes de usar el fixture.

Para reanudar los contenedores ya preparados, arranca primero `supabase_db_wiiports_account_local`. Tras estar sano, arranca las instancias `supabase_inbucket_wiiports_account_local`, `supabase_auth_wiiports_account_local`, `supabase_rest_wiiports_account_local`, `supabase_storage_wiiports_account_local` y `supabase_kong_wiiports_account_local`. El servidor de la plantilla OTP se reanuda con:

```powershell
docker exec -d supabase_inbucket_wiiports_account_local /bin/busybox nc -lk -p 8026 -e /tmp/wl-recovery-template.sh
```

Lee `supabase status -o json` del proyecto local dentro de una variable y pasa únicamente `API_URL` y `PUBLISHABLE_KEY` al entorno de la prueba. Evita imprimir el JSON completo porque también contiene claves del servidor de desarrollo.
