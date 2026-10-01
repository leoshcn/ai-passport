# Starts the Windows discovery listener, then docker compose.
# The device API is published on 0.0.0.0:8787. The console is published on 0.0.0.0:8790.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$discover = Join-Path $Root "backend\xhs_discover.py"
$proc = Start-Process -FilePath "py" -ArgumentList @("-3", $discover) -WorkingDirectory $Root -PassThru -WindowStyle Hidden
try {
    & docker compose up
    exit $LASTEXITCODE
} finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}
