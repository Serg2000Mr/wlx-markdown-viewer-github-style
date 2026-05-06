#ifndef E_BOUNDS
#define E_BOUNDS                         _HRESULT_TYPEDEF_(0x8000000BL)
#endif

#include <direct.h>
#include <windows.h>
#include <VersionHelpers.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <objidl.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
#include "browserhost.h"
#include "ListerPlugin.h"
#include "resource.h"
#include <ExDispID.h>
#include <locale>
#include <iostream>
#include <vector>
#include <iterator>
#include <codecvt>
#include <algorithm>
#include <unordered_map>
#include <list>
#include <mutex>
#include <thread>
#include <atomic>
#include <strsafe.h>
#include "functions.h"

#include "Markdown/markdown.h"

HHOOK hook_keyb = NULL;
HHOOK hook_getmsg = NULL;
HIMAGELIST img_list = NULL;
int num_lister_windows = 0;

// used to by the refresh function
char FileToLoadCopy[MAX_PATH];
HWND ParentWinCopy;
int ShowFlagsCopy;

// Temporary HTML file path for markdown rendering
wchar_t TempHtmlFilePath[MAX_PATH] = L"";

CSmallStringList html_extensions;
CSmallStringList markdown_extensions;
CSmallStringList def_signatures;
CSmallStringList trans_hotkeys;
CSmallStringList typing_trans_hotkeys;
char html_template[512];
char html_template_dark[512];
char renderer_extensions[2048];

namespace {
	std::mutex gGdiPlusMutex;
	ULONG_PTR gGdiPlusToken = 0;
	HICON gTranslateIcon = NULL;
	int gTranslateToolbarImageIndex = -1;
	HICON gFastFontIcon = NULL;
	int gFastFontToolbarImageIndex = -1;

	// Требует, чтобы вызывающий уже держал gGdiPlusMutex. Используется внутри
	// LoadTranslateIcon32/LoadFastFontIcon24, которые держат lock на всём периоде
	// от инициализации GDI+ до использования Gdiplus::Bitmap — иначе возможна
	// гонка с ShutdownGdiPlusIfIdle при пересечении open/close циклов.
	static void EnsureGdiPlusLocked()
	{
		if (gGdiPlusToken != 0)
			return;
		Gdiplus::GdiplusStartupInput input;
		Gdiplus::GdiplusStartup(&gGdiPlusToken, &input, NULL);
	}

	// issue #2: единая точка shutdown GDI+. Безопасно вызывать из любых не-DllMain
	// мест (UI-поток ListCloseWindow, RAII-guard на ранних возвратах ListLoad).
	// Закрытие происходит только если GDI+ был стартован и активных Lister-окон нет —
	// иначе оставляем для других открытых окон.
	static void ShutdownGdiPlusIfIdle()
	{
		std::lock_guard<std::mutex> lock(gGdiPlusMutex);
		if (gGdiPlusToken != 0 && num_lister_windows == 0) {
			Gdiplus::GdiplusShutdown(gGdiPlusToken);
			gGdiPlusToken = 0;
		}
	}

	static HICON LoadTranslateIcon32()
	{
		if (gTranslateIcon)
			return gTranslateIcon;

		// Lock на всём периоде использования GDI+: иначе ShutdownGdiPlusIfIdle()
		// в параллельном ListCloseWindow может закрыть GDI+ между EnsureGdiPlusLocked
		// и созданием Gdiplus::Bitmap → undefined behavior внутри декодера.
		std::lock_guard<std::mutex> lock(gGdiPlusMutex);

		EnsureGdiPlusLocked();
		if (gGdiPlusToken == 0)
			return NULL;

		HRSRC resInfo = FindResourceW(hinst, MAKEINTRESOURCEW(IDR_TRANSLATE_PNG), MAKEINTRESOURCEW(10));
		if (!resInfo)
			return NULL;
		DWORD resSize = SizeofResource(hinst, resInfo);
		if (resSize == 0)
			return NULL;
		HGLOBAL resData = LoadResource(hinst, resInfo);
		if (!resData)
			return NULL;
		void* resPtr = LockResource(resData);
		if (!resPtr)
			return NULL;

		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, resSize);
		if (!hMem)
			return NULL;
		void* memPtr = GlobalLock(hMem);
		if (!memPtr)
		{
			GlobalFree(hMem);
			return NULL;
		}
		memcpy(memPtr, resPtr, resSize);
		GlobalUnlock(hMem);

