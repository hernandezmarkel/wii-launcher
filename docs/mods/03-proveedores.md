# 03 — Proveedores

## GameBanana (implementado: `mods/src/gamebanana.cpp`)

Comprobado en vivo el 18/11/2026 (sin clave ni cuenta):

| Uso | Endpoint |
|---|---|
| Listado por juego (20 por página) | `https://gamebanana.com/apiv11/Mod/Index?_nPage=N&_nPerpage=20&_aFilters[Generic_Game]=ID` |
| Búsqueda | `https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=ID&_sSearchString=...&_nPage=N` |
| Ficha + archivos (tamaño, MD5, antivirus, versión, licencia) | `https://gamebanana.com/apiv11/Mod/<id>/ProfilePage` |
| Árbol del ZIP **sin descargarlo** | `https://api.gamebanana.com/Core/Item/Data?itemtype=File&itemid=<id>&fields=Metadata().aArchiveFilesList()` |
| Descarga | `https://gamebanana.com/dl/<id>` → 302 → `https://files.gamebanana.com/mods/<archivo>` |

IDs: Wii Party 6370, Mario Party 9 6248, Mario Kart Wii 5896, Brawl 5678.

Política del cliente:
- Solo HTTPS al puerto 443 de `gamebanana.com`, `api.gamebanana.com` y `files.gamebanana.com`; se comprueba en
  **cada salto** de redirección (D11). Sin credenciales en la URL.
- ≥ 350 ms entre peticiones; reintentos con espera ante 429/5xx/sin red; caché de 10 min y uso de la caché antigua
  sin conexión (B05/E02). User-Agent identificable del launcher, sin datos del usuario.
- Respuestas tratadas como no confiables: JSON con límites, registros incompletos o de otro juego descartados,
  URLs ajenas descartadas, HTML de licencia/descripción convertido a texto (B12).
- Antes de ofrecer «Descargar» se hace `quick_check` con el árbol: rechaza Gecko, REL/DOL, ejecutables, imágenes de
  disco, texturas de Dolphin y paquetes cuyo contenido no está en el disco del usuario. Los Riivolution quedan «se
  comprueba al descargar». Los .7z/.rar se remiten a la página (no hay extractor integrado).
- Descarga solo tras consentimiento explícito en la UI, con el tamaño anunciado como tope, a
  `mods/<juego>/cache/downloads`; comprobación de tamaño y MD5 del proveedor; después, el mismo análisis completo y
  la misma revisión que una importación manual. El MD5 detecta corrupción; no prueba autenticidad.
- Licencias: se muestran; la mayoría de mods de estos juegos son CC BY-NC-ND (uso personal sí, redistribuir no). Por
  eso el anfitrión de una sala nunca reparte archivos.

Sondeo (solo metadatos, 18/11/2026) de los 39 mods de Wii Party: 11 de datos, el resto con REL, Gecko, imágenes de
disco o ejecutables. Ejemplos reales con la CLI (`wlmods wiiparty gb-check <id>`): «Mr. (Ant) Tenna over Party
Phil» → compatible (26 archivos); «Blackjack Island» → incompatible (trae `mr001.rel.lz`); «Globe Trot - Resting
Status Idle Fix» → incompatible (Gecko); «ISO Patcher Dii Party» → incompatible (`BUILD.bat`).

Métricas (portátil, en batería): listado 780 ms sin caché, 10 ms con caché; búsqueda 258 ms.

## Game Jolt

Su Game API (trofeos, puntuaciones, sesiones, almacén de datos) no ofrece un catálogo de mods ni descargas de
terceros. No se integra; no se inventan endpoints.

## Nexus Mods

Requiere clave de API y no tiene páginas para estos juegos de Wii. Fuera del MVP.

## Local

ZIP o carpeta elegidos por el usuario (o arrastrados). Funciona sin internet.
