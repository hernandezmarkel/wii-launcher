# Estado del gestor de mods

Actualizado el 23/11/2026 sobre la beta.5 publicada. La [auditoría](07-auditoria-beta5.md)
recoge el punto de partida y las [pruebas de esta rama](08-validacion-beta5.md) recogen los resultados.

| Juego | Estado actual |
|---|---|
| Wii Party | Sustitución de archivos y Riivolution de datos habilitados. Prueba A/B previa y descarga asistida verificada por loopback con GameBanana real. |
| Mario Party 9 | Sustitución de archivos y Riivolution de datos habilitados tras la prueba real en SSQE01, revisión `5a2b71d`. |
| Mario Kart Wii | El port WiiCompiled requiere un mecanismo de carga de mods. |
| Super Smash Bros. Brawl | Port previsto. |

La pestaña Mods ofrece importación, perfiles, activación, orden, opciones de Riivolution,
dependencias y desinstalación. Explorar consulta GameBanana. Actualizaciones conserva el
paquete anterior y la configuración del perfil. El host y el invitado acuerdan la huella
antes de entrar. La descarga asistida revalida los hashes y la sala, y activa un perfil
temporal. El contrato de cuenta exporta referencias por juego y perfil, y permite calcular
qué paquetes faltan en otro PC.

## Pendiente

- Una partida con dos equipos físicos y un mod que afecte al gameplay.
- Una actualización entre dos versiones reales disponibles del mismo ZIP de GameBanana.
  El escenario de actualización y recuperación se probó con un proveedor simulado.
- Conectar el contrato de cuenta a Supabase cuando el proyecto tenga login.
- Los formatos 7z y RAR se pueden importar descomprimidos como carpeta.

Las pruebas de esta rama utilizaron almacenes de mods en carpetas temporales. Los
repositorios de los ports y los perfiles habituales quedan fuera de esta rama.
