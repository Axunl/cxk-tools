@echo off
rem ---------------------------------------------------------------------------
rem Build the Windows distributables: portable zip + Inno Setup installer.
rem
rem Usage: scripts\package.bat [Release|Debug] [--no-installer]
rem
rem This script is environment driven so the same file works locally and in CI:
rem   QTDIR       Qt kit directory (default: C:\Qt\6.8.3\msvc2022_64)
rem   QT_ROOT_DIR Qt kit directory used by install-qt-action (honoured as fallback)
rem   CMAKE       cmake executable (default: cmake from PATH)
rem
rem Steps: configure+build -> cmake --install into dist\windows-x64 -> windeployqt
rem        -> prune redundant Qt runtime -> zip -> Inno Setup (if ISCC available).
rem
rem IMPORTANT: keep this file PURE ASCII. cmd.exe parses batch files with the
rem local code page (GBK/936 on Chinese Windows); UTF-8 comments swallow line
rem breaks and the whole script dies.
rem ---------------------------------------------------------------------------
setlocal

set "ROOT=%~dp0.."
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"
if /i "%BUILD_TYPE%"=="--no-installer" set "BUILD_TYPE=Release"
set "MAKE_INSTALLER=1"
for %%a in (%*) do if /i "%%~a"=="--no-installer" set "MAKE_INSTALLER=0"

rem ---- Qt kit -----------------------------------------------------------------
if not defined QTDIR if defined QT_ROOT_DIR set "QTDIR=%QT_ROOT_DIR%"
if not defined QTDIR set "QTDIR=C:\Qt\6.8.3\msvc2022_64"
if not exist "%QTDIR%\bin\Qt6Core.dll" (
    echo [error] Qt kit not found: %QTDIR%
    echo         set QTDIR to a Qt 6 msvc kit directory
    exit /b 1
)

rem ---- cmake ------------------------------------------------------------------
set "CMAKE_CMD=%CMAKE%"
if not defined CMAKE_CMD (
    for /f "delims=" %%c in ('where cmake 2^>nul') do if not defined CMAKE_CMD set "CMAKE_CMD=%%c"
)
if not defined CMAKE_CMD if exist "C:\Qt\Tools\CMake_64\bin\cmake.exe" set "CMAKE_CMD=C:\Qt\Tools\CMake_64\bin\cmake.exe"
if not defined CMAKE_CMD (
    echo [error] cmake not found
    exit /b 1
)

rem ---- ninja (optional; falls back to the Visual Studio generator) -------------
set "NINJA_CMD="
if exist "C:\Qt\Tools\Ninja\ninja.exe" set "NINJA_CMD=C:\Qt\Tools\Ninja\ninja.exe"
if not defined NINJA_CMD (
    for /f "delims=" %%n in ('where ninja 2^>nul') do if not defined NINJA_CMD set "NINJA_CMD=%%n"
)

rem ---- MSVC environment -------------------------------------------------------
rem Only needed when cl is not already on PATH (CI sets it up via msvc-dev-cmd).
rem Kept at top level (no if-blocks) on purpose: a label-based fallback inside a
rem parenthesized block does not work in cmd, and build.bat relies on the same
rem structure for the Windows SDK workaround below.
where cl >nul 2>&1
if not errorlevel 1 goto msvc_ready

set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [error] no MSVC environment and no vcvars64.bat found
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

rem Windows SDK fallback: vcvars cannot locate the SDK when reg.exe is blocked by
rem policy, so add the newest installed SDK by hand (same trick as build.bat).
if not "%WindowsSdkDir%"=="" goto msvc_ready
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%v"
if "%SDKVER%"=="" goto msvc_ready
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"
set "PATH=%PATH%;%SDKROOT%\bin\%SDKVER%\x64"
echo [env] Windows SDK fallback in use: %SDKVER%

:msvc_ready

pushd "%ROOT%"

