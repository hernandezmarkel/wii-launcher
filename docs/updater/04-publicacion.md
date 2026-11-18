# Publicar una versión del launcher

Un commit o un push **nunca** llega a los usuarios. Solo llega una Release de GitHub **publicada a mano** por el
propietario, creada por `release.yml` a partir de una etiqueta. Hay tres controles seguidos:

1. Empujar la etiqueta `vX.Y.Z` (o `vX.Y.Z-beta.N`): solo quien tiene permiso de escritura.
2. Aprobar el job «Firmar y crear la release» en el entorno `release`: es el único que ve la clave de firma.
3. Revisar el **borrador** y pulsar «Publish release». Hasta entonces ningún launcher lo ve.

## Preparación única (con autorización del propietario)

1. Crear el repo y subir el código (`docs/updater/01-repositorio.md`).
2. Settings → Environments → New environment `release` → *Required reviewers*: el propietario. *Deployment branches and
   tags*: solo etiquetas `v*`. **Crear el entorno antes de empujar la primera etiqueta** (si no existe, GitHub lo crea
   sin protección).
3. En ese entorno, *Environment secrets* → `WL_UPDATE_SIGNING_KEY` = contenido de
   `%USERPROFILE%\.wii-launcher-claves\produccion.key` (192 caracteres hex). Guardar además una copia offline de ese
   archivo (USB o gestor de contraseñas): sin ella no se pueden firmar más versiones para los launchers ya repartidos.
4. Settings → Rules → Rulesets: proteger etiquetas `v*` (nadie las borra ni las mueve) y la rama `main`.
5. Settings → Actions → General: *Workflow permissions* = «Read repository contents».

## Cada versión

```bash
git switch main && git pull
# 1. Subir la versión en CMakeLists.txt: project(wii_launcher VERSION 1.0.1 ...)
# 2. Añadir la sección "## [1.0.1] - AAAA-MM-DD" a CHANGELOG.md con viñetas "- " (son las Novedades del launcher)
git commit -am "Versión 1.0.1"
git push
# 3. Esperar al CI verde y probar el ZIP del artefacto de CI
git tag -a v1.0.1 -m "Launcher 1.0.1"
git push origin v1.0.1
```

Después: Actions → Release → aprobar → revisar el borrador (archivos, `SHA256SUMS.txt`, notas) → *Publish release*.
Para una beta: `project(... VERSION 1.1.0)` y etiqueta `v1.1.0-beta.1`; el workflow la crea como *pre-release* y solo
la ven quienes marquen «Recibir versiones beta». Para promocionar beta → estable se publica `v1.1.0` (nueva etiqueta y
nuevo build): nunca se reutilizan los archivos de la beta.

### Qué contiene la Release

| Archivo | Para qué |
|---|---|
| `update.txt` + `update.txt.sig` | manifiesto firmado que leen los launchers |
| `wii-launcher.exe`, `manifests.*.toml`, `fonts.*.ttf`, `LEEME.txt`, `LICENCIAS.txt`, `CHANGELOG.md` | archivos sueltos que baja el updater (solo los que cambian) |
| `WiiLauncher-X.Y.Z-actualizacion.zip` | solo el launcher: ZIP puente para quien ya tiene el paquete (≈3,6 MB) |
| `WiiPorts-X.Y.Z.zip` | **paquete completo para empezar de cero**: launcher + instaladores de los juegos + servidor de Mario Kart LAN (≈400 MB) |

Los instaladores no están en el repo: viven en una release aparte (`instaladores-AAAA-MM-DD`, pre-release y sin «v», así
que el updater la ignora) fijada por hash en `packaging/instaladores.lock`. Para cambiarlos: preparar la carpeta sin
`pgdata`, `pgpass.local`, `pg.log`, `run`, `wfc-server/config.xml`, `logs`, `state` ni carpetas `obj`; comprimirla;
publicarla como nueva pre-release `instaladores-<fecha>`; y actualizar `tag`, `asset` y `sha256` en el `.lock`. El updater
nunca descarga ni actualiza instaladores: solo viajan en el ZIP completo.
| `SHA256SUMS.txt` | sumas para comprobar a mano |

Nunca: instaladores de juegos, `servidor-mkw`, discos, claves, `prefs.ini` ni registros (`tools/comprobar-publicable.sh`
lo comprueba en CI y en release; `empaquetar.sh` solo copia la lista cerrada).

### Qué garantiza cada firma

- **Firma ECDSA del manifiesto** (nuestra): el launcher solo instala archivos cuyo hash está en un manifiesto firmado con la
  clave privada del propietario. Protege aunque GitHub o la cuenta se vean comprometidos, mientras la clave no se filtre.
- **Attestation de GitHub** (`attest-build-provenance`): prueba qué workflow, commit y etiqueta produjeron cada archivo.
  Se comprueba con `gh attestation verify <archivo> --repo <dueño>/wii-launcher`. El launcher no la usa.
- **Authenticode** (Windows): no hay certificado. SmartScreen puede avisar al abrir un exe bajado a mano (no al
  actualizar). Opción gratuita para código abierto: SignPath Foundation (pendiente de solicitar).

## Detener una versión defectuosa

1. Si sigue en borrador: borrarla. Nadie la ha visto.
2. Si ya está publicada: **no** se borran ni se cambian sus archivos. Se publica enseguida `X.Y.(Z+1)` con la corrección
   (o con el código de la versión buena anterior). Los launchers nunca bajan de versión solos.
3. Mientras tanto, se puede marcar como *pre-release* para que `latest` vuelva a la anterior: los que aún no actualizaron
   dejan de verla; los que ya la tienen esperan a la corrección.
4. Si el fallo impide arrancar, el propio updater ya habrá vuelto a la versión anterior en cada PC (comprobación de salud).

## Límites y costes (plan gratuito)

- Repo público: minutos de Actions gratis en runners estándar. Un build del launcher tarda unos minutos y **no compila
  ningún juego**. Repo privado: los minutos de Windows cuentan con multiplicador sobre la cuota gratuita.
- Assets de Release: máximo 2 GiB por archivo; los nuestros pesan ~10 MB.
- `releases/latest/download/...` no consume cuota de la API REST. La API (solo canal beta) permite 60 peticiones/hora
  por IP sin token; el launcher usa ETag y espera al menos 15 min tras un 403/429.

## Staging local (sin GitHub)

`tests/e2e/staging-e2e.ps1` monta un feed en `http://127.0.0.1` con la misma estructura (`latest/update.txt`,
`download/vX.Y.Z/<asset>`, `releases.json`) y prueba la actualización real y los fallos. Solo funciona con exes
compilados con `-DWL_UPDATER_TESTING=ON`; el release se niega a publicar uno así.
