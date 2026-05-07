#define MAKEDLL
#include "markdown.h"
#include <windows.h>
#include <shlwapi.h>
#include <mutex>
#include <string>
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

namespace {
	std::once_flag gEngineInitOnce;
	HMODULE gEngineLib = NULL;
	PConvertMarkdownToHtml gConvertFunc = nullptr;
	PFreeHtmlBuffer gFreeFunc = nullptr;
	std::string gInitErrorHtml;
	std::once_flag gRuntimeLogOnce;
	std::wstring gRuntimeLogPath;

	std::wstring GetRuntimeLogPath()
	{
		std::call_once(gRuntimeLogOnce, []() {
			wchar_t localAppData[MAX_PATH]{};
			DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
			if (n != 0 && n < ARRAYSIZE(localAppData))
			{
				std::wstring baseDir = std::wstring(localAppData) + L"\\TotalCommanderMarkdownViewPlugin";
				CreateDirectoryW(baseDir.c_str(), NULL);
				gRuntimeLogPath = baseDir + L"\\MarkdownViewGitHubStyle_Runtime.log";
				return;
			}

			wchar_t tempDir[MAX_PATH]{};
			n = GetTempPathW(MAX_PATH, tempDir);
			if (n == 0 || n >= MAX_PATH)
				return;
			gRuntimeLogPath = std::wstring(tempDir) + L"MarkdownViewGitHubStyle_Runtime.log";
		});
		return gRuntimeLogPath;
	}

	void AppendRuntimeLogLine(const std::wstring& line)
	{
		std::wstring path = GetRuntimeLogPath();
		if (path.empty())
			return;
		HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
			return;
		std::wstring s = line + L"\r\n";
		DWORD written = 0;
		WriteFile(h, s.c_str(), (DWORD)(s.size() * sizeof(wchar_t)), &written, NULL);
		CloseHandle(h);
	}

#ifndef _WIN64
	using hostfxr_handle = void*;

	using hostfxr_initialize_for_runtime_config_fn = int(__cdecl*)(const wchar_t*, const void*, hostfxr_handle*);
	using hostfxr_get_runtime_delegate_fn = int(__cdecl*)(hostfxr_handle, int, void**);
	using hostfxr_close_fn = int(__cdecl*)(hostfxr_handle);
	using load_assembly_and_get_function_pointer_fn = int(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);

	constexpr int hdt_load_assembly_and_get_function_pointer = 5;
	const wchar_t* kUnmanagedCallersOnlyMethod = reinterpret_cast<const wchar_t*>(-1);