rem ---- configure + build ------------------------------------------------------
if not exist "build\CMakeCache.txt" (
    echo [step] configure
    if defined NINJA_CMD (
        "%CMAKE_CMD%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
            -DCMAKE_PREFIX_PATH=%QTDIR% -DCMAKE_MAKE_PROGRAM=%NINJA_CMD% ^
            -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
    ) else (
        "%CMAKE_CMD%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=%QTDIR%
    )
    if errorlevel 1 goto fail
)

echo [step] build
"%CMAKE_CMD%" --build build --config %BUILD_TYPE% --parallel
if errorlevel 1 goto fail

rem Version comes from build\xtools-version.txt, written by CMake from project(VERSION).
set "VERSION="
if exist "build\xtools-version.txt" set /p VERSION=<"build\xtools-version.txt"
if not defined VERSION (
    echo [error] build\xtools-version.txt missing - configure failed?
    goto fail
)
echo [info] version %VERSION%

rem ---- staging ----------------------------------------------------------------
rem cmake --install is the single staging entry point on all platforms: it puts
rem the executable at the root and plugins in plugins\, exactly like build\bin.
set "STAGE=dist\windows-x64"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
echo [step] install to %STAGE%
"%CMAKE_CMD%" --install build --prefix "%STAGE%" --config %BUILD_TYPE%
if errorlevel 1 goto fail

rem ---- Qt runtime -------------------------------------------------------------
set "WINDEPLOYQT=%QTDIR%\bin\windeployqt.exe"
if not exist "%WINDEPLOYQT%" (
    set "WINDEPLOYQT="
    for /f "delims=" %%w in ('where windeployqt 2^>nul') do if not defined WINDEPLOYQT set "WINDEPLOYQT=%%w"
)
if not defined WINDEPLOYQT (
    echo [error] windeployqt not found - cannot produce a runnable package
    goto fail
)

rem Size matters here, hence the aggressive switches:
rem   --no-system-dxc-compiler  skips dxcompiler.dll + dxil.dll (~15 MB, Qt Quick only)
rem   --no-opengl-sw            skips opengl32sw.dll (~20 MB, no OpenGL here)
rem   --skip-plugin-types       drops the SVG icon engine and the TUIO plugin
rem   --no-compiler-runtime     skips vc_redist.x64.exe (~25 MB installer!)
rem The CRT is still shipped, just as app-local DLLs copied below: that is what
rem Microsoft allows, it costs ~1 MB, and it saves users from "VCRUNTIME140.dll
rem is missing" without shipping (and silently running) a 25 MB redist package.
echo [step] windeployqt
"%WINDEPLOYQT%" --release --no-translations --no-system-d3d-compiler ^
    --no-system-dxc-compiler --no-opengl-sw --no-ffmpeg --no-quick-import ^
    --no-compiler-runtime --skip-plugin-types iconengines,generic ^
    "%STAGE%\cxk-tools.exe"
if errorlevel 1 goto fail

rem ---- app-local VC runtime ---------------------------------------------------
rem vcvars / msvc-dev-cmd export VCToolsRedistDir; when neither ran (cl already on
rem PATH) we ask PowerShell to pick the newest installed redist under Visual Studio.
set "CRTDIR="
if defined VCToolsRedistDir for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do if not defined CRTDIR set "CRTDIR=%%d"
if defined CRTDIR (
    echo [step] copy VC runtime from %CRTDIR%
    copy /y "%CRTDIR%\*.dll" "%STAGE%\" >nul
) else (
    echo [step] locate VC runtime under Visual Studio
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\VC\Redist\MSVC\*\x64' -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1; if ($crt) { Copy-Item (Join-Path $crt.FullName '*.dll') '%STAGE%\' -Force; Write-Host ('CRT_SOURCE=' + $crt.FullName) } else { Write-Host 'CRT_NOT_FOUND' }"
)

echo [step] prune redundant Qt runtime
"%CMAKE_CMD%" -DXTools_STAGE_DIR="%ROOT%\%STAGE%" -P "%ROOT%\cmake\PruneQtRuntime.cmake"
if errorlevel 1 goto fail