		IStream* stream = nullptr;
		if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream)) || !stream)
		{
			GlobalFree(hMem);
			return NULL;
		}

		Gdiplus::Bitmap bmp(stream, FALSE);
		stream->Release();
		if (bmp.GetLastStatus() != Gdiplus::Ok)
			return NULL;

		HICON icon = NULL;
		if (bmp.GetHICON(&icon) != Gdiplus::Ok || !icon)
			return NULL;

		gTranslateIcon = icon;
		return gTranslateIcon;
	}

	static HICON LoadFastFontIcon24()
	{
		if (gFastFontIcon)
			return gFastFontIcon;

		// См. комментарий в LoadTranslateIcon32 — lock на всё использование GDI+.
		std::lock_guard<std::mutex> lock(gGdiPlusMutex);

		EnsureGdiPlusLocked();
		if (gGdiPlusToken == 0)
			return NULL;

		HRSRC resInfo = FindResourceW(hinst, MAKEINTRESOURCEW(IDR_FASTFONT_PNG), MAKEINTRESOURCEW(10));
		if (!resInfo)
			return NULL;
		DWORD resSize = SizeofResource(hinst, resInfo);
		if (resSize == 0)
			return NULL;
		HGLOBAL resData = LoadResource(hinst, resInfo);
		if (!resData)
			return NULL;
		void* resPtr = LockResource(resData);
		if (!resPtr)
			return NULL;

		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, resSize);
		if (!hMem)
			return NULL;
		void* memPtr = GlobalLock(hMem);
		if (!memPtr)
		{
			GlobalFree(hMem);
			return NULL;
		}
		memcpy(memPtr, resPtr, resSize);
		GlobalUnlock(hMem);

		IStream* stream = nullptr;
		if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream)) || !stream)
		{
			GlobalFree(hMem);
			return NULL;
		}

		Gdiplus::Bitmap bmp(stream, FALSE);
		stream->Release();
		if (bmp.GetLastStatus() != Gdiplus::Ok)
			return NULL;

		HICON icon = NULL;
		if (bmp.GetHICON(&icon) != Gdiplus::Ok || !icon)
			return NULL;

		gFastFontIcon = icon;
		return gFastFontIcon;
	}

	struct MarkdownHtmlCacheKey
	{
		std::wstring filenameLower;
		std::wstring cssLower;
		std::wstring extensionsLower;
		ULONGLONG lastWrite;
		ULONGLONG fileSize;

		bool operator==(const MarkdownHtmlCacheKey& other) const
		{
			return lastWrite == other.lastWrite
				&& fileSize == other.fileSize
				&& filenameLower == other.filenameLower
				&& cssLower == other.cssLower
				&& extensionsLower == other.extensionsLower;
		}
	};

	struct MarkdownHtmlCacheKeyHash
	{
		size_t operator()(const MarkdownHtmlCacheKey& k) const
		{
			size_t h = 0;
			auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
			mix(std::hash<std::wstring>{}(k.filenameLower));
			mix(std::hash<std::wstring>{}(k.cssLower));
			mix(std::hash<std::wstring>{}(k.extensionsLower));
			mix((size_t)k.lastWrite);
			mix((size_t)(k.lastWrite >> 32));
			mix((size_t)k.fileSize);
			mix((size_t)(k.fileSize >> 32));
			return h;
		}
	};

	struct MarkdownHtmlCacheEntry
	{
		std::string html;
		std::list<MarkdownHtmlCacheKey>::iterator lruIt;
	};

	const size_t kMaxMarkdownHtmlCacheEntries = 8;
	const size_t kMaxMarkdownHtmlCacheEntryBytes = 2 * 1024 * 1024;
	std::mutex gMarkdownHtmlCacheMutex;
	std::list<MarkdownHtmlCacheKey> gMarkdownHtmlCacheLru;
	std::unordered_map<MarkdownHtmlCacheKey, MarkdownHtmlCacheEntry, MarkdownHtmlCacheKeyHash> gMarkdownHtmlCache;

	const UINT WM_MD_RENDER_DONE = WM_APP + 0x531;
	std::atomic<unsigned long long> gMdRenderToken{ 0 };
	std::mutex gMdRenderMutex;
	std::unordered_map<HWND, unsigned long long> gMdRenderCurrentToken;
	struct MdRenderResult { unsigned long long token; std::string html; };
	std::unordered_map<HWND, MdRenderResult> gMdRenderResults;

	bool gTranslateEnabled = false;
	bool gTranslateAuto = false;
	bool gFastFontButtonEnabled = true;
	char gTranslateTargetLang[16]{};
	bool gSyntaxHighlightEnabled = true;
	std::wstring gTranslateUiTranslate = L"Translate";
	std::wstring gTranslateUiBusy = L"Translating...";
	std::wstring gTranslateUiOriginal = L"Original";

	static std::wstring ToLowerWin(std::wstring s)
	{
		if (!s.empty())
			CharLowerBuffW(&s[0], (DWORD)s.size());
		return s;
	}

	static std::wstring GetUserLocaleNameLower()
	{
		wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
		if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
		{
			LANGID langId = GetUserDefaultUILanguage();
			LCID lcid = MAKELCID(langId, SORT_DEFAULT);
			LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0);
		}
		return ToLowerWin(std::wstring(name));
	}

	static std::string GetUserLangTagForTranslate()
	{
		std::wstring loc = GetUserLocaleNameLower();
		if (loc.rfind(L"zh", 0) == 0)
		{
			if (loc.find(L"hant") != std::wstring::npos || loc.find(L"-tw") != std::wstring::npos || loc.find(L"-hk") != std::wstring::npos || loc.find(L"-mo") != std::wstring::npos)
				return "zh-TW";
			return "zh-CN";
		}
		if (loc.size() >= 2)
		{
			std::string code;
			code.push_back((char)loc[0]);
			code.push_back((char)loc[1]);
			return code;
		}
		return "en";
	}

	static void InitFastFontSettings()
	{
		gFastFontButtonEnabled = GetPrivateProfileInt("FastFont", "Enabled", 1, options.IniFileName) != 0;
	}

	static const wchar_t* GetFastFontTooltip()
	{
		std::string lang = GetUserLangTagForTranslate();
		if (lang == "ru")    return L"Режим бионического шрифта для быстрого чтения";
		if (lang == "zh-CN") return L"用于快速阅读的仿生字体模式";
		if (lang == "zh-TW") return L"用於快速閱讀的仿生字體模式";
		if (lang == "es")    return L"Modo de fuente biónica para lectura rápida";
		if (lang == "hi")    return L"तेज़ पढ़ने के लिए बायोनिक फ़ॉन्ट मोड";
		if (lang == "ar")    return L"وضع الخط البيوني للقراءة السريعة";
		if (lang == "pt")    return L"Modo de fonte biônica para leitura rápida";
		return L"Bionic font mode for speed reading";
	}

	static void InitTranslateUiStrings(const std::string& langTag)
	{
		if (langTag == "ru")
		{
			gTranslateUiTranslate = L"Перевести";
			gTranslateUiBusy = L"Перевожу...";
			gTranslateUiOriginal = L"Исходный";
			return;
		}
		if (langTag == "es")
		{
			gTranslateUiTranslate = L"Traducir";
			gTranslateUiBusy = L"Traduciendo...";
			gTranslateUiOriginal = L"Original";
			return;
		}
		if (langTag == "fr")
		{
			gTranslateUiTranslate = L"Traduire";
			gTranslateUiBusy = L"Traduction...";
			gTranslateUiOriginal = L"Original";
			return;
		}
		if (langTag == "de")
		{
			gTranslateUiTranslate = L"Übersetzen";
			gTranslateUiBusy = L"Übersetze...";
			gTranslateUiOriginal = L"Original";
			return;
		}
		if (langTag == "it")
		{
			gTranslateUiTranslate = L"Traduci";
			gTranslateUiBusy = L"Traduzione...";
			gTranslateUiOriginal = L"Originale";
			return;
		}
		if (langTag == "pt")
		{
			gTranslateUiTranslate = L"Traduzir";
			gTranslateUiBusy = L"Traduzindo...";
			gTranslateUiOriginal = L"Original";
			return;
		}
		if (langTag == "ja")
		{
			gTranslateUiTranslate = L"翻訳";
			gTranslateUiBusy = L"翻訳中...";
			gTranslateUiOriginal = L"原文";
			return;
		}
		if (langTag == "ko")
		{
			gTranslateUiTranslate = L"번역";
			gTranslateUiBusy = L"번역 중...";
			gTranslateUiOriginal = L"원문";
			return;
		}
		if (langTag == "zh-CN")
		{
			gTranslateUiTranslate = L"翻译";
			gTranslateUiBusy = L"翻译中...";
			gTranslateUiOriginal = L"原文";
			return;
		}
		if (langTag == "zh-TW")
		{
			gTranslateUiTranslate = L"翻譯";
			gTranslateUiBusy = L"翻譯中...";
			gTranslateUiOriginal = L"原文";
			return;
		}
		gTranslateUiTranslate = L"Translate";
		gTranslateUiBusy = L"Translating...";
		gTranslateUiOriginal = L"Original";
	}

	static void InitTranslateSettings()
	{
		gTranslateEnabled = GetPrivateProfileInt("Translate", "Enabled", 0, options.IniFileName) != 0;
		gTranslateAuto = GetPrivateProfileInt("Translate", "Auto", 0, options.IniFileName) != 0;
		char targetBuf[16]{};
		GetPrivateProfileString("Translate", "Target", "auto", targetBuf, (DWORD)sizeof(targetBuf), options.IniFileName);

		std::string userLang = GetUserLangTagForTranslate();
		InitTranslateUiStrings(userLang);

		std::string target = targetBuf;
		std::transform(target.begin(), target.end(), target.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (target.empty() || target == "auto")
			target = userLang;
		if (target.size() >= sizeof(gTranslateTargetLang))
			target.resize(sizeof(gTranslateTargetLang) - 1);
		memset(gTranslateTargetLang, 0, sizeof(gTranslateTargetLang));
		memcpy(gTranslateTargetLang, target.c_str(), target.size());
	}

	static void InitSyntaxHighlightSettings()
	{
		gSyntaxHighlightEnabled = GetPrivateProfileInt("Renderer", "EnableSyntaxHighlight", 1, options.IniFileName) != 0;
	}

	static std::string WideToUtf8(const std::wstring& ws)
	{
		if (ws.empty())
			return std::string();
		int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
		if (needed <= 0)
			return std::string();
		std::string out;
		out.resize(needed);
		WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], needed, nullptr, nullptr);
		return out;
	}

	static bool TryMultiByteToWide(UINT codePage, const char* input, std::wstring& out)
	{
		out.clear();
		if (!input)
			return false;

		int needed = MultiByteToWideChar(codePage, 0, input, -1, nullptr, 0);
		if (needed <= 0)
			return false;

		out.assign((size_t)needed, L'\0');
		int written = MultiByteToWideChar(codePage, 0, input, -1, &out[0], needed);
		if (written <= 0)
		{
			out.clear();
			return false;
		}

		if (!out.empty() && out.back() == L'\0')
			out.pop_back();

		return true;
	}

	static bool TryAnyToWide(const char* input, std::wstring& out)
	{
		if (TryMultiByteToWide(CP_ACP, input, out))
			return true;
		if (TryMultiByteToWide(CP_UTF8, input, out))
			return true;
		return false;
	}

	static std::string EscapeJsSingleQuoted(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		for (char c : s)
		{
			if (c == '\\' || c == '\'')
			{
				out.push_back('\\');
				out.push_back(c);
			}
			else if (c == '\r')
				out += "\\r";
			else if (c == '\n')
				out += "\\n";
			else
				out.push_back(c);
		}
		return out;
	}

	static bool TryGetFileInfo(const std::wstring& path, ULONGLONG& size, ULONGLONG& lastWrite)
	{
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
			return false;
		ULARGE_INTEGER s{};
		s.LowPart = fad.nFileSizeLow;
		s.HighPart = fad.nFileSizeHigh;
		size = s.QuadPart;
		ULARGE_INTEGER t{};
		t.LowPart = fad.ftLastWriteTime.dwLowDateTime;
		t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
		lastWrite = t.QuadPart;
		return true;
	}

	static bool TryGetCachedHtml(const MarkdownHtmlCacheKey& key, std::string& outHtml)
	{
		std::lock_guard<std::mutex> lock(gMarkdownHtmlCacheMutex);
		auto it = gMarkdownHtmlCache.find(key);
		if (it == gMarkdownHtmlCache.end())
			return false;
		gMarkdownHtmlCacheLru.erase(it->second.lruIt);
		gMarkdownHtmlCacheLru.push_front(key);
		it->second.lruIt = gMarkdownHtmlCacheLru.begin();
		outHtml = it->second.html;
		return true;
	}

	static void PutCachedHtml(const MarkdownHtmlCacheKey& key, std::string html)
	{
		if (html.size() > kMaxMarkdownHtmlCacheEntryBytes)
			return;

		std::lock_guard<std::mutex> lock(gMarkdownHtmlCacheMutex);
		auto it = gMarkdownHtmlCache.find(key);
		if (it != gMarkdownHtmlCache.end()) {
			it->second.html = std::move(html);
			gMarkdownHtmlCacheLru.erase(it->second.lruIt);
			gMarkdownHtmlCacheLru.push_front(key);
			it->second.lruIt = gMarkdownHtmlCacheLru.begin();
			return;
		}

		gMarkdownHtmlCacheLru.push_front(key);
		MarkdownHtmlCacheEntry entry{ std::move(html), gMarkdownHtmlCacheLru.begin() };
		gMarkdownHtmlCache.emplace(key, std::move(entry));

		while (gMarkdownHtmlCache.size() > kMaxMarkdownHtmlCacheEntries) {
			auto lastIt = gMarkdownHtmlCacheLru.end();
			--lastIt;
			gMarkdownHtmlCache.erase(*lastIt);
			gMarkdownHtmlCacheLru.pop_back();
		}
	}

	static std::string BuildBaseHref(CBrowserHost* browserHost, const wchar_t* folderPath)
	{
		bool useInternal = false;
		if (browserHost && browserHost->mWebView) {
			CComQIPtr<ICoreWebView2_3> webView3 = browserHost->mWebView;
			useInternal = (bool)webView3 && browserHost->mFolderMappingEnabled;
		}

		if (useInternal)
			return "https://markdown.internal/_markdown_preview_temp.html";

		if (!folderPath || !folderPath[0])
			return "file:///";

		std::wstring baseFile = folderPath;
		if (!baseFile.empty() && baseFile.back() != L'\\')
			baseFile.push_back(L'\\');
		baseFile += L"_markdown_preview_temp.html";

		wchar_t fileUrl[2048]{};
		DWORD cch = ARRAYSIZE(fileUrl);
		HRESULT hr = UrlCreateFromPathW(baseFile.c_str(), fileUrl, &cch, 0);
		if (SUCCEEDED(hr) && fileUrl[0])
			return WideToUtf8(fileUrl);

		return "file:///";
	}

	static bool BuildSafeTempHtmlPath(wchar_t* outPath, size_t outCount)
	{
		if (!outPath || outCount == 0)
			return false;

		outPath[0] = L'\0';

		wchar_t localAppData[MAX_PATH]{};
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
		if (n == 0 || n >= ARRAYSIZE(localAppData))
			return false;

		std::wstring baseDir = std::wstring(localAppData) + L"\\TotalCommanderMarkdownViewPlugin";
		std::wstring dir = baseDir + L"\\l";
		CreateDirectoryW(baseDir.c_str(), NULL);
		CreateDirectoryW(dir.c_str(), NULL);

		std::wstring fullPath = dir + L"\\_markdown_preview_temp.html";
		if (fullPath.size() + 1 > outCount)
			return false;

		wcscpy_s(outPath, outCount, fullPath.c_str());
		return true;
	}

	static bool FindTagInsertPosCi(const std::string& html, const char* tagName, size_t& insertPos);

	static void EnsureBaseTag(std::string& html, const std::string& baseHref)
	{
		std::string lower;
		lower.resize(html.size());
		std::transform(html.begin(), html.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (lower.find("<base") != std::string::npos)
			return;
		std::string baseTag = "<base href='";
		baseTag += baseHref;
		baseTag += "'>";
		size_t headInsertPos = 0;
		if (!FindTagInsertPosCi(html, "head", headInsertPos))
			return;
		html.insert(headInsertPos, baseTag);
	}

	static bool FindTagInsertPosCi(const std::string& html, const char* tagName, size_t& insertPos)
	{
		std::string lower;
		lower.resize(html.size());
		std::transform(html.begin(), html.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });

		std::string needle = "<";
		needle += tagName;
		size_t p = lower.find(needle);
		if (p == std::string::npos)
			return false;
		size_t gt = lower.find('>', p);
		if (gt == std::string::npos)
			return false;
		insertPos = gt + 1;
		return true;
	}

	static void InjectInPageAnchorHandler(std::string& html)
	{
		if (html.find("id='__mdv_anchor_fix'") != std::string::npos)
			return;

		size_t headPos = 0;
		if (!FindTagInsertPosCi(html, "head", headPos))
			return;

		std::string script = "<script id='__mdv_anchor_fix'>(function(){"
			"document.addEventListener('click',function(e){"
			" var a=e.target;"
			" while(a&&a.tagName!=='A'){a=a.parentElement;}"
			" if(!a) return;"
			" var href=a.getAttribute('href');"
			" if(!href||href.length<2||href[0]!=='#') return;"
			" var id=href.slice(1);"
			" var el=document.getElementById(id);"
			" if(!el){"
			"  try{"
			"   var d=decodeURIComponent(id);"
			"   el=document.getElementById(d)||document.getElementsByName(d)[0];"
			"  }catch(_){ }"
			" }"
			" if(!el) return;"
			" e.preventDefault();"
			" try{el.scrollIntoView({block:'start'});}catch(_){try{el.scrollIntoView(true);}catch(__){}}"
			" try{history.replaceState(null,'',href);}catch(_){try{location.hash=href;}catch(__){}}"
			"},true);"
			"})();</script>";

		html.insert(headPos, script);
	}

	static void InjectGoogleTranslateWidget(std::string& html)
	{
		if (!gTranslateEnabled)
			return;
		if (html.find("id='__mdv_translate'") != std::string::npos)
			return;

		size_t headPos = 0;
		if (!FindTagInsertPosCi(html, "head", headPos))
			return;

		std::string target = gTranslateTargetLang[0] ? std::string(gTranslateTargetLang) : GetUserLangTagForTranslate();

		std::string uiTranslate = EscapeJsSingleQuoted(WideToUtf8(gTranslateUiTranslate));
		std::string uiBusy = EscapeJsSingleQuoted(WideToUtf8(gTranslateUiBusy));
		std::string uiOriginal = EscapeJsSingleQuoted(WideToUtf8(gTranslateUiOriginal));

		// Бронебойный CSS: фиксируем body и скрываем все, что похоже на Google Translate
		std::string headInject = "<style>"
			"html,body{top:0!important;margin-top:0!important;position:relative!important;}"
			".goog-te-banner-frame,iframe.goog-te-banner-frame{display:none!important;visibility:hidden!important;}"
			".skiptranslate:not(#__mdv_gt){display:none!important;}"
			"#__mdv_gt .skiptranslate{display:block!important;visibility:visible!important;}"
			"#goog-gt-tt,#goog-gt-vt,.goog-te-balloon-frame{display:none!important;visibility:hidden!important;}"
			"</style>";

		// Скрипт инициализации с уведомлением C++ о состоянии
		headInject += "<script>"
			"window.__mdv_gt_lbl_translate='" + uiTranslate + "';"
			"window.__mdv_gt_lbl_busy='" + uiBusy + "';"
			"window.__mdv_gt_lbl_original='" + uiOriginal + "';"
			"window.__mdv_gt_target='" + EscapeJsSingleQuoted(target) + "';"
			"window.__mdv_gt_t0=0;"
			"window.__mdv_gt_now=function(){ try{ return (window.performance && performance.now)? performance.now(): Date.now(); }catch(e){ return Date.now(); } };"
			"window.__mdv_gt_diag=function(ev,extra){"
			" try{"
			"  if(window.chrome && window.chrome.webview){"
			"   var dt=window.__mdv_gt_t0? (window.__mdv_gt_now()-window.__mdv_gt_t0) : 0;"
			"   window.chrome.webview.postMessage('mdv_gt_diag|'+dt.toFixed(1)+'|'+(ev||'')+'|'+(extra||''));"
			"  }"
			" }catch(e){}"
			"};"
			"window.__mdv_gt_loaded=false;"
			"window.__mdv_gt_ready=false;"
			"window.__mdv_gt_pending=false;"
			"window.__mdv_gt_notify=function(state,text){"
			" if(window.chrome && window.chrome.webview){"
			"  window.chrome.webview.postMessage({type:'gt_state',state:state,text:text});"
			" }"
			"};"
			"window.__mdv_gt_preload=function(){"
			" if(window.__mdv_gt_ready || window.__mdv_gt_loaded) return;"
			" window.__mdv_gt_loaded=true;"
			" var s=document.createElement('script');"
			" s.onerror=function(){ window.__mdv_gt_loaded=false; };"
			" s.src='https://translate.google.com/translate_a/element.js?cb=__mdv_gt_boot';"
			" document.head.appendChild(s);"
			"};"
			"try{ window.__mdv_gt_preload(); }catch(e){}"
			"window.__mdv_gt_load=function(f){"
			" if(window.__mdv_gt_state==='done' && !f){"
			"  try{if(window.__mdv_gt_doneTimer) clearTimeout(window.__mdv_gt_doneTimer);}catch(e){}"
			"  try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			"  try{"
			"   document.cookie='googtrans=;expires=Thu, 01 Jan 1970 00:00:00 GMT;path=/';"
			"   if(location.hostname) document.cookie='googtrans=;expires=Thu, 01 Jan 1970 00:00:00 GMT;domain='+location.hostname+';path=/';"
			"  }catch(e){}"
			"  try{sessionStorage.setItem('__mdv_no_auto','1');}catch(e){}"
			"  try{if(window.__mdv_gt_revertTimer) clearTimeout(window.__mdv_gt_revertTimer);}catch(e){}"
			"  window.__mdv_gt_revertTimer=setTimeout(function(){"
			"   window.__mdv_gt_state='idle';"
			"   window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			"   window.location.reload();"
			"  }, 200);"
			"  return;"
			" }"
			" if(window.__mdv_gt_state==='busy'){"
			"  try{"
			"   if(window.__mdv_gt_busyAt && (Date.now()-window.__mdv_gt_busyAt)>20000){"
			"    window.__mdv_gt_loaded=false;"
			"    window.__mdv_gt_state='idle';"
			"    window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			"   } else { return; }"
			"  }catch(e){ return; }"
			" }"
			" window.__mdv_gt_t0=window.__mdv_gt_now();"
			" window.__mdv_gt_diag('start', f?'auto':'click');"
			" window.__mdv_gt_state='busy';"
			" window.__mdv_gt_busyAt=Date.now();"
			" try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			" window.__mdv_gt_watchdog=setTimeout(function(){"
			"  if(window.__mdv_gt_state==='busy'){"
			"   window.__mdv_gt_loaded=false;"
			"   window.__mdv_gt_state='idle';"
			"   window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			"  }"
			" }, 20000);"
			" try{"
			"  var t=(window.__mdv_gt_target||'en');"
			"  var v='/auto/'+t;"
			"  document.cookie='googtrans='+v+';path=/';"
			"  if(location.hostname) document.cookie='googtrans='+v+';domain='+location.hostname+';path=/';"
			" }catch(e){}"
			" window.__mdv_gt_diag('cookie','set');"
			" window.__mdv_gt_notify('busy',window.__mdv_gt_lbl_busy);"
			" if(window.__mdv_gt_ready){ window.__mdv_gt_init(); return; }"
			" if(window.__mdv_gt_loaded){ window.__mdv_gt_pending=true; window.__mdv_gt_diag('pending','script'); return; }"
			" window.__mdv_gt_loaded=true;"
			" window.__mdv_gt_pending=true;"
			" window.__mdv_gt_diag('script','load');"
			" var s=document.createElement('script');"
			" s.onerror=function(){"
			"  try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			"  window.__mdv_gt_loaded=false;"
			 "  window.__mdv_gt_ready=false;"
			 "  window.__mdv_gt_pending=false;"
			"  window.__mdv_gt_state='idle';"
			"  window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			 "  window.__mdv_gt_diag('script','error');"
			" };"
			" s.src='https://translate.google.com/translate_a/element.js?cb=__mdv_gt_boot';"
			" document.head.appendChild(s);"
			"};"
			"window.__mdv_gt_boot=function(){"
			" window.__mdv_gt_ready=true;"
			" window.__mdv_gt_diag('script','ready');"
			" try{"
			"  if(!window.__mdv_gt_el) window.__mdv_gt_el = new google.translate.TranslateElement({pageLanguage:'auto',autoDisplay:false},'__mdv_gt');"
			" }catch(e){}"
			" if(window.__mdv_gt_pending && window.__mdv_gt_state==='busy'){"
			"  window.__mdv_gt_pending=false;"
			"  try{ window.__mdv_gt_init(); }catch(e){}"
			" }"
			"};"
			"window.__mdv_gt_init=function(){"
			" window.__mdv_gt_diag('init','');"
			" try{"
			"  if(!window.__mdv_gt_el) window.__mdv_gt_el = new google.translate.TranslateElement({pageLanguage:'auto',autoDisplay:false},'__mdv_gt');"
			" }catch(e){}"
			" var target='" + target + "';"
			" var checkTries=0;"
			" var done=0;"
			" var apply=function(){"
			"  if(done) return;"
			"  var s=document.querySelector('select.goog-te-combo');"
			"  if(s && s.options && s.options.length>0){"
			"   done=1;"
			"   window.__mdv_gt_diag('combo','found');"
			"   s.value=target; s.dispatchEvent(new Event('change',{bubbles:true}));"
			"   window.__mdv_gt_diag('combo','change');"
			"   finish();"
			"   return true;"
			"  }"
			"  return false;"
			" };"
			" try{"
			"  var root=document.getElementById('__mdv_gt');"
			"  if(root && !root.__mdv_gt_mo){"
			"   root.__mdv_gt_mo=1;"
			"   var mo=new MutationObserver(function(){ try{ if(apply()) mo.disconnect(); }catch(e){} });"
			"   mo.observe(root,{childList:true,subtree:true});"
			"  }"
			" }catch(e){}"
			" var check=function(){"
			"  if(apply()) return;"
			"  if(++checkTries < 150) { setTimeout(check, 100); }"
			"  else {"
			"   try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			"   window.__mdv_gt_loaded=false;"
			"   window.__mdv_gt_state='idle';"
			"   window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			"   window.__mdv_gt_diag('combo','timeout');"
			"  }"
			" };"
			" var finish=function(){"
			"  var tries=0;"
			"  var scheduleDone=function(){"
			"   if(window.__mdv_gt_state==='done') return;"
			"   if(window.__mdv_gt_doneTimer) return;"
			"   window.__mdv_gt_doneTimer=setTimeout(function(){"
			"    try{window.__mdv_gt_doneTimer=null;}catch(e){}"
			"    var isTrans=document.documentElement.classList.contains('translated-ltr') || document.documentElement.classList.contains('translated-rtl');"
			"    if(!isTrans) return;"
			"    window.__mdv_gt_state='done';"
			"    try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			"    window.__mdv_gt_diag('translated','done');"
			"    window.__mdv_gt_notify('done',window.__mdv_gt_lbl_original);"
			"   }, 2000);"
			"  };"
			"  var waitTranslate=function(){"
			"   var isTrans=document.documentElement.classList.contains('translated-ltr') || document.documentElement.classList.contains('translated-rtl');"
			"   if(isTrans){"
			"    scheduleDone();"
			"   } else if(++tries < 150) { setTimeout(waitTranslate, 100); }"
			"   else {"
			"    try{if(window.__mdv_gt_watchdog) clearTimeout(window.__mdv_gt_watchdog);}catch(e){}"
			"    window.__mdv_gt_state='idle';"
			"    window.__mdv_gt_notify('idle',window.__mdv_gt_lbl_translate);"
			"   }"
			"  };"
			"  setTimeout(waitTranslate, 0);"
			" };"
			" check();"
			"};"
			"document.addEventListener('DOMContentLoaded',function(){"
			" var auto=" + (gTranslateAuto ? "1" : "0") + ";"
			" var noAuto=null; try{noAuto=sessionStorage.getItem('__mdv_no_auto'); sessionStorage.removeItem('__mdv_no_auto');}catch(e){}"
			" try{ window.__mdv_gt_preload(); }catch(e){}"
			" if(auto && !noAuto) window.__mdv_gt_load(true);"
			"});"
			"</script>";

		html.insert(headPos, headInject);

		size_t bodyPos = 0;
		if (!FindTagInsertPosCi(html, "body", bodyPos))
			return;
		std::string bodyInject = "<div id='__mdv_gt' style='position:fixed;left:0;top:0;height:40px;width:240px;overflow:hidden;opacity:0;pointer-events:none;z-index:-1'></div>";
		html.insert(bodyPos, bodyInject);
	}

	static std::string BuildLoadingHtml()
	{
		return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
			"<style>body{font-family:Segoe UI,Arial,sans-serif;margin:16px;color:#444}"
			".t{font-size:12px;opacity:.8}</style></head>"
			"<body><div class='t'>Rendering...</div></body></html>";
	}

	static bool IsWebViewSearchHotkey(const CAtlString& keyName)
	{
		return keyName == "Ctrl+F" || keyName == "F3" || keyName == "Shift+F3";
	}

	static bool IsIssue9Key(WPARAM key)
	{
		return key == VK_CONTROL || key == 'F' || key == VK_F3;
	}

	static bool IsIssue9KeyMessage(UINT message)
	{
		return message == WM_KEYDOWN || message == WM_KEYUP ||
			message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
	}

	static const char* MessageName(UINT message)
	{
		switch (message)
		{
			case WM_KEYDOWN: return "WM_KEYDOWN";
			case WM_KEYUP: return "WM_KEYUP";
			case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
			case WM_SYSKEYUP: return "WM_SYSKEYUP";
			case WM_IEVIEW_HOTKEY: return "WM_IEVIEW_HOTKEY";
			default: return "MSG";
		}
	}

	static void BuildWindowBrief(HWND hwnd, char* out, size_t cchOut)
	{
		if (!out || cchOut == 0)
			return;
		out[0] = '\0';
		if (!hwnd)
		{
			StringCchCopyA(out, cchOut, "NULL");
			return;
		}

		char cls[96]{};
		char title[128]{};
		GetClassNameA(hwnd, cls, ARRAYSIZE(cls));
		GetWindowTextA(hwnd, title, ARRAYSIZE(title));
		StringCchPrintfA(out, cchOut,
			"%p cls='%s' title='%.60s' parent=%p root=%p owner=%p visible=%d enabled=%d",
			hwnd, cls, title, GetParent(hwnd), GetAncestor(hwnd, GA_ROOT),
			GetWindow(hwnd, GW_OWNER), IsWindowVisible(hwnd) ? 1 : 0, IsWindowEnabled(hwnd) ? 1 : 0);
	}

	static void DebugLogWindowBrief(const char* location, const char* label, HWND hwnd)
	{
		char brief[512]{};
		char line[640]{};
		BuildWindowBrief(hwnd, brief, ARRAYSIZE(brief));
		StringCchPrintfA(line, ARRAYSIZE(line), "%s=%s", label ? label : "hwnd", brief);
		DebugLog(location, line);
	}

	static HWND gLastHotkeyBrowserWnd = NULL;
	static ULONGLONG gNativeWebFindAcceleratorUntil = 0;
	static const char* PROP_PENDING_WEB_FIND = "HTMLView_PendingWebFind";
	static const char* PROP_PENDING_WEB_FIND_DEADLINE = "HTMLView_PendingWebFindDeadline";
	static const UINT_PTR TIMER_PENDING_WEB_FIND = 0x4F39;
	static const UINT PENDING_WEB_FIND_RETRY_MS = 50;
	static const DWORD PENDING_WEB_FIND_TIMEOUT_MS = 10000;

	static bool IsNativeWebFindAcceleratorInProgress()
	{
		return gNativeWebFindAcceleratorUntil && GetTickCount64() < gNativeWebFindAcceleratorUntil;
	}

	static void ArmNativeWebFindAcceleratorGuard()
	{
		gNativeWebFindAcceleratorUntil = GetTickCount64() + 1200;
	}

	static bool HasTickPassed(DWORD deadline)
	{
		return (LONG)(GetTickCount() - deadline) >= 0;
	}

	static bool AreWebFindTriggerKeysReleased()
	{
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 &&
			(GetAsyncKeyState('F') & 0x8000) == 0;
	}

	static void ClearPendingWebFind(HWND hWnd)
	{
		if (!hWnd)
			return;
		KillTimer(hWnd, TIMER_PENDING_WEB_FIND);
		RemoveProp(hWnd, PROP_PENDING_WEB_FIND);
		RemoveProp(hWnd, PROP_PENDING_WEB_FIND_DEADLINE);
	}

	static void ArmPendingWebFind(HWND hWnd)
	{
		if (!hWnd)
			return;

		SetProp(hWnd, PROP_PENDING_WEB_FIND, (HANDLE)1);
		SetProp(hWnd, PROP_PENDING_WEB_FIND_DEADLINE,
			(HANDLE)(UINT_PTR)(GetTickCount() + PENDING_WEB_FIND_TIMEOUT_MS));
		SetTimer(hWnd, TIMER_PENDING_WEB_FIND, PENDING_WEB_FIND_RETRY_MS, NULL);
		DebugLog("issue#9:PendingWebFind", "armed pending Ctrl+F until WebView2 document ready");
	}

	static bool RunPendingWebFindIfReady(HWND hWnd)
	{
		if (!hWnd || !GetProp(hWnd, PROP_PENDING_WEB_FIND))
			return false;

		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd, PROP_BROWSER);
		if (!browser_host)
		{
			DebugLog("issue#9:PendingWebFind", "cancel pending Ctrl+F: browser host is gone");
			ClearPendingWebFind(hWnd);
			return true;
		}

		if (browser_host->mIsWebView2Initialized && browser_host->mIsWebView2DocumentReady && browser_host->mWebViewController)
		{
			if (!AreWebFindTriggerKeysReleased())
			{
				DebugLog("issue#9:PendingWebFind", "waiting trigger keys release before ShowWebFind");
				return false;
			}

			DebugLog("issue#9:PendingWebFind", "WebView2 document ready -> ShowWebFind");
			ClearPendingWebFind(hWnd);
			ArmNativeWebFindAcceleratorGuard();
			browser_host->ShowWebFind();
			return true;
		}

		DWORD deadline = (DWORD)(UINT_PTR)GetProp(hWnd, PROP_PENDING_WEB_FIND_DEADLINE);
		if (!deadline || HasTickPassed(deadline))
		{
			char buf[160]{};
			sprintf_s(buf, "timeout waiting WebView2 hwnd=%p webView2=%d documentReady=%d controller=%p",
				hWnd, browser_host->mIsWebView2Initialized ? 1 : 0,
				browser_host->mIsWebView2DocumentReady ? 1 : 0, browser_host->mWebViewController.p);
			DebugLog("issue#9:PendingWebFind", buf);
			ClearPendingWebFind(hWnd);
			return true;
		}

		return false;
	}

	static bool IsWindowInRoot(HWND root, HWND hwnd)
	{
		return hwnd && root && (hwnd == root || IsChild(root, hwnd) || GetAncestor(hwnd, GA_ROOT) == root);
	}

	static HWND GetFallbackHotkeyBrowserWnd()
	{
		if (!gLastHotkeyBrowserWnd || !IsWindow(gLastHotkeyBrowserWnd) || !GetProp(gLastHotkeyBrowserWnd, PROP_BROWSER))
			return NULL;

		HWND root = GetAncestor(gLastHotkeyBrowserWnd, GA_ROOT);
		HWND foreground = GetForegroundWindow();
		HWND active = GetActiveWindow();
		if (IsWindowInRoot(root, foreground) || IsWindowInRoot(root, active))
			return gLastHotkeyBrowserWnd;

		return NULL;
	}

	static bool ShouldSuppressListerSearchHotkey(MSG* msg, HWND* BrowserWnd)
	{
		if (BrowserWnd)
			*BrowserWnd = NULL;
		if (!msg || !IsIssue9KeyMessage(msg->message) || !IsIssue9Key(msg->wParam))
			return false;

		CAtlString key_name = GetFullKeyName((WORD)msg->wParam);
		if (!IsWebViewSearchHotkey(key_name))
			return false;

		HWND browserFromMsg = GetBrowserHostWnd(msg->hwnd);
		if (browserFromMsg)
		{
			gLastHotkeyBrowserWnd = browserFromMsg;
			return false;
		}

		HWND focus = GetFocus();
		HWND browserFromFocus = GetBrowserHostWnd(focus);
		HWND browserWnd = browserFromFocus ? browserFromFocus : GetFallbackHotkeyBrowserWnd();
		if (!browserWnd)
			return false;

		HWND browserRoot = GetAncestor(browserWnd, GA_ROOT);
		if (!IsWindowInRoot(browserRoot, msg->hwnd))
			return false;

		CBrowserHost* browser_host = (CBrowserHost*)GetProp(browserWnd, PROP_BROWSER);
		if (!browser_host)
			return false;

		if (BrowserWnd)
			*BrowserWnd = browserWnd;
		return true;
	}
}

