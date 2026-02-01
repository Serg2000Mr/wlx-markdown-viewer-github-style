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
    pause
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
    pause
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
    pause
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
"%MSBUILD%" Markdown\Markdown.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="%ROOT_DIR%\\"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR building Markdown x64
    pause
    exit /b %ERRORLEVEL%
)

:: --- 3. Build MarkdownView (Lister Plugin) ---
echo.
echo === Building MarkdownView (Lister Plugin) ===

:: Build x64
echo [x64] Building MarkdownView...
"%MSBUILD%" MarkdownView\MarkdownView.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="%ROOT_DIR%\\"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR building MarkdownView x64
    pause
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

echo === Build Successful (x64)! ===
pause
