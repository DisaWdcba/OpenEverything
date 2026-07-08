@echo off
setlocal enabledelayedexpansion

set SRC_DIR=%~dp0src
set OUT_DIR=%~dp0build

echo === Building OpenEverything (C/Win32) ===
echo.

:: Find vcvars64.bat
set VCVARS=

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -find "VC\Auxiliary\Build\vcvars64.bat" 2^>nul`) do (
        set VCVARS=%%i
        goto :found
    )
)

for %%r in ("%ProgramFiles(x86)%\Microsoft Visual Studio" "%ProgramFiles%\Microsoft Visual Studio") do (
    for %%y in (18 17 16) do (
        for %%e in (Community Professional Enterprise BuildTools Preview) do (
            if exist "%%~r\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                set VCVARS=%%~r\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat
                goto :found
            )
        )
    )
)
:found

if "%VCVARS%"=="" (
    echo ERROR: MSVC Build Tools not found.
    echo Please install Visual Studio Build Tools.
    exit /b 1
)

echo Using: %VCVARS%
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to initialize Visual Studio environment.
    exit /b 1
)

:: Compile flags
set CFLAGS=/nologo /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0600
set LFLAGS=/nologo user32.lib kernel32.lib comctl32.lib shell32.lib comdlg32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib shlwapi.lib uxtheme.lib

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo.
echo [1/5] Compiling core modules...
cl %CFLAGS% /c /Fo"%OUT_DIR%\\ntfs.obj" "%SRC_DIR%\ntfs.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: ntfs.c & goto :fail

cl %CFLAGS% /c /Fo"%OUT_DIR%\\index.obj" "%SRC_DIR%\index.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: index.c & goto :fail

cl %CFLAGS% /c /Fo"%OUT_DIR%\\search.obj" "%SRC_DIR%\search.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: search.c & goto :fail

cl %CFLAGS% /c /Fo"%OUT_DIR%\\config.obj" "%SRC_DIR%\config.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: config.c & goto :fail

cl %CFLAGS% /c /Fo"%OUT_DIR%\\cache.obj" "%SRC_DIR%\cache.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: cache.c & goto :fail

cl %CFLAGS% /c /Fo"%OUT_DIR%\\ipc.obj" "%SRC_DIR%\ipc.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: ipc.c & goto :fail

echo [2/5] Compiling UI...
cl %CFLAGS% /c /Fo"%OUT_DIR%\\ui.obj" "%SRC_DIR%\ui.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: ui.c & goto :fail

echo [3/5] Compiling main...
cl %CFLAGS% /c /Fo"%OUT_DIR%\\main.obj" "%SRC_DIR%\main.c" /link >nul 2>&1
if errorlevel 1 echo FAILED: main.c & goto :fail

echo [4/5] Compiling resources...
rc /nologo /fo "%OUT_DIR%\\OpenEverything.res" "%SRC_DIR%\OpenEverything.rc"
if errorlevel 1 echo FAILED: OpenEverything.rc & goto :fail

echo [5/5] Linking...
cl %CFLAGS% /Fe"%OUT_DIR%\OpenEverything.exe" ^
    "%OUT_DIR%\ntfs.obj" ^
    "%OUT_DIR%\index.obj" ^
    "%OUT_DIR%\search.obj" ^
    "%OUT_DIR%\config.obj" ^
    "%OUT_DIR%\cache.obj" ^
    "%OUT_DIR%\ipc.obj" ^
    "%OUT_DIR%\ui.obj" ^
    "%OUT_DIR%\main.obj" ^
    "%OUT_DIR%\OpenEverything.res" ^
    /link %LFLAGS%

if errorlevel 1 goto :fail

echo.
echo ==========================================
echo   Build SUCCESSFUL
echo   Output: %OUT_DIR%\OpenEverything.exe
echo ==========================================
goto :end

:fail
echo.
echo ==========================================
echo   Build FAILED
echo   Run with verbose output for details:
echo   cl %CFLAGS% /c src\file.c
echo ==========================================
endlocal
exit /b 1

:end
endlocal
exit /b 0
