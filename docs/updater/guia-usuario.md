# Actualizar el launcher (guía para jugadores)

## Una sola vez: pasar a la versión que se actualiza sola

Si tienes la carpeta `WiiPorts-clase` que se repartió en clase, esa versión todavía no sabe actualizarse. Haz esto una vez:

1. Cierra el launcher y cualquier juego.
2. Descarga `WiiLauncher-1.0.0-actualizacion.zip` (te pasaremos el enlace).
3. Descomprímelo y copia **lo que hay dentro** de la carpeta `WiiLauncher-1.0.0` encima de tu carpeta `WiiPorts-clase`.
   Windows preguntará si quieres reemplazar archivos: di que sí.
4. Abre `wii-launcher.exe`. Abajo a la izquierda pondrá «Launcher v1.0.0».

No pierdes nada: tus juegos instalados, partidas, nombre y ajustes están en otra carpeta (`%LOCALAPPDATA%`) y los
instaladores y el servidor de Mario Kart siguen donde estaban.

## A partir de ahí

- Al abrir el launcher, comprueba en segundo plano si hay versión nueva (como mucho cada 6 horas). Puedes jugar mientras.
- Si la hay, aparece un aviso abajo a la derecha: **Actualizar**, **Más tarde** o **Novedades**.
- «Actualizar» descarga solo lo que cambió (normalmente unos 9 MB). Luego pulsa **Reiniciar y actualizar**: el launcher se
  cierra y se vuelve a abrir en la versión nueva en unos segundos.
- Si tienes un juego abierto, espera a cerrarlo: el launcher no se reinicia a mitad de partida.
- En **Ajustes del launcher** (abajo a la izquierda) ves tu versión, puedes buscar actualizaciones a mano y leer las novedades.

## Preguntas

**¿Y si no tengo Internet?** El launcher funciona igual y puedes jugar. Dirá que no pudo comprobar; nada más.

**¿Qué pasa si se va la luz o se cuelga mientras actualiza?** Al volver a abrirlo deshace la actualización a medias y
vuelve a tu versión anterior. Te lo dirá con un mensaje. Si la versión nueva no arranca bien, también vuelve sola.

**Me dice que la carpeta es de solo lectura.** Has puesto el launcher en «Archivos de programa» o similar. Mueve la
carpeta entera al Escritorio o a Documentos. No hace falta ser administrador.

**Me dice que hay otra ventana del launcher abierta.** Ciérrala (mira también en la barra de tareas) y pulsa otra vez.

**¿Cómo vuelvo a la versión anterior a mano?** En la carpeta del launcher hay una carpeta oculta `.wl-backup\<versión>` con
los archivos anteriores. Cierra el launcher y cópialos encima. También puedes descargar el ZIP de una versión anterior
desde la página de Releases.

**¿Qué son las versiones beta?** Novedades en pruebas. Solo las recibes si marcas «Recibir versiones beta» en Ajustes del
launcher. Pueden fallar.

**¿Dónde aviso de un fallo?** En el grupo, o abriendo un *issue* en el repositorio. Adjunta
`%LOCALAPPDATA%\WiiLauncher\updater.log` (no contiene tu nombre de usuario ni rutas personales).
