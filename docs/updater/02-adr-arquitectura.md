# ADR-U01: arquitectura de la autoactualización del launcher

Estado: **aceptada** (18/11/2026). Ámbito: solo el launcher (`wii-launcher.exe`, `manifests/`, `fonts/`, textos).
Los juegos, sus instaladores y `servidor-mkw/` quedan fuera (serán otro proyecto, versionado aparte).

## Contexto

- C++17 + SDL3 + ImGui, compilado con MinGW, enlazado estático, distribuido como **ZIP portable** (auditoría §1 y §3).
- Lo que cambia con el launcher pesa ~10 MB; el paquete completo pesa 424 MB por los instaladores de juegos.
- Se juega en clase: muchos PCs salen a Internet por **la misma IP**. La API REST de GitHub sin autenticar permite 60
  peticiones/hora **por IP**, así que no puede ser la vía normal de comprobación.
- No hay certificado Authenticode. No se puede pedir elevación (UAC) para actualizar.

## Opciones evaluadas

| Opción | Encaje | Motivo |
|---|---|---|
| **Velopack** (bootstrapper + `Update.exe`, C/C++ disponible) | parcial | Obliga a pasar de ZIP portable a instalación propia en `%LOCALAPPDATA%\<app>` con su `Update.exe` y a empaquetar con `vpk` (.NET). Su biblioteca C++ se distribuye pensando en MSVC; aquí se compila con MinGW. Su autenticidad descansa en HTTPS y en la firma Authenticode opcional, que no tenemos: sin certificado no hay clave anclada que proteja ante un feed comprometido (regla 5 del prompt). Coste de migración alto para ~10 MB. |
| **WinSparkle** (C, appcast con firma EdDSA) | parcial | Sí ancla una clave EdDSA, pero espera un instalador `.exe/.msi` por versión y trae su propia UI (no ImGui, no navegable con mando). Añade una DLL y un instalador que hoy no existen. |
| **MSIX / App Installer** | no | Requiere firma de código de confianza en cada PC y cambia por completo la instalación. |
| Tauri / electron-updater | no aplica | Otro stack. Cambiar de framework no se justifica. |
| **Updater propio mínimo con piezas del sistema** | **sí (elegida)** | Ver abajo. |

## Decisión

Updater propio y pequeño, construido sobre **APIs de Windows ya presentes** (sin dependencias nuevas):

- **Transporte:** WinHTTP (TLS y validación de certificados del sistema). Redirecciones **manuales**: cada salto se valida
  contra una lista de hosts HTTPS permitidos.
- **Autenticidad:** manifiesto de versión **firmado con ECDSA P-256 / SHA-256**. La **clave pública va compilada** en el exe.
  Verificación con CNG (`BCryptVerifySignature`). La clave privada solo existe fuera del repo (secreto de GitHub Actions y
  copia offline del propietario).
- **Integridad:** cada archivo del paquete figura en el manifiesto firmado con **tamaño y SHA-256** (CNG). Nada se instala si
  no coincide byte a byte.
- **Sin archivos comprimidos:** la actualización se publica como **archivos sueltos** en la Release. Así desaparecen las
  clases de fallo *zip slip* y *archive bomb*, y se gana **actualización diferencial por archivo** (solo se descarga lo que
  cambió; normalmente el exe).
- **Instalación fuera del proceso:** una copia del **exe actual** (versión conocida y sana) hace de aplicador
  (`--apply-update`). Espera a que el launcher se cierre, aparta los archivos viejos a una copia de seguridad, coloca los
  nuevos con renombrado atómico, arranca la versión nueva y **espera su señal de salud**. Si no llega, deshace.
- **Sin ejecución remota:** el manifiesto no contiene comandos; no se ejecuta nada descargado salvo el nuevo
  `wii-launcher.exe`, y solo después de verificar firma y hash.

### Por qué ECDSA de CNG y no Ed25519

Ed25519 sería preferible en abstracto, pero no está en CNG en todas las versiones de Windows 10 que pueden tener los
amigos y obligaría a incluir una biblioteca criptográfica de terceros. ECDSA P-256 está en CNG desde Windows Vista, con
nonce aleatorio gestionado por el sistema. La herramienta `wl_sign` (en este repo) usa la misma CNG para generar claves y
firmar, así que no hace falta OpenSSL ni Python.

## Componentes (en `updater/`)