	bool FileExists(const std::wstring& path)
	{
		DWORD attrs = GetFileAttributesW(path.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::wstring GetEnvVar(const wchar_t* name)
	{
		DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
		if (needed == 0)
			return L"";
		std::wstring value;
		value.resize(needed);
		DWORD written = GetEnvironmentVariableW(name, &value[0], needed);
		if (written == 0)
			return L"";
		if (!value.empty() && value.back() == L'\0')
			value.pop_back();
		return value;
	}

	bool DirectoryExists(const std::wstring& path)
	{
		DWORD attrs = GetFileAttributesW(path.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	std::vector<int> ParseVersionParts(const std::wstring& s)
	{
		std::vector<int> parts;
		int current = 0;
		bool inNumber = false;
		for (wchar_t ch : s)
		{
			if (ch >= L'0' && ch <= L'9')
			{
				inNumber = true;
				current = current * 10 + (ch - L'0');
			}
			else
			{
				if (inNumber)
				{
					parts.push_back(current);
					current = 0;
					inNumber = false;
				}
				if (ch == L'-')
					break;
			}
		}
		if (inNumber)
			parts.push_back(current);
		return parts;
	}

	bool IsVersionGreater(const std::vector<int>& a, const std::vector<int>& b)
	{
		size_t n = (a.size() > b.size()) ? a.size() : b.size();
		for (size_t i = 0; i < n; i++)
		{
			int av = (i < a.size()) ? a[i] : 0;
			int bv = (i < b.size()) ? b[i] : 0;
			if (av != bv)
				return av > bv;
		}
		return false;
	}

	std::wstring FindLatestHostFxrDll(const std::wstring& dotnetRoot)
	{
		std::wstring fxrRoot = dotnetRoot;
		if (!fxrRoot.empty() && (fxrRoot.back() == L'\\' || fxrRoot.back() == L'/'))
			fxrRoot.pop_back();
		fxrRoot += L"\\host\\fxr\\";

		if (!DirectoryExists(fxrRoot))
			return L"";

		std::wstring pattern = fxrRoot + L"*";
		WIN32_FIND_DATAW data{};
		HANDLE hFind = FindFirstFileW(pattern.c_str(), &data);
		if (hFind == INVALID_HANDLE_VALUE)
			return L"";

		std::wstring bestDirName;
		std::vector<int> bestVer;

		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				continue;
			const wchar_t* name = data.cFileName;
			if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
				continue;

			std::wstring dirName = name;
			std::vector<int> ver = ParseVersionParts(dirName);
			if (ver.empty())
				continue;

			if (bestDirName.empty() || IsVersionGreater(ver, bestVer))
			{
				bestDirName = dirName;
				bestVer = std::move(ver);
			}
		} while (FindNextFileW(hFind, &data));

		FindClose(hFind);

		if (bestDirName.empty())
			return L"";

		return fxrRoot + bestDirName + L"\\hostfxr.dll";
	}

	std::wstring FindDotnetRootX86();

	std::wstring FindHostFxrForX86(const std::wstring& engineDir)
	{
		std::wstring localHostFxr = engineDir + L"\\hostfxr.dll";
		if (FileExists(localHostFxr))
			return localHostFxr;

		std::wstring dotnetRoot = FindDotnetRootX86();
		if (dotnetRoot.empty())
			return L"";

		return FindLatestHostFxrDll(dotnetRoot);
	}

	std::wstring FindDotnetRootX86()
	{
		std::wstring root = GetEnvVar(L"DOTNET_ROOT(x86)");
		if (!root.empty())
			return root;

		root = GetEnvVar(L"DOTNET_ROOT");
		if (!root.empty())
			return root;

		if (DirectoryExists(L"C:\\Program Files (x86)\\dotnet"))
			return L"C:\\Program Files (x86)\\dotnet";

		if (DirectoryExists(L"C:\\Program Files\\dotnet"))
			return L"C:\\Program Files\\dotnet";

		return L"";
	}

	bool InitManagedMarkdigEngine(const std::wstring& pluginDir)
	{
		std::wstring appLocalDir = pluginDir + L"\\dotnet-x86";
		std::wstring appLocalRuntimeConfig = appLocalDir + L"\\MarkdigNative-x86.runtimeconfig.json";
		std::wstring appLocalAssembly = appLocalDir + L"\\MarkdigNative-x86.dll";

		std::wstring engineDir = (FileExists(appLocalRuntimeConfig) && FileExists(appLocalAssembly))
			? appLocalDir
			: pluginDir;
		if (engineDir == appLocalDir)
		{
			SetEnvironmentVariableW(L"DOTNET_ROOT(x86)", engineDir.c_str());
			SetEnvironmentVariableW(L"DOTNET_ROOT", engineDir.c_str());
		}

		std::wstring hostfxrPath = FindHostFxrForX86(engineDir);
		AppendRuntimeLogLine(L"x86 hostfxrPath=" + hostfxrPath);
		if (hostfxrPath.empty())
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>hostfxr.dll not found. Install .NET Desktop Runtime x86 or place app-local runtime.</p></body></html>";
			return false;
		}

		HMODULE hHostFxr = LoadLibraryW(hostfxrPath.c_str());
		if (!hHostFxr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to load hostfxr.dll.</p></body></html>";
			return false;
		}

		auto hostfxr_initialize_for_runtime_config =
			(hostfxr_initialize_for_runtime_config_fn)GetProcAddress(hHostFxr, "hostfxr_initialize_for_runtime_config");
		auto hostfxr_get_runtime_delegate =
			(hostfxr_get_runtime_delegate_fn)GetProcAddress(hHostFxr, "hostfxr_get_runtime_delegate");
		auto hostfxr_close =
			(hostfxr_close_fn)GetProcAddress(hHostFxr, "hostfxr_close");

		if (!hostfxr_initialize_for_runtime_config || !hostfxr_get_runtime_delegate || !hostfxr_close)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>hostfxr exports not found.</p></body></html>";
			return false;
		}

		std::wstring runtimeConfig = engineDir + L"\\MarkdigNative-x86.runtimeconfig.json";
		std::wstring assemblyPath = engineDir + L"\\MarkdigNative-x86.dll";
		AppendRuntimeLogLine(L"x86 runtimeConfig=" + runtimeConfig);
		AppendRuntimeLogLine(L"x86 assemblyPath=" + assemblyPath);

		if (GetFileAttributesW(runtimeConfig.c_str()) == INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>MarkdigNative-x86 runtime files not found.</p></body></html>";
			return false;
		}

		hostfxr_handle cxt = nullptr;
		int rc = hostfxr_initialize_for_runtime_config(runtimeConfig.c_str(), nullptr, &cxt);
		AppendRuntimeLogLine(L"x86 hostfxr_initialize_for_runtime_config rc=" + std::to_wstring(rc));
		if (rc != 0 || !cxt)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to initialize .NET runtime.</p></body></html>";
			return false;
		}

		void* loadAssemblyAndGetFunctionPointerVoid = nullptr;
		rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &loadAssemblyAndGetFunctionPointerVoid);
		hostfxr_close(cxt);
		AppendRuntimeLogLine(L"x86 hostfxr_get_runtime_delegate rc=" + std::to_wstring(rc));

