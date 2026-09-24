<#
.SYNOPSIS
    Builds the firmware projects across presets (debug and release) for dual-core targets.

.DESCRIPTION
    Multi-Project CI Matrix Build tool (DRIVER_ARCHITECTURE_GUIDELINES.md §5).
    Iterates through the firmware projects and presets, builds CM7 and CM4 targets,
    measures binary sizes using arm-none-eabi-size, and outputs a formatted summary table.

.PARAMETER Projects
    List of project directories or project folder names to build.
    Defaults to prj_01 (template), prj_02, prj_03, and prj_04.

.PARAMETER Presets
    Presets to build. Defaults to @('debug', 'release').

.PARAMETER Fresh
    Passes --fresh to CMake configure.

.PARAMETER Clean
    Removes the build directory before configuring.

.EXAMPLE
    .\tools\build_all.ps1
    Build all 4 projects in debug and release (8 configurations).

.EXAMPLE
    .\tools\build_all.ps1 -Projects prj_04_adc_dma_bm_eth -Presets release
#>
[CmdletBinding()]
param(
    [string[]] $Projects,
    [string[]] $Presets = @('debug', 'release'),
    [switch]   $Fresh,
    [switch]   $Clean
)

$ErrorActionPreference = 'Stop'

# Ensure toolchain is on PATH
$envScript = Join-Path $PSScriptRoot 'env.ps1'
if (Test-Path $envScript) {
    . $envScript -Quiet
}

$WorkspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$DefaultProjects = @(
    '03_firmware_templates/prj_01_template_dualcore',
    '02_firmware_projects/prj_02_eth_lwip_dualcore',
    '02_firmware_projects/prj_03_adc_dma_eth',
    '02_firmware_projects/prj_04_adc_dma_bm_eth'
)

$TargetProjects = @()
if ($Projects) {
    foreach ($p in $Projects) {
        # Check if direct path or matches subfolder
        $matched = $false
        foreach ($dp in $DefaultProjects) {
            if ($dp -eq $p -or (Split-Path $dp -Leaf) -eq $p -or $dp.EndsWith($p)) {
                $TargetProjects += $dp
                $matched = $true
                break
            }
        }
        if (-not $matched) {
            if (Test-Path (Join-Path $WorkspaceRoot $p)) {
                $TargetProjects += $p
            } else {
                Write-Warning "Project '$p' not recognized or path does not exist."
            }
        }
    }
} else {
    $TargetProjects = $DefaultProjects
}

$Results = New-Object System.Collections.Generic.List[object]
$OverallSuccess = $true

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " STM32H755 Multi-Project CI Matrix Build" -ForegroundColor Cyan
Write-Host " Projects: $($TargetProjects.Count) | Presets: $($Presets -join ', ')" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host ""

