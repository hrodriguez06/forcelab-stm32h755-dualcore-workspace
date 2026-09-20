<#
.SYNOPSIS
    Shim. The implementation lives once in the workspace, at tools/env.ps1.

.DESCRIPTION
    Keeping a copy per project is what let these scripts drift. This file
    exists so the documented "tools/env.ps1" still works from a project
    directory while there is only one script to maintain. Arguments are
    forwarded unchanged, so -Root, -Check and -Quiet behave as before.

    Only the process PATH is touched, so running the real script from here
    reaches the caller's session all the same.

    Override the workspace location with the STM32_WORKSPACE_TOOLS
    environment variable when a project is built outside this repository.
#>

$tools = if ($env:STM32_WORKSPACE_TOOLS) {
    $env:STM32_WORKSPACE_TOOLS
} else {
    Join-Path $PSScriptRoot '../../../tools'
}
$target = Join-Path $tools 'env.ps1'

if (-not (Test-Path $target)) {
    Write-Error "workspace tools not found at $tools - check the workspace layout, or set STM32_WORKSPACE_TOOLS"
    exit 1
}

& $target @args
