@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================
rem  UIプリセット (m_ui_preset_aviutl2) ビルド
rem  Usage:
rem    build.bat
rem    build.bat Release
rem    build.bat Debug
rem    build.bat clean
rem    build.bat clean Release
rem ============================================================

set "CONFIG=Release"
set "DO_CLEAN=0"
set "SUBST_DRIVE=X:"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="clean" (
  set "DO_CLEAN=1"
  shift
  goto parse_args
)
if /I "%~1"=="Debug" (
  set "CONFIG=Debug"
  shift
  goto parse_args
)
if /I "%~1"=="Release" (
  set "CONFIG=Release"
  shift
  goto parse_args
)
echo [ERROR] Unknown argument: %~1
exit /b 1
:args_done

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

subst %SUBST_DRIVE% /d >nul 2>&1
subst %SUBST_DRIVE% "%ROOT%"
if errorlevel 1 (
  echo [ERROR] subst %SUBST_DRIVE% failed.
  exit /b 1
)
if not exist "%SUBST_DRIVE%\CMakeLists.txt" (
  echo [ERROR] CMakeLists.txt not found on %SUBST_DRIVE%
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install Visual Studio with C++ desktop development.
  exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo [ERROR] Visual Studio with C++ toolset not found.
  exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  exit /b 1
)
if not exist "%CMAKE%" (
  echo [ERROR] cmake.exe not found: %CMAKE%
  exit /b 1
)
if not exist "%NINJA%" (
  echo [ERROR] ninja.exe not found: %NINJA%
  exit /b 1
)

echo === m_ui_preset_aviutl2 Build ===
echo Root   : %ROOT%
echo Drive  : %SUBST_DRIVE%
echo Config : %CONFIG%
echo.

call "%VCVARS%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to load MSVC environment.
  exit /b 1
)

if "%DO_CLEAN%"=="1" (
  echo Cleaning build...
  if exist "%SUBST_DRIVE%\build" rmdir /s /q "%SUBST_DRIVE%\build"
)

if not exist "%SUBST_DRIVE%\build" mkdir "%SUBST_DRIVE%\build"

echo ^>^>^> Configure
"%CMAKE%" -S %SUBST_DRIVE%\ -B %SUBST_DRIVE%\build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% "-DCMAKE_MAKE_PROGRAM=%NINJA%"
if errorlevel 1 (
  echo [ERROR] CMake configure failed.
  exit /b 1
)

echo ^>^>^> Build
"%CMAKE%" --build %SUBST_DRIVE%\build --target m_ui_preset_aviutl2
if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)

echo.
echo Artifacts:
dir /b "%SUBST_DRIVE%\build\plugins\*.aux2" 2>nul

echo.
echo Build succeeded.
echo Output: %ROOT%\build\plugins\m_ui_preset_aviutl2.aux2
exit /b 0
