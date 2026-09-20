<#
.SYNOPSIS
    Puts the STM32 build toolchain (CMake, Ninja, arm-none-eabi-gcc) on PATH for
    this PowerShell session.

.DESCRIPTION
    This workspace has no standalone CMake install: the tools ship inside
    STM32CubeIDE, under plugin directories whose names carry a build stamp, e.g.

        com.st.stm32cube.ide.mcu.externaltools.cmake.win32_1.1.200.202605190741
                                                                 ^^^^^^^^^^^^^^
    That stamp changes on every CubeIDE update, so any path written by hand
    rots at the next update. This script resolves the paths by wildcard instead,
    and prefers STM32CubeCLT when it is installed, since that one has stable
    paths and is the better long-term home for a command-line build.

    Only $env:PATH is touched, and only for the current process. Nothing is
    written to the registry or to the user environment.

.PARAMETER Root
    Explicit STM32CubeCLT or STM32CubeIDE installation directory. Skips the
    search. Use this when several versions are installed and the newest is not
    the one you build with.

.PARAMETER Check
    Report what was found and leave PATH untouched. Exit code 1 if a required
    tool is missing.

.PARAMETER Quiet
    No output on success.

.EXAMPLE
    .\tools\env.ps1
    cmake --build --preset debug

.EXAMPLE
    .\tools\env.ps1 -Check
    Verify the toolchain without modifying the session.

.NOTES
    Run it from anywhere; it does not change the working directory. The CMake
    presets live in the project root, so `cmake --preset debug` still has to be
    run from there.
#>
[CmdletBinding()]
param(
    [string] $Root,
    [switch] $Check,
    [switch] $Quiet
)

# --- What we need. 'Required' tools abort; the rest are for flashing/debugging
#     and only produce a warning, so a machine without them can still build. ---
$Tools = @(
    @{ Key = 'cmake';      Exe = 'cmake.exe';                Plugin = 'cmake';              Required = $true;  Note = 'build system generator' }
    @{ Key = 'ninja';      Exe = 'ninja.exe';                Plugin = 'ninja';              Required = $true;  Note = 'build backend' }
    @{ Key = 'gcc';        Exe = 'arm-none-eabi-gcc.exe';    Plugin = 'gnu-tools-for-stm32'; Required = $true;  Note = 'cross compiler' }
    @{ Key = 'programmer'; Exe = 'STM32_Programmer_CLI.exe'; Plugin = 'cubeprogrammer';     Required = $false; Note = 'flashing, option bytes (BCM7/BCM4)' }
    @{ Key = 'openocd';    Exe = 'openocd.exe';              Plugin = 'openocd';            Required = $false; Note = 'alternative flash/debug backend' }
    @{ Key = 'gdbserver';  Exe = 'ST-LINK_gdbserver.exe';    Plugin = 'stlink-gdb-server';  Required = $false; Note = 'dual-core GDB attach' }
)

$searched = New-Object System.Collections.Generic.List[string]

function Get-InstallRoots {
    param([string] $Explicit)

    if ($Explicit) {
        if (-not (Test-Path $Explicit)) {
            Write-Host "-Root does not exist: $Explicit" -ForegroundColor Red
            exit 1
        }
        $full = (Resolve-Path $Explicit).Path
        # A CubeIDE root must be classified as 'ide': its tools sit five levels
        # down (STM32CubeIDE\plugins\<plugin>\tools\bin), past the generic search.
        foreach ($candidate in @((Join-Path $full 'STM32CubeIDE\plugins'), (Join-Path $full 'plugins'), $full)) {
            if ((Split-Path $candidate -Leaf) -eq 'plugins' -and (Test-Path $candidate)) {
                return @([pscustomobject]@{ Path = $candidate; Kind = 'ide' })
            }
        }
        return @([pscustomobject]@{ Path = $full; Kind = 'clt' })
    }

    $found = New-Object System.Collections.Generic.List[object]

    # STM32CubeCLT first: stable directory layout, meant for command-line builds.
    foreach ($base in @("$env:ProgramFiles\STMicroelectronics", 'C:\ST', "$env:LOCALAPPDATA\Programs")) {
        if (-not (Test-Path $base)) { continue }
        Get-ChildItem $base -Directory -Filter 'STM32CubeCLT*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $found.Add([pscustomobject]@{ Path = $_.FullName; Kind = 'clt' }) }
    }

    # Then STM32CubeIDE, whose tools live under plugins\...externaltools.<name>.win32_<stamp>\tools\bin
    foreach ($base in @('C:\ST', "$env:ProgramFiles\STMicroelectronics", "$env:LOCALAPPDATA\Programs")) {
        if (-not (Test-Path $base)) { continue }
        Get-ChildItem $base -Directory -Filter 'STM32CubeIDE*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                $plugins = Join-Path $_.FullName 'STM32CubeIDE\plugins'
                if (-not (Test-Path $plugins)) { $plugins = Join-Path $_.FullName 'plugins' }
                if (Test-Path $plugins) {
                    $found.Add([pscustomobject]@{ Path = $plugins; Kind = 'ide' })
                }
            }
    }

    return $found
}

