@echo off
setlocal
set "ROOT=%~dp0.."
set "DIST=%ROOT%\dist"
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VS_VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [installer] vcvars64 failed
  exit /b 1
)

cd /d "%~dp0."

echo [installer] Uninstall.exe
rc /nologo /fo uninstall.res uninstall.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE uninstall.cpp uninstall.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib /OUT:"%DIST%\Uninstall.exe"
if errorlevel 1 exit /b 1

echo [installer] payload
python gen_payload.py "%DIST%" "%~dp0."
if errorlevel 1 exit /b 1

echo [installer] DeepSeekTraySetup.exe
rc /nologo /fo payload.res installer.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE installer.cpp payload.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib /OUT:"%DIST%\DeepSeekTraySetup.exe"
if errorlevel 1 exit /b 1

echo [installer] OK: %DIST%\DeepSeekTraySetup.exe



