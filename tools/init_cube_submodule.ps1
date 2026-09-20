<#
.SYNOPSIS
    Restores the STM32CubeH7 submodule: partial, shallow and sparse.

.DESCRIPTION
    `git submodule update --init --recursive` does not work for this submodule,
    and the failure modes are not obvious:

      * ST's `Projects/` tree has paths that pass the Windows 260-character
        limit once nested this deep. The checkout dies on "Filename too long"
        and git rolls the registration back, leaving nothing behind.
      * `--recursive` initialises all 52 inner submodules, including 44 BSP
        repositories for boards this product does not have.
      * A plain `--depth 1` still downloads every blob in the package - ~350 MB
        of object store for ~40 MB of files anyone here will ever open.

    So the restore is: a partial (`--filter=blob:none`), shallow (`--depth 1`),
    sparse clone limited to `Drivers` and `Middlewares`, with only the four
    inner submodules the firmware compiles against. Measured on this workspace:
    **26 MB of object store instead of 359 MB**, and about 20 seconds.

    The commit is taken from the gitlink recorded in the index, not from
    whatever `master` happens to be, so this stays correct after upstream moves.

.PARAMETER Project
    Project whose external/STM32CubeH7 to restore. Defaults to the only one.

.PARAMETER Force
    Re-create the submodule even if it already has content.

.PARAMETER Check
    Report whether the submodule is usable and change nothing. Exit code 1 if
    it is missing or incomplete.

.EXAMPLE
    .\tools\init_cube_submodule.ps1
    .\tools\init_cube_submodule.ps1 -Check

.NOTES
    Partial clone leaves the repository dependent on the network for blobs it
    never downloaded - `git log -p` inside the package will fetch on demand.
    Building never does: everything the compiler reads is materialised here.
#>
[CmdletBinding()]
param(
    [string] $Project = '02_firmware_projects/prj_eth_lwip_node',
    [switch] $Force,
    [switch] $Check,
    [switch] $Quiet
)

# NOT 'Stop'. git writes progress to stderr, and Windows PowerShell 5.1 wraps
# every stderr line from a native command in an ErrorRecord - with 'Stop' the
# script dies on "Cloning into ..." while git is succeeding. Correctness comes
# from checking $LASTEXITCODE after each call instead, which is what git means.
$ErrorActionPreference = 'Continue'

$Root   = Split-Path -Parent $PSScriptRoot
$RelPath = "$Project/external/STM32CubeH7"
$AbsPath = Join-Path $Root $RelPath
$Url    = 'https://github.com/STMicroelectronics/STM32CubeH7.git'

# Sparse cone: the libraries the firmware links. Not Projects/ (ST's examples
# for every board, and the source of the long-path failures) and not
# Documentation/. The H745 examples are already in the documentation hub.
$Cone = @('Drivers', 'Middlewares')

# The four inner submodules the build needs, out of the 52 the package declares.
$Inner = @(
    'Drivers/STM32H7xx_HAL_Driver'
    'Drivers/CMSIS/Device/ST/STM32H7xx'
    'Middlewares/Third_Party/LwIP'
    'Middlewares/Third_Party/FreeRTOS'
)

# What CMakeLists.txt probes to decide the package is usable.
$Sentinel = 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth.c'

function Say($msg, $colour = 'Gray') { if (-not $Quiet) { Write-Host $msg -ForegroundColor $colour } }

