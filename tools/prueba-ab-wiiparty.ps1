# Prueba A/B real de un mod en Wii Party (docs/mods/06-pruebas_resultados.md): lanza el juego con el perfil
# indicado mediante wlmods (el mismo núcleo que la interfaz), entra al menú principal con un guion de
# entrada (A+B en el título) y guarda fotogramas deterministas. Devuelve la carpeta de capturas.
param(
    [Parameter(Mandatory)] [string] $Capturas,
    [string] $Perfil = '',
    [int] $Segundos = 50,
    [string] $Wlmods = (Join-Path $PSScriptRoot '..\build-mods\wlmods.exe')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Capturas | Out-Null
$env:WL_MODS_EXPERIMENTAL = '1'                       # solo para hacer la prueba A/B de un juego aún no verificado
$env:WP_LOG_DISC = '1'
$env:WP_LOG_FILE = Join-Path $Capturas 'game.log'
$env:WP_INPUT_SCRIPT = '20000,21000,0C00;30000,30300,0C00'   # A+B en la pantalla de título
$env:WP_SAVE_FRAME = Join-Path $Capturas 'f_%03d.png'
$env:WP_SAVE_EVERY = '60'
$env:WP_DETERMINISTIC = '1700000000'
$args = @('wiiparty', 'launch', '--seconds', "$Segundos")
if ($Perfil) { $args += @('--profile', $Perfil) }
& $Wlmods @args
Remove-Item Env:WP_INPUT_SCRIPT, Env:WP_SAVE_FRAME, Env:WP_SAVE_EVERY, Env:WP_DETERMINISTIC, Env:WP_LOG_DISC, Env:WL_MODS_EXPERIMENTAL
$Capturas
