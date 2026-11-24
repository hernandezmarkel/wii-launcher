# Informe de sesión

- Fecha: 24 de noviembre de 2026.
- Rama: `codex/account-cloud-sync`. Base: `5683a97`.
- Último checkpoint de implementación verificado: `32e8d93`. `git log -7 --oneline` incluye después el cierre documental.
- Fase 0: autorización A–D recibida y worktree separado creado. Inventario en [00_AUDIT.md](00_AUDIT.md).
- Staging remoto: autorizado en el proyecto de pruebas que indique el usuario. Falta identificación y acceso delegado.
- Supabase local: Docker 29.8.1 y CLI 3.3.0 (checksum oficial), con Auth, PostgREST, Storage y SMTP de prueba. La CLI publicó puertos en todas las interfaces pese a la opción de la red. Se detuvo el stack antes de insertar cuentas y se reconstruyeron sus tres puertos de prueba con asignación explícita a 127.0.0.1. Verificado desde Docker y Windows.
- Fases 1–2: contrato, cliente de Auth, cuenta en los ajustes y renovación en segundo plano. DPAPI ligado al origen. Registro, login, renovación y cambio de contraseña mediante un código recibido por SMTP de prueba verificados en el backend local. La confirmación por correo del registro está cubierto por contrato; el stack local tenía el registro sin confirmación obligatoria.
- Fase 3: migraciones de perfiles y guardados aplicadas al Postgres local. La prueba real de perfiles A/B pasó para lectura, actualización y borrado ajenos.
- Pruebas base: `ctest --test-dir build-account --output-on-failure` pasó los dos ejecutables. `wl_account_tests.exe --live` verificó registro A/B, login incorrecto, renovación, logout, DPAPI y RLS de perfil. Compiló `wii_launcher`.
- Checkpoint de cuentas: `4f3bbc3`.
- Fase 4: snapshot canónico verificado byte a byte, rechazo de SHA incorrecto y ruta ascendente, operación bloqueada con juego abierto simulado, y recuperación de journal tras interrumpir el cambio de directorio. Fixtures sintéticos.
- Fase 5, checkpoint `1f59ddb`: Supabase local real, guardado A→B restaurado con los mismos bytes, escrituras concurrentes desde una revisión común, ambas copias legibles y reintento idempotente. Storage negó listado/descarga/subida/borrado cruzados. La cola persistente se validó cortando la petición entre la subida real y el registro del head. Conservó la copia local y confirmó la misma revisión tras reanudar.
- Fases 6–7, checkpoint `51eb24d`: copia y comparación de referencias desde Cuenta, CAS de catálogo y alias visible en salas de los ports con lobby. La subida y descarga de tres referencias entre dos clientes ficticios y el aislamiento del catálogo se verificaron en Supabase local. Quedan pendientes la instalación asistida desde la cuenta, la validación del alias en dos PCs y el Mii predeterminado.
- Regresión de UI: compilación Release del launcher y arranque en vista Cuenta como invitado con SDL de prueba. No se usaron partidas personales.
- Datos: se usarán únicamente instalaciones aisladas y fixtures sintéticos. Producción, push, merges y releases quedan fuera del alcance.
- Checkpoint de integración `354de64`. El launcher ofrece activación explícita de una NAND separada por cuenta, sincronización y restauración con respaldo. **Jugar** usa esa NAND en el modo sin conexión y programa el snapshot tras una salida limpia. El recibo protegido conserva identidad local para arrancar offline sin exponer el token. Las salas conservan su ruta y Ready anteriores.
- Integración real adicional: `./tests/e2e/account-port-local.ps1` arrancó el MP9 instalado con un fixture temporal, creó el título y SYSCONF en la NAND de la cuenta y no creó la NAND por defecto en el fixture. La prueba viva del servicio verificó activación, subida, restauración y logout con dos instalaciones ficticias.
- Cuenta ofrece historial de 25 revisiones compatibles, elección explícita y restauración con respaldo. El servicio restauró una revisión histórica y exportó una copia desde una instalación separada usando Supabase local. El launcher reanuda las copias pendientes al abrir con la cuenta correcta y respeta la espera incremental de la cola.
- Fase 8: regresión del actualizador con feed HTTP aislado en 127.0.0.1. `UPD-02,SEC-01,NET-06,NET-07` produjeron **9/9 PASS**. Se verificó actualización firmada, preservación de datos sintéticos, comprobación de salud, limpieza tras éxito, descarga diferencial y rechazo de corrupción, corte y tamaño excesivo. La compilación normal del launcher quedó restaurada.
- Infraestructura local detenida al terminar las pruebas. Persisten los fixtures sintéticos y las migraciones para repetir la sesión. Los cambios de CI harán ejecutar también la suite de cuentas cuando se autorice la integración remota.
- Siguiente tarea: conectar el staging identificado, validar correo de confirmación y dos PCs físicos, y preparar la importación opt-in de instalaciones antiguas y la instalación asistida de mods.

