# Entorno de compilación local (portátil de Markel): MinGW + CMake + Ninja del toolchain de MP9.
# Uso: . .\tools\entorno-mods.ps1   (luego cmake/ninja están en el PATH de esta consola)
$tc = Join-Path $env:LOCALAPPDATA 'MarioParty9Online\build\deps\toolchain'
$env:PATH = @(
    (Join-Path $tc 'mingw\MinGW\bin'),
    (Join-Path $tc 'cmake\cmake-4.4.3-windows-x86_64\bin'),
    (Join-Path $tc 'ninja'),
    $env:PATH
) -join ';'
# zlib (lector ZIP de los mods) viene dentro de ese MinGW.
$env:ZLIB_ROOT = Join-Path $tc 'mingw\MinGW'
