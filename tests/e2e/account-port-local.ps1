param(
    [string]$SourceInstall = (Join-Path $env:LOCALAPPDATA 'MarioParty9Online'),
    [string]$BuildDirectory = 'build-account'
)

$ErrorActionPreference = 'Stop'
$fixture = Join-Path $env:TEMP ('wl-account-port-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'bin'), (Join-Path $fixture 'games/mp9') -Force
$null = New-Item -ItemType File -Path (Join-Path $fixture '.account-port-fixture')
foreach ($name in 'mp9.exe', 'SDL3.dll') {
    $source = Join-Path $SourceInstall "bin/$name"
    $destination = Join-Path $fixture "bin/$name"
    try { $null = New-Item -ItemType HardLink -Path $destination -Target $source }
    catch { Copy-Item -LiteralPath $source -Destination $destination }
}
Copy-Item -LiteralPath (Join-Path $SourceInstall '.launcher-revision') -Destination $fixture
$null = New-Item -ItemType Junction -Path (Join-Path $fixture 'games/mp9/extracted') -Target (Join-Path $SourceInstall 'games/mp9/extracted')
# El emulador abre los datos extraídos para lectura. Ejecutable, ajustes, NAND y logs se usan desde el fixture.
$env:WP_NET_SESSION = $null
& (Join-Path $BuildDirectory 'wl_account_tests.exe') --port-smoke $fixture
exit $LASTEXITCODE
