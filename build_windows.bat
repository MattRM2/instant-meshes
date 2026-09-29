@echo off
rem ---------------------------------------------------------------------------
rem  build_windows.bat -- Compile Instant Meshes (Release, x64) sous Windows
rem
rem  Utilise le CMake 3.x fourni avec Visual Studio 2022 : CMake 4.x refuse
rem  le reglage "cmake_policy(SET CMP0042 OLD)" du sous-module GLFW.
rem
rem  Usage :  build_windows.bat            compile puis attend une touche
rem           build_windows.bat clean      supprime build\ avant de compiler
rem           build_windows.bat nopause    sans pause finale (scripts, CI locale)
rem  Resultat : build\Release\InstantMeshes.exe
rem ---------------------------------------------------------------------------
setlocal EnableExtensions

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"
set "DO_CLEAN=0"
set "DO_PAUSE=1"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="clean"   set "DO_CLEAN=1"
if /i "%~1"=="nopause" set "DO_PAUSE=0"
shift
goto parse_args
:args_done

rem --- Sous-modules (nanogui, tbb, ...) -------------------------------------
if not exist "%ROOT%ext\nanogui\ext\glfw\CMakeLists.txt" (
    echo [INFO] Sous-modules absents, initialisation...
    git -C "%ROOT%." submodule update --init --recursive
    if errorlevel 1 goto fail_submodules
)

rem --- Localisation de Visual Studio 2022 via vswhere ------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto fail_vs

set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH goto fail_vs

set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE_EXE%" goto fail_cmake

echo [INFO] Visual Studio : %VS_PATH%
"%CMAKE_EXE%" --version | findstr /b "cmake"

rem --- Nettoyage optionnel ---------------------------------------------------
if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" (
    echo [INFO] Suppression de build\ ...
    rmdir /s /q "%BUILD_DIR%"
)

rem --- Configuration puis compilation ----------------------------------------
"%CMAKE_EXE%" -S "%ROOT%." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -Wno-dev -Wno-deprecated
if errorlevel 1 goto fail_build

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto fail_build

if not exist "%BUILD_DIR%\Release\InstantMeshes.exe" goto fail_build

echo.
echo [OK] Compilation terminee : %BUILD_DIR%\Release\InstantMeshes.exe

rem --- Tests unitaires (cible im_tests, option INSTANT_MESHES_TESTS) --------
if exist "%BUILD_DIR%\Release\im_tests.exe" (
    echo [INFO] Tests unitaires...
    "%BUILD_DIR%\Release\im_tests.exe" > "%BUILD_DIR%\im_tests.log" 2>&1
    if errorlevel 1 goto fail_tests
    findstr /c:" passed, " "%BUILD_DIR%\im_tests.log"
    echo [OK] Tests unitaires reussis
)

rem --- Tests de bout en bout de la ligne de commande (Python requis) --------
where python >nul 2>&1
if errorlevel 1 (
    echo [INFO] Python introuvable, tests de la ligne de commande ignores
) else (
    echo [INFO] Tests de la ligne de commande...
    python "%ROOT%tests\test_cli.py" "%BUILD_DIR%\Release\InstantMeshes.exe" > "%BUILD_DIR%\test_cli.log" 2>&1
    if errorlevel 1 goto fail_cli
    findstr /c:" passed, " "%BUILD_DIR%\test_cli.log"
    echo [OK] Tests de la ligne de commande reussis
)
set "RC=0"
goto end

:fail_submodules
echo [ERREUR] Impossible d'initialiser les sous-modules git.
set "RC=1"
goto end

:fail_vs
echo [ERREUR] Visual Studio 2022 avec les outils C++ (MSVC x64) est introuvable.
set "RC=1"
goto end

:fail_cmake
echo [ERREUR] CMake fourni avec Visual Studio introuvable :
echo          %CMAKE_EXE%
echo          Installer le composant "Outils CMake C++ pour Windows" via Visual Studio Installer.
set "RC=1"
goto end

:fail_build
echo.
echo [ERREUR] La compilation a echoue.
set "RC=1"
goto end

:fail_cli
echo.
echo [ERREUR] Des tests de la ligne de commande echouent :
type "%BUILD_DIR%\test_cli.log" | findstr /c:"FAILED" /c:" passed, "
set "RC=1"
goto end

:fail_tests
echo.
echo [ERREUR] Des tests unitaires echouent :
type "%BUILD_DIR%\im_tests.log" | findstr /c:"FAILED" /c:" passed, "
set "RC=1"
goto end

:end
if "%DO_PAUSE%"=="1" pause
exit /b %RC%
