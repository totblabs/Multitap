@echo off
setlocal
set "MULTITAP_ROOT=%~dp0"
set "PUB_CACHE=%MULTITAP_ROOT%.tools\pub-cache"
set "CI=true"
set "FLUTTER_SUPPRESS_ANALYTICS=true"
if not exist "%MULTITAP_ROOT%.tools\flutter\bin\flutter.bat" (
  echo Flutter SDK is missing. Run tools\setup-flutter.ps1 first.
  exit /b 1
)
call "%MULTITAP_ROOT%.tools\flutter\bin\flutter.bat" --suppress-analytics %*
exit /b %errorlevel%
