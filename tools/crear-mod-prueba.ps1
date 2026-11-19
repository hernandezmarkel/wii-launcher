# Crea en local el mod de prueba de Wii Party (prueba A/B de docs/mods/06-pruebas_resultados.md).
# Parte del mess.bin del disco extraído DEL PROPIO USUARIO y cambia «Gira mundial» (botón del menú principal)
# por «MOD FUNCIONA» (misma longitud, UTF-16BE; todas las apariciones). El resultado contiene datos del juego:
# no se sube al repositorio ni se distribuye.
#   -Grande: además añade 64 KiB de relleno al final, para probar la recolocación de archivos en la FST.
param(
    [Parameter(Mandatory)] [string] $Salida,
    [string] $Juego = (Join-Path $env:LOCALAPPDATA 'WiiPartyOnline\games\wiiparty\extracted'),
    [string] $Buscar = 'Gira mundial',
    [string] $Texto = 'MOD FUNCIONA',
    [switch] $Grande
)
$ErrorActionPreference = 'Stop'
$origen = Join-Path $Juego 'files\locale\sp_EU\mess.bin'
$bytes = [IO.File]::ReadAllBytes($origen)
$enc = [Text.Encoding]::BigEndianUnicode
$patron = $enc.GetBytes($Buscar)
$nuevo = $enc.GetBytes($Texto)
if ($nuevo.Length -ne $patron.Length) { throw "El texto nuevo debe tener la misma longitud que el original" }
$pos = @()
for ($i = 0; $i -le $bytes.Length - $patron.Length; $i += 2) {
    $ok = $true
    for ($j = 0; $j -lt $patron.Length; $j++) { if ($bytes[$i + $j] -ne $patron[$j]) { $ok = $false; break } }
    if ($ok) { $pos += $i; [Array]::Copy($nuevo, 0, $bytes, $i, $nuevo.Length) }
}
if ($pos.Count -eq 0) { throw "No se encuentra el texto en $origen (¿otro idioma o región?)" }
if ($Grande) { $bytes = $bytes + (New-Object byte[] 65536) }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("wl-mod-prueba-" + [guid]::NewGuid())
$dest = Join-Path $tmp 'PruebaTexto\files\locale\sp_EU'
New-Item -ItemType Directory -Force $dest | Out-Null
[IO.File]::WriteAllBytes((Join-Path $dest 'mess.bin'), $bytes)
Set-Content -Encoding utf8 (Join-Path $tmp 'PruebaTexto\LEEME.txt') "Mod de prueba del launcher: cambia un texto del menú principal."
New-Item -ItemType Directory -Force (Split-Path $Salida) | Out-Null
if (Test-Path $Salida) { Remove-Item $Salida }
Compress-Archive -Path (Join-Path $tmp 'PruebaTexto') -DestinationPath $Salida
Remove-Item -Recurse -Force $tmp
"Creado $Salida ($($pos.Count) cambios, $($bytes.Length) bytes)"
