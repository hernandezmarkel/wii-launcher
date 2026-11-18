# Sala de Wii Party (copia literal)

`lobby.cpp`, `lobby.h`, `sha256.cpp` y `sha256.h` son copias exactas de `launcher/src/` del launcher
online de Wii Party (WP_LAUNCHER_BUILD 2611-029), sacadas de `wiiparty-online-launcher-src.zip`.
No se han modificado. Si cambian en el launcher original, se vuelven a copiar enteros.

| Archivo | sha256 |
|---|---|
| lobby.cpp | 4c939c28ba17cf43845a47b8ca355de8654d7d3d958f0224fcfc377732c2d130 |
| lobby.h | 7f5061917b83a972f8f2f42f2b95c89e8bdd3682811815b9ea4505934f7fed53 |
| sha256.cpp | 985e4aa02cabed1ba4e824bef465af6817029034d8ef96d68501c61a1b0580ff |
| sha256.h | 182685232661e693397841888c9a40a0f8520b5c6ed196e1afe625635de74a18 |

El adaptador (`../../wiiparty_adapter.cpp`) solo añade:

- `wake_main_loop()` vacío: la interfaz ya refresca cada 33 ms con una sala abierta.
- La identidad del HELLO: build `2611-029` y content = sha256(`.launcher-revision` + "\n" +
  `build/wiiparty/translation.stamp`)[0..16], que es lo mismo que calcula `App::online_content()`
  cuando la instalación está al día.
- El anfitrión queda "listo" automáticamente (en el original pulsaba Listo); lo confirma al pulsar Empezar.

Compatibilidad: quien use este launcher puede jugar con quien use el launcher antiguo 2611-029 con la
misma build del juego. Con otra versión del launcher antiguo, la sala rechaza la conexión ("otra versión
del launcher"), igual que hacía el original.
