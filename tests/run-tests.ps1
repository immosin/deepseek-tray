#Requires -Version 5.1
<#
    Проверка запросов и разбора JSON на локальном mock-сервере (без сети и TLS).
    Запуск: powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'dist\DeepSeekTray.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $root 'build-qmake\DeepSeekTray.exe' }
if (-not (Test-Path $exe)) { throw 'Сначала соберите проект (build-qmake.bat).' }

$python = (Get-Command python).Source
$dataDir = Join-Path $root 'test-data'
Remove-Item $dataDir -Recurse -Force -ErrorAction SilentlyContinue
$env:DSBAL_DATA_DIR = $dataDir
$env:PATH = "C:\Windows\System32;C:\Windows"

$mock = Start-Process -FilePath $python -ArgumentList (Join-Path $PSScriptRoot 'mock_api.py') -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$failed = 0
function Invoke-Case {
    param([string]$Name, [string]$Url, [int]$ExpectedExit, [string[]]$ExpectOut)
    $out = & $exe --check --url $Url 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ok = ($code -eq $ExpectedExit)
    foreach ($fragment in $ExpectOut) {
        if ($out -notmatch [regex]::Escape($fragment)) { $ok = $false }
    }
    if ($ok) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else {
        Write-Host "  [FAIL] $Name (exit=$code)`n$out" -ForegroundColor Red
        $script:failed++
    }
}

try {
    Write-Host 'Проверки:'
    Invoke-Case 'баланс 200'      'http://127.0.0.1:8765/user/balance' 0 @('available=1', '44.10 CNY', '2.24 USD')
    Invoke-Case '401 неверный ключ' 'http://127.0.0.1:8765/unauth'     4 @('401')
    Invoke-Case '500 сервер'      'http://127.0.0.1:8765/boom'         1 @('500')

    if (Test-Path (Join-Path $dataDir 'last-balance.json')) {
        Write-Host '  [ok]   last-balance.json сохранён' -ForegroundColor Green
    } else {
        Write-Host '  [FAIL] last-balance.json не создан' -ForegroundColor Red
        $failed++
    }
}
finally {
    Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue
}

if ($failed -gt 0) { Write-Host "$failed проверок провалено" -ForegroundColor Red; exit 1 }
Write-Host 'Все проверки пройдены.' -ForegroundColor Green
