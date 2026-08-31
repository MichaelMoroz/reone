# Copyright (c) 2026 The reone project contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Vulkan validation gate: the build must be validation-clean before anything
# else is measured against it. See doc/tasks/GPU-CONTRACTS.md, phase 5.
#
# Runs a short character-generation sequence - the exact shape that exposed
# the post-merge device loss: two module reloads with GUI scene graphs
# surviving both - under --vkvalidation 1, and fails on ANY VUID line or any
# nonzero engine exit. Zero is the invariant this gate defends; the day the
# baseline is "a few known warnings", the delta method that diagnosed the
# original fault stops working.
param(
    [Parameter(Mandatory = $true)][string]$Kotor1Dir,
    # The fault this gate was born from reproduced at 1024 and 3440 but NOT at
    # 1920, so the gate runs more than one resolution by default.
    [int[][]]$Resolutions = @(@(1024, 768), @(3440, 1440))
)

$ErrorActionPreference = "Stop"

$repoDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$enginePath = Join-Path $repoDir "build\bin\engine.exe"
if (-not (Test-Path -LiteralPath $enginePath -PathType Leaf)) {
    throw "Release build artifact not found: $enginePath"
}
if (-not (Test-Path -LiteralPath $Kotor1Dir -PathType Container)) {
    throw "Game directory not found: $Kotor1Dir"
}

$runDir = Join-Path $repoDir "build\validation-gate"
Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction Ignore
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$commandPath = Join-Path $runDir "commands.txt"
@(
    "graphics on"
    "openchargen class"
    "pause 60"
    "warp danm14aa"
    "openchargen quick-or-custom"
    "pause 60"
    "warp danm14aa"
    "openchargen quick"
    "pause 60"
    "capture $((Join-Path $runDir "gate.tga").Replace('\', '/'))"
    "quit"
) | Set-Content -LiteralPath $commandPath -Encoding utf8

$failures = @()
foreach ($res in $Resolutions) {
    $w = $res[0]; $h = $res[1]
    $log = Join-Path $runDir "gate-${w}x${h}.log"
    Write-Host "validation gate ${w}x${h}"
    Push-Location $runDir
    try {
        & $enginePath --game $Kotor1Dir --commands-file $commandPath `
            --width $w --height $h --winscale 100 --fullscreen 0 `
            --headless 1 --dev 0 --vsync 0 --mode retro --vkvalidation 1 `
            --guiscale 1 --guiborderscale 1 --guilistscale 0.5 *> $log
        $engineExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $vuids = (Select-String -LiteralPath $log -Pattern 'VUID-[A-Za-z0-9-]+' -AllMatches).Matches.Value |
        Sort-Object -Unique
    if ($engineExit -ne 0) {
        $failures += "${w}x${h}: engine exited $engineExit"
    }
    if ($vuids) {
        $failures += "${w}x${h}: $(@($vuids).Count) distinct validation errors"
        @($vuids) | ForEach-Object { $failures += "    $_" }
    }
}

if ($failures) {
    Write-Host "VALIDATION GATE FAILED"
    $failures | ForEach-Object { Write-Host "  $_" }
    Write-Host "Full engine output is under $runDir"
    exit 1
}
Write-Host "validation gate clean: $(@($Resolutions).Count) resolutions, zero VUIDs"
exit 0