void RefreshBrowser();

void StoreRefreshParams(const char* FileToLoad, HWND ParentWin, int ShowFlags)
{
	FileToLoadCopy[0] = '\0';
	if (FileToLoad)
		StringCchCopyA(FileToLoadCopy, ARRAYSIZE(FileToLoadCopy), FileToLoad);
	ParentWinCopy = ParentWin;
	ShowFlagsCopy = ShowFlags;
}

LRESULT CALLBACK HookKeybProc(int nCode,WPARAM wParam,LPARAM lParam)
{
	if (nCode<0/* || dbg_DontExecHook*/)
		return CallNextHookEx(hook_keyb, nCode, wParam, lParam);
	HWND focus = GetFocus();
	HWND directBrowserWnd = GetBrowserHostWnd(focus);
	HWND fallbackBrowserWnd = NULL;
	HWND BrowserWnd = directBrowserWnd;
	if (directBrowserWnd)
		gLastHotkeyBrowserWnd = directBrowserWnd;
	else
	{
		fallbackBrowserWnd = GetFallbackHotkeyBrowserWnd();
		BrowserWnd = fallbackBrowserWnd;
	}
	// issue #9 diagnostics: keep this path visible in logs
	{
		bool key_down = (lParam & 0x80000000) == 0;
		bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		char buf[320];
		sprintf_s(buf, "nCode=%d vKey=0x%X(%c) lParam=0x%lX down=%d ctrl=%d focus=%p direct=%p fallback=%p browserWnd=%p last=%p fg=%p active=%p",
			nCode, (unsigned)wParam, (wParam>=0x20 && wParam<0x7F) ? (char)wParam : '?',
			(unsigned long)lParam, key_down ? 1 : 0, ctrl ? 1 : 0, focus,
			directBrowserWnd, fallbackBrowserWnd, BrowserWnd, gLastHotkeyBrowserWnd,
			GetForegroundWindow(), GetActiveWindow());
		DebugLog("issue#9:HookKeybProc", buf);
		if (IsIssue9Key(wParam))
		{
			DebugLogWindowBrief("issue#9:HookKeybProc", "focus", focus);
			DebugLogWindowBrief("issue#9:HookKeybProc", "foreground", GetForegroundWindow());
			DebugLogWindowBrief("issue#9:HookKeybProc", "active", GetActiveWindow());
			DebugLogWindowBrief("issue#9:HookKeybProc", "directBrowserWnd", directBrowserWnd);
			DebugLogWindowBrief("issue#9:HookKeybProc", "fallbackBrowserWnd", fallbackBrowserWnd);
			DebugLogWindowBrief("issue#9:HookKeybProc", "lastBrowserWnd", gLastHotkeyBrowserWnd);
		}
	}
	if(BrowserWnd)
	{
		DebugLog("issue#9:HookKeybProc", "SendMessage WM_IEVIEW_HOTKEY begin");
		SendMessage(BrowserWnd,WM_IEVIEW_HOTKEY,wParam,lParam);
		DebugLog("issue#9:HookKeybProc", "SendMessage WM_IEVIEW_HOTKEY end");
	}
	return CallNextHookEx(hook_keyb, nCode, wParam, lParam);
}