## Evidencias y límites de la matriz

| Casos | Evidencia |
|---|---|
| AUTH-01 a AUTH-04 y PROFILE-01 | Supabase local real. Registro sin confirmación obligatoria, credenciales válidas e inválidas, OTP por Mailpit, DPAPI Windows, refresh, logout y alias duplicado rechazado. La confirmación obligatoria de registro se verificó solo con respuesta simulada. |
| AUTH-05 | Arranque de la UI como invitado sin configuración Auth. MP9 real montó la NAND aislada en el smoke offline. |
| AUTH-06 a AUTH-08 | RLS real del perfil y Storage con cuentas A/B y lectura anónima de perfil denegada. Los casos de red emiten estado sanitizado. Falta auditoría por rol de todas las tablas y del instalable antes de distribuir. |
| SAVE-01 a SAVE-03 | Código fuente de ambos adaptadores y fixtures locales. Prueba con bloqueo de juego simulado, sin sustituir archivos personales. |
| SAVE-04 a SAVE-06 | Storage y Postgres reales, dos sesiones de la misma cuenta, carpetas distintas y divergencia concurrente conservada. Ambas versiones aparecen en historial. Se probó recuperar la revisión anterior y exportar un bundle. No se probaron dos PCs físicos. |
| SAVE-07 a SAVE-09 | Fallo inyectado entre subida y commit real, persistencia y reintento; journal de restauración y rechazo de contenido alterado/traversal con fixtures. Pendiente corte de socket a mitad de transferencia. |
| SAVE-10 y SAVE-11 | Límites de bucket y tamaño aplicados. Falta agotar cuota en staging. Un auto-update real aislado preservó el guardado y las preferencias del fixture, sin sesión de cuenta durante ese caso. |
| MOD-01 a MOD-06 | Catálogo real de tres referencias sintéticas, CAS y RLS; pruebas existentes de reconstrucción y Ready pasan. Pendiente instalar desde un catálogo cloud con consentimiento y proveedor real. |
| PROFILE-02 | Mii en juego pendiente. Se localizaron los formatos y la integración de importación de los ports sin tocar RFL. |
| REG-01 a REG-03 | CTest de núcleo/updater/mods y pruebas de cuenta pasaron. Compilación Release Windows, pantalla de invitado, arranque real de MP9 en fixture y 9 comprobaciones E2E del updater. |

## Consideraciones antes de distribuir

- La sincronización está operativa en pruebas locales para juego sin conexión con NAND de cuenta activa. La importación de guardados compartidos antiguos necesita un flujo explícito, copia verificada y prueba por port.
- PostgreSQL limita las revisiones registradas por cuenta y Storage limita el tamaño de cada objeto. Todavía falta medir cuotas reales del proyecto y diseñar una limpieza autorizada de objetos huérfanos.
- Las copias de catálogo muestran qué referencias faltan. La descarga y la activación siguen siendo acciones separadas del gestor local.
- El backup de restauración local conserva la revisión anterior. No hay política de borrado automático de copias.
