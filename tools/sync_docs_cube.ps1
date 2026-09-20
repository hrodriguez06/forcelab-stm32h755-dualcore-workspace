<#
.SYNOPSIS
    Rebuilds the greppable Cube mirror inside the documentation hub.

.DESCRIPTION
    `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/` is a pruned copy of
    the ST package kept so that HAL, CMSIS device headers, LwIP and FreeRTOS can
    be grepped from one place. Four of its directories are Git submodules of the
    upstream package, and dropping `.git/` during the prune left them empty.
    They were filled by copying from the checkout the firmware actually compiles
    against.

    Those four trees are therefore a derived copy, not a source of truth, so the
    workspace `.gitignore` excludes them and this script regenerates them. The
    rest of the pruned package - CMSIS Core, `Projects/`, `Documentation/` - IS
    tracked, because it came from a manual prune that nothing reproduces.

    Run this after a fresh clone, and again whenever a project moves to a
    different STM32CubeH7 version: otherwise the hub documents an API the
    firmware no longer uses.

.PARAMETER From
    Project whose `external/STM32CubeH7` is the source. Defaults to
    `02_firmware_projects/prj_eth_lwip_node`.

.PARAMETER Force
    Replace trees that are already populated. Without it, a non-empty
    destination is left alone and reported.

.PARAMETER Check
    Report what is present or missing and change nothing. Exit code 1 if any
    tree is empty.

.EXAMPLE
    .\tools\sync_docs_cube.ps1
    .\tools\sync_docs_cube.ps1 -Check
    .\tools\sync_docs_cube.ps1 -From 02_firmware_projects/prj_other -Force
#>
[CmdletBinding()]
param(
    [string] $From = '02_firmware_projects/prj_eth_lwip_node',
    [switch] $Force,
    [switch] $Check,
    [switch] $Quiet
)

$WorkspaceRoot = Split-Path -Parent $PSScriptRoot
$HubCube = Join-Path $WorkspaceRoot '00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7'

# The four submodule paths that the prune emptied and that the .gitignore skips.
# Keep this list in step with both of those.
$Trees = @(
    'Drivers/STM32H7xx_HAL_Driver'
    'Drivers/CMSIS/Device/ST/STM32H7xx'
    'Middlewares/Third_Party/LwIP'
    'Middlewares/Third_Party/FreeRTOS'
)

function Count-Files([string] $Path) {
    if (-not (Test-Path $Path)) { return 0 }
    return @(Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue).Count
}

$sourceRoot = Join-Path $WorkspaceRoot (Join-Path $From 'external/STM32CubeH7')

if (-not (Test-Path $HubCube)) {
    Write-Host "Documentation hub not found at $HubCube" -ForegroundColor Red
    exit 1
}

if (-not $Check -and -not (Test-Path $sourceRoot)) {
    Write-Host "Source checkout not found: $sourceRoot" -ForegroundColor Red
    Write-Host "  That project has no external/STM32CubeH7. Fetch it there first," -ForegroundColor DarkGray
    Write-Host "  or point at another project with -From." -ForegroundColor DarkGray
    exit 1
}

$missing = 0
$copied = 0

foreach ($tree in $Trees) {
    $dst = Join-Path $HubCube $tree
    $have = Count-Files $dst

    if ($Check) {
        if ($have -gt 0) {
            if (-not $Quiet) { Write-Host ("  OK     {0,6} files  {1}" -f $have, $tree) -ForegroundColor Green }
        }
        else {
            $missing++
            if (-not $Quiet) { Write-Host ("  EMPTY  {0,6}         {1}" -f '', $tree) -ForegroundColor Yellow }
        }
        continue
    }

    if ($have -gt 0 -and -not $Force) {
        if (-not $Quiet) { Write-Host ("  skip   {0,6} files  {1}  (already populated; -Force to replace)" -f $have, $tree) -ForegroundColor DarkGray }
        continue
    }

    $src = Join-Path $sourceRoot $tree
    if ((Count-Files $src) -eq 0) {
        Write-Host ("  MISS            {0}  (not in {1})" -f $tree, $From) -ForegroundColor Red
        $missing++
        continue
    }

    if (Test-Path $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    Copy-Item -Path (Join-Path $src '*') -Destination $dst -Recurse -Force

    $now = Count-Files $dst
    $copied++
    if (-not $Quiet) { Write-Host ("  copied {0,6} files  {1}" -f $now, $tree) -ForegroundColor Green }
}

if ($Check) {
    if ($missing) {
        if (-not $Quiet) {
            Write-Host ''
            Write-Host "$missing tree(s) empty. Grepping HAL, LwIP or FreeRTOS from the hub will find nothing." -ForegroundColor Yellow
            Write-Host "Fix with:  .\tools\sync_docs_cube.ps1" -ForegroundColor DarkGray
        }
        exit 1
    }
    if (-not $Quiet) { Write-Host "`nAll four trees populated." -ForegroundColor Green }
    exit 0
}

if (-not $Quiet) {
    Write-Host ''
    Write-Host "Copied $copied of $($Trees.Count) trees from $From." -ForegroundColor Green
    Write-Host 'These are a snapshot of what that project compiles against, not a tracked version.' -ForegroundColor DarkGray
}
exit $(if ($missing) { 1 } else { 0 })
