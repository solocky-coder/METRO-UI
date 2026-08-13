@echo off
setlocal

set "BUILD_DIR=build-experimental-multigroup"
set "TOOLCHAIN="

echo ============================================
echo  DYSEKT-SF - Experimental Multi-Group Build
echo ============================================
echo.
echo WARNING: Debug test build only. Do not ship it.
echo.

if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
) else if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake"
) else (
    echo [WARNING] vcpkg not found at C:\vcpkg or %USERPROFILE%\vcpkg
    echo [WARNING] Continuing without vcpkg -- FluidSynth may not resolve.
    echo.
)

echo [1/3] Removing old experimental build folder...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if errorlevel 1 goto :error

echo [2/3] Configuring Debug with experimental multi-group enabled...
cmake -B "%BUILD_DIR%" -S . ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DDYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP=ON ^
    %TOOLCHAIN%
if errorlevel 1 goto :error

echo [3/3] Building Debug...
cmake --build "%BUILD_DIR%" --config Debug
if errorlevel 1 goto :error

echo.
echo ============================================
echo  Experimental Debug build complete.
echo ============================================
echo Toggle EXP: MULTI-GROUP, then load or reload an SF2 file.
echo The toggle does not affect an instrument that is already loaded.
echo.
pause
exit /b 0

:error
echo.
echo [ERROR] Experimental build failed. See the output above.
pause
exit /b 1
