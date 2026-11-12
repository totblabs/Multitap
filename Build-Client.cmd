@echo off
setlocal
if /I "%~1"=="--portable" goto portable
pushd "%~dp0apps\windows_client"
call "%~dp0flutter.cmd" build windows --release
set "MULTITAP_RESULT=%errorlevel%"
popd
exit /b %MULTITAP_RESULT%

:portable
if not "%~2"=="" (
  echo Use tools\build-portable-windows.ps1 for custom portable build options.
  exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-portable-windows.ps1" -Variant Client
exit /b %errorlevel%