rem ---- portable zip -----------------------------------------------------------
set "ZIP=%ROOT%\dist\cxk-tools-%VERSION%-windows-x64.zip"
set "SETUP=%ROOT%\dist\cxk-tools-%VERSION%-windows-x64-setup.exe"
rem Delete this run's outputs first: if the installer step is skipped, a stale
rem setup.exe from a previous run would make "skipped" look like "succeeded"
rem and would also poison the size report below.
if exist "%ZIP%" del /q "%ZIP%"
if exist "%SETUP%" del /q "%SETUP%"
echo [step] zip
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; Compress-Archive -Path '%ROOT%\%STAGE%\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal -Force"
if errorlevel 1 goto fail

rem ---- Inno Setup installer ---------------------------------------------------
rem KEEP EVERYTHING BELOW AT TOP LEVEL. Two cmd traps broke the first CI run
rem (both reproduced locally, see AGENTS.md troubleshooting table):
rem   1) %VAR% is expanded when the whole if-block is parsed, so "set then use
rem      in the same block" yields the stale/empty value -> '""' is not recognized;
rem   2) %ProgramFiles(x86)% contains ')' which closes the block early, and the
rem      leftover text runs as a command -> "'C:\Program' is not recognized".
rem Hence: labels + goto, and the search lives in a call-ed subroutine.
if "%MAKE_INSTALLER%"=="0" goto skip_installer
call :find_iscc
if not defined ISCC goto no_iscc
echo [step] Inno Setup: %ISCC%
"%ISCC%" /DAppVersion=%VERSION% installer\windows\cxk-tools.iss
if errorlevel 1 goto fail
goto skip_installer

:no_iscc
rem CI must ship an installer, so release.yml exports XTOOLS_REQUIRE_INSTALLER=1
rem and a missing ISCC fails the build instead of silently publishing a release
rem without setup.exe. Locally the same situation is only a warning.
if defined XTOOLS_REQUIRE_INSTALLER goto require_iscc
echo [warn] ISCC.exe not found - installer skipped, portable zip only
echo        install Inno Setup 6 (https://jrsoftware.org/isdl.php) or set ISCC=...
goto skip_installer

:require_iscc
echo [error] ISCC.exe not found but XTOOLS_REQUIRE_INSTALLER is set
echo         install Inno Setup 6 (https://jrsoftware.org/isdl.php) or set ISCC=...
goto fail

:skip_installer
rem ---- report -----------------------------------------------------------------
echo.
powershell -NoProfile -Command ^
    "$s=(Get-ChildItem -Recurse -File '%ROOT%\%STAGE%' | Measure-Object -Sum Length).Sum; Write-Host ('STAGE_BYTES=' + $s + '  (' + [math]::Round($s/1MB,1) + ' MB)')"
for %%z in ("%ZIP%") do echo ZIP_BYTES=%%~zz
if exist "%SETUP%" for %%s in ("%SETUP%") do echo SETUP_BYTES=%%~zs
echo.
echo PACKAGE_OK  --^>  %ROOT%\dist
popd
exit /b 0

rem ---- helpers ----------------------------------------------------------------
rem Locate ISCC.exe: env ISCC -> PATH -> per-user install -> machine-wide install.
rem A subroutine (not an if-block) for the reasons listed at the Inno section.
:find_iscc
rem Explorer's "Copy as path" hands out a quoted string; strip quotes so the
rem call below can wrap it safely (a quoted value would become ""C:\..."" ).
if defined ISCC set "ISCC=%ISCC:"=%"
if defined ISCC exit /b 0
for /f "delims=" %%i in ('where iscc 2^>nul') do if not defined ISCC set "ISCC=%%i"
if defined ISCC exit /b 0
if not defined LOCALAPPDATA goto :find_iscc_pf
if not exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" goto :find_iscc_pf
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
exit /b 0
:find_iscc_pf
if not exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" goto :find_iscc_pf64
set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
exit /b 0
:find_iscc_pf64
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
exit /b 0

:fail
echo.
echo PACKAGE_FAILED
popd
exit /b 1
