<#
Prepara el ZIP de instaladores que va dentro de WiiPorts-<versión>.zip (ver packaging/instaladores.lock).
Copia en solo lectura desde las carpetas de origen, quita datos privados y lo que no se usa para instalar, y comprime.

  powershell -ExecutionPolicy Bypass -File packaging\preparar-instaladores.ps1 -Clase <dist\WiiPorts-clase> `
      -MkwPaquete <mkwii-recomp\lan\paquete> -Salida <carpeta> -Fecha 2026-11-19

Qué se quita y por qué (todo lo demás se copia tal cual):
- Servidor de Mario Kart: pgdata, pgpass.local, pg.log, run, wfc-server\config.xml, logs, state (datos y contraseñas
  locales; Iniciar-ServidorLAN.ps1 los crea en el primer arranque).
- Carpetas obj de compilaciones .NET (llevan rutas del PC donde se compilaron).
- llvm-mingw: sysroots y runtimes de aarch64, arm64ec, armv7 e i686 (el instalador solo compila x86_64-w64-mingw32),
  Python embebido, lldb (depurador), clangd y clang-tidy (herramientas de editor).
- CMake: documentación (doc, man).
- PostgreSQL: StackBuilder y sus DLL wx, bibliotecas estáticas (.lib, .a) y programas que el servidor no usa.
  Se conservan postgres, initdb, pg_ctl, psql, todas las demás DLL, share y las licencias.
#>
param(
    [Parameter(Mandatory)] [string]$Clase,
    [Parameter(Mandatory)] [string]$MkwPaquete,
    [Parameter(Mandatory)] [string]$Salida,
    [Parameter(Mandatory)] [string]$Fecha
)
$ErrorActionPreference = 'Stop'
$st = Join-Path $Salida "instaladores-$Fecha"
if (Test-Path $st) { Remove-Item -Recurse -Force $st }
New-Item -ItemType Directory -Force "$st\instaladores" | Out-Null
function Size($p) { (Get-ChildItem $p -Recurse -File -Force | Measure-Object Length -Sum).Sum }

# 1. Copia
Copy-Item "$Clase\instaladores\mp9online-launcher.exe", "$Clase\instaladores\wiipartyrecomp-launcher.exe" "$st\instaladores\"
Copy-Item -Recurse "$Clase\instaladores\licencias" "$st\instaladores\licencias"
robocopy "$MkwPaquete\instalador" "$st\instaladores\mkwii" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 /XD obj | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy instalador: $LASTEXITCODE" }
robocopy "$MkwPaquete\servidor" "$st\servidor-mkw" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 /XD pgdata run logs state obj /XF pgpass.local pg.log config.xml | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy servidor: $LASTEXITCODE" }
$antes = Size $st

# 2. llvm-mingw: solo x86_64
$llvm = "$st\instaladores\mkwii\toolchain\llvm-mingw"
foreach ($d in 'aarch64-w64-mingw32', 'arm64ec-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32', 'python') { Remove-Item -Recurse -Force "$llvm\$d" }
Get-ChildItem "$llvm\bin" -File | Where-Object {
    $_.Name -match '^(aarch64|arm64ec|armv7|i686)-' -or $_.Name -match '^(clangd|clang-tidy|lldb.*|liblldb.*|libpython.*|python.*)\.(exe|dll)$'
} | Remove-Item -Force
Get-ChildItem "$llvm\lib\clang" -Recurse -File | Where-Object { $_.Name -match 'i386|i686|aarch64|arm64|armhf|-arm\.|armv7' } | Remove-Item -Force

# 3. CMake sin documentación
foreach ($d in 'doc', 'man') { Remove-Item -Recurse -Force "$st\instaladores\mkwii\toolchain\CMake\$d" }

# 4. PostgreSQL: lo que usa Iniciar-ServidorLAN.ps1
$pg = "$st\servidor-mkw\tools\pgsql"
$keepExe = 'postgres.exe', 'initdb.exe', 'pg_ctl.exe', 'psql.exe'
Get-ChildItem "$pg\bin" -File | Where-Object {
    ($_.Extension -eq '.exe' -and $keepExe -notcontains $_.Name) -or $_.Name -like 'wx*.dll'
} | Remove-Item -Force
Get-ChildItem "$pg\lib" -File | Where-Object { $_.Extension -in '.lib', '.a' } | Remove-Item -Force

# 5. Comprobaciones: nada privado, nada del juego, lo imprescindible presente
foreach ($bad in 'servidor-mkw\pgdata', 'servidor-mkw\pgpass.local', 'servidor-mkw\pg.log', 'servidor-mkw\run', 'servidor-mkw\wfc-server\config.xml') {
    if (Test-Path "$st\$bad") { throw "No debe ir: $bad" }
}
if (Get-ChildItem $st -Recurse -File -Include *.iso, *.wbfs, *.rvz, *.wia, *.dol, *.rel) { throw 'Hay archivos de disco o del juego.' }
if (Get-ChildItem $st -Recurse -Directory -Filter obj) { throw 'Quedan carpetas obj.' }
foreach ($need in 'instaladores\mkwii\Instalar-MKWii-LAN.ps1', 'instaladores\mkwii\toolchain\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe',
    'instaladores\mkwii\toolchain\llvm-mingw\x86_64-w64-mingw32\lib', 'instaladores\mkwii\toolchain\CMake\bin\cmake.exe',
    'instaladores\mkwii\toolchain\Translator\Translator.Cli.exe', 'instaladores\mkwii\toolchain\nod\nodtool.exe',
    'servidor-mkw\tools\pgsql\bin\initdb.exe', 'servidor-mkw\tools\pgsql\bin\pg_ctl.exe', 'servidor-mkw\tools\pgsql\bin\psql.exe',
    'servidor-mkw\tools\pgsql\bin\postgres.exe', 'servidor-mkw\wfc-server\LICENSE', 'instaladores\mkwii\src\LICENSE') {
    if (-not (Test-Path "$st\$need")) { throw "Falta $need" }
}
$despues = Size $st
"Copiado {0:N0} MB -> tras limpiar {1:N0} MB" -f ($antes / 1MB), ($despues / 1MB)

# 6. ZIP con rutas «/» (tar de Windows), compresión máxima de deflate
$zip = Join-Path $Salida "WiiPorts-instaladores-$Fecha.zip"
if (Test-Path $zip) { Remove-Item $zip }
Push-Location $st
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip --options zip:compression-level=9 instaladores servidor-mkw
Pop-Location
if ($LASTEXITCODE) { throw "tar: $LASTEXITCODE" }
"{0}: {1:N1} MB, sha256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), (Get-FileHash $zip).Hash.ToLower()