LRESULT CALLBACK HookGetMsgProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0 && lParam)
	{
		MSG* msg = (MSG*)lParam;
		if (msg && IsIssue9KeyMessage(msg->message) && IsIssue9Key(msg->wParam))
		{
			CAtlString key_name = GetFullKeyName((WORD)msg->wParam);
			char buf[320]{};
			sprintf_s(buf, "%s remove=%lu hwnd=%p vKey=0x%X key='%s' lParam=0x%lX focus=%p browserFromMsg=%p fg=%p active=%p",
				MessageName(msg->message), (unsigned long)wParam, msg->hwnd,
				(unsigned)msg->wParam, (const char*)key_name, (unsigned long)msg->lParam,
				GetFocus(), GetBrowserHostWnd(msg->hwnd), GetForegroundWindow(), GetActiveWindow());
			DebugLog("issue#9:HookGetMsgProc", buf);
			DebugLogWindowBrief("issue#9:HookGetMsgProc", "msg.hwnd", msg->hwnd);
			DebugLogWindowBrief("issue#9:HookGetMsgProc", "focus", GetFocus());
			DebugLogWindowBrief("issue#9:HookGetMsgProc", "foreground", GetForegroundWindow());
			DebugLogWindowBrief("issue#9:HookGetMsgProc", "active", GetActiveWindow());
			DebugLogWindowBrief("issue#9:HookGetMsgProc", "lastBrowserWnd", gLastHotkeyBrowserWnd);
		}

		HWND suppressedForBrowser = NULL;
		if (ShouldSuppressListerSearchHotkey(msg, &suppressedForBrowser))
		{
			char suppressBuf[240]{};
			CAtlString key_name = GetFullKeyName((WORD)msg->wParam);
			sprintf_s(suppressBuf, "suppress duplicate Lister search hotkey %s key='%s' hwnd=%p browser=%p",
				MessageName(msg->message), (const char*)key_name, msg->hwnd, suppressedForBrowser);
			DebugLog("issue#9:HookGetMsgProc", suppressBuf);
			msg->message = WM_NULL;
			msg->wParam = 0;
			msg->lParam = 0;
		}
	}
	return CallNextHookEx(hook_getmsg, nCode, wParam, lParam);
}

