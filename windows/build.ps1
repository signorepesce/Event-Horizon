<#
Builds event_horizon.exe on Windows without zig, using MinGW-w64 gcc or MSVC.

    powershell -ExecutionPolicy Bypass -File windows\build.ps1 [-Compiler auto|gcc|msvc] [-Run]

gcc (with windres) must be on PATH, e.g. from https://github.com/skeeto/w64devkit or
`winget install BrechtSanders.WinLibs.POSIX.UCRT`. MSVC is found through vswhere and needs the
"Desktop development with C++" workload (MSVC compiler plus a Windows SDK).
#>
param(
    [ValidateSet('auto', 'gcc', 'msvc')] [string] $Compiler = 'auto',
    [switch] $Run
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$out = 'build\windows-x64'
$exe = "$out\event_horizon.exe"
$sources = 'windows\main.c', 'core\wallpaper.c', 'core\mpv.c'
$libs = 'user32', 'comctl32', 'shell32', 'dwmapi', 'wtsapi32', 'comdlg32', 'ole32', 'advapi32', 'gdi32', 'uuid'
New-Item -ItemType Directory -Force $out | Out-Null

function Find-Msvc {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { return $null }
    $vcvars = Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $vcvars) { return $vcvars }
    return $null
}

function Invoke-Checked([string] $file, [string[]] $arguments) {
    & $file @arguments
    if ($LASTEXITCODE -ne 0) { throw "$file failed with exit code $LASTEXITCODE" }
}

$hasGcc = [bool](Get-Command gcc -ErrorAction SilentlyContinue) -and [bool](Get-Command windres -ErrorAction SilentlyContinue)
$vcvars = Find-Msvc
if ($Compiler -eq 'auto') {
    if ($hasGcc) { $Compiler = 'gcc' }
    elseif ($vcvars) { $Compiler = 'msvc' }
    else {
        throw "No C compiler found. Install MinGW-w64 (gcc + windres on PATH), e.g. " +
            "'winget install BrechtSanders.WinLibs.POSIX.UCRT', or the MSVC 'Desktop development with C++' workload."
    }
}

if ($Compiler -eq 'gcc') {
    if (-not $hasGcc) { throw 'gcc and windres must both be on PATH.' }
    Write-Host 'Compiling with gcc'
    Invoke-Checked windres @('windows\app.rc', '-O', 'coff', '-o', "$out\app.res.o")
    $warnings = '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes',
        '-Wcast-qual', '-Wundef', '-Wvla', '-Wformat=2', '-Wconversion', '-Wunused-macros', '-Wdouble-promotion',
        '-Wno-unused-parameter', '-Wno-format-truncation'
    $arguments = @('-std=c11', '-O2') + $warnings + @('-municode', '-mwindows') + $sources +
        @("$out\app.res.o", '-o', $exe) + ($libs | ForEach-Object { "-l$_" })
    Invoke-Checked gcc $arguments
    Remove-Item "$out\app.res.o"
} else {
    if (-not $vcvars) { throw 'MSVC with the C++ build tools was not found.' }
    Write-Host 'Compiling with MSVC'
    $objects = "$root\$out\obj\"
    New-Item -ItemType Directory -Force $objects | Out-Null
    $commands = @(
        "`"$vcvars`" >nul",
        "rc /nologo /fo `"$out\app.res`" windows\app.rc",
        ("cl /nologo /std:c11 /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fo`"$objects`" " +
            ($sources -join ' ') + " `"$out\app.res`" /Fe`"$exe`" /link /SUBSYSTEM:WINDOWS " +
            (($libs | ForEach-Object { "$_.lib" }) -join ' '))
    ) -join ' && '
    cmd /c $commands
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed with exit code $LASTEXITCODE" }
    Remove-Item -Recurse "$out\obj", "$out\app.res"
}
Write-Host "Built: $exe"

$mpvPlaces = (Join-Path $out 'mpv.exe'), "$env:ProgramFiles\MPV Player\mpv.exe", "$env:ProgramFiles\mpv\mpv.exe",
    "$env:LOCALAPPDATA\Programs\mpv\mpv.exe", "$env:USERPROFILE\scoop\shims\mpv.exe"
if (-not ($mpvPlaces | Where-Object { Test-Path $_ }) -and -not (Get-Command mpv.exe -ErrorAction SilentlyContinue)) {
    Write-Warning "mpv.exe not found. Put it next to $exe or on PATH (https://mpv.io), e.g. 'winget install shinchiro.mpv'."
}
if (-not (Get-ChildItem media\Gargantua-*.mp4 -ErrorAction SilentlyContinue)) {
    Write-Host 'No Gargantua-*.mp4 videos in media\; built-in mode plays the low-resolution media\gargantua.gif instead.'
}

if ($Run) {
    Start-Process (Resolve-Path $exe)
    Write-Host 'Running - look for the Event Horizon icon in the system tray.'
}
