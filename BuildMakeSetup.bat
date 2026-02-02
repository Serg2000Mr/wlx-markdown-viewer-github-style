:: This script creates a release (setup) package using a prebuilt project.
@echo off
setlocal

SET VCINSTALLDIR=d:\Program Files\Microsoft Visual Studio\2022\Professional
SET PATH=%PATH%;%VCINSTALLDIR%\MSBuild\Current\Bin

@REM Build x86
SET PACKAGE=ReleaseWLX\x86
rmdir /S /Q bin\Release
rmdir /S /Q %PACKAGE%

mkdir %PACKAGE%
xcopy /s Build\ %PACKAGE%
mkdir %PACKAGE%\doc
copy Readme.md %PACKAGE%\doc\

rmdir /S /Q bin\Release\
call "%VCINSTALLDIR%\VC\Auxiliary\Build\vcvars32.bat"
MSBUILD MarkdownView.sln /t:Build /p:Configuration=Release;Platform=Win32

rmdir /S /Q bin\Release\net8.0
xcopy /s bin\Release\ %PACKAGE%\
del /Q %PACKAGE%\*.lib
del /Q %PACKAGE%\*.deps.json
del /Q %PACKAGE%\*.pdb
del /Q %PACKAGE%\*.exp

del /Q *.x86.zip
powershell.exe -nologo -noprofile -command "& { Add-Type -A 'System.IO.Compression.FileSystem'; [IO.Compression.ZipFile]::CreateFromDirectory('%PACKAGE%\', 'Release-' + (get-date -Format yyyyMMdd) +'.x86.zip'); }"


@REM Build x64
SET PACKAGE=ReleaseWLX\x64
rmdir /S /Q bin\Release
rmdir /S /Q %PACKAGE%

mkdir %PACKAGE%
xcopy /s Build\ %PACKAGE%
mkdir %PACKAGE%\doc
copy Readme.md %PACKAGE%\doc\

rmdir /S /Q bin\Release\
call "%VCINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat"
msbuild MarkdownView.sln /t:Build /p:Configuration=Release;Platform=x64

rmdir /S /Q bin\Release\net8.0
xcopy /s bin\Release\ %PACKAGE%\
del /Q %PACKAGE%\*.lib
del /Q %PACKAGE%\*.deps.json
del /Q %PACKAGE%\*.pdb
del /Q %PACKAGE%\*.exp

del /Q *.x64.zip
powershell.exe -nologo -noprofile -command "& { Add-Type -A 'System.IO.Compression.FileSystem'; [IO.Compression.ZipFile]::CreateFromDirectory('%PACKAGE%\', 'Release-' + (get-date -Format yyyyMMdd) +'.x64.zip'); }"

:end
pause