Push-Location $Root
try {
    if (-not (Test-Path (Join-Path $Root '.git'))) {
        Write-Host "Not a git repository: $Root" -ForegroundColor Red
        exit 1
    }

    # --- Is it already usable? ---------------------------------------------
    $ok = Test-Path (Join-Path $AbsPath $Sentinel)

    if ($Check) {
        if ($ok) {
            $head = (git -C $AbsPath rev-parse --short HEAD 2>$null)
            Say "  OK     STM32CubeH7 present at $head" 'Green'
            foreach ($m in $Inner) {
                $n = @(Get-ChildItem -LiteralPath (Join-Path $AbsPath $m) -Recurse -File -ErrorAction SilentlyContinue).Count
                $sha = (git -C (Join-Path $AbsPath $m) rev-parse --short HEAD 2>$null)
                Say ("         {0,5} files  {1}  {2}" -f $n, $sha, $m) 'DarkGray'
            }
            exit 0
        }
        Say "  MISSING  $RelPath is absent or incomplete" 'Yellow'
        Say "  Fix with:  .\tools\init_cube_submodule.ps1" 'DarkGray'
        exit 1
    }

    if ($ok -and -not $Force) {
        Say "Already present at $(git -C $AbsPath rev-parse --short HEAD). Use -Force to re-create." 'DarkGray'
        exit 0
    }

    # --- The commit to land on comes from the index, not from master --------
    $entry = git ls-files -s -- $RelPath
    if (-not $entry) {
        Write-Host "No gitlink recorded for $RelPath - is this the right workspace?" -ForegroundColor Red
        exit 1
    }
    $Wanted = ($entry -split '\s+')[1]
    Say "Recorded commit: $Wanted" 'DarkGray'

    if (Test-Path $AbsPath) {
        Say 'Removing the existing checkout...' 'DarkGray'
        git submodule deinit -f -- $RelPath | Out-Null
        Remove-Item -LiteralPath $AbsPath -Recurse -Force -ErrorAction SilentlyContinue
        $mod = Join-Path $Root ".git/modules/$RelPath"
        if (Test-Path $mod) { Remove-Item -LiteralPath $mod -Recurse -Force }
    }

    # --- Partial + shallow + sparse ----------------------------------------
    Say 'Cloning (partial, shallow, sparse)...' 'Cyan'
    git -c core.longpaths=true clone --depth 1 --filter=blob:none --sparse $Url $AbsPath
    if ($LASTEXITCODE) { Write-Host 'clone failed' -ForegroundColor Red; exit 1 }

    # A shallow clone of master may not contain the recorded commit if upstream
    # has moved since. Fetch exactly that object when it is missing.
    $have = (git -C $AbsPath cat-file -t $Wanted 2>$null)
    if ($have -ne 'commit') {
        Say "master has moved; fetching $Wanted ..." 'Yellow'
        git -C $AbsPath fetch --depth 1 --filter=blob:none origin $Wanted
        if ($LASTEXITCODE) {
            Write-Host "Could not fetch $Wanted from origin." -ForegroundColor Red
            exit 1
        }
    }
    git -C $AbsPath checkout -q --detach $Wanted
    if ($LASTEXITCODE) { Write-Host "checkout $Wanted failed" -ForegroundColor Red; exit 1 }

    Say "Sparse cone: $($Cone -join ', ')" 'Cyan'
    git -C $AbsPath sparse-checkout set @Cone
    if ($LASTEXITCODE) { Write-Host 'sparse-checkout failed' -ForegroundColor Red; exit 1 }

    Say 'Initialising the four inner submodules the build needs...' 'Cyan'
    git -C $AbsPath -c core.longpaths=true submodule update --init --depth 1 --filter=blob:none -- @Inner
    if ($LASTEXITCODE) { Write-Host 'inner submodule update failed' -ForegroundColor Red; exit 1 }

    # --- Hand the clone over to the parent repository -----------------------
    # Not piped: piping a native command makes PowerShell 5.1 render its stderr
    # as a red error block. Unpiped, git's "Migrating git directory..." and
    # "registered for path ..." print as the plain progress they are.
    git submodule absorbgitdirs -- $RelPath
    git submodule init -- $RelPath

    # --- Verify -------------------------------------------------------------
    if (-not (Test-Path (Join-Path $AbsPath $Sentinel))) {
        Write-Host "Finished, but $Sentinel is still missing - the build will not configure." -ForegroundColor Red
        exit 1
    }

    Say ''
    Say "STM32CubeH7 restored at $(git -C $AbsPath rev-parse --short HEAD)" 'Green'
    foreach ($m in $Inner) {
        $sha = (git -C (Join-Path $AbsPath $m) rev-parse --short HEAD 2>$null)
        Say ("  {0}  {1}" -f $sha, $m) 'DarkGray'
    }
    Say ''
    # Deliberately not measuring .git here: Get-ChildItem -Recurse cannot walk
    # the deep paths under .git/modules and silently under-reports. Use
    #     git count-objects -vH
    # in the workspace root, and again inside the submodule, for real numbers.
    exit 0
}
finally {
    Pop-Location
}
