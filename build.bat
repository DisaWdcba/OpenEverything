@echo off
setlocal enabledelayedexpansion

set ROOT=%~dp0
set SRC_DIR=%ROOT%src
set OUT_DIR=%ROOT%build
set TEST_DIR=%ROOT%tests

set CONFIG=release
set BUILD_TESTS=0
set DO_CLEAN=0

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--debug"   set CONFIG=debug
if /i "%~1"=="--release" set CONFIG=release
if /i "%~1"=="--tests"   set BUILD_TESTS=1
if /i "%~1"=="--clean"   set DO_CLEAN=1
if /i "%~1"=="--help"    goto :usage
if /i "%~1"=="-h"        goto :usage
shift
goto :parse_args

:usage
echo Usage: build.bat [--debug^|--release] [--tests] [--clean]
echo.
echo   --debug     Unoptimized build with debug info (/Od /Zi /MTd /RTC1)
echo   --release   Optimized build (default)
echo   --tests     Also build cache, JSON, service, and GUI console tests
echo   --clean     Delete build outputs first
exit /b 0

:args_done

echo === Building OpenEverything (C/Win32, %CONFIG%) ===
echo.

if "%DO_CLEAN%"=="1" (
    echo Cleaning %OUT_DIR% ...
    if exist "%OUT_DIR%\*.obj" del /q "%OUT_DIR%\*.obj"
    if exist "%OUT_DIR%\*.res" del /q "%OUT_DIR%\*.res"
    if exist "%OUT_DIR%\OpenEverything.exe" del /q "%OUT_DIR%\OpenEverything.exe"
    if exist "%OUT_DIR%\OpenEverythingCLI.exe" del /q "%OUT_DIR%\OpenEverythingCLI.exe"
    if exist "%OUT_DIR%\OpenEverythingService.exe" del /q "%OUT_DIR%\OpenEverythingService.exe"
    if exist "%OUT_DIR%\OpenEverythingCLI.res" del /q "%OUT_DIR%\OpenEverythingCLI.res"
    if exist "%OUT_DIR%\cache_roundtrip.exe" del /q "%OUT_DIR%\cache_roundtrip.exe"
    if exist "%OUT_DIR%\json_tests.exe" del /q "%OUT_DIR%\json_tests.exe"
    if exist "%OUT_DIR%\service_client_tests.exe" del /q "%OUT_DIR%\service_client_tests.exe"
    if exist "%OUT_DIR%\gui_console_tests.exe" del /q "%OUT_DIR%\gui_console_tests.exe"
    echo.
)

:: ---- Locate the MSVC environment ----
:: vswhere output goes through a temp file rather than `for /f` backquotes:
:: the backquoted form re-parses the command through `cmd /c`, whose
:: first/last-quote stripping mangles quoted paths that contain parentheses
:: ("Program Files (x86)").
set VCVARS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSWHERE_OUT=%TEMP%\openeverything_vswhere.tmp

if exist "%VSWHERE%" (
    "%VSWHERE%" -latest -products * -find VC\Auxiliary\Build\vcvars64.bat >"%VSWHERE_OUT%" 2>nul
)
if exist "%VSWHERE_OUT%" (
    set /p VCVARS=<"%VSWHERE_OUT%"
    del /q "%VSWHERE_OUT%" >nul 2>&1
)
if defined VCVARS if exist "%VCVARS%" goto :found_vcvars

set VCVARS=
for %%r in ("%ProgramFiles(x86)%\Microsoft Visual Studio" "%ProgramFiles%\Microsoft Visual Studio") do (
    for %%y in (2022 18 17 16) do (
        for %%e in (Community Professional Enterprise BuildTools Preview) do (
            if exist "%%~r\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                set VCVARS=%%~r\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat
                goto :found_vcvars
            )
        )
    )
)

:found_vcvars

if not defined VCVARS (
    echo ERROR: MSVC Build Tools not found.
    echo Install "Desktop development with C++" from the Visual Studio Installer.
    exit /b 1
)

echo Using: %VCVARS%
:: stderr is silenced for this call only: the VS "18" vcvars64.bat internally
:: runs an unqualified `vswhere.exe` and prints a harmless "not recognized"
:: complaint when it is not on PATH. Our own compile steps stay unredirected.
call "%VCVARS%" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: Visual Studio environment did not provide cl.exe.
    exit /b 1
)