void InitProc()
{
	if(!options.valid)
		InitOptions();

	if(!hook_keyb&&(options.flags&OPT_KEEPHOOKNOWINDOWS))
		hook_keyb = SetWindowsHookEx(WH_KEYBOARD, HookKeybProc, hinst, (options.flags&OPT_GLOBALHOOK)?0:GetCurrentThreadId());
	if(!hook_getmsg&&(options.flags&OPT_KEEPHOOKNOWINDOWS))
		hook_getmsg = SetWindowsHookEx(WH_GETMESSAGE, HookGetMsgProc, hinst, (options.flags&OPT_GLOBALHOOK)?0:GetCurrentThreadId());
	if(!img_list)
	{
		unsigned char toolbar_bpp = (options.toolbar>>2)&3;
		if(toolbar_bpp==2)
			toolbar_bpp = IsWindowsXPOrGreater() ? 1 : 0;
		if(toolbar_bpp==1)
			img_list = ImageList_LoadImage(hinst, MAKEINTRESOURCE(IDB_BITMAP2), 24, 0, CLR_NONE, IMAGE_BITMAP, LR_CREATEDIBSECTION);
		else
			img_list = ImageList_LoadImage(hinst, MAKEINTRESOURCE(IDB_BITMAP1), 24, 0, CLR_DEFAULT, IMAGE_BITMAP, LR_CREATEDIBSECTION);
	}

	if(!markdown_extensions.valid())
		markdown_extensions.load_from_ini(options.IniFileName, "Extensions", "MarkdownExtensions");
	if(!html_extensions.valid())
		html_extensions.load_from_ini(options.IniFileName, "Extensions", "HTMLExtensions");
	if(!def_signatures.valid())
		def_signatures.load_sign_from_ini(options.IniFileName, "Extensions", "DefaultSignatures");
	if(!typing_trans_hotkeys.valid())
		typing_trans_hotkeys.load_from_ini(options.IniFileName, "Hotkeys", "TypingTranslationHotkeys");
	if(!trans_hotkeys.valid())
		trans_hotkeys.load_from_ini(options.IniFileName, "Hotkeys", "TranslationHotkeys");

	GetPrivateProfileString("Renderer", "Extensions", "", &renderer_extensions[0], 2048, options.IniFileName);
	GetPrivateProfileString("Renderer", "CustomCSS", "", &html_template[0], 512, options.IniFileName);
	GetPrivateProfileString("Renderer", "CustomCSSDark", "", &html_template_dark[0], 512, options.IniFileName);
	InitTranslateSettings();
	InitSyntaxHighlightSettings();
	InitFastFontSettings();
	if (img_list && gTranslateToolbarImageIndex < 0)
	{
		HICON icon32 = LoadTranslateIcon32();
		if (icon32)
		{
			HICON icon24 = (HICON)CopyImage(icon32, IMAGE_ICON, 24, 24, 0);
			if (!icon24)
				icon24 = icon32;
			int idx = ImageList_AddIcon(img_list, icon24);
			if (icon24 != icon32)
				DestroyIcon(icon24);
			if (idx >= 0)
				gTranslateToolbarImageIndex = idx;
		}
	}
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if(message==WM_CREATE)
	{
	}
	else if(message==WM_DESTROY && !(options.flags&OPT_QUICKQIUT))
	{
		ClearPendingWebFind(hWnd);
		HWND status = (HWND)GetProp(hWnd, PROP_STATUS);
		HWND toolbar = (HWND)GetProp(hWnd, PROP_TOOLBAR);
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		{
			std::lock_guard<std::mutex> lock(gMdRenderMutex);
			gMdRenderCurrentToken.erase(hWnd);
			gMdRenderResults.erase(hWnd);
		}
		RemoveProp(hWnd, PROP_BROWSER);
		RemoveProp(hWnd, PROP_STATUS);
		RemoveProp(hWnd, PROP_TOOLBAR);
		if(status)
			DestroyWindow(status);
		if(toolbar)
			DestroyWindow(toolbar);
		if(browser_host)
		{
			if(options.flags&OPT_SAVEPOS)
				browser_host->SavePosition();
			browser_host->Quit();
		}
	}
	else if(message==WM_TIMER && wParam==TIMER_PENDING_WEB_FIND)
	{
		RunPendingWebFindIfReady(hWnd);
		return 0;
	}
	else if(message==WM_SIZE)
	{
		HWND status = (HWND)GetProp(hWnd, PROP_STATUS);
		if(status)
		{
			RECT status_rc,rc;
			GetClientRect(hWnd,&rc);
			GetWindowRect(status,&status_rc);
			MoveWindow(status,0,rc.bottom-(status_rc.bottom-status_rc.top),rc.right-rc.left,status_rc.bottom-status_rc.top,TRUE);
			InvalidateRect(status,NULL,TRUE);
		}
		HWND toolbar = (HWND)GetProp(hWnd, PROP_TOOLBAR);
		if(toolbar)
		{
			SendMessage(toolbar, TB_AUTOSIZE, 0, 0);
			DWORD btnSize = (DWORD)SendMessage(toolbar, TB_GETBUTTONSIZE, 0, 0);
			int toolbarHeight = (int)HIWORD(btnSize);
			if (toolbarHeight <= 0)
			{
				RECT toolbar_rc;
				GetWindowRect(toolbar, &toolbar_rc);
				toolbarHeight = toolbar_rc.bottom - toolbar_rc.top;
				if (toolbarHeight <= 0)
					toolbarHeight = 28;
			}

			RECT rc{};
			GetClientRect(hWnd, &rc);
			MoveWindow(toolbar, 0, 0, rc.right - rc.left, toolbarHeight, TRUE);
			InvalidateRect(toolbar,NULL,TRUE);
		}

		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		if(browser_host)
			browser_host->Resize();

		if (status && IsWindow(status))
			SetWindowPos(status, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		if (toolbar && IsWindow(toolbar))
			SetWindowPos(toolbar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	else if (message == WM_MD_RENDER_DONE)
	{
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd, PROP_BROWSER);
		if (browser_host)
		{
			std::string html;
			{
				std::lock_guard<std::mutex> lock(gMdRenderMutex);
				auto tokIt = gMdRenderCurrentToken.find(hWnd);
				auto resIt = gMdRenderResults.find(hWnd);
				if (tokIt != gMdRenderCurrentToken.end() && resIt != gMdRenderResults.end() && tokIt->second == resIt->second.token)
				{
					html = std::move(resIt->second.html);
					gMdRenderResults.erase(resIt);
				}
			}
			if (!html.empty())
				browser_host->LoadWebBrowserFromStreamWrapper((const BYTE*)html.data(), (int)html.size());
		}
		return 0;
	}
	else if(message==WM_SETFOCUS)
	{
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		if(browser_host)
			browser_host->Focus();
	}
	else if(message==WM_COMMAND)
	{
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		HWND toolbar = (HWND)GetProp(hWnd, PROP_TOOLBAR);
		if (browser_host && (lParam == (LPARAM)toolbar))
		{
			switch(LOWORD(wParam))
			{
			case TBB_BACK:
				if (lParam == (LPARAM)toolbar)
					browser_host->GoBack();
				break;
			case TBB_FORWARD:
				if (lParam == (LPARAM)toolbar)
					browser_host->GoForward();
				break;
			case TBB_STOP:
				if (lParam == (LPARAM)toolbar)
					browser_host->Stop();
				break;
			case TBB_REFRESH:
				if (lParam == (LPARAM)toolbar)
					RefreshBrowser(); // instead of browser_host->mWebBrowser->Refresh();
				break;
			case TBB_PRINT:
				if (lParam == (LPARAM)toolbar)
					SendMessage(hWnd, WM_IEVIEW_PRINT, 0, 0);
				break;
			case TBB_COPY:
				if (lParam == (LPARAM)toolbar)
					SendMessage(hWnd, WM_IEVIEW_COMMAND, lc_copy, 0);
				break;
			/*case TBB_PASTE:
				SendMessage(hWnd, WM_IEVIEW_COMMAND, lc_ieview_paste, 0);
				break;*/
			case TBB_SEARCH:
				if (lParam == (LPARAM)toolbar)
				{
					if(browser_host->mFocusType==fctQuickView)
						SetFocus(hWnd);
					DebugLog("issue#9:TBB_SEARCH", "toolbar search -> ShowWebFind");
					browser_host->ShowWebFind();
				}
				break;
			case TBB_TRANSLATE:
				browser_host->ExecuteScript(L"if(window.__mdv_gt_load) window.__mdv_gt_load();");
				break;
			case TBB_FASTFONT:
				if (lParam == (LPARAM)toolbar)
				{
					LRESULT st = SendMessage(toolbar, TB_GETSTATE, TBB_FASTFONT, 0);
					HWND parent = GetParent(hWnd);
					if (st != -1 && (st & TBSTATE_CHECKED))
					{
						SetProp(parent, PROP_FASTFONT, (HANDLE)1);
						browser_host->ApplyFastFont();
					}
					else
					{
						RemoveProp(parent, PROP_FASTFONT);
						browser_host->RemoveFastFont();
					}
				}
				break;
			}
		}
	}
	else if(message==WM_NOTIFY)
	{
		LPNMHDR hdr = (LPNMHDR)lParam;
		if (hdr && hdr->code == TBN_GETINFOTIPW)
		{
			LPNMTBGETINFOTIPW tip = (LPNMTBGETINFOTIPW)lParam;
			if (tip->iItem == TBB_FASTFONT && tip->pszText && tip->cchTextMax > 0)
			{
				const wchar_t* text = GetFastFontTooltip();
				wcsncpy_s(tip->pszText, tip->cchTextMax, text, _TRUNCATE);
				return 0;
			}
		}
	}
	else if(message==WM_IEVIEW_SEARCH||message==WM_IEVIEW_SEARCHW)
	{
		// issue #9 diagnostics: keep this path visible in logs
		{
			char buf[160];
			if (message == WM_IEVIEW_SEARCH)
				sprintf_s(buf, "WM_IEVIEW_SEARCH (ANSI) text='%s' lParam=0x%lX",
					wParam ? (const char*)wParam : "(null)", (unsigned long)lParam);
			else
				sprintf_s(buf, "WM_IEVIEW_SEARCHW (Wide) lParam=0x%lX", (unsigned long)lParam);
			DebugLog("issue#9:WM_IEVIEW_SEARCH", buf);
		}

		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd ,PROP_BROWSER);
		if(browser_host)
		{
			long flags = 0;
			//if(lParam&lcs_findfirst)
			//	flags |= ;
			if(lParam&lcs_matchcase)
				flags |= 4;
			if(lParam&lcs_wholewords)
				flags |= 2;
			//if(lParam&lcs_backwards)
			//	flags |= 1;
			if(message==WM_IEVIEW_SEARCH)
				browser_host->FindText(CComBSTR((char*)wParam), flags, lParam&lcs_backwards);
			else if(message==WM_IEVIEW_SEARCHW)
				browser_host->FindText(CComBSTR((WCHAR*)wParam), flags, lParam&lcs_backwards);
		}
	}
	else if(message==WM_IEVIEW_PRINT)
	{
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd ,PROP_BROWSER);
		if (browser_host)
			browser_host->Print();
	}
	else if(message==WM_IEVIEW_COMMAND)
	{
		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		if ( browser_host )
		{
			if (browser_host->mIsWebView2Initialized && browser_host->mWebView)
			{
				if (wParam == lc_selectall)
					browser_host->mWebView->ExecuteScript(L"document.execCommand('selectAll');", nullptr);
				else if (wParam == lc_copy)
					browser_host->mWebView->ExecuteScript(L"document.execCommand('copy');", nullptr);
			}
			else if (browser_host->mWebBrowser)
			{
				CComQIPtr<IOleCommandTarget, &IID_IOleCommandTarget> pCmd = browser_host->mWebBrowser;
				if ( pCmd )
				{
					if(wParam==lc_selectall)
						pCmd->Exec(NULL, OLECMDID_SELECTALL, OLECMDEXECOPT_DODEFAULT, NULL,NULL);
					else if(wParam==lc_copy)
						pCmd->Exec(NULL, OLECMDID_COPY, OLECMDEXECOPT_DODEFAULT, NULL,NULL);
				}
			}
		}
	}
	else if(IsIssue9KeyMessage(message) && IsIssue9Key(wParam))
	{
		CAtlString key_name = GetFullKeyName((WORD)wParam);
		char buf[320]{};
		sprintf_s(buf, "%s hwnd=%p vKey=0x%X key='%s' lParam=0x%lX focus=%p fg=%p active=%p",
			MessageName(message), hWnd, (unsigned)wParam, (const char*)key_name,
			(unsigned long)lParam, GetFocus(), GetForegroundWindow(), GetActiveWindow());
		DebugLog("issue#9:WndProcKey", buf);
		DebugLogWindowBrief("issue#9:WndProcKey", "hWnd", hWnd);
		DebugLogWindowBrief("issue#9:WndProcKey", "parent", GetParent(hWnd));
		DebugLogWindowBrief("issue#9:WndProcKey", "focus", GetFocus());
	}
	else if(message==WM_IEVIEW_HOTKEY)
	{
		// issue #9 diagnostics: keep this path visible in logs
		{
			char buf[120];
			sprintf_s(buf, "vKey=0x%X(%c) lParam=0x%lX",
				(unsigned)wParam, (wParam>=0x20 && wParam<0x7F) ? (char)wParam : '?',
				(unsigned long)lParam);
			DebugLog("issue#9:WM_IEVIEW_HOTKEY", buf);
		}

		CBrowserHost* browser_host = (CBrowserHost*)GetProp(hWnd,PROP_BROWSER);
		if ( browser_host )
		{
			bool alt_down = 0x20000000&lParam;
			bool key_down = 0x80000000&lParam;
			UINT Msg = key_down?(alt_down?WM_SYSKEYUP:WM_KEYUP):(alt_down?WM_SYSKEYDOWN:WM_KEYDOWN);
			CAtlString key_name = GetFullKeyName((WORD)wParam);
			bool formFocused = browser_host->FormFocused();
			char stateBuf[360]{};
			bool webViewSearchHotkey = IsWebViewSearchHotkey(key_name);
			bool webViewSearchKey = browser_host->mIsWebView2Initialized && browser_host->mIsWebView2DocumentReady && webViewSearchHotkey;
			sprintf_s(stateBuf, "%s key='%s' translatedMsg=%s webView2=%d documentReady=%d webSearchKey=%d formFocused=%d capture=%p parent=%p focus=%p fg=%p active=%p",
				MessageName(message), (const char*)key_name, MessageName(Msg),
				browser_host->mIsWebView2Initialized ? 1 : 0,
				browser_host->mIsWebView2DocumentReady ? 1 : 0,
				webViewSearchKey ? 1 : 0,
				formFocused ? 1 : 0, GetCapture(), GetParent(hWnd), GetFocus(), GetForegroundWindow(), GetActiveWindow());
			DebugLog("issue#9:WM_IEVIEW_HOTKEY", stateBuf);
			DebugLogWindowBrief("issue#9:WM_IEVIEW_HOTKEY", "hWnd", hWnd);
			DebugLogWindowBrief("issue#9:WM_IEVIEW_HOTKEY", "parent", GetParent(hWnd));
			if(key_name=="Ctrl+Insert")
				SendMessage(hWnd, WM_IEVIEW_COMMAND, lc_copy, 0);
			if(webViewSearchHotkey && (!browser_host->mIsWebView2Initialized || !browser_host->mIsWebView2DocumentReady))
			{
				if (Msg == WM_KEYDOWN && key_name == "Ctrl+F")
				{
					DebugLog("issue#9:WM_IEVIEW_HOTKEY", browser_host->mIsWebView2Initialized ?
						"Ctrl+F deferred until WebView2 document ready" :
						"Ctrl+F deferred until WebView2 initialization");
					ArmPendingWebFind(hWnd);
				}
				else
				{
					DebugLog("issue#9:WM_IEVIEW_HOTKEY", "skip plugin replay; search hotkey before WebView2 initialization");
				}
				return 0;
			}
			if(webViewSearchKey)
			{
				if (IsNativeWebFindAcceleratorInProgress())
				{
					DebugLog("issue#9:WM_IEVIEW_HOTKEY", "skip plugin replay; native WebView2 find accelerator is in progress");
					return 0;
				}
				if (Msg == WM_KEYDOWN && key_name == "Ctrl+F")
				{
					DebugLog("issue#9:WM_IEVIEW_HOTKEY", "Ctrl+F queued until WebView2 document ready and trigger keys released");
					ArmPendingWebFind(hWnd);
					return 0;
				}
				DebugLog("issue#9:WM_IEVIEW_HOTKEY", "skip plugin replay; WebView2 owns F3/Shift+F3 search");
				return 0;
			}
			if(formFocused)
			{
				if(typing_trans_hotkeys.find(key_name)&&!GetCapture())
					SendMessage(hWnd, Msg, wParam, lParam);
			}
			else
			{
				if(trans_hotkeys.find(key_name)&&!GetCapture())
					SendMessage(hWnd, Msg, wParam, lParam);
			}
			browser_host->ProcessHotkey(Msg, (DWORD)(UINT_PTR)wParam, (DWORD)(ULONG_PTR)lParam);
		}
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}

HWND Create_Toolbar(HWND ListWin)
{
	if (img_list && gTranslateToolbarImageIndex < 0)
	{
		HICON icon32 = LoadTranslateIcon32();
		if (icon32)
		{
			HICON icon24 = (HICON)CopyImage(icon32, IMAGE_ICON, 24, 24, 0);
			if (!icon24)
				icon24 = icon32;
			int idx = ImageList_AddIcon(img_list, icon24);
			if (icon24 != icon32)
				DestroyIcon(icon24);
			if (idx >= 0)
				gTranslateToolbarImageIndex = idx;
		}
	}

	if (img_list && gFastFontToolbarImageIndex < 0)
	{
		HICON icon = LoadFastFontIcon24();
		if (icon)
		{
			int idx = ImageList_AddIcon(img_list, icon);
			if (idx >= 0)
				gFastFontToolbarImageIndex = idx;
		}
	}

	BYTE fastFontState = TBSTATE_ENABLED;
	if (GetProp(GetParent(ListWin), PROP_FASTFONT))
		fastFontState |= TBSTATE_CHECKED;

	TBBUTTON tb_buttons[] =
	{
		{0, TBB_BACK,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{1, TBB_FORWARD,	TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{2, TBB_STOP,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{3, TBB_REFRESH,	TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{4, 0,				TBSTATE_ENABLED, BTNS_SEP,	  NULL},
		{5, TBB_COPY,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		//{6, TBB_PASTE,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{4, 0,				TBSTATE_ENABLED, BTNS_SEP,	  NULL},
		{4, TBB_PRINT,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{4, 0,				TBSTATE_ENABLED, BTNS_SEP,	  NULL},
		{7, TBB_SEARCH,		TBSTATE_ENABLED, BTNS_BUTTON, NULL},
		{4, 0,				TBSTATE_ENABLED, BTNS_SEP,	  NULL},
		{gFastFontToolbarImageIndex >= 0 ? gFastFontToolbarImageIndex : I_IMAGENONE, TBB_FASTFONT, fastFontState, BTNS_CHECK, NULL},
		{4, 0,				TBSTATE_ENABLED, BTNS_SEP,	  NULL},
		{gTranslateToolbarImageIndex >= 0 ? gTranslateToolbarImageIndex : I_IMAGENONE, TBB_TRANSLATE, (BYTE)(gTranslateEnabled ? TBSTATE_ENABLED : 0), BTNS_BUTTON, NULL}
	};
	const int tb_count = (int)(sizeof(tb_buttons) / sizeof(tb_buttons[0]));
	for (int i = 0; i < tb_count; i++)
		tb_buttons[i].iString = -1;

	char parent_class_name[64];
	GetClassName(GetParent(ListWin), parent_class_name, 64);
	if(strncmp(parent_class_name, "TFormViewUV", 11)==0)
		tb_buttons[5].fsState = tb_buttons[6].fsState = tb_buttons[9].fsState = TBSTATE_HIDDEN;

	// Кнопка бионического шрифта скрывается, если в ini выставлено [FastFont] Enabled=0.
	// Скрываем и кнопку (индекс 11), и идущий за ней разделитель (12), чтобы между
	// поиском и Translate не оставалось двух разделителей подряд.
	if (!gFastFontButtonEnabled)
		tb_buttons[11].fsState = tb_buttons[12].fsState = TBSTATE_HIDDEN;

	HWND toolbar = CreateWindowEx(0, TOOLBARCLASSNAME, NULL, WS_CHILD | WS_CLIPSIBLINGS | CCS_TOP | CCS_NODIVIDER | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS, 0, 0, 0, 0, ListWin, NULL, hinst, NULL);
	SetProp(ListWin, PROP_TOOLBAR, toolbar);
	SendMessage(toolbar, TB_BUTTONSTRUCTSIZE, (WPARAM) sizeof(TBBUTTON), 0);
	SendMessage(toolbar, TB_SETIMAGELIST, 0, (LPARAM)img_list);
	SendMessage(toolbar, TB_SETUNICODEFORMAT, TRUE, 0);
	SendMessage(toolbar, TB_SETPADDING, 0, MAKELPARAM(0, 0));

	SendMessage(toolbar, TB_ADDBUTTONS, (WPARAM)tb_count, (LPARAM)&tb_buttons);
	SendMessage(toolbar, TB_AUTOSIZE, 0, 0);

	ShowWindow(toolbar, SW_SHOW);
	{
		RECT rc{};
		GetClientRect(ListWin, &rc);
		SendMessage(ListWin, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
	}
	return toolbar;
}

CComBSTR GetUrlFromFilename(char* FileToLoad)
{
	if (!FileToLoad || !FileToLoad[0])
		return NULL;

	CAtlString url;
	char ext[MAX_PATH];
	_splitpath(FileToLoad, NULL, NULL, NULL, ext);
	strlwr(ext);
	size_t len = strlen(FileToLoad);
	if ((options.flags & OPT_DIRS) && len > 0 && FileToLoad[len - 1] == '\\')
		url = FileToLoad;
	else if( html_extensions.find(ext+1) )
		url = FileToLoad;
	else if(def_signatures.check_signature(FileToLoad, options.flags&OPT_SIGNSKIPSPACES))
		url = FileToLoad;
	if(url.IsEmpty() || url.Right(3)=="..\\")
		return NULL;
	return CComBSTR(url);
}

void do_events()
{
	MSG msg;
	BOOL result;

	while (::PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
	{
		result = ::GetMessage(&msg, NULL, 0, 0);
		if (result == 0) // WM_QUIT
		{
			::PostQuitMessage((int)msg.wParam);
			break;
		}
		else if (result == -1)
		{
			// Handle errors/exit application, etc.
		}
		else
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}
}

void prepare_browser(CBrowserHost* browser_host)
{
	browser_host->Navigate(L"about:blank");

	// for WebView2 we don't need to wait for about:blank usually,
	// but for IE we do.
	if (browser_host->mWebBrowser) {
		READYSTATE rs;
		do
		{
			browser_host->mWebBrowser->get_ReadyState(&rs);
			do_events();
		} while (rs != READYSTATE_COMPLETE);
	}
}

void CleanupTempHtmlFile()
{
	if (TempHtmlFilePath[0] != L'\0')
	{
		DeleteFileW(TempHtmlFilePath);
		TempHtmlFilePath[0] = L'\0';
	}
}

void browser_show_file(CBrowserHost* browserHost, const char* filename, bool useDarkTheme)
{
	InitTranslateSettings();
	InitFastFontSettings();
	DebugLog("main.cpp:browser_show_file", useDarkTheme ? "dark=1" : "dark=0");
	InitSyntaxHighlightSettings();

	std::string shSuffix = gSyntaxHighlightEnabled
		? std::string("|sh:on|sh-theme:") + (useDarkTheme ? "dark" : "light")
		: std::string("|sh:off");

	CHAR css[MAX_PATH];
	css[0] = '\0';
	GetModuleFileName(hinst, css, MAX_PATH);
	PathRemoveFileSpec(css);
	StringCchCatA(css, ARRAYSIZE(css), "\\");
	StringCchCatA(css, ARRAYSIZE(css), useDarkTheme ? html_template_dark : html_template);

	CleanupTempHtmlFile();

	std::wstring wFile;
	if (!TryAnyToWide(filename, wFile))
	{
		std::string html = "<html><body><h1>Error</h1><p>Failed to decode file name.</p></body></html>";
		browserHost->LoadWebBrowserFromStreamWrapper((const BYTE*)html.data(), (int)html.size());
		return;
	}

	wchar_t folderPath[MAX_PATH];
	if (SUCCEEDED(StringCchCopyW(folderPath, ARRAYSIZE(folderPath), wFile.c_str())))
	{
		PathRemoveFileSpecW(folderPath);
		browserHost->UpdateFolderMapping(folderPath);
	}
	else
	{
		folderPath[0] = L'\0';
		browserHost->UpdateFolderMapping(L"");
	}

	if (browserHost->mIsWebView2Initialized && browserHost->mWebView) {
		CComQIPtr<ICoreWebView2_3> webView3 = browserHost->mWebView;
		if (webView3) {
			ULONGLONG fileSize = 0;
			ULONGLONG lastWrite = 0;

			bool hasInfo = TryGetFileInfo(wFile, fileSize, lastWrite);

			std::wstring wCss;
			TryMultiByteToWide(CP_ACP, css, wCss);

			std::wstring wExt;
			TryMultiByteToWide(CP_ACP, renderer_extensions, wExt);
			if (gTranslateEnabled) {
				std::string trKey = std::string("|gt:") + (gTranslateTargetLang[0] ? gTranslateTargetLang : "ru") + (gTranslateAuto ? "|1" : "|0");
				int trLen = MultiByteToWideChar(CP_ACP, 0, trKey.c_str(), (int)trKey.size(), NULL, 0);
				if (trLen > 0) {
					std::wstring wTr(trLen, 0);
					MultiByteToWideChar(CP_ACP, 0, trKey.c_str(), (int)trKey.size(), &wTr[0], trLen);
					wExt.append(ToLowerWin(std::move(wTr)));
				}
			}

			{
				int shLen = MultiByteToWideChar(CP_ACP, 0, shSuffix.c_str(), (int)shSuffix.size(), NULL, 0);
				std::wstring wSh(shLen, 0);
				MultiByteToWideChar(CP_ACP, 0, shSuffix.c_str(), (int)shSuffix.size(), &wSh[0], shLen);
				wExt.append(ToLowerWin(std::move(wSh)));
			}

			MarkdownHtmlCacheKey key{
				ToLowerWin(std::wstring(wFile)),
				ToLowerWin(std::move(wCss)),
				ToLowerWin(std::move(wExt)),
				lastWrite,
				fileSize
			};

			std::string cachedHtml;
			if (hasInfo && TryGetCachedHtml(key, cachedHtml)) {
				browserHost->LoadWebBrowserFromStreamWrapper((const BYTE*)cachedHtml.data(), (int)cachedHtml.length());
				return;
			}

			std::string loading = BuildLoadingHtml();
			browserHost->LoadWebBrowserFromStreamWrapper((const BYTE*)loading.data(), (int)loading.length());

			HWND targetWnd = browserHost->mParentWin;
			unsigned long long token = ++gMdRenderToken;
			{
				std::lock_guard<std::mutex> lock(gMdRenderMutex);
				gMdRenderCurrentToken[targetWnd] = token;
				gMdRenderResults.erase(targetWnd);
			}

			std::string fileStr(filename);
			std::string cssStr(css);
			std::string extStr = std::string(renderer_extensions) + shSuffix;
			std::string baseHref = BuildBaseHref(browserHost, folderPath);

			std::thread([targetWnd, token, fileStr = std::move(fileStr), cssStr = std::move(cssStr), extStr = std::move(extStr), baseHref = std::move(baseHref), key = std::move(key), hasInfo]() mutable {
				Markdown md = Markdown();
				std::string html = md.ConvertToHtmlAscii(fileStr, cssStr, extStr);
				EnsureBaseTag(html, baseHref);
				InjectInPageAnchorHandler(html);
				InjectGoogleTranslateWidget(html);
				if (hasInfo)
					PutCachedHtml(key, html);

				bool shouldPost = false;
				{
					std::lock_guard<std::mutex> lock(gMdRenderMutex);
					auto it = gMdRenderCurrentToken.find(targetWnd);
					if (it != gMdRenderCurrentToken.end() && it->second == token) {
						gMdRenderResults[targetWnd] = MdRenderResult{ token, std::move(html) };
						shouldPost = true;
					}
				}
				if (shouldPost)
					PostMessage(targetWnd, WM_MD_RENDER_DONE, 0, 0);
			}).detach();

			return;
		}
	}

	Markdown md = Markdown();
	std::string html = md.ConvertToHtmlAscii(std::string(filename), std::string(css), std::string(renderer_extensions) + shSuffix);
	std::string baseHref = BuildBaseHref(browserHost, folderPath);
	EnsureBaseTag(html, baseHref);
	InjectInPageAnchorHandler(html);
	InjectGoogleTranslateWidget(html);

	if (!BuildSafeTempHtmlPath(TempHtmlFilePath, ARRAYSIZE(TempHtmlFilePath)))
	{
		browserHost->LoadWebBrowserFromStreamWrapper((const BYTE*)html.data(), (int)html.size());
		return;
	}

	FILE* f = _wfopen(TempHtmlFilePath, L"wb");
	if (f)
	{
		const unsigned char utf8Bom[3] = { 0xEF, 0xBB, 0xBF };
		fwrite(utf8Bom, 1, 3, f);
		fwrite(html.data(), 1, html.size(), f);
		fclose(f);

        browserHost->Navigate(TempHtmlFilePath);
	}
	else
	{
		browserHost->LoadWebBrowserFromStreamWrapper((const BYTE*)html.data(), (int)html.size());
	}
}

bool is_markdown(const char* FileToLoad)
{
	CAtlString url;
	char ext[MAX_PATH];
	_splitpath(FileToLoad, NULL, NULL, NULL, ext);
	strlwr(ext);

	bool isMd = markdown_extensions.find(ext + 1);
	char msg[256]{};
	sprintf_s(msg, "ext=%s is_markdown=%d", ext, isMd ? 1 : 0);
	DebugLog("main.cpp:is_markdown", msg);
	return isMd;
}

int __stdcall ListLoadNext(HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags)
{
	if (!FileToLoad || !FileToLoad[0])
		return LISTPLUGIN_ERROR;

	CComBSTR url = GetUrlFromFilename(FileToLoad);
	if (url.Length() == 0 && !is_markdown(FileToLoad))
		return LISTPLUGIN_ERROR;

	CBrowserHost* browser_host = (CBrowserHost*)GetProp(PluginWin, PROP_BROWSER);
	if(!browser_host)
		return LISTPLUGIN_ERROR;

	StoreRefreshParams(FileToLoad, ParentWin, ShowFlags);

	if (is_markdown(FileToLoad))
		browser_show_file(browser_host, FileToLoad, ShowFlags & lcp_darkmode);
	else
		browser_host->Navigate(url);

	return LISTPLUGIN_OK;
}

HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
{
	char msgFlags[64]{};
	sprintf_s(msgFlags, "ShowFlags=0x%X", (unsigned int)ShowFlags);
	DebugLog("main.cpp:ListLoad", msgFlags);
	HRESULT hrOle = OleInitialize(NULL);
	InitProc();

	// RAII-guard: на любом раннем выходе из ListLoad без передачи владения окном
	// в ListCloseWindow вызывается OleUninitialize. Иначе на каждый неудачный
	// ListLoad утекала OLE-инициализация (и удерживались COM proxy DLL).
	struct OleGuard {
		bool dismissed = false;
		~OleGuard() { if (!dismissed) OleUninitialize(); }
	} oleGuard;

	// RAII-guard: InitProc() выше может загрузить PNG-иконки тулбара через
	// LoadTranslateIcon32/LoadFastFontIcon24, что стартует GDI+ ещё до того,
	// как окно принято Total Commander. Если ListLoad вернёт NULL на ранней
	// проверке, ListCloseWindow для этого случая не вызовется и GDI+ останется
	// инициализированным до выгрузки DLL — нарушая парность Startup/Shutdown.
	// На неуспешных путях RAII вызывает ShutdownGdiPlusIfIdle (он закроет GDI+
	// только если других Lister-окон не осталось).
	struct GdiPlusGuard {
		bool dismissed = false;
		~GdiPlusGuard() { if (!dismissed) ShutdownGdiPlusIfIdle(); }
	} gdiPlusGuard;

	if (!FileToLoad || !FileToLoad[0])
		return NULL;

	CComBSTR url = GetUrlFromFilename(FileToLoad);

	if (url.Length() == 0 && !is_markdown(FileToLoad))
		return NULL;

	RECT Rect;
	GetClientRect(ParentWin, &Rect);

	HWND ListWin;
	HWND status;
	HWND toolbar;
	CBrowserHost* browser_host;
	bool qiuck_view = WS_CHILD&GetWindowLong(ParentWin, GWL_STYLE);
	bool need_toolbar = (!qiuck_view&&(options.toolbar&1))||(qiuck_view&&(options.toolbar&2));
	bool need_statusbar = (!qiuck_view&&(options.status&1))||(qiuck_view&&(options.status&2));

	ListWin = CreateWindow(MAIN_WINDOW_CLASS, "IEViewMainWindow", WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, Rect.right, Rect.bottom, ParentWin, NULL, hinst, NULL);
	if(!ListWin)
		return NULL;
	if( need_statusbar )
		status = CreateStatusWindow(WS_CHILD|WS_VISIBLE,"",ListWin,0);
	else
		status = NULL;
	SetProp(ListWin, PROP_STATUS, status);
	if( need_toolbar )
		toolbar = Create_Toolbar(ListWin);
	else
		toolbar = NULL;
	SetProp(ListWin, PROP_TOOLBAR, toolbar);
	browser_host = new CBrowserHost;

	browser_host->mFocusType = qiuck_view?fctQuickView:fctLister;
	if(!browser_host->CreateBrowser(ListWin))
	{
        browser_host->Release();
		DestroyWindow(ListWin);
		return NULL;
	}

	StoreRefreshParams(FileToLoad, ParentWin, ShowFlags);

	if(is_markdown(FileToLoad))
		browser_show_file(browser_host, FileToLoad, ShowFlags & lcp_darkmode);
	else
		browser_host->Navigate(url);

	SetProp(ListWin, PROP_BROWSER, browser_host);

	if(/*!(options.flags&OPT_KEEPHOOKNOWINDOWS)&&*/hook_keyb==NULL/*&&num_lister_windows==0*/)
		hook_keyb = SetWindowsHookEx(WH_KEYBOARD, HookKeybProc, hinst, (options.flags&OPT_GLOBALHOOK)?0:GetCurrentThreadId());
	if(hook_getmsg==NULL)
		hook_getmsg = SetWindowsHookEx(WH_GETMESSAGE, HookGetMsgProc, hinst, (options.flags&OPT_GLOBALHOOK)?0:GetCurrentThreadId());
	++num_lister_windows;

	// Окно создано и принимается Total Commander — владение OLE-инициализацией
	// и GDI+ (через num_lister_windows-счётчик) переходит в ListCloseWindow.
	oleGuard.dismissed = true;
	gdiPlusGuard.dismissed = true;
	return ListWin;
}

void RefreshBrowser()
{
	ListLoad(ParentWinCopy, FileToLoadCopy, ShowFlagsCopy);
}

int __stdcall ListSendCommand(HWND ListWin,int Command,int Parameter)
{
	if(Command==lc_copy || Command==lc_selectall)
	{
		SendMessage(ListWin, WM_IEVIEW_COMMAND, Command, Parameter);
		return LISTPLUGIN_OK;
	}
	return LISTPLUGIN_ERROR;
}

int _stdcall ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
	// issue #9 diagnostics: keep this path visible in logs
	{
		char buf[160];
		sprintf_s(buf, "ListWin=%p text='%s' param=0x%X",
			ListWin, SearchString ? SearchString : "(null)", (unsigned)SearchParameter);
		DebugLog("issue#9:ListSearchText", buf);
		DebugLogWindowBrief("issue#9:ListSearchText", "ListWin", ListWin);
		DebugLogWindowBrief("issue#9:ListSearchText", "parent", GetParent(ListWin));
		DebugLogWindowBrief("issue#9:ListSearchText", "focus", GetFocus());
	}
	SendMessage(ListWin, WM_IEVIEW_SEARCH, (WPARAM)SearchString, SearchParameter);
	return LISTPLUGIN_OK;
}

int _stdcall ListSearchTextW(HWND ListWin, WCHAR* SearchString, int SearchParameter)
{
	// issue #9 diagnostics: keep this path visible in logs
	{
		char buf[160];
		sprintf_s(buf, "ListWin=%p param=0x%X (Wide-mode)", ListWin, (unsigned)SearchParameter);
		DebugLog("issue#9:ListSearchTextW", buf);
		DebugLogWindowBrief("issue#9:ListSearchTextW", "ListWin", ListWin);
		DebugLogWindowBrief("issue#9:ListSearchTextW", "parent", GetParent(ListWin));
		DebugLogWindowBrief("issue#9:ListSearchTextW", "focus", GetFocus());
	}
	SendMessage(ListWin, WM_IEVIEW_SEARCHW, (WPARAM)SearchString, SearchParameter);
	return LISTPLUGIN_OK;
}

void __stdcall ListCloseWindow(HWND ListWin)
{
    DestroyWindow(ListWin);
	--num_lister_windows;

	// Освобождаем общий WebView2 Environment ДО OleUninitialize().
	// Иначе COM-объект может быть разрушен уже после деинициализации COM (например при выгрузке DLL),
	// что приводит к падению Total Commander при закрытии процесса.
	if (num_lister_windows == 0) {
		CBrowserHost::sSharedEnvironment.Release();

		// issue #2: GdiplusShutdown — здесь, не в DllMain. Microsoft запрещает вызывать
		// GdiplusShutdown под loader lock (gdiplus.h doc): GdiPlus ждёт завершения своих
		// worker-threads, которые под loader lock завершиться не могут → cm_UnloadPlugins
		// вешает Total Commander.
		ShutdownGdiPlusIfIdle();
	}

	OleUninitialize();

	// Удаляем временный HTML файл
	CleanupTempHtmlFile();

	if(!(options.flags&OPT_KEEPHOOKNOWINDOWS)&&hook_keyb&&num_lister_windows==0)
	{
		UnhookWindowsHookEx(hook_keyb);
		hook_keyb = NULL;
	}
	if(!(options.flags&OPT_KEEPHOOKNOWINDOWS)&&hook_getmsg&&num_lister_windows==0)
	{
		UnhookWindowsHookEx(hook_getmsg);
		hook_getmsg = NULL;
	}
	return;
}

int __stdcall ListPrint(HWND ListWin,char* FileToPrint,char* DefPrinter,int PrintFlags,RECT* Margins)
{
	SendMessage(ListWin, WM_IEVIEW_PRINT, (WPARAM)FileToPrint,0);
	return LISTPLUGIN_OK;
}

BOOL APIENTRY DllMain( HANDLE hModule, DWORD  reason_for_call, LPVOID lpReserved)
{
	if(reason_for_call==DLL_PROCESS_ATTACH)
	{
		hinst = (HINSTANCE)hModule;
		num_lister_windows = 0;
		WNDCLASS wc = {	0,//CS_HREDRAW | CS_VREDRAW,
						(WNDPROC)WndProc,0,0,hinst,NULL,
						LoadCursor(NULL, IDC_ARROW),
						NULL,NULL,MAIN_WINDOW_CLASS};
		RegisterClass(&wc);
	}
	else if(reason_for_call==DLL_PROCESS_DETACH)
	{
		// issue #2: GdiplusShutdown НЕ вызывается отсюда — он перенесён в
		// ListCloseWindow (последнее окно). Microsoft явно запрещает GdiplusShutdown
		// из DllMain (gdiplus.h doc): GdiPlus при shutdown ждёт завершения worker-threads,
		// которые под loader lock завершиться не могут → cm_UnloadPlugins вешает Total Commander.
		if(hook_keyb)
			UnhookWindowsHookEx(hook_keyb);
		if(hook_getmsg)
			UnhookWindowsHookEx(hook_getmsg);
		if(img_list)
			ImageList_Destroy(img_list);
		if (gTranslateIcon)
		{
			DestroyIcon(gTranslateIcon);
			gTranslateIcon = NULL;
		}
		if (gFastFontIcon)
		{
			DestroyIcon(gFastFontIcon);
			gFastFontIcon = NULL;
		}
	}
	return TRUE;
}
