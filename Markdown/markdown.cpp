#include "markdown.h"
#include <windows.h>
#include <vector>

typedef char* (__stdcall *PConvertMarkdownToHtml)(const char*, const char*, const char*);
typedef void (__stdcall *PFreeHtmlBuffer)(char*);

Markdown::Markdown() {}
Markdown::~Markdown() {}

std::string __stdcall Markdown::ConvertToHtmlAscii(
    std::string filename,
    std::string cssFile,
    std::string extensions
) {
    HMODULE hLib = LoadLibraryW(L"MarkdigNative.dll");
    if (!hLib) {
        return "<html><body><h1>Error</h1><p>Could not load MarkdigNative.dll</p></body></html>";
    }

    auto convertFunc = (PConvertMarkdownToHtml)GetProcAddress(hLib, "ConvertMarkdownToHtml");
    auto freeFunc = (PFreeHtmlBuffer)GetProcAddress(hLib, "FreeHtmlBuffer");

    if (!convertFunc || !freeFunc) {
        FreeLibrary(hLib);
        return "<html><body><h1>Error</h1><p>Could not find functions in MarkdigNative.dll</p></body></html>";
    }

    char* resultPtr = convertFunc(filename.c_str(), cssFile.c_str(), extensions.c_str());
    std::string result = resultPtr ? resultPtr : "";
    
    if (resultPtr) {
        freeFunc(resultPtr);
    }

    FreeLibrary(hLib);
    return result;
}
