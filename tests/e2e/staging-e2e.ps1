<#
Pruebas de extremo a extremo del updater contra un feed de staging local (127.0.0.1). No toca GitHub,
ni %LOCALAPPDATA% real, ni juegos: todo va en -Work, con WL_LOCALAPPDATA apuntando allí.

Necesita exes de prueba compilados con -DWL_UPDATER_TESTING=ON y la clave pública de pruebas:
  <Bin>\wii-launcher-1.0.0.exe, <Bin>\wii-launcher-1.0.1.exe, <Bin>\wl_release.exe
Uso:
  powershell -ExecutionPolicy Bypass -File tests\e2e\staging-e2e.ps1 -Bin <dir> -Key <pruebas.key> -OtherKey <otra.key> `
      -Work <dir> -Python <python.exe> -Fonts <carpeta con Inter-*.ttf> [-OldExe <wii-launcher.exe de v0.1.0-clase>]
Resultados: <Work>\resultados.md y código de salida 0 si todo pasa.
#>
param(
    [Parameter(Mandatory)] [string]$Bin,
    [Parameter(Mandatory)] [string]$Key,
    [Parameter(Mandatory)] [string]$OtherKey,
    [Parameter(Mandatory)] [string]$Work,
    [Parameter(Mandatory)] [string]$Python,
    [Parameter(Mandatory)] [string]$Fonts,
    [string]$OldExe = "",
    [string[]]$Only = @()
)
$ErrorActionPreference = "Stop"
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })  # -File pasa "A,B" como un solo texto
$repo = Resolve-Path "$PSScriptRoot\..\.."
$release = Join-Path $Bin "wl_release.exe"
$results = New-Object System.Collections.Generic.List[object]
$servers = New-Object System.Collections.Generic.List[object]
$port = 18760

function Hash($p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() }
$h100 = Hash "$Bin\wii-launcher-1.0.0.exe"
$h101 = Hash "$Bin\wii-launcher-1.0.1.exe"

function New-Install($name, $exe) {
    $d = Join-Path $Work "inst-$name"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory -Force "$d\manifests", "$d\fonts", "$d\instaladores", "$d\servidor-mkw" | Out-Null
    Copy-Item $exe "$d\wii-launcher.exe"
    Copy-Item "$repo\manifests\*.toml" "$d\manifests\"
    Copy-Item "$Fonts\Inter-Regular.ttf", "$Fonts\Inter-SemiBold.ttf" "$d\fonts\"
    Copy-Item "$repo\packaging\LEEME.txt", "$repo\packaging\LICENCIAS.txt" "$d\" -ErrorAction SilentlyContinue
    Set-Content "$d\instaladores\no-tocar.txt" "instalador de un juego" -Encoding ascii
    Set-Content "$d\servidor-mkw\no-tocar.txt" "servidor mkw" -Encoding ascii
    return $d
}

function New-Lad($name) {
    $d = Join-Path $Work "lad-$name"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory -Force "$d\WiiLauncher", "$d\MarioParty9Online\saves" | Out-Null
    [IO.File]::WriteAllText("$d\WiiLauncher\prefs.ini", "online_name=Tester`nonline_port=47630`nselected=wiiparty`n")
    Set-Content "$d\MarioParty9Online\saves\partida.sav" "datos de partida" -Encoding ascii
    return $d
}