		if (rc != 0 || !loadAssemblyAndGetFunctionPointerVoid)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to get .NET runtime delegate.</p></body></html>";
			return false;
		}

		auto loadAssemblyAndGetFunctionPointer = (load_assembly_and_get_function_pointer_fn)loadAssemblyAndGetFunctionPointerVoid;

		const wchar_t* typeName = L"MarkdigNative.Lib, MarkdigNative-x86";

		void* convertPtr = nullptr;
		AppendRuntimeLogLine(L"x86 bind ConvertMarkdownToHtml begin");
		rc = loadAssemblyAndGetFunctionPointer(
			assemblyPath.c_str(),
			typeName,
			L"ConvertMarkdownToHtml",
			kUnmanagedCallersOnlyMethod,
			nullptr,
			&convertPtr);
		AppendRuntimeLogLine(L"x86 bind ConvertMarkdownToHtml rc=" + std::to_wstring(rc));
		if (rc != 0 || !convertPtr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to bind ConvertMarkdownToHtml.</p></body></html>";
			return false;
		}

		void* freePtr = nullptr;
		AppendRuntimeLogLine(L"x86 bind FreeHtmlBuffer begin");
		rc = loadAssemblyAndGetFunctionPointer(
			assemblyPath.c_str(),
			typeName,
			L"FreeHtmlBuffer",
			kUnmanagedCallersOnlyMethod,
			nullptr,
			&freePtr);
		AppendRuntimeLogLine(L"x86 bind FreeHtmlBuffer rc=" + std::to_wstring(rc));
		if (rc != 0 || !freePtr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to bind FreeHtmlBuffer.</p></body></html>";
			return false;
		}

		gConvertFunc = (PConvertMarkdownToHtml)convertPtr;
		gFreeFunc = (PFreeHtmlBuffer)freePtr;
		return true;
	}
#endif

