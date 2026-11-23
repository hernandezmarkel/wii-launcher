# 05 — Mods y salas online

Objetivo: que nadie juegue online con mods distintos, sin reescribir el netcode de cada juego.

## Dos piezas

1. **Gate en el anfitrión (sin tocar el protocolo):** el token de sala que ya se compara en el HELLO lleva la huella
   de los mods activos (`<token>+mods:<32 hex>`). Si no coincide, el anfitrión rechaza la conexión con el protocolo de
   siempre. Con perfil sin mods el token es idéntico al de antes (compatible con launchers sin gestor de mods, E03).
   Un cliente que manipule su interfaz no puede «forzar Ready»: ni siquiera entra (C13). Un launcher antiguo nunca
   entra en una sala con mods ni al revés (C24).
2. **Canal aparte para comparar antes de entrar:** el anfitrión publica su manifiesto en TCP, puerto de la sala + 10
   (`mods/src/lobby_mods.cpp`). Petición `MODS 1\n`, respuesta: el JSON y cierre. Solo responde; no acepta nada más.

## Manifiesto (`mods/src/session.cpp`)

Campos de `CONTRATOS_MODS_Y_SESIONES.md` §3: `protocol`, `protocol_version`, `lobby_id`, `manifest_revision`
(= época del perfil), `game_id`, `base_game_compatibility_id`, `mod_loader_version`, `compatibility_policy`,
`effective_mod_fingerprint`, `mods[]` (`id`, `name`, `version`, `source`, `source_id`,
`effective_content_sha256`, `config_sha256`, `load_order`, `choices`) y `capabilities`.
Validación del cliente: ≤ 64 KiB, profundidad 6, ≤ 200 mods, ids `[a-z0-9._-]`, hashes de 64 hex, fuentes
conocidas (`local`, `gamebanana`); cualquier otra (URL, `file://`, IP) se trata como «desconocida» y no se descarga.
Campos de más (p. ej. `download_url`) se ignoran.

## Comparación (estados tipados)

`EXACT_MATCH`, `MISSING`, `WRONG_VERSION`, `WRONG_HASH`, `DISABLED`, `EXTRA_ACTIVE`, `LOAD_ORDER`, `CONFIG_MISMATCH`,
`NO_AUTHORIZED_SOURCE`, `INCOMPATIBLE_GAME` (otro disco/región/loader: descargar mods no lo arregla).
«Compatible» solo si la huella efectiva local es idéntica a la del anfitrión (cero falsos positivos).

## Flujo del cliente (pantalla «Unirse» → tarjeta «Mods de la sala»)

1. «Comparar mods»: resumen («El anfitrión usa N mods. Te faltan X, Y son distintos…») y lista con motivo.
2. Si todo está instalado: «Usar los mods de la sala» crea el perfil temporal `session` (igual que el del anfitrión,
   opciones incluidas) y guarda a qué perfil volver.
3. Si falta algo con fuente GameBanana: «Buscar los que faltan» consulta fichas y tamaños sin descargar; «Descargar N
   (X MB)» pide confirmación; luego descarga, valida, instala y **comprueba que el contenido es exactamente el del
   anfitrión** (si GameBanana tiene otra versión, se desinstala y se avisa).
4. «Unirme automáticamente cuando todo esté listo» (desmarcado por defecto): solo entonces se une solo, y solo si la
   comparación dice «compatible» (C17/C18).
5. Al salir de la sala, o al cerrar la partida online, se vuelve al perfil habitual. Si el launcher se cerró a medias,
   se vuelve al abrirlo.
6. Antes de lanzar la partida online se recalcula la huella: si los mods cambiaron desde que se entró, no se abre el
   juego (D12).

## Límites conocidos

- Las salas de Wii Party / MP9 no tienen estado «pendiente de mods»: no se entra como «no listo» con mods distintos;
  se compara antes de unirse (modalidad «sin participante pendiente» de los contratos).
- Si el anfitrión cambia sus mods con la sala abierta, los que entren después no coincidirán y él mismo no podrá lanzar
  (falla cerrado); hay que cerrar y volver a abrir la sala.
- El canal de manifiesto usa otro puerto: si un cortafuegos lo bloquea, la comparación falla con un mensaje claro y el
  gate del HELLO sigue protegiendo.

## Continuación en beta.5

El manifiesto v1 puede incluir la revisión del port, ID del ZIP, MD5 del proveedor y SHA-256
del paquete. Las referencias antiguas siguen siendo legibles. Para recuperar una versión
antigua hace falta una variante inequívoca y el contenido debe coincidir antes de que el
almacén confirme la instalación. El anfitrión publica el manifiesto que corresponde a la
huella negociada al crear la sala.

Al pulsar «Unirse» se compara primero con el anfitrión. Si el canal de metadatos no responde,
el HELLO existente sigue aplicando su comprobación de juego y mods. Al terminar la descarga
asistida se comprueba que la sala sigue disponible y se activa el perfil temporal. «Listo»
y «Empezar» esperan a que un hilo de trabajo vuelva a verificar el perfil y los paquetes
activos. La compatibilidad del contenido no cambia el protocolo del port.
