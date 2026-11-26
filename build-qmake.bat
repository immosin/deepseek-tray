@echo off
setlocal
set "QT_DIR=C:\Qt\5.15.2\msvc2022"
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0"
set "BUILD=%ROOT%build-qmake"
set "LOG=%ROOT%build-qmake.log"

call "%VS_VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [build] vcvars64 failed > "%LOG%"
  exit /b 1
)
set "PATH=%QT_DIR%\bin;%PATH%"

if not exist "%BUILD%" mkdir "%BUILD%"
pushd "%BUILD%"

echo [build] qmake > "%LOG%"
qmake.exe "%ROOT%deepseek-tray.pro" -spec win32-msvc "CONFIG+=release" >> "%LOG%" 2>&1
if errorlevel 1 ( echo [build] qmake failed & popd & exit /b 1 )

echo [build] nmake >> "%LOG%"
nmake /NOLOGO >> "%LOG%" 2>&1
if errorlevel 1 ( echo [build] nmake failed & popd & exit /b 1 )

popd
echo [build] OK
exit /b 0
