@echo off
setlocal
set "MULTITAP_INSTALLER=%~dp0.tools\downloads\vs_BuildTools.exe"
if not exist "%MULTITAP_INSTALLER%" (
  echo Microsoft Build Tools installer is missing.
  echo Download: https://aka.ms/vs/17/release/vs_buildtools.exe
  pause
  exit /b 1
)
echo Installing Microsoft Windows C++ build prerequisites for Multitap.
echo Windows may ask you to approve administrator access.
echo Configuration: %~dp0tools\windows-buildtools.vsconfig
"%MULTITAP_INSTALLER%" --passive --wait --norestart --installPath "%~dp0.tools\visualstudio-buildtools" --config "%~dp0tools\windows-buildtools.vsconfig" --addProductLang en-US
set "MULTITAP_RESULT=%errorlevel%"
echo Installer exit code: %MULTITAP_RESULT%
pause
exit /b %MULTITAP_RESULT%
