@echo off
REM Generates the Visual Studio project files (.vcxproj) next to vs\Nxrth.sln.
REM The .sln is committed; the per-machine project files are generated here so
REM they carry YOUR local source/vcpkg paths (they are gitignored).
REM
REM Requires: CMake (3.21+) and vcpkg. Set VCPKG_ROOT or keep vcpkg at C:\vcpkg.
setlocal
set "VCPKG=%VCPKG_ROOT%"
if "%VCPKG%"=="" set "VCPKG=C:\vcpkg"
if not exist "%VCPKG%\scripts\buildsystems\vcpkg.cmake" (
  echo [generate-vs] vcpkg not found at "%VCPKG%". Set VCPKG_ROOT and retry.
  exit /b 1
)
cmake -S "%~dp0.." -B "%~dp0..\vs" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG%\scripts\buildsystems\vcpkg.cmake"
if errorlevel 1 exit /b 1
echo.
echo [generate-vs] Done. Open vs\Nxrth.sln in Visual Studio.