foreach ($projRel in $TargetProjects) {
    $projDir = Join-Path $WorkspaceRoot $projRel
    $projName = Split-Path $projRel -Leaf

    if (-not (Test-Path $projDir)) {
        Write-Host "Skipping missing project: $projRel" -ForegroundColor Yellow
        continue
    }

    foreach ($preset in $Presets) {
        $configDesc = "$projName [$preset]"
        Write-Host ">>> Configuring & Building $configDesc ..." -ForegroundColor Yellow
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $status = "OK"
        $errDetail = ""

        Push-Location $projDir
        try {
            if ($Clean) {
                $buildDir = Join-Path $projDir "build/$preset"
                if (Test-Path $buildDir) {
                    Remove-Item -Recurse -Force $buildDir
                }
            }

            $cmakeArgs = @('--preset', $preset)
            if ($Fresh) {
                $cmakeArgs += '--fresh'
            }

            $cfgOut = & cmake @cmakeArgs 2>&1
            if ($LASTEXITCODE -ne 0) {
                $status = "CONFIG_FAIL"
                $errDetail = ($cfgOut | Out-String).Trim()
                $OverallSuccess = $false
            } else {
                $buildOut = & cmake --build --preset $preset 2>&1
                if ($LASTEXITCODE -ne 0) {
                    $status = "BUILD_FAIL"
                    $errDetail = ($buildOut | Out-String).Trim()
                    $OverallSuccess = $false
                }
            }
        } catch {
            $status = "ERROR"
            $errDetail = $_.ToString()
            $OverallSuccess = $false
        } finally {
            Pop-Location
        }
        $sw.Stop()

        # Parse sizes if build succeeded
        $cores = @('CM7', 'CM4')
        foreach ($core in $cores) {
            $elfPath = Join-Path $projDir "build/$preset/$core/$($core.ToLower())_firmware.elf"
            $textSize = $null
            $dataSize = $null
            $bssSize = $null
            $decSize = $null

            if ($status -eq "OK" -and (Test-Path $elfPath)) {
                try {
                    $sizeLines = & arm-none-eabi-size $elfPath 2>&1
                    if ($sizeLines.Count -ge 2) {
                        # Header: text data bss dec hex filename
                        # Data:   26732 132 55608 82472 14228 ...
                        $tokens = ($sizeLines[1] -split '\s+') | Where-Object { $_ -ne '' }
                        if ($tokens.Count -ge 4) {
                            $textSize = [int]$tokens[0]
                            $dataSize = [int]$tokens[1]
                            $bssSize  = [int]$tokens[2]
                            $decSize  = [int]$tokens[3]
                        }
                    }
                } catch { }
            }

            $Results.Add([pscustomobject]@{
                Project  = $projName
                Preset   = $preset
                Core     = $core
                Status   = if ($status -eq "OK") { "PASS" } else { $status }
                Duration = [string]::Format("{0:0.0}s", $sw.Elapsed.TotalSeconds)
                Text     = $textSize
                Data     = $dataSize
                Bss      = $bssSize
                Total    = $decSize
            })
        }

        if ($status -eq "OK") {
            Write-Host "  -> PASS in $([string]::Format('{0:0.0}s', $sw.Elapsed.TotalSeconds))" -ForegroundColor Green
        } else {
            Write-Host "  -> $status in $([string]::Format('{0:0.0}s', $sw.Elapsed.TotalSeconds))" -ForegroundColor Red
            if ($errDetail) {
                Write-Host "     $($errDetail.Split("`n")[0])" -ForegroundColor Red
            }
        }
    }
}

Write-Host ""
Write-Host "==========================================================================================" -ForegroundColor Cyan
Write-Host " Matrix Build & Binary Size Summary Table" -ForegroundColor Cyan
Write-Host "==========================================================================================" -ForegroundColor Cyan

# Output formatted table
$Results | Format-Table -Property @(
    @{ Label = 'Project';  Expression = { $_.Project }; Width = 26 },
    @{ Label = 'Preset';   Expression = { $_.Preset };  Width = 9 },
    @{ Label = 'Core';     Expression = { $_.Core };    Width = 6 },
    @{ Label = 'Status';   Expression = { $_.Status };  Width = 11 },
    @{ Label = 'Text (B)'; Expression = { if ($_.Text -ne $null) { $_.Text } else { '-' } }; Width = 10; Alignment = 'Right' },
    @{ Label = 'Data (B)'; Expression = { if ($_.Data -ne $null) { $_.Data } else { '-' } }; Width = 10; Alignment = 'Right' },
    @{ Label = 'Bss (B)';  Expression = { if ($_.Bss -ne $null)  { $_.Bss }  else { '-' } }; Width = 10; Alignment = 'Right' },
    @{ Label = 'Total (B)';Expression = { if ($_.Total -ne $null){ $_.Total }else { '-' } }; Width = 10; Alignment = 'Right' },
    @{ Label = 'Time';     Expression = { $_.Duration }; Width = 8; Alignment = 'Right' }
)

if (-not $OverallSuccess) {
    Write-Host "Matrix build finished with ERRORS." -ForegroundColor Red
    exit 1
} else {
    Write-Host "All 8 builds PASSED successfully." -ForegroundColor Green
    exit 0
}
