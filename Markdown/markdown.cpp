#define MAKEDLL
#include "markdown.h"
#include <windows.h>
#include <shlwapi.h>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")

typedef char* (__stdcall *PConvertMarkdownToHtml)(const char*, const char*, const char*);
typedef void (__stdcall *PFreeHtmlBuffer)(char*);

// Helper to get current DLL handle
HMODULE GetCurrentModule() {
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)GetCurrentModule, &hModule);
    return hModule;
}

Markdown::Markdown() {}
Markdown::~Markdown() {}

std::string __stdcall Markdown::ConvertToHtmlAscii(
    std::string filename,
    std::string cssFile,
    std::string extensions
) {
    wchar_t dllPath[MAX_PATH];
    HMODULE hCurrentDll = GetCurrentModule();
    GetModuleFileNameW(hCurrentDll, dllPath, MAX_PATH);
    PathRemoveFileSpecW(dllPath);

#ifdef _WIN64
    PathAppendW(dllPath, L"MarkdigNative-x64.dll");
    const char* dllName = "MarkdigNative-x64.dll";
#else
    PathAppendW(dllPath, L"MarkdigNative-x86.dll");
    const char* dllName = "MarkdigNative-x86.dll";
#endif

    HMODULE hLib = LoadLibraryW(dllPath);
    if (!hLib) {
        hLib = LoadLibraryA(dllName);
    }

    if (!hLib) {
        return "<html><body><h1>Error</h1><p>Could not load MarkdigNative DLL.</p></body></html>";
    }

    auto convertFunc = (PConvertMarkdownToHtml)GetProcAddress(hLib, "ConvertMarkdownToHtml");
    auto freeFunc = (PFreeHtmlBuffer)GetProcAddress(hLib, "FreeHtmlBuffer");

    if (!convertFunc || !freeFunc) {
        FreeLibrary(hLib);
        return "<html><body><h1>Error</h1><p>Could not find functions in MarkdigNative DLL</p></body></html>";
    }

    char* resultPtr = convertFunc(filename.c_str(), cssFile.c_str(), extensions.c_str());
    std::string result = resultPtr ? resultPtr : "";
    
    if (resultPtr) {
        freeFunc(resultPtr);
    }

    FreeLibrary(hLib);
    return result;
}