#ifdef _WIN64
	using hostfxr_handle = void*;

	using hostfxr_initialize_for_runtime_config_fn = int(__cdecl*)(const wchar_t*, const void*, hostfxr_handle*);
	using hostfxr_get_runtime_delegate_fn = int(__cdecl*)(hostfxr_handle, int, void**);
	using hostfxr_close_fn = int(__cdecl*)(hostfxr_handle);
	using load_assembly_and_get_function_pointer_fn = int(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);

	constexpr int hdt_load_assembly_and_get_function_pointer = 5;
	const wchar_t* kUnmanagedCallersOnlyMethod = reinterpret_cast<const wchar_t*>(-1);

	bool FileExists(const std::wstring& path)
	{
		DWORD attrs = GetFileAttributesW(path.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::wstring GetEnvVar(const wchar_t* name)
	{
		DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
		if (needed == 0)
			return L"";
		std::wstring value;
		value.resize(needed);
		DWORD written = GetEnvironmentVariableW(name, &value[0], needed);
		if (written == 0)
			return L"";
		if (!value.empty() && value.back() == L'\0')
			value.pop_back();
		return value;
	}

	bool DirectoryExists(const std::wstring& path)
	{
		DWORD attrs = GetFileAttributesW(path.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	std::vector<int> ParseVersionParts(const std::wstring& s)
	{
		std::vector<int> parts;
		int current = 0;
		bool inNumber = false;
		for (wchar_t ch : s)
		{
			if (ch >= L'0' && ch <= L'9')
			{
				inNumber = true;
				current = current * 10 + (ch - L'0');
			}
			else
			{
				if (inNumber)
				{
					parts.push_back(current);
					current = 0;
					inNumber = false;
				}
				if (ch == L'-')
					break;
			}
		}
		if (inNumber)
			parts.push_back(current);
		return parts;
	}

	bool IsVersionGreater(const std::vector<int>& a, const std::vector<int>& b)
	{
		size_t n = (a.size() > b.size()) ? a.size() : b.size();
		for (size_t i = 0; i < n; i++)
		{
			int av = (i < a.size()) ? a[i] : 0;
			int bv = (i < b.size()) ? b[i] : 0;
			if (av != bv)
				return av > bv;
		}
		return false;
	}

	std::wstring FindLatestHostFxrDll(const std::wstring& dotnetRoot)
	{
		std::wstring fxrRoot = dotnetRoot;
		if (!fxrRoot.empty() && (fxrRoot.back() == L'\\' || fxrRoot.back() == L'/'))
			fxrRoot.pop_back();
		fxrRoot += L"\\host\\fxr\\";

		if (!DirectoryExists(fxrRoot))
			return L"";

		std::wstring pattern = fxrRoot + L"*";
		WIN32_FIND_DATAW data{};
		HANDLE hFind = FindFirstFileW(pattern.c_str(), &data);
		if (hFind == INVALID_HANDLE_VALUE)
			return L"";

		std::wstring bestDirName;
		std::vector<int> bestVer;

		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				continue;
			const wchar_t* name = data.cFileName;
			if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
				continue;

			std::wstring dirName = name;
			std::vector<int> ver = ParseVersionParts(dirName);
			if (ver.empty())
				continue;

			if (bestDirName.empty() || IsVersionGreater(ver, bestVer))
			{
				bestDirName = dirName;
				bestVer = std::move(ver);
			}
		} while (FindNextFileW(hFind, &data));

		FindClose(hFind);

		if (bestDirName.empty())
			return L"";

		return fxrRoot + bestDirName + L"\\hostfxr.dll";
	}

	std::wstring FindDotnetRootX64()
	{
		std::wstring root = GetEnvVar(L"DOTNET_ROOT");
		if (!root.empty())
			return root;

		if (DirectoryExists(L"C:\\Program Files\\dotnet"))
			return L"C:\\Program Files\\dotnet";

		if (DirectoryExists(L"C:\\Program Files (x86)\\dotnet"))
			return L"C:\\Program Files (x86)\\dotnet";

		return L"";
	}

	std::wstring FindHostFxrForX64(const std::wstring& engineDir)
	{
		std::wstring localHostFxr = engineDir + L"\\hostfxr.dll";
		if (FileExists(localHostFxr))
			return localHostFxr;

		std::wstring dotnetRoot = FindDotnetRootX64();
		if (dotnetRoot.empty())
			return L"";

		return FindLatestHostFxrDll(dotnetRoot);
	}

	bool InitManagedMarkdigEngineX64(const std::wstring& pluginDir)
	{
		std::wstring appLocalDir = pluginDir + L"\\dotnet-x64";
		std::wstring appLocalRuntimeConfig = appLocalDir + L"\\MarkdigNative-x64.runtimeconfig.json";
		std::wstring appLocalAssembly = appLocalDir + L"\\MarkdigNative-x64.dll";

		std::wstring engineDir = (FileExists(appLocalRuntimeConfig) && FileExists(appLocalAssembly))
			? appLocalDir
			: pluginDir;
		if (engineDir == appLocalDir)
		{
			SetEnvironmentVariableW(L"DOTNET_ROOT", engineDir.c_str());
		}

		std::wstring hostfxrPath = FindHostFxrForX64(engineDir);
		AppendRuntimeLogLine(L"x64 hostfxrPath=" + hostfxrPath);
		if (hostfxrPath.empty())
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>hostfxr.dll not found. Install .NET Desktop Runtime x64 or place app-local runtime.</p></body></html>";
			return false;
		}

		HMODULE hHostFxr = LoadLibraryW(hostfxrPath.c_str());
		if (!hHostFxr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to load hostfxr.dll.</p></body></html>";
			return false;
		}

		auto hostfxr_initialize_for_runtime_config =
			(hostfxr_initialize_for_runtime_config_fn)GetProcAddress(hHostFxr, "hostfxr_initialize_for_runtime_config");
		auto hostfxr_get_runtime_delegate =
			(hostfxr_get_runtime_delegate_fn)GetProcAddress(hHostFxr, "hostfxr_get_runtime_delegate");
		auto hostfxr_close =
			(hostfxr_close_fn)GetProcAddress(hHostFxr, "hostfxr_close");

		if (!hostfxr_initialize_for_runtime_config || !hostfxr_get_runtime_delegate || !hostfxr_close)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>hostfxr exports not found.</p></body></html>";
			return false;
		}

		std::wstring runtimeConfig = engineDir + L"\\MarkdigNative-x64.runtimeconfig.json";
		std::wstring assemblyPath = engineDir + L"\\MarkdigNative-x64.dll";
		AppendRuntimeLogLine(L"x64 runtimeConfig=" + runtimeConfig);
		AppendRuntimeLogLine(L"x64 assemblyPath=" + assemblyPath);

		if (GetFileAttributesW(runtimeConfig.c_str()) == INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>MarkdigNative-x64 runtime files not found.</p></body></html>";
			return false;
		}

		hostfxr_handle cxt = nullptr;
		int rc = hostfxr_initialize_for_runtime_config(runtimeConfig.c_str(), nullptr, &cxt);
		AppendRuntimeLogLine(L"x64 hostfxr_initialize_for_runtime_config rc=" + std::to_wstring(rc));
		if (rc != 0 || !cxt)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to initialize .NET runtime.</p></body></html>";
			return false;
		}

		void* loadAssemblyAndGetFunctionPointerVoid = nullptr;
		rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &loadAssemblyAndGetFunctionPointerVoid);
		hostfxr_close(cxt);
		AppendRuntimeLogLine(L"x64 hostfxr_get_runtime_delegate rc=" + std::to_wstring(rc));

		if (rc != 0 || !loadAssemblyAndGetFunctionPointerVoid)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to get .NET runtime delegate.</p></body></html>";
			return false;
		}

		auto loadAssemblyAndGetFunctionPointer = (load_assembly_and_get_function_pointer_fn)loadAssemblyAndGetFunctionPointerVoid;

		std::wstring typeNameStr = L"MarkdigNative.Lib, MarkdigNative-x64";
		const wchar_t* typeName = typeNameStr.c_str();

		void* convertPtr = nullptr;
		AppendRuntimeLogLine(L"x64 bind ConvertMarkdownToHtml begin");
		rc = loadAssemblyAndGetFunctionPointer(
			assemblyPath.c_str(),
			typeName,
			L"ConvertMarkdownToHtml",
			kUnmanagedCallersOnlyMethod,
			nullptr,
			&convertPtr);
		AppendRuntimeLogLine(L"x64 bind ConvertMarkdownToHtml rc=" + std::to_wstring(rc));
		if (rc != 0 || !convertPtr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to bind ConvertMarkdownToHtml.</p></body></html>";
			return false;
		}

		void* freePtr = nullptr;
		AppendRuntimeLogLine(L"x64 bind FreeHtmlBuffer begin");
		rc = loadAssemblyAndGetFunctionPointer(
			assemblyPath.c_str(),
			typeName,
			L"FreeHtmlBuffer",
			kUnmanagedCallersOnlyMethod,
			nullptr,
			&freePtr);
		AppendRuntimeLogLine(L"x64 bind FreeHtmlBuffer rc=" + std::to_wstring(rc));
		if (rc != 0 || !freePtr)
		{
			gInitErrorHtml = "<html><body><h1>Error</h1><p>Failed to bind FreeHtmlBuffer.</p></body></html>";
			return false;
		}

		gConvertFunc = (PConvertMarkdownToHtml)convertPtr;
		gFreeFunc = (PFreeHtmlBuffer)freePtr;
		return true;
	}