:: ---- Flags ----
:: Compiler diagnostics are deliberately NOT redirected to nul. Hiding them
:: meant a failed build printed only "FAILED: <file>.c" with no reason.
:: C4100 (unreferenced formal parameter) is unavoidable in Win32 callbacks.
set CFLAGS=/nologo /W4 /wd4100 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0600
set LDFLAGS=/nologo user32.lib kernel32.lib comctl32.lib shell32.lib comdlg32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib shlwapi.lib uxtheme.lib dwmapi.lib

if /i "%CONFIG%"=="debug" (
    set CFLAGS=%CFLAGS% /Od /Zi /MTd /D_DEBUG /RTC1
    set LDEXTRA=/DEBUG
) else (
    set CFLAGS=%CFLAGS% /O2 /MT /DNDEBUG
    set LDEXTRA=
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set MODULES=ntfs index search config cache ipc ui main

:: ---- Compile ----
echo [1/7] Compiling GUI modules (%CONFIG%) ...
set OBJS=
for %%m in (%MODULES%) do (
    echo   - %%m.c
    cl %CFLAGS% /c /Fo"%OUT_DIR%\%%m.obj" /Fd"%OUT_DIR%\OpenEverything.pdb" "%SRC_DIR%\%%m.c"
    if errorlevel 1 (
        echo.
        echo Build FAILED while compiling %%m.c ^(see the error above^).
        exit /b 1
    )
    set OBJS=!OBJS! "%OUT_DIR%\%%m.obj"
)

:: ---- Shared CLI and MCP runtime ----
echo [2/7] Compiling shared CLI and MCP runtime ...
cl %CFLAGS% /c /Fo"%OUT_DIR%\service_client.obj" /Fd"%OUT_DIR%\OpenEverythingCLI.pdb" "%SRC_DIR%\service_client.c"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling service_client.c.
    exit /b 1
)
cl %CFLAGS% /c /Fo"%OUT_DIR%\json.obj" /Fd"%OUT_DIR%\OpenEverythingCLI.pdb" "%SRC_DIR%\json.c"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling json.c.
    exit /b 1
)
cl %CFLAGS% /c /Fo"%OUT_DIR%\cli.obj" /Fd"%OUT_DIR%\OpenEverythingCLI.pdb" "%SRC_DIR%\cli.c"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling cli.c.
    exit /b 1
)
cl %CFLAGS% /c /Fo"%OUT_DIR%\cli_main.obj" /Fd"%OUT_DIR%\OpenEverythingCLI.pdb" "%SRC_DIR%\cli_main.c"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling cli_main.c.
    exit /b 1
)

:: ---- Resources ----
echo [3/7] Compiling resources ...
rc /nologo /fo "%OUT_DIR%\OpenEverything.res" "%SRC_DIR%\OpenEverything.rc"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling OpenEverything.rc.
    exit /b 1
)
rc /nologo /fo "%OUT_DIR%\OpenEverythingCLI.res" "%SRC_DIR%\OpenEverythingCLI.rc"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling OpenEverythingCLI.rc.
    exit /b 1
)

:: ---- Link dual-mode GUI ----
echo [4/7] Linking GUI with CLI and MCP support ...
cl %CFLAGS% /Fe"%OUT_DIR%\OpenEverything.exe" !OBJS! ^
    "%OUT_DIR%\cli.obj" "%OUT_DIR%\json.obj" "%OUT_DIR%\service_client.obj" ^
    "%OUT_DIR%\OpenEverything.res" /link %LDFLAGS% %LDEXTRA%
if errorlevel 1 (
    echo.
    echo Build FAILED while linking OpenEverything.exe.
    exit /b 1
)

:: ---- Standalone CLI and MCP server ----
echo [5/7] Linking CLI and MCP server ...
cl %CFLAGS% /Fe"%OUT_DIR%\OpenEverythingCLI.exe" ^
    "%OUT_DIR%\cli_main.obj" "%OUT_DIR%\cli.obj" ^
    "%OUT_DIR%\json.obj" "%OUT_DIR%\service_client.obj" ^
    "%OUT_DIR%\OpenEverythingCLI.res" ^
    "%OUT_DIR%\cache.obj" "%OUT_DIR%\config.obj" ^
    "%OUT_DIR%\index.obj" "%OUT_DIR%\search.obj" ^
    /link %LDFLAGS% %LDEXTRA%