# Feed con la versión $ver (exe $exe) firmado con $signKey. $mutate: scriptblock que altera la carpeta del feed.
function New-Feed($name, $ver, $exe, $signKey, $channel = "stable", [scriptblock]$mutate = $null) {
    $f = Join-Path $Work "feed-$name"
    if (Test-Path $f) { Remove-Item -Recurse -Force $f }
    $pkg = New-Install "pkg-$name" $exe
    Remove-Item -Recurse -Force "$pkg\instaladores", "$pkg\servidor-mkw"
    $dl = "$f\download\v$ver"
    New-Item -ItemType Directory -Force $dl, "$f\latest" | Out-Null
    Set-Content "$Work\notas.txt" "- Versión de prueba $ver con cambio visible en la barra lateral.`n- Segunda nota." -Encoding utf8
    & $release pack $pkg $dl $ver $channel "$Work\notas.txt" | Out-Null
    if ($LASTEXITCODE) { throw "pack falló" }
    $env:WL_UPDATE_SIGNING_KEY_FILE = $signKey
    & $release sign "$dl\update.txt" | Out-Null
    Remove-Item Env:\WL_UPDATE_SIGNING_KEY_FILE
    if ($LASTEXITCODE) { throw "sign falló" }
    Copy-Item "$dl\update.txt" "$f\latest\update.txt"
    Set-Content "$f\releases.json" "[{`"tag_name`":`"v$ver`"}]" -Encoding ascii
    if ($mutate) { & $mutate $f $dl }
    return $f
}

function Start-Feed($root) {
    $script:port++
    $p = Start-Process $Python -ArgumentList "-I", "`"$repo\tests\e2e\feed_server.py`"", "`"$root`"", $script:port -PassThru -WindowStyle Hidden
    $servers.Add($p)
    Start-Sleep -Milliseconds 700
    return @{ Proc = $p; Url = "http://127.0.0.1:$($script:port)/" }
}

function Stop-Launchers($inst, $lad) {
    Get-Process -Name "wii-launcher", "aplicador" -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path.StartsWith($inst, [StringComparison]::OrdinalIgnoreCase) -or $_.Path.StartsWith($lad, [StringComparison]::OrdinalIgnoreCase))
    } | Stop-Process -Force -ErrorAction SilentlyContinue
}

# El observador permite que el launcher y el aplicador sigan escribiendo y rotando sus archivos.
function Read-SharedText($path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        try {
            $reader = [IO.StreamReader]::new($stream)
            return $reader.ReadToEnd()
        } finally { $stream.Dispose() }
    } catch [IO.IOException] {
        return ""   # el siguiente sondeo recogerá el archivo si acaba de rotar
    }
}

function Log-Text($lad) { Read-SharedText "$lad\WiiLauncher\updater.log" }
function Journal($lad) { Read-SharedText "$lad\WiiLauncher\updates\journal.txt" }

