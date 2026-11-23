param([string]$BuildDirectory = 'build-account')

$ErrorActionPreference = 'Stop'
if ($env:WL_SUPABASE_URL -notmatch '^http://127\.0\.0\.1:\d+$' -or -not $env:WL_SUPABASE_PUBLISHABLE_KEY) {
    throw 'Configura la URL loopback y la clave publicable de Supabase local.'
}
$test = Join-Path $BuildDirectory 'wl_account_tests.exe'
& $test --live
exit $LASTEXITCODE