#endif

	void InitEngine()
	{
		wchar_t dllPath[MAX_PATH];
		HMODULE hCurrentDll = GetCurrentModule();
		GetModuleFileNameW(hCurrentDll, dllPath, MAX_PATH);
		PathRemoveFileSpecW(dllPath);

#ifdef _WIN64
		std::wstring pluginDir = dllPath;
		wchar_t dllPathX64[MAX_PATH];
		wcscpy_s(dllPathX64, dllPath);
		PathAppendW(dllPathX64, L"MarkdigNative-x64.dll");
		const char* dllName = "MarkdigNative-x64.dll";
#endif

#ifndef _WIN64
		std::wstring pluginDir = dllPath;
		if (!InitManagedMarkdigEngine(pluginDir))
			return;
		return;
#else
		gEngineLib = LoadLibraryW(dllPathX64);
		if (!gEngineLib) {
			gEngineLib = LoadLibraryA(dllName);
		}

		if (gEngineLib) {
			gConvertFunc = (PConvertMarkdownToHtml)GetProcAddress(gEngineLib, "ConvertMarkdownToHtml");
			gFreeFunc = (PFreeHtmlBuffer)GetProcAddress(gEngineLib, "FreeHtmlBuffer");
			if (gConvertFunc && gFreeFunc)
				return;

			FreeLibrary(gEngineLib);
			gEngineLib = NULL;
			gConvertFunc = nullptr;
			gFreeFunc = nullptr;
		}

		InitManagedMarkdigEngineX64(pluginDir);
#endif
	}

	struct EngineShutdown
	{
		~EngineShutdown()
		{
			if (gEngineLib) {
				FreeLibrary(gEngineLib);
				gEngineLib = NULL;
			}
		}
	};

	EngineShutdown gEngineShutdown;
}

std::string __stdcall Markdown::ConvertToHtmlAscii(
    std::string filename,
    std::string cssFile,
    std::string extensions
) {
	std::call_once(gEngineInitOnce, InitEngine);

	if (!gConvertFunc || !gFreeFunc) {
		return gInitErrorHtml.empty()
			? "<html><body><h1>Error</h1><p>MarkdigNative engine not initialized.</p></body></html>"
			: gInitErrorHtml;
	}

	char* resultPtr = gConvertFunc(filename.c_str(), cssFile.c_str(), extensions.c_str());
	std::string result = resultPtr ? resultPtr : "";

	if (resultPtr) {
		gFreeFunc(resultPtr);
	}

	return result;
}
