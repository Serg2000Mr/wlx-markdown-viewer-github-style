@echo off
setlocal enabledelayedexpansion

:: Set working directory to the script's directory
cd /d "%~dp0"

:: Remove trailing backslash from current directory
set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

echo.
echo === Build Script for wlx-markdown-viewer (Native AOT) ===
echo.

:: Close Total Commander if running
set "TC_WAS_RUNNING="
tasklist /FI "IMAGENAME eq TOTALCMD64.EXE" 2>nul | find /I "TOTALCMD64.EXE" >nul && set "TC_WAS_RUNNING=1"

echo Closing TOTALCMD64.EXE if running...
taskkill /IM TOTALCMD64.EXE /F >nul 2>&1
if %ERRORLEVEL% EQU 0 (echo TOTALCMD64.EXE closed.) else (echo TOTALCMD64.EXE was not running.)
echo.

:: --- Configuration for Visual Studio 2026 (Internal version 18) ---
set "VS_PATH=C:\Program Files\Microsoft Visual Studio\18\Community"
set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"
set "DOTNET=dotnet"

:: 1. Check MSBuild
if not exist "%MSBUILD%" (
    echo [DEBUG] MSBuild not found at expected path: %MSBUILD%
    echo Searching for MSBuild.exe in %VS_PATH%...
    for /f "delims=" %%i in ('where /R "%VS_PATH%" MSBuild.exe 2^>nul') do (
        set "MSBUILD=%%i"
        goto :found_msbuild
    )
    
    set "BT_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    echo Searching for MSBuild.exe in %BT_PATH%...
    for /f "delims=" %%i in ('where /R "%BT_PATH%" MSBuild.exe 2^>nul') do (
        set "MSBUILD=%%i"
        goto :found_msbuild
    )

    echo ERROR: MSBuild.exe not found. Please ensure Visual Studio 2026 is installed.
    timeout /t 5
    exit /b 1
)

:found_msbuild
echo Found MSBuild: "%MSBUILD%"

:: 2. Check Dotnet SDK
where dotnet >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Found dotnet in PATH
) else (
    echo dotnet not found in PATH, searching in %VS_PATH%...
    for /f "delims=" %%i in ('where /R "%VS_PATH%" dotnet.exe 2^>nul') do (
        set "DOTNET=%%i"
        goto :found_dotnet
    )
    echo ERROR: dotnet.exe not found. Please install .NET 8 SDK.
    timeout /t 5
    exit /b 1
)

:found_dotnet
echo Using dotnet: "%DOTNET%"

:: Create output directory
if not exist "bin\Release" mkdir "bin\Release"

:: --- 1. Build MarkdigNative (C# Native AOT) ---
echo.
echo === Building MarkdigNative (C# Native AOT) ===

:: Build x64
echo [x64] Building MarkdigNative...
cd MarkdigNative
"%DOTNET%" publish -r win-x64 -c Release /p:PublishAot=true /p:AssemblyName=MarkdigNative-x64
if %ERRORLEVEL% NEQ 0 (
    echo ERROR building MarkdigNative x64
    timeout /t 5
    exit /b %ERRORLEVEL%
)
copy /y bin\Release\net8.0\win-x64\publish\MarkdigNative-x64.dll ..\bin\Release\
copy /y bin\Release\net8.0\win-x64\publish\MarkdigNative-x64.pdb ..\bin\Release\

:: Build x86
echo [x86] NOTE: Native AOT is NOT supported for x86. Skipping...
cd ..

:: --- 2. Build Markdown (C++ Bridge) ---
echo.
echo === Building Markdown (C++ Bridge) ===

:: Build x64
echo [x64] Building Markdown bridge...
:: MSBuild trick: pass path with double backslash at the end to prevent quote escaping
"%MSBUILD%" Markdown\Markdown.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="%ROOT_DIR%\\"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR building Markdown x64
    timeout /t 5
    exit /b %ERRORLEVEL%
)

:: --- 3. Build MarkdownView (Lister Plugin) ---
echo.
echo === Building MarkdownView (Lister Plugin) ===

:: Build x64
echo [x64] Building MarkdownView...
"%MSBUILD%" MarkdownView\MarkdownView.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="%ROOT_DIR%\\"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR building MarkdownView x64
    timeout /t 5
    exit /b %ERRORLEVEL%
)

echo.
echo Required files for distribution (x64):
echo   - MarkdownView.wlx64
echo   - Markdown-x64.dll
echo   - MarkdigNative-x64.dll
echo.

:: Copy resources
echo Copying resources to bin\Release...
copy /y Build\MarkdownView.ini bin\Release\ >nul
xcopy /s /e /y /i Build\css bin\Release\css >nul

:: --- 4. Update installed plugin (Total Commander already closed above) ---
set "TC_PLUGIN=c:\Program Files\totalcmd\plugins\wlx\MarkdownView"
echo.
echo === Updating installed plugin at %TC_PLUGIN% ===
if not exist "%TC_PLUGIN%" (
    echo Creating folder...
    mkdir "%TC_PLUGIN%"
)
if not exist "%TC_PLUGIN%\css" (
    mkdir "%TC_PLUGIN%\css"
)
if not exist "%TC_PLUGIN%" (
    echo ERROR: Cannot access "%TC_PLUGIN%".
    echo        Most likely you need to run BuildAll.bat "as Administrator" to write into Program Files.
    echo        Skipping installed plugin update.
) else (
    copy /y bin\Release\MarkdownView.wlx64 "%TC_PLUGIN%"
    copy /y bin\Release\Markdown-x64.dll "%TC_PLUGIN%"
    copy /y bin\Release\MarkdigNative-x64.dll "%TC_PLUGIN%"
    copy /y bin\Release\MarkdownView.ini "%TC_PLUGIN%"
    xcopy /s /e /y /i bin\Release\css "%TC_PLUGIN%\css"
    echo Plugin updated.
)

echo.
echo === Build Successful (x64)! ===

if defined TC_WAS_RUNNING (
    echo Reopening Total Commander...
    start "" "c:\Program Files\totalcmd\TOTALCMD64.EXE"
    REM If TC is installed in another folder, change the path above.
)

timeout /t 2
