param(
    [Parameter(Mandatory=$true)][string]$Bin,
    [Parameter(Mandatory=$true)][int]$Com,
    [string]$Baud = "1500000"
)

$ErrorActionPreference = "Stop"

# BK_LOADER is normally provided by the environment (set from board.mk/config.local.mk)
if (-not $env:BK_LOADER) {
    Write-Error "BK_LOADER environment variable not set (see boards/<board>/board.mk)"
}
if (-not (Test-Path $Bin)) {
    Write-Error "image not found: $Bin"
}

$dir = Split-Path $env:BK_LOADER -Parent
Write-Host "Flashing $Bin -> COM$Com @ $Baud"
& (Join-Path $dir "bk_loader.exe") download -p $Com -b $Baud -i (Resolve-Path $Bin).Path -s 0
if ($LASTEXITCODE -ne 0) { Write-Error "flash failed" }
Write-Host "Flash OK - board rebooted"