function Resolve-ToolDir {
    param([pscustomobject] $Root, [hashtable] $Tool)

    switch ($Root.Kind) {
        'ide' {
            # plugins\com.st.stm32cube.ide.mcu.externaltools.<plugin>*\tools\bin
            $dir = Get-ChildItem $Root.Path -Directory `
                       -Filter "com.st.stm32cube.ide.mcu.externaltools.$($Tool.Plugin)*" `
                       -ErrorAction SilentlyContinue |
                   Sort-Object Name -Descending |
                   ForEach-Object { Join-Path $_.FullName 'tools\bin' } |
                   Where-Object { Test-Path (Join-Path $_ $Tool.Exe) } |
                   Select-Object -First 1
            return $dir
        }
        default {
            # CLT / unknown root: layout is not guaranteed, so look for the exe.
            $hit = Get-ChildItem $Root.Path -Filter $Tool.Exe -Recurse -Depth 6 -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) { return $hit.DirectoryName }
            return $null
        }
    }
}

# --- Resolve ----------------------------------------------------------------
$roots = @(Get-InstallRoots -Explicit $Root)
if ($roots.Count -eq 0) {
    Write-Error @"
No STM32CubeCLT or STM32CubeIDE installation found.
Looked under: C:\ST, $env:ProgramFiles\STMicroelectronics, $env:LOCALAPPDATA\Programs
Pass one explicitly:  .\tools\env.ps1 -Root 'C:\ST\STM32CubeIDE_2.2.0'
"@
    exit 1
}

$resolved = @{}
$dirs     = New-Object System.Collections.Generic.List[string]
$missing  = New-Object System.Collections.Generic.List[hashtable]

foreach ($tool in $Tools) {
    $dir = $null
    foreach ($r in $roots) {
        $searched.Add($r.Path) | Out-Null
        $dir = Resolve-ToolDir -Root $r -Tool $tool
        if ($dir) { break }
    }
    if ($dir) {
        $resolved[$tool.Key] = Join-Path $dir $tool.Exe
        if (-not $dirs.Contains($dir)) { $dirs.Add($dir) }
    }
    else {
        $missing.Add($tool) | Out-Null
    }
}

$missingRequired = @($missing | Where-Object { $_.Required })

# --- Report / apply ---------------------------------------------------------
if (-not $Quiet -or $missingRequired.Count) {
    Write-Host ''
    Write-Host 'STM32 toolchain' -ForegroundColor Cyan
    foreach ($tool in $Tools) {
        if ($resolved.ContainsKey($tool.Key)) {
            $mark = '  OK   '
            $path = $resolved[$tool.Key]
        }
        else {
            $mark = if ($tool.Required) { '  MISS ' } else { '  --   ' }
            $path = "not found  ($($tool.Note))"
        }
        $colour = if ($resolved.ContainsKey($tool.Key)) { 'Green' }
                  elseif ($tool.Required)               { 'Red' }
                  else                                  { 'DarkGray' }
        Write-Host ("{0}{1,-24} {2}" -f $mark, $tool.Exe, $path) -ForegroundColor $colour
    }
    Write-Host ''
}

if ($missingRequired.Count) {
    Write-Host ("Missing required tool(s): {0}" -f (($missingRequired | ForEach-Object { $_.Exe }) -join ', ')) -ForegroundColor Red
    Write-Host ("Searched: {0}" -f (($searched | Select-Object -Unique) -join '; ')) -ForegroundColor DarkGray
    Write-Host "Point at the installation explicitly:  .\tools\env.ps1 -Root 'C:\ST\STM32CubeIDE_2.2.0'" -ForegroundColor DarkGray
    exit 1
}

if ($Check) {
    if (-not $Quiet) { Write-Host 'PATH not modified (-Check).' -ForegroundColor DarkGray }
    exit 0
}

# Prepend, skipping entries already present so repeated sourcing stays clean.
$current = $env:PATH -split ';' | Where-Object { $_ }
$toAdd   = @($dirs | Where-Object { $current -notcontains $_ })

if ($toAdd.Count) {
    $env:PATH = ($toAdd -join ';') + ';' + $env:PATH
}

if (-not $Quiet) {
    if ($toAdd.Count) {
        Write-Host ("Added {0} director{1} to PATH for this session." -f $toAdd.Count, $(if ($toAdd.Count -eq 1) { 'y' } else { 'ies' })) -ForegroundColor Green
    }
    else {
        Write-Host 'PATH already had every toolchain directory.' -ForegroundColor DarkGray
    }

    $versions = @(
        @{ Label = 'cmake'; Cmd = { (cmake --version    | Select-Object -First 1) } }
        @{ Label = 'ninja'; Cmd = { 'ninja ' + (ninja --version) } }
        @{ Label = 'gcc';   Cmd = { (arm-none-eabi-gcc --version | Select-Object -First 1) } }
    )
    foreach ($v in $versions) {
        try { Write-Host ('  ' + (& $v.Cmd)) -ForegroundColor DarkGray } catch { }
    }

    Write-Host ''
    Write-Host 'Build from the project root (where CMakePresets.json lives):' -ForegroundColor DarkGray
    Write-Host '  cmake --preset debug          # configure  (add --fresh after renaming files)' -ForegroundColor DarkGray
    Write-Host '  cmake --build --preset debug  # build' -ForegroundColor DarkGray
    Write-Host ''
}
