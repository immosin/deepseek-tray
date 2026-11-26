#Requires -Version 5.1
<#
    Полная сборка: приложение -> dist (Qt DLL) -> Uninstall.exe -> DeepSeekTraySetup.exe.
    Требуется: Qt 5.15.2 (msvc2022), Visual Studio 2022, Python 3 (генератор payload).
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$qtBin = 'C:\Qt\5.15.2\msvc2022\bin'

& (Join-Path $root 'build-qmake.bat')
if ($LASTEXITCODE -ne 0) { throw 'Сборка не удалась (build-qmake.bat).' }

$exe = Join-Path $root 'build-qmake\DeepSeekTray.exe'
$dist = Join-Path $root 'dist'
if (-not (Test-Path $exe)) { throw "Нет $exe" }

Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item $exe $dist

& (Join-Path $qtBin 'windeployqt.exe') --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw --compiler-runtime --dir $dist (Join-Path $dist 'DeepSeekTray.exe') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'windeployqt не удался.' }

& (Join-Path $root 'installer\build-installer.bat')
if ($LASTEXITCODE -ne 0) { throw 'Сборка установщика не удалась.' }

$sizeMb = [math]::Round(((Get-ChildItem $dist -Recurse | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "Готово: $dist ($sizeMb МБ), установщик dist\DeepSeekTraySetup.exe"