| Contrato del prompt | Implementación | Responsabilidad |
|---|---|---|
| `VersionProvider` | `version.h` (`wl::kVersion` generado por CMake) + `semver.cpp` | versión instalada, comparación SemVer, canal |
| `ReleaseFeed` | `feed.cpp` | URL del manifiesto por canal, caché, ETag, backoff 403/429 |
| `UpdateVerifier` | `update_manifest.cpp` + `crypto_win.cpp` | formato, firma, app/canal/plataforma, anti-downgrade, rutas permitidas |
| `UpdateDownloader` | `downloader.cpp` sobre `http.h` (WinHTTP o falso en tests) | descarga a staging, límites, progreso, cancelación, reintentos |
| `UpdateInstaller` | `apply.cpp` | aplicador, copia de seguridad, diario, salud, rollback, recuperación al arrancar |
| `UpdateUI` | `app/update_ui.cpp` | estados `idle, checking, available, downloading, verifying, ready, installing, success, error` |
| `UpdateTelemetry` | `log.cpp` | `updater.log` local, sin rutas personales, rotado a 256 KB; no se envía nada |

El núcleo del updater no depende de SDL ni ImGui: se prueba con `wl_tests` usando un transporte HTTP falso.

## Formato del manifiesto firmado (`update.txt` + `update.txt.sig`)

Texto UTF-8, una clave por línea (`clave = valor`), sin comillas. La firma cubre **los bytes exactos** del archivo.

```text
format = 1
app = wii-launcher
channel = stable            # stable | beta
version = 1.0.1             # SemVer; la Release se llama v1.0.1
platform = windows-x86_64
published = 2026-11-20T18:00:00Z
min_from = 1.0.0            # versión mínima desde la que se puede saltar (opcional)
note = Mejoras de interfaz y correcciones de estabilidad.
note = (una línea por punto de las novedades)
file = <sha256 hex> <bytes> <nombre del asset> <ruta relativa>
file = ...
```

Reglas del verificador (todas obligatorias, cualquier fallo = rechazo sin instalar):

1. `update.txt` ≤ 64 KB, firma DER ECDSA ≤ 80 bytes, firma válida con la clave anclada.
2. `format == 1`, `app == wii-launcher`, `platform == windows-x86_64`, `channel` igual al pedido (beta solo si el usuario
   lo eligió; stable nunca acepta un manifiesto beta).
3. `version` > versión instalada **y** ≥ la mayor versión vista y verificada antes (anti-replay/downgrade, guardado en prefs).
4. Rutas: solo `wii-launcher.exe`, `manifests/<nombre>.toml`, `fonts/<nombre>.ttf`, `LEEME.txt`, `LICENCIAS.txt`,
   `CHANGELOG.md`. Sin `..`, sin raíz, sin `:`, sin `\`. Nombre de asset: `[A-Za-z0-9._-]{1,100}`.
5. Tamaño por archivo ≤ 64 MB y total ≤ 128 MB. `wii-launcher.exe` obligatorio.

## Origen de confianza y red

- Repositorio fijado en compilación (`WL_UPDATE_REPO`, p. ej. `hernandezmarkel/wii-launcher`).
- **Stable:** `https://github.com/<repo>/releases/latest/download/update.txt` (y `.sig`). `latest` nunca devuelve
  pre-releases ni borradores y **no consume la cuota de la API REST**.
- **Beta (opt-in):** `https://api.github.com/repos/<repo>/releases?per_page=10` con `ETag`/`If-None-Match` (un 304 no gasta
  cuota), se elige la versión más alta (beta o estable) y se baja su `update.txt`.
- Archivos: `https://github.com/<repo>/releases/download/v<version>/<asset>` (la versión y el asset salen del manifiesto firmado).
- Hosts permitidos (solo HTTPS, puerto 443): `github.com`, `api.github.com`, `objects.githubusercontent.com`,
  `release-assets.githubusercontent.com`. Máximo 5 redirecciones.
- Frecuencia: comprobación automática como mucho **una vez cada 6 h** (y nunca más de una por arranque). Manual: cuando el
  usuario pulse, con un mínimo de 30 s entre pulsaciones. 403/429: respeta `Retry-After`/`X-RateLimit-Reset`, mínimo 15 min.
- Timeouts: resolver/conectar 10 s, envío 10 s, recepción 30 s. Reintentos de descarga: 3 con espera 1 s, 4 s, 10 s.
- Builds de prueba (`-DWL_UPDATER_TESTING=ON`) aceptan además `WL_UPDATE_FEED=http://127.0.0.1:<puerto>/` para el feed de
  staging local. El workflow de release **falla** si esa opción está activada.