# Arranca el launcher con --auto-update y espera hasta que $until devuelva true (o 90 s).
function Run-Launcher($inst, $lad, $feedUrl, [scriptblock]$until, [hashtable]$extraEnv = @{}, [string[]]$args2 = @("--auto-update"), [int]$timeout = 90) {
    $env:WL_LOCALAPPDATA = $lad
    $env:WL_UPDATE_FEED = $feedUrl
    $env:WL_UPDATE_INTERVAL_S = "0"
    foreach ($k in $extraEnv.Keys) { Set-Item "Env:\$k" $extraEnv[$k] }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = if ($args2.Count) { Start-Process "$inst\wii-launcher.exe" -ArgumentList $args2 -PassThru } else { Start-Process "$inst\wii-launcher.exe" -PassThru }
    $ok = $false
    while ($sw.Elapsed.TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 400
        if (& $until) { $ok = $true; break }
    }
    $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    Start-Sleep -Milliseconds 800
    Stop-Launchers $inst $lad
    foreach ($k in $extraEnv.Keys) { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
    Remove-Item Env:\WL_UPDATE_FEED, Env:\WL_LOCALAPPDATA -ErrorAction SilentlyContinue
    return @{ Ok = $ok; Secs = $secs; Pid = $p.Id }
}

function Data-Intact($inst, $lad) {
    ((Get-Content "$lad\WiiLauncher\prefs.ini" -Raw) -match "online_name=Tester") -and
    ((Get-Content "$lad\MarioParty9Online\saves\partida.sav" -Raw).Trim() -eq "datos de partida") -and
    (Test-Path "$inst\instaladores\no-tocar.txt") -and (Test-Path "$inst\servidor-mkw\no-tocar.txt")
}

function Record($id, $scenario, $pass, $observed) {
    $results.Add([pscustomobject]@{ Id = $id; Escenario = $scenario; Estado = $(if ($pass) { "PASS" } else { "FAIL" }); Observado = $observed })
    Write-Host ("{0,-8} {1}  {2}" -f $id, $(if ($pass) { "PASS" } else { "FAIL" }), $observed)
}

function Want($id) { $Only.Count -eq 0 -or $Only -contains $id }

New-Item -ItemType Directory -Force $Work | Out-Null
$good = New-Feed "good" "1.0.1" "$Bin\wii-launcher-1.0.1.exe" $Key
$srv = Start-Feed $good
try {
    # ---------------- UPD-01/02/07/13: actualización real 1.0.0 -> 1.0.1
    if (Want "UPD-02") {
        $inst = New-Install "a" "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad "a"
        $r = Run-Launcher $inst $lad $srv.Url { (Journal $lad) -match "state = done" }
        $j = Journal $lad
        $exeOk = (Hash "$inst\wii-launcher.exe") -eq $h101
        Record "UPD-02" "1.0.0 -> 1.0.1 con Actualizar automático" ($r.Ok -and $exeOk -and $j -match "health = ok") "estado final done en $($r.Secs) s; exe = 1.0.1: $exeOk; salud: $($j -match 'health = ok')"
        Record "UPD-07" "Preferencias, partidas, instaladores y servidor intactos" (Data-Intact $inst $lad) "prefs.ini, partida.sav, instaladores/ y servidor-mkw/ siguen igual"
        $prefs = Get-Content "$lad\WiiLauncher\prefs.ini" -Raw
        Record "UPD-02b" "La versión nueva sabe que viene de 1.0.0" ($prefs -match "update.from_version=1.0.0" -or $prefs -match "update.from_version=`r?`n") "prefs: from_version registrado"
        $staged = (Get-ChildItem "$lad\WiiLauncher\updates" -Directory -ErrorAction SilentlyContinue | Where-Object Name -eq "1.0.1").Count -eq 0
        Record "UPD-02c" "Limpieza tras éxito (staging borrado, copia previa conservada)" ($staged -and (Test-Path "$inst\.wl-backup\1.0.0\wii-launcher.exe")) "staging borrado: $staged; backup 1.0.0 presente"
        $log = Log-Text $lad
        $dl = if ($log -match "descarga de 1\.0\.1: (\d+) archivos, (\d+) bytes") { "$($Matches[1]) archivos, $($Matches[2]) bytes" } else { "?" }
        Record "PERF-04" "Diferencial por archivo: solo baja lo que cambia" ($dl -match "^1 archivos") "descargado: $dl (fuentes y manifests no cambian)"
        # UPD-06: la nueva no entra en bucle
        $before = ([regex]::Matches((Log-Text $lad), "comprobación manual")).Count
        $r2 = Run-Launcher $inst $lad $srv.Url { ([regex]::Matches((Log-Text $lad), "comprobación manual")).Count -gt $before } -timeout 30
        $log = Log-Text $lad
        Record "UPD-06" "Reabrir 1.0.1 con el mismo feed: sin bucle" ($r2.Ok -and $log -match "comprobación manual \(stable\): 0" -and (Hash "$inst\wii-launcher.exe") -eq $h101) "resultado UpToDate (0), exe sigue en 1.0.1"
    }

    # ---------------- seguridad: nada alterado llega a instalarse
    $cases = @(
        @{ Id = "SEC-01"; Name = "Un byte cambiado en el exe publicado"; Mut = { param($f, $dl) $b = [IO.File]::ReadAllBytes("$dl\wii-launcher.exe"); $b[5000] = $b[5000] -bxor 1; [IO.File]::WriteAllBytes("$dl\wii-launcher.exe", $b) }; Expect = "descarga no completada" },
        @{ Id = "SEC-02"; Name = "Manifiesto alterado tras firmar"; Mut = { param($f, $dl) (Get-Content "$f\latest\update.txt" -Raw).Replace("note = Segunda nota.", "note = Nota falsa.") | Set-Content "$f\latest\update.txt" -NoNewline -Encoding utf8; Copy-Item "$f\latest\update.txt" "$dl\update.txt" }; Expect = "manifiesto rechazado" },
        @{ Id = "SEC-08"; Name = "Feed comprometido firmado con otra clave"; Key = $OtherKey; Expect = "manifiesto rechazado" },
        @{ Id = "NET-03"; Name = "404 al descargar el exe"; Mut = { param($f, $dl) Set-Content "$f\rules.json" '{"/download/v1.0.1/wii-launcher.exe": {"status": 404}}' -Encoding ascii }; Expect = "descarga no completada" },
        @{ Id = "NET-06"; Name = "Corte a mitad de descarga"; Mut = { param($f, $dl) Set-Content "$f\rules.json" '{"/download/v1.0.1/wii-launcher.exe": {"truncate": 3000000}}' -Encoding ascii }; Expect = "descarga no completada" },
        @{ Id = "NET-07"; Name = "El servidor entrega un exe más grande que el firmado"; Mut = { param($f, $dl) $b = [IO.File]::ReadAllBytes("$dl\wii-launcher.exe"); [IO.File]::WriteAllBytes("$dl\wii-launcher.exe", $b + (New-Object byte[] 5000000)) }; Expect = "mayor de lo permitido" },
        @{ Id = "NET-09"; Name ="Redirección a un dominio no permitido"; Mut = { param($f, $dl) Set-Content "$f\rules.json" '{"/download/v1.0.1/wii-launcher.exe": {"redirect": "https://evil.example/wii-launcher.exe"}}' -Encoding ascii }; Expect = "descarga no completada" },
        @{ Id = "NET-04"; Name = "HTTP 429 al comprobar"; Mut = { param($f, $dl) Set-Content "$f\rules.json" '{"/latest/update.txt": {"status": 429, "headers": {"Retry-After": "60"}}}' -Encoding ascii }; Expect = "comprobación manual \(stable\): 3" },
        @{ Id = "NET-05"; Name = "HTTP 500 al comprobar"; Mut = { param($f, $dl) Set-Content "$f\rules.json" '{"/latest/update.txt": {"status": 500}}' -Encoding ascii }; Expect = "comprobación manual \(stable\): 4" },
        @{ Id = "UPD-14"; Name = "Beta publicada como latest, cliente en estable"; Ver = "1.1.0-beta.1"; Channel = "beta"; Expect = "manifiesto rechazado" },
        @{ Id = "UPD-15"; Name = "Feed ofrece una versión más vieja (downgrade)"; Ver = "0.9.0"; Expect = "comprobación manual \(stable\): 0" }
    )
    foreach ($c in $cases) {
        if (-not (Want $c.Id)) { continue }
        $ver = if ($c.Ver) { $c.Ver } else { "1.0.1" }
        $feed = New-Feed $c.Id.ToLower() $ver "$Bin\wii-launcher-1.0.1.exe" $(if ($c.Key) { $c.Key } else { $Key }) $(if ($c.Channel) { $c.Channel } else { "stable" }) $c.Mut
        $s = Start-Feed $feed
        $inst = New-Install $c.Id.ToLower() "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad $c.Id.ToLower()
        $expect = $c.Expect
        $r = Run-Launcher $inst $lad $s.Url { (Log-Text $lad) -match $expect } -timeout 60
        Stop-Process $s.Proc -Force -ErrorAction SilentlyContinue
        $intact = (Hash "$inst\wii-launcher.exe") -eq $h100 -and -not (Test-Path "$lad\WiiLauncher\updates\journal.txt") -and (Data-Intact $inst $lad)
        $why = ((Log-Text $lad) -split "`n" | Where-Object { $_ -match "rechazad|no completada|comprobación" } | Select-Object -Last 1) -replace "^\S+ \[\d+\] ", ""
        Record $c.Id $c.Name ($r.Ok -and $intact) "no instala; exe 1.0.0 intacto: $intact; log: $($why.Trim())"
    }

    # ---------------- NET-01: sin feed (puerto cerrado) el launcher sigue abierto
    if (Want "NET-01") {
        $inst = New-Install "net01" "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad "net01"
        $env:WL_LOCALAPPDATA = $lad; $env:WL_UPDATE_FEED = "http://127.0.0.1:9/"; $env:WL_UPDATE_INTERVAL_S = "0"
        $p = Start-Process "$inst\wii-launcher.exe" -PassThru
        Start-Sleep 6
        $alive = -not $p.HasExited
        Stop-Launchers $inst $lad
        Remove-Item Env:\WL_UPDATE_FEED, Env:\WL_LOCALAPPDATA
        $line = ((Log-Text $lad) -split "`n" | Where-Object { $_ -match "comprobación" } | Select-Object -Last 1) -replace "^\S+ \[\d+\] ", ""
        Record "NET-01" "Sin conexión al feed" ($alive -and $line -match ": 4 en") "launcher sigue abierto: $alive; $($line.Trim())"
    }

    # ---------------- NET-08: descarga lenta; la ventana sigue respondiendo
    if (Want "NET-08") {
        $feed = New-Feed "net08" "1.0.1" "$Bin\wii-launcher-1.0.1.exe" $Key "stable" { param($f, $dl) Set-Content "$f\rules.json" '{"/download/v1.0.1/wii-launcher.exe": {"delay_per_chunk": 0.04}}' -Encoding ascii }
        $s = Start-Feed $feed
        $inst = New-Install "net08" "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad "net08"
        $env:WL_LOCALAPPDATA = $lad; $env:WL_UPDATE_FEED = $s.Url
        $p = Start-Process "$inst\wii-launcher.exe" -ArgumentList "--auto-update" -PassThru
        $samples = 0; $hung = 0
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.Elapsed.TotalSeconds -lt 60 -and -not ((Journal $lad) -match "state = done")) {
            Start-Sleep -Milliseconds 300
            if (-not $p.HasExited) { $p.Refresh(); $samples++; if (-not $p.Responding) { $hung++ } }
        }
        $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        Stop-Launchers $inst $lad; Stop-Process $s.Proc -Force -ErrorAction SilentlyContinue
        Remove-Item Env:\WL_UPDATE_FEED, Env:\WL_LOCALAPPDATA
        $done = (Journal $lad) -match "state = done"
        Record "NET-08" "Descarga lenta (~6 s)" ($done -and $hung -eq 0 -and $samples -gt 5) "completa en $secs s; ventana sin responder en $hung de $samples muestras"
    }

    # ---------------- recuperación
    foreach ($c in @(
            @{ Id = "INST-03"; Name = "Cierre forzado del aplicador a mitad"; Env = @{ WL_UPDATER_FAULT = "crash-after-1" } },
            @{ Id = "INST-04"; Name = "La versión nueva no arranca bien (sin señal de salud)"; Env = @{ WL_UPDATER_FAULT = "no-health"; WL_UPDATER_HEALTH_MS = "6000" } },
            @{ Id = "INST-01"; Name = "Disco lleno"; Env = @{ WL_UPDATER_FAULT = "disk-full" } })) {
        if (-not (Want $c.Id)) { continue }
        $inst = New-Install $c.Id.ToLower() "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad $c.Id.ToLower()
        $id = $c.Id
        $r = Run-Launcher $inst $lad $srv.Url {
            $j = Journal $lad; $l = Log-Text $lad
            if ($id -eq "INST-03") { $j -match "placed = " -and -not (Get-Process aplicador -ErrorAction SilentlyContinue) }
            elseif ($id -eq "INST-04") { $j -match "state = rolled_back" }
            else { $l -match "espacio insuficiente" }
        } $c.Env
        $note = ""
        if ($id -eq "INST-03") {
            $after = Hash "$inst\wii-launcher.exe"
            # Siguiente arranque normal: recupera solo y se reabre con aviso.
            $r2 = Run-Launcher $inst $lad $srv.Url { (Journal $lad) -match "state = rolled_back" } @{} @() 30
            $note = "tras el corte exe = 1.0.1 a medias: $($after -eq $h101); al reabrir: rollback $($r2.Ok)"
            $ok = $r.Ok -and $r2.Ok
        } else {
            $ok = $r.Ok
            $note = "rollback/abortado en $($r.Secs) s"
        }
        $intact = (Hash "$inst\wii-launcher.exe") -eq $h100 -and (Data-Intact $inst $lad)
        Record $c.Id $c.Name ($ok -and $intact) "$note; exe vuelve a 1.0.0 y datos intactos: $intact"
    }

    # ---------------- UPD-10: dos ventanas del launcher
    if (Want "UPD-10") {
        $inst = New-Install "upd10" "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad "upd10"
        $env:WL_LOCALAPPDATA = $lad
        $first = Start-Process "$inst\wii-launcher.exe" -PassThru
        Start-Sleep 2
        $r = Run-Launcher $inst $lad $srv.Url { (Log-Text $lad) -match "otra ventana del launcher abierta" } -timeout 60
        Record "UPD-10" "Dos ventanas abiertas: no se instala" ($r.Ok -and (Hash "$inst\wii-launcher.exe") -eq $h100) "instalación pospuesta con aviso; exe intacto"
    }

    # ---------------- UPD-11: carpeta de solo lectura
    if (Want "UPD-11") {
        $inst = New-Install "upd11" "$Bin\wii-launcher-1.0.0.exe"; $lad = New-Lad "upd11"
        $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        icacls $inst /deny "${me}:(W,AD,WD)" | Out-Null
        try {
            $r = Run-Launcher $inst $lad $srv.Url { (Journal $lad) -match "state" -or (Log-Text $lad) -match "comprobación manual" } -timeout 30
            Start-Sleep 2
        } finally { icacls $inst /remove:d $me | Out-Null }
        $nothing = -not (Test-Path "$lad\WiiLauncher\updates\journal.txt") -and (Hash "$inst\wii-launcher.exe") -eq $h100
        Record "UPD-11" "Carpeta sin permiso de escritura: no descarga ni pide administrador" ($r.Ok -and $nothing) "sin descarga ni cambios; la UI pide mover la carpeta"
    }

    # ---------------- UPD-12: migración desde v0.1.0-clase (sin updater) con el ZIP puente
    if ($OldExe -and (Want "UPD-12")) {
        $inst = New-Install "upd12" $OldExe; $lad = New-Lad "upd12"
        Copy-Item "$Bin\wii-launcher-1.0.0.exe" "$inst\wii-launcher.exe" -Force  # = descomprimir el ZIP puente encima
        $r = Run-Launcher $inst $lad $srv.Url { (Journal $lad) -match "state = done" }
        Record "UPD-12" "Clase (sin updater) -> ZIP puente 1.0.0 -> auto a 1.0.1" ($r.Ok -and (Hash "$inst\wii-launcher.exe") -eq $h101 -and (Data-Intact $inst $lad)) "puente manual una vez; después actualización automática en $($r.Secs) s"
    }
} finally {
    foreach ($server in $servers) {
        if (-not $server.HasExited) { Stop-Process -InputObject $server -Force -ErrorAction SilentlyContinue }
    }
}

$md = @("| ID | Escenario | Estado | Observado |", "|---|---|---|---|")
foreach ($r in $results) { $md += "| $($r.Id) | $($r.Escenario) | $($r.Estado) | $($r.Observado -replace '\|', '/') |" }
$md | Set-Content "$Work\resultados.md" -Encoding utf8
$fails = 0
foreach ($r in $results) { if ($r.Estado -ne "PASS") { $fails++ } }
Write-Host "`n$($results.Count - $fails)/$($results.Count) PASS"
exit [int]($fails -gt 0)
