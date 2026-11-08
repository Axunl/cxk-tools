@echo off
rem ---------------------------------------------------------------------------
rem Assemble a portable dist/ folder: exe + Qt runtime + plugins.
rem Usage: scripts\package.bat
rem
rem KEEP THIS FILE PURE ASCII (see the note in build.bat).
rem ---------------------------------------------------------------------------
setlocal

set BUILD_TYPE=%~1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release

set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
set QTDIR=C:\Qt\6.8.3\msvc2022_64
set WINDEPLOYQT=%QTDIR%\bin\windeployqt.exe

if not exist "%WINDEPLOYQT%" (
  echo [error] windeployqt not found: %WINDEPLOYQT%
  exit /b 1
)

set "ROOT=%~dp0.."
pushd "%ROOT%"

rem 1) build first, so dist always matches current sources
call scripts\build.bat %BUILD_TYPE%
if errorlevel 1 goto fail

if not exist build\bin\cxk-tools.exe (
  echo [error] build\bin\cxk-tools.exe not found
  goto fail
)

rem 2) fresh dist
if exist dist rmdir /s /q dist
mkdir dist
mkdir dist\plugins

copy /y build\bin\cxk-tools.exe dist\ >nul
if errorlevel 1 goto fail

rem 3) our own tool plugins live next to the exe, in plugins\
copy /y build\bin\plugins\*.dll dist\plugins\ >nul 2>&1

rem 4) pull in the Qt runtime (skip the parts a widgets-only app never uses)
"%WINDEPLOYQT%" --release --no-system-d3d-compiler --no-quick-import dist\cxk-tools.exe
if errorlevel 1 goto fail

echo.
echo PACKAGE_OK  --^>  %ROOT%\dist
popd
exit /b 0

:fail
echo.
echo PACKAGE_FAILED
popd
exit /b 1
