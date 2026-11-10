@echo off
rem ---------------------------------------------------------------------------
rem Build cxk-tools on Windows (MSVC + Qt6 + Ninja).
rem Usage: scripts\build.bat [Debug^|Release]
rem
rem IMPORTANT: keep this file PURE ASCII. cmd.exe parses batch files with the
rem local code page (GBK/936 on Chinese Windows); UTF-8 comments swallow line
rem breaks and the whole script dies.
rem ---------------------------------------------------------------------------
setlocal

set BUILD_TYPE=%~1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release

rem Machine-specific toolchain locations. Change these when moving to a new PC.
set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
set QTDIR=C:\Qt\6.8.3\msvc2022_64
set CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe
set NINJA=C:\Qt\Tools\Ninja\ninja.exe

if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [error] vcvars64.bat not found: %VSDIR%
  exit /b 1
)
if not exist "%QTDIR%\bin\Qt6Core.dll" (
  echo [error] Qt not found: %QTDIR%
  exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

rem Windows SDK fallback: in restricted environments vcvars may fail to locate
rem the SDK (reg.exe blocked by policy), so add the newest installed SDK by hand.
if not "%WindowsSdkDir%"=="" goto sdk_ok
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%v"
if "%SDKVER%"=="" goto sdk_ok
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"
set "PATH=%PATH%;%SDKROOT%\bin\%SDKVER%\x64"
set "WindowsSdkDir=%SDKROOT%\"
set "WindowsSDKVersion=%SDKVER%\"
echo [env] Windows SDK fallback in use: %SDKVER%
:sdk_ok

set "ROOT=%~dp0.."
pushd "%ROOT%"

rem ---- onnxruntime (optional PP-OCR backend) ---------------------------------
rem Detect a locally unpacked onnxruntime package and pass it to CMake. Absent, the
rem build still succeeds but the OCR plugin falls back to system OCR / tesseract
rem (FindOnnxruntime.cmake is "not found is not an error" by design).
rem Use Onnxruntime_ROOT if already set, else look under build\_third_party.
rem (Onnxruntime_ROOT is the CMake-canonical name that find_package auto-honours;
rem  FindOnnxruntime.cmake also accepts the legacy ONNXRUNTIME_ROOT spelling.)
set "ONNX_ARGS="
if not defined Onnxruntime_ROOT if exist "%ROOT%\build\_third_party\onnxruntime-win-x64\include\onnxruntime_cxx_api.h" set "Onnxruntime_ROOT=%ROOT%\build\_third_party\onnxruntime-win-x64"
if defined Onnxruntime_ROOT if exist "%Onnxruntime_ROOT%\include\onnxruntime_cxx_api.h" set "ONNX_ARGS=-DOnnxruntime_ROOT=%Onnxruntime_ROOT%"
if defined ONNX_ARGS echo [env] onnxruntime found: %Onnxruntime_ROOT%

"%CMAKE%" -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
  -DCMAKE_PREFIX_PATH=%QTDIR% ^
  -DCMAKE_MAKE_PROGRAM=%NINJA% ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl %ONNX_ARGS%
if errorlevel 1 goto fail

"%CMAKE%" --build build --parallel
if errorlevel 1 goto fail

echo.
echo BUILD_OK  --^>  %ROOT%\build\bin\cxk-tools.exe
popd
exit /b 0

:fail
echo.
echo BUILD_FAILED
popd
exit /b 1
