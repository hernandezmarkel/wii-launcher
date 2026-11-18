# Seguridad y recuperación del updater

Complementa al ADR (`02-adr-arquitectura.md`), que tiene el modelo de amenazas completo.

## La clave de firma

- Privada: `%USERPROFILE%\.wii-launcher-claves\produccion.key` en el PC del propietario (fuera de cualquier repo) y, una
  vez autorizado, el secreto `WL_UPDATE_SIGNING_KEY` del **entorno** `release` de GitHub. Hace falta una copia offline.
- Pública: compilada en el exe (`WL_UPDATE_PUBKEY` en `CMakeLists.txt`), también en `produccion.pub`.
- Generada el 18/11/2026 con `wl_release keygen` (CNG, ECDSA P-256). La clave de pruebas de staging es otra y nunca va
  en un build de release.
- `tools/comprobar-publicable.sh` rechaza cualquier archivo rastreado con aspecto de clave privada (192 hex), PEM o token.

### Si se pierde la clave privada

Los launchers ya repartidos solo aceptan esa clave: no podrán recibir más actualizaciones automáticas. Habrá que
repartir a mano (una vez) una versión con clave nueva, igual que la migración inicial. Por eso la copia offline.

### Si se filtra la clave privada (rotación)

1. Generar una clave nueva (`wl_release keygen nueva.key`).
2. Publicar cuanto antes una versión firmada **con la clave vieja** cuyo exe ya trae la **clave pública nueva**.
3. Cambiar el secreto de GitHub por la nueva y borrar la vieja.
4. Un atacante con la clave vieja podría publicar una versión falsa a quien no haya actualizado todavía: por eso el paso 2
   corre prisa, y además necesitaría poder publicar en el repo (cuenta o CDN comprometidos).

## Qué se rechaza (y dónde está probado)

| Ataque o fallo | Defensa | Prueba |
|---|---|---|
| Exe con un byte cambiado | SHA-256 del manifiesto firmado | SEC-01 (unit + e2e) |
| Manifiesto alterado / firmado con otra clave | ECDSA con clave anclada | SEC-02, SEC-08 |
| Versión antigua firmada (replay), downgrade | versión > instalada y ≥ mayor vista | SEC-03, UPD-15 |
| Beta colada en estable | canal firmado + coherencia con SemVer | UPD-14 |
| Otra plataforma | `platform` firmado | SEC-04 |
| `../`, rutas absolutas, `\`, instaladores, servidor | lista cerrada de rutas | SEC-05 |
| Archivo enorme | límites 64 MB/archivo, 128 MB total, corte durante la descarga | SEC-06, NET-07 |
| Comandos de postinstalación | el formato no los admite (clave desconocida = rechazo) | SEC-07 |
| HTTP plano, otro dominio, redirección maliciosa | WinHTTP sin redirección automática + lista de hosts HTTPS | NET-09 |

## Recuperación

- **Corte durante la descarga:** los archivos van a `.part` y solo se renombran tras comprobar el hash; la instalación
  no se toca hasta que todo está verificado.
- **Corte durante la instalación:** el diario `%LOCALAPPDATA%\WiiLauncher\updates\journal.txt` dice qué se movió. El
  siguiente arranque del launcher lo deshace antes de abrir la ventana (INST-03).
- **La versión nueva no arranca:** el aplicador espera su señal de salud 60 s; si no llega, la cierra y restaura (INST-04).
- **Disco lleno, sin permisos, antivirus:** se comprueba antes de tocar nada; si un movimiento falla a mitad, rollback (INST-01).
- **A mano:** `.wl-backup\<versión anterior>\` en la carpeta del launcher conserva la última versión sustituida.

## Registro

`%LOCALAPPDATA%\WiiLauncher\updater.log`, rotado a 256 KB, sin red ni telemetría. Sustituye `%USERPROFILE%` y el
nombre de usuario. Contiene: comprobaciones (resultado y tiempo), descargas, rechazos con motivo y pasos del aplicador.
