# PowerShell version script for GitHub Actions compatibility
# Generates src/Version.h with date-based version numbers

$utc = (Get-Date).ToUniversalTime()

$year = $utc.Year
$month = $utc.Month
$day = $utc.Day
$hour = $utc.Hour
$minute = $utc.Minute

$revision = $hour * 60 + $minute

$versionHeader = @"
#pragma once
#define V_MAJOR $year
#define V_MINOR $month
#define V_BUILD $day
#define V_REVISION $revision
"@

# Get the script directory (should be src/)
if ($PSScriptRoot) {
    $srcDir = $PSScriptRoot
} else {
    $srcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}

$versionFile = Join-Path $srcDir "version.h"
$versionHeader | Out-File -FilePath $versionFile -Encoding ASCII -NoNewline

Write-Host "Generated version.h: V_MAJOR=$year V_MINOR=$month V_BUILD=$day V_REVISION=$revision"