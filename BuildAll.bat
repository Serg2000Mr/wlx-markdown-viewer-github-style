@echo off
setlocal

:: Path to MSBuild and Dotnet
set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
set DOTNET="C:\Program Files\Microsoft Visual Studio\18\Community\dotnet\net8.0\runtime\dotnet.exe"

:: Try to find dotnet SDK if runtime is not enough
where dotnet >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    set DOTNET=dotnet
)

echo --- Building MarkdigNative (C# Native AOT) ---
cd MarkdigNative
%DOTNET% publish -r win-x64 -c Release /p:PublishAot=true
if %ERRORLEVEL% NEQ 0 (
    echo Error building MarkdigNative
    exit /b %ERRORLEVEL%
)
copy /y bin\Release\net8.0\win-x64\publish\MarkdigNative.dll ..\bin\Release\
cd ..

echo --- Building Markdown (C++ Bridge) ---
%MSBUILD% Markdown\Markdown.vcxproj /p:Configuration=Release /p:Platform=x64
if %ERRORLEVEL% NEQ 0 (
    echo Error building Markdown
    exit /b %ERRORLEVEL%
)

echo --- Building MarkdownView (Lister Plugin) ---
%MSBUILD% MarkdownView\MarkdownView.vcxproj /p:Configuration=Release /p:Platform=x64
if %ERRORLEVEL% NEQ 0 (
    echo Error building MarkdownView
    exit /b %ERRORLEVEL%
)

echo --- Build Successful! ---
echo Files are in the 'bin\Release' folder.
echo Don't forget to copy MarkdigNative.dll along with MarkdownView.wlx.
pause
