<#
.SYNOPSIS
    Builds the 05_i2c bench target firmware of the NUCLEO-F334R8.

.DESCRIPTION
    Puts the STM32CubeIDE toolchain on PATH (tools/env.ps1), configures the
    'f334' preset of this directory (build/f334) and builds it, then prints
    the section sizes and the number of compiler warnings found in the log.
    The firmware is an instrument of the 05_i2c bench (plan D2), not a
    catalogue project: README.md says what it does and how to flash it.

.PARAMETER Clean
    Removes build/f334 before configuring.

.PARAMETER Werror
    Configures with CMAKE_C_FLAGS=-Werror (a separate check; the id the
    firmware prints then differs, because CMAKE_C_FLAGS is part of it).

.EXAMPLE
    .\tools\i2c_target_f334\build.ps1 -Clean
#>
[CmdletBinding()]
param(
    [switch] $Clean,
    [switch] $Werror
)

$ErrorActionPreference = 'Stop'

$F334Dir = $PSScriptRoot
$F334Workspace = (Resolve-Path (Join-Path $F334Dir '..\..')).Path
. (Join-Path $F334Workspace 'tools\env.ps1') -Quiet

$F334Build = Join-Path $F334Dir 'build\f334'
if ($Clean -and (Test-Path $F334Build)) {
    Remove-Item -Recurse -Force $F334Build
}

$F334Flags = if ($Werror) { '-Werror' } else { '' }
$F334Log = New-Object System.Collections.Generic.List[string]

$F334Rc = 0
Push-Location $F334Dir
try {
    & cmake --preset f334 --fresh "-DCMAKE_C_FLAGS=$F334Flags" 2>&1 | ForEach-Object { $F334Log.Add("$_"); Write-Host "$_" }
    $F334Rc = $LASTEXITCODE
    if ($F334Rc -eq 0) {
        & cmake --build --preset f334 2>&1 | ForEach-Object { $F334Log.Add("$_"); Write-Host "$_" }
        $F334Rc = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
if ($F334Rc -ne 0) {
    Write-Host "BUILD FAILED (exit code $F334Rc)"
    exit 1
}

$F334Elf = Join-Path $F334Build 'i2c_target_f334.elf'
$F334Warnings = @($F334Log | Where-Object { $_ -match 'warning:' }).Count
Write-Host ''
Write-Host "ELF      : $F334Elf"
& arm-none-eabi-size $F334Elf
Write-Host "Warnings : $F334Warnings"
Write-Host "Flash    : cd $F334Dir; py ..\flash.py --sn <F334 ST-LINK SN> --elf build\f334\i2c_target_f334.elf"
if ($F334Warnings -ne 0) { exit 1 }
