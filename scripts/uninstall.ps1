# Undo install.ps1: restore (or remove) the FreeTrack registry path. Game files were never modified.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$key = 'HKCU:\Software\FreeTrack\FreeTrackClient'
$backup = Join-Path $projectRoot 'config\freetrack-path.backup'
if (Test-Path -LiteralPath $backup) {
    $previous = (Get-Content -LiteralPath $backup -Raw).Trim()
    Set-ItemProperty -Path $key -Name Path -Value $previous
    Remove-Item -LiteralPath $backup
    Write-Output "Restored previous FreeTrack path: $previous"
} else {
    Remove-Item -Path $key -Recurse -ErrorAction SilentlyContinue
    $parent = 'HKCU:\Software\FreeTrack'
    if ((Test-Path $parent) -and -not (Get-ChildItem $parent)) { Remove-Item -Path $parent }
    Write-Output 'Removed FreeTrack registry path.'
}
# The launcher's X4 extensions (generated, each marked by its <name>.txt): HUD distance, seat position.
foreach ($name in 'x4vr_hud', 'x4vr_seat') {
    $extension = Join-Path (Split-Path $projectRoot -Parent) "extensions\$name"
    if (Test-Path -LiteralPath (Join-Path $extension "$name.txt")) {
        Remove-Item -LiteralPath $extension -Recurse -Force
        Write-Output "Removed the $name extension."
    }
}
Write-Output 'In X4 you may turn "OpenTrack Support" off again (Options > Controls > Head Tracking Support).'
