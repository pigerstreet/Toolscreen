@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "BUILD_PRESET=release"
set "TEST_PRESET=release"
set "RUN_TESTS=0"
set "FAILURE_STEP="
set "FAILURE_CODE=1"
set "ARTIFACT_CONFIG_DIR=Release"

for %%A in (%*) do (
    if /I "%%~A"=="release" (
        set "BUILD_PRESET=release"
        set "TEST_PRESET=release"
        set "ARTIFACT_CONFIG_DIR=Release"
    ) else if /I "%%~A"=="debug" (
        set "BUILD_PRESET=debug"
        set "TEST_PRESET=debug"
        set "ARTIFACT_CONFIG_DIR=Debug"
    ) else if /I "%%~A"=="--test" (
        set "RUN_TESTS=1"
    ) else (
        goto :usage
    )
)

set "ARTIFACT_DIR=%SCRIPT_DIR%out\build\bin\%ARTIFACT_CONFIG_DIR%"

pushd "%SCRIPT_DIR%" >nul || exit /b 1

echo Configuring with preset vs2022-x64...
cmake --preset vs2022-x64
if errorlevel 1 (
    set "FAILURE_STEP=Configure preset vs2022-x64"
    set "FAILURE_CODE=10"
    goto :fail
)

echo Building DLL with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target Toolscreen
if errorlevel 1 (
    set "FAILURE_STEP=Build Toolscreen DLL"
    set "FAILURE_CODE=20"
    goto :fail
)

echo Copying DLL into EXE packaging inputs...
cmake --build --preset %BUILD_PRESET% --target toolscreen_prepare_easyinject_exe_inputs
if errorlevel 1 (
    set "FAILURE_STEP=Prepare EasyInject EXE inputs"
    set "FAILURE_CODE=30"
    goto :fail
)

echo Reconfiguring with preset vs2022-x64 so the EXE packaging step sees the copied DLL...
cmake --preset vs2022-x64
if errorlevel 1 (
    set "FAILURE_STEP=Reconfigure preset vs2022-x64 for EXE packaging"
    set "FAILURE_CODE=40"
    goto :fail
)

if exist "%ARTIFACT_DIR%\toolscreen-installer.jar" del /q "%ARTIFACT_DIR%\toolscreen-installer.jar"
if exist "%ARTIFACT_DIR%\toolscreen-installer.exe" del /q "%ARTIFACT_DIR%\toolscreen-installer.exe"
if exist "%ARTIFACT_DIR%\toolscreen-installer.pdb" del /q "%ARTIFACT_DIR%\toolscreen-installer.pdb"
if exist "%ARTIFACT_DIR%\tioolscrteen-downloader.jar" del /q "%ARTIFACT_DIR%\tioolscrteen-downloader.jar"
if exist "%ARTIFACT_DIR%\tioolscrteen-downloader.exe" del /q "%ARTIFACT_DIR%\tioolscrteen-downloader.exe"
if exist "%ARTIFACT_DIR%\tioolscrteen-downloader.pdb" del /q "%ARTIFACT_DIR%\tioolscrteen-downloader.pdb"

echo Building EXE package with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target installer_exe
if errorlevel 1 (
    set "FAILURE_STEP=Build installer EXE package"
    set "FAILURE_CODE=50"
    goto :fail
)

echo Building Toolscreen downloader EXE with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target downloader_exe
if errorlevel 1 (
    set "FAILURE_STEP=Build Toolscreen downloader EXE"
    set "FAILURE_CODE=55"
    goto :fail
)

echo Building JAR package with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target jar
if errorlevel 1 (
    set "FAILURE_STEP=Build JAR package"
    set "FAILURE_CODE=60"
    goto :fail
)

if "%RUN_TESTS%"=="1" (
    echo Running tests with preset %TEST_PRESET%...
    ctest --preset %TEST_PRESET%
    if errorlevel 1 (
        set "FAILURE_STEP=Run CTest preset %TEST_PRESET%"
        set "FAILURE_CODE=70"
        goto :fail
    )
)

popd >nul
exit /b 0

:usage
echo Usage: build.bat [release^|debug] [--test]
popd >nul 2>nul
exit /b 2

:fail
if defined FAILURE_STEP echo Build failed during: %FAILURE_STEP%
popd >nul
exit /b %FAILURE_CODE%