## Instalación, recuperación y datos

Archivos de trabajo en `%LOCALAPPDATA%\WiiLauncher\updates\` (nunca dentro de los juegos):

1. Descarga cada archivo cambiado a `updates\<versión>\staging\<asset>.part`, comprueba tamaño y SHA-256 y lo renombra.
   Los archivos cuyo hash ya coincide con el instalado no se descargan.
2. «Reiniciar y actualizar»: solo si no hay ningún juego lanzado desde el launcher en marcha y no hay otra ventana del
   launcher abierta. Copia el exe actual a `updates\aplicador.exe` y lo lanza con
   `--apply-update <staging> --target <carpeta> --wait-pid <pid>`; el launcher se cierra.
3. El aplicador toma `updates\.lock` (exclusivo), espera al proceso, comprueba que la carpeta es escribible y que hay
   espacio, escribe el **diario** `updates\journal.txt` y por cada archivo: mueve el viejo a
   `<carpeta>\.wl-backup\<versión vieja>\` y renombra el nuevo a su sitio (mismo volumen = atómico).
4. Arranca el exe nuevo con `--post-update`. El exe nuevo escribe `health = ok` en el diario tras su primer fotograma.
   Si en 60 s no hay señal o el proceso muere, el aplicador **restaura** la copia y arranca la versión vieja con aviso.
5. Al arrancar, cualquier launcher que encuentre un diario a medias (corte de luz, cierre forzado) termina el rollback
   antes de mostrar la ventana.

Nunca se toca `%LOCALAPPDATA%\WiiLauncher\prefs.ini`, las carpetas de juegos, `instaladores/` ni `servidor-mkw/`.
Se conserva la última copia de seguridad (`.wl-backup`) para volver atrás a mano.

Carpeta sin permiso de escritura (p. ej. `Program Files`): el launcher lo detecta antes de descargar y explica cómo
moverlo. **No pide elevación.**

## Canales, versionado y publicación

- SemVer `vMAJOR.MINOR.PATCH`; pre-release `-beta.N` para el canal beta. La versión vive en `CMakeLists.txt`
  (`project(... VERSION x.y.z)`) y el workflow de release comprueba que coincide con la etiqueta.
- Un commit **no** publica nada: solo una etiqueta `v*` aprobada (con entorno protegido `release`) dispara la release.
- Stable = Release normal (marcada *latest*). Beta = *pre-release*: nunca la ve un cliente en stable.
- Retirar una versión defectuosa: publicar `x.y.(z+1)` que corrige (los clientes nunca bajan de versión automáticamente).

## Modelo de amenazas (resumen)

| Amenaza | Defensa |
|---|---|
| Feed o Release alterados (cuenta o CDN comprometidos) | firma con clave anclada; sin la clave privada no hay manifiesto válido |
| Archivo alterado o truncado | SHA-256 + tamaño del manifiesto firmado |
| Replay de una versión antigua firmada | anti-downgrade + mayor versión vista |
| Cruce de canal o plataforma | `channel`/`platform` firmados |
| Redirección a otro dominio / HTTP plano | redirecciones manuales con lista de hosts HTTPS |
| Path traversal / archivos fuera de destino | lista cerrada de rutas permitidas |
| Payload gigante | límites por archivo y totales antes y durante la descarga |
| Corte, disco lleno, antivirus | staging + diario + copia + rollback; la versión vieja sigue arrancando |
| Robo de la clave privada | rotación: nueva release firmada con la vieja que trae la nueva clave pública; ver `seguridad-y-recuperacion.md` |
| Retener actualizaciones (freeze) | no se defiende (no hay expiración); el riesgo es quedarse en una versión vieja, no instalar algo falso |

Qué **no** cubre: la firma ECDSA autentica el paquete ante el updater, pero no es firma Authenticode; Windows SmartScreen
puede avisar al abrir un exe nuevo descargado a mano. La *attestation* de GitHub (provenance) demuestra qué workflow y
commit produjeron cada archivo, pero el cliente no la comprueba: es para auditoría.

## Migración

La versión repartida en clase (`v0.1.0-clase`) no tiene updater: **una única** actualización manual. Se publica un ZIP
pequeño `WiiLauncher-<versión>-actualizacion.zip` (exe + manifests + fuentes + textos) que se descomprime encima de la
carpeta `WiiPorts-clase` existente. Preferencias, juegos e instaladores se conservan porque no están en ese ZIP o viven en
`%LOCALAPPDATA%`. Desde esa versión, todo es automático.