if errorlevel 1 (
    echo.
    echo Build FAILED while linking OpenEverythingCLI.exe.
    exit /b 1
)

:: ---- Privileged indexing service ----
echo [6/7] Compiling indexing service ...
cl %CFLAGS% /c /Fo"%OUT_DIR%\service.obj" /Fd"%OUT_DIR%\OpenEverythingService.pdb" "%SRC_DIR%\service.c"
if errorlevel 1 (
    echo.
    echo Build FAILED while compiling service.c.
    exit /b 1
)

echo [7/7] Linking indexing service ...
cl %CFLAGS% /Fe"%OUT_DIR%\OpenEverythingService.exe" ^
    "%OUT_DIR%\service.obj" "%OUT_DIR%\service_client.obj" ^
    "%OUT_DIR%\cache.obj" "%OUT_DIR%\config.obj" ^
    "%OUT_DIR%\index.obj" "%OUT_DIR%\ntfs.obj" ^
    /link %LDFLAGS% %LDEXTRA%
if errorlevel 1 (
    echo.
    echo Build FAILED while linking OpenEverythingService.exe.
    exit /b 1
)

:: ---- Optional regression test ----
if "%BUILD_TESTS%"=="1" (
    echo.
    echo Building tests ...
    if not exist "%OUT_DIR%\tests" mkdir "%OUT_DIR%\tests"
    cl %CFLAGS% /I"%SRC_DIR%" /Fe"%OUT_DIR%\cache_roundtrip.exe" /Fo"%OUT_DIR%\tests\\" ^
        "%TEST_DIR%\cache_roundtrip.c" ^
        "%SRC_DIR%\cache.c" "%SRC_DIR%\index.c" "%SRC_DIR%\search.c" ^
        /link /nologo user32.lib kernel32.lib shlwapi.lib shell32.lib ole32.lib advapi32.lib psapi.lib %LDEXTRA%
    if errorlevel 1 (
        echo.
        echo Build FAILED while building tests.
        exit /b 1
    )
    echo   Output: %OUT_DIR%\cache_roundtrip.exe
    echo   Run against a directory holding an index.dat, e.g.:
    echo     build\cache_roundtrip.exe %%LOCALAPPDATA%%\OpenEverything
    echo   Note: it rewrites index.dat in that directory. Use a copy.
    cl %CFLAGS% /I"%SRC_DIR%" /Fe"%OUT_DIR%\json_tests.exe" /Fo"%OUT_DIR%\tests\\" ^
        "%TEST_DIR%\json_tests.c" "%SRC_DIR%\json.c" ^
        /link /nologo kernel32.lib %LDEXTRA%
    if errorlevel 1 (
        echo.
        echo Build FAILED while building json_tests.exe.
        exit /b 1
    )
    echo   Output: %OUT_DIR%\json_tests.exe
    cl %CFLAGS% /I"%SRC_DIR%" /Fe"%OUT_DIR%\service_client_tests.exe" /Fo"%OUT_DIR%\tests\\" ^
        "%TEST_DIR%\service_client_tests.c" "%OUT_DIR%\service_client.obj" ^
        /link /nologo kernel32.lib shell32.lib %LDEXTRA%
    if errorlevel 1 (
        echo.
        echo Build FAILED while building service_client_tests.exe.
        exit /b 1
    )
    echo   Output: %OUT_DIR%\service_client_tests.exe
    cl %CFLAGS% /I"%SRC_DIR%" /Fe"%OUT_DIR%\gui_console_tests.exe" /Fo"%OUT_DIR%\tests\\" ^
        "%TEST_DIR%\gui_console_tests.c" ^
        /link /nologo kernel32.lib user32.lib %LDEXTRA%
    if errorlevel 1 (
        echo.
        echo Build FAILED while building gui_console_tests.exe.
        exit /b 1
    )
    echo   Output: %OUT_DIR%\gui_console_tests.exe
)

echo.
echo ==========================================
echo   Build SUCCESSFUL (%CONFIG%)
echo   GUI + CLI/MCP: %OUT_DIR%\OpenEverything.exe
echo   Standalone CLI/MCP: %OUT_DIR%\OpenEverythingCLI.exe
echo   Service: %OUT_DIR%\OpenEverythingService.exe
echo ==========================================
exit /b 0
