#ifndef E_BOUNDS
#define E_BOUNDS                         _HRESULT_TYPEDEF_(0x8000000BL)
#endif

#include <ExDispID.h>
#include <mshtmdid.h>
#include <comutil.h>
#include <shlobj.h>
#include <strsafe.h>

#include <vector>
#include "browserhost.h"
#include "functions.h"

using namespace Microsoft::WRL;

// Статический shared environment для переиспользования
CComPtr<ICoreWebView2Environment> CBrowserHost::sSharedEnvironment;

static bool IsTempPreviewFilePath(const std::wstring& path)
{
	const wchar_t* filename = PathFindFileNameW(path.c_str());
	if (!filename || !filename[0])
		return false;
	return _wcsicmp(filename, L"_markdown_preview_temp.html") == 0;
}

static bool IsInternalUri(const std::wstring& uri)
{
	if (uri.empty())
		return false;

	if (_wcsnicmp(uri.c_str(), L"data:text/html", 14) == 0)
		return true;
	if (_wcsnicmp(uri.c_str(), L"about:blank", 10) == 0)
		return true;
	if (_wcsnicmp(uri.c_str(), L"file:", 5) == 0)
		return true;

	wchar_t host[256]{};
	DWORD cchHost = ARRAYSIZE(host);
	if (SUCCEEDED(UrlGetPartW(uri.c_str(), host, &cchHost, URL_PART_HOSTNAME, 0)) && host[0])
		return _wcsicmp(host, L"markdown.internal") == 0;

	return false;
}

static std::wstring ExtractJsonStringField(const wchar_t* json, const wchar_t* fieldName)
{
	if (!json || !fieldName || !fieldName[0])
		return L"";

	std::wstring key = L"\"";
	key += fieldName;
	key += L"\"";

	const wchar_t* pos = wcsstr(json, key.c_str());
	if (!pos)
		return L"";

	pos += key.length();
	while (*pos == L' ' || *pos == L'\t' || *pos == L'\r' || *pos == L'\n')
		++pos;
	if (*pos != L':')
		return L"";
	++pos;
	while (*pos == L' ' || *pos == L'\t' || *pos == L'\r' || *pos == L'\n')
		++pos;
	if (*pos != L'\"')
		return L"";
	++pos;

	std::wstring value;
	while (*pos)
	{
		if (*pos == L'\"')
			break;

		if (*pos == L'\\' && pos[1])
		{
			++pos;
			switch (*pos)
			{
			case L'\"': value += L'\"'; break;
			case L'\\': value += L'\\'; break;
			case L'/': value += L'/'; break;
			case L'b': value += L'\b'; break;
			case L'f': value += L'\f'; break;
			case L'n': value += L'\n'; break;
			case L'r': value += L'\r'; break;
			case L't': value += L'\t'; break;
			default: value += *pos; break;
			}
		}
		else
		{
			value += *pos;
		}

		++pos;
	}

	return value;
}

CBrowserHost::CBrowserHost() :
	mImagesHidden(false),
	mEventsCookie(0),
	mRefCount(1),
	fSearchHighlightMode(1),
	fStatusBarUnlockTime(0),
	mIsWebView2Initialized(false),
	mIsWebView2DocumentReady(false),
	mIsShuttingDown(false),
	mZoomFactor(1.0),
	mScrollTop(0),
	mFolderMappingEnabled(false),
	mAssetsMappingEnabled(false)
{
	mLastSearchString.Empty();
	mLastSearchFlags = 0;
}
CBrowserHost::~CBrowserHost()
{
}
void CBrowserHost::Quit()
{
	// Может быть вызвано в момент, когда WebView2 ещё создаётся асинхронно.
	// В таком случае колбэки должны завершиться без доступа к уже уничтоженному окну.
	mIsShuttingDown = true;

	if (mWebViewController)
	{
		mWebViewController->Close();
		mWebViewController.Release();
	}
	if (mWebView)
	{
		mWebView.Release();
	}

	if(mWebBrowser)
	{
		AtlUnadvise((IUnknown*)mWebBrowser, DIID_DWebBrowserEvents, mEventsCookie);

		mWebBrowser->Stop();
		mWebBrowser->Quit();

		CComQIPtr<IOleObject> ole_object(mWebBrowser);
		if(ole_object)
			ole_object->Close(0);

		if(!(options.flags&OPT_MOZILLA))
			mWebBrowser.Release();
	}

	mParentWin = NULL;
	Release();
}

CLSID CLSID_MozillaBrowser = {0x1339B54C,0x3453,0x11D2,{0x93,0xB9,0x00,0x00,0x00,0x00,0x00,0x00}};
bool CBrowserHost::CreateBrowser(HWND hParent)
{
	mParentWin = hParent;
	// Держим объект живым на время асинхронной инициализации WebView2.
	// В конце успешной/неуспешной инициализации обязательно делаем Release().
	AddRef();

    // Use AppData\Local for WebView2 user data to avoid permission issues in Program Files
    wchar_t appDataPath[MAX_PATH];
    std::wstring wUserDataPath;

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appDataPath))) {
        wUserDataPath = std::wstring(appDataPath) + L"\\TotalCommanderMarkdownViewPlugin\\wv2data";
        CreateDirectoryW((std::wstring(appDataPath) + L"\\TotalCommanderMarkdownViewPlugin").c_str(), NULL);
        CreateDirectoryW(wUserDataPath.c_str(), NULL);
    } else {
        // Fallback to temp folder
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        wUserDataPath = std::wstring(tempPath) + L"TotalCommanderMarkdownViewPlugin\\wv2data";
        CreateDirectoryW((std::wstring(tempPath) + L"TotalCommanderMarkdownViewPlugin").c_str(), NULL);
        CreateDirectoryW(wUserDataPath.c_str(), NULL);
    }

	// Общий обработчик создания Controller — используется и для первого запуска, и для повторного.
	auto onControllerCreated = [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
		// Если окно уже закрывается, не продолжаем инициализацию/навигацию.
		if (mIsShuttingDown || mParentWin == NULL || !IsWindow(mParentWin)) {
			if (controller) {
				controller->Close();
			}

			Release();
			return S_OK;
		}

		if (FAILED(result) || controller == nullptr) {
			Release();
			return FAILED(result) ? result : E_FAIL;
		}

		mWebViewController = controller;
		HRESULT hr_wv = mWebViewController->get_CoreWebView2(&mWebView);
		if (FAILED(hr_wv) || !mWebView) {
			Release();
			return hr_wv;
		}

		// Register events
		mWebView->add_DocumentTitleChanged(
			Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
				[this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
					UpdateTitle();
					return S_OK;
				}).Get(),
			nullptr);

		mWebView->AddScriptToExecuteOnDocumentCreated(
			L"window.addEventListener('scroll', () => { window.chrome.webview.postMessage({type: 'scroll', top: window.pageYOffset}); });"
			L"document.addEventListener('mouseover', function(e) {"
			L" var a=e.target&&e.target.closest?e.target.closest('a[href]'):null;"
			L" if(a) window.chrome.webview.postMessage({type:'linkhover',href:a.getAttribute('href')||''});"
			L"}, true);"
			L"document.addEventListener('mouseout', function(e) {"
			L" var a=e.target&&e.target.closest?e.target.closest('a[href]'):null;"
			L" if(!a) return;"
			L" var to=e.relatedTarget;"
			L" if(to&&a.contains(to)) return;"
			L" window.chrome.webview.postMessage({type:'linkout'});"
			L"}, true);",
			nullptr);

		mWebView->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
					auto UpdateToolbarButtonText = [this](int buttonId, const std::wstring& text) {
						if (buttonId == TBB_TRANSLATE)
						{
							HWND translateBtn = (HWND)GetProp(mParentWin, PROP_TRANSLATEBTN);
							if (translateBtn)
							{
								SetWindowTextW(translateBtn, text.c_str());
								RECT rc{};
								GetClientRect(mParentWin, &rc);
								PostMessage(mParentWin, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
								return;
							}
						}

						HWND toolbar = (HWND)GetProp(mParentWin, PROP_TOOLBAR);
						if (!toolbar)
							return;

						TBBUTTONINFOW tbbi = { 0 };
						tbbi.cbSize = sizeof(TBBUTTONINFOW);
						tbbi.dwMask = TBIF_TEXT;
						tbbi.pszText = (LPWSTR)text.c_str();
						SendMessageW(toolbar, TB_SETBUTTONINFOW, buttonId, (LPARAM)&tbbi);
						SendMessage(toolbar, TB_AUTOSIZE, 0, 0);
					};

					LPWSTR strMessage = nullptr;
					if (SUCCEEDED(args->TryGetWebMessageAsString(&strMessage)) && strMessage)
					{
						std::wstring s(strMessage);
						CoTaskMemFree(strMessage);

						return S_OK;
					}

					LPWSTR message = nullptr;
					args->get_WebMessageAsJson(&message);
					if (message && wcsstr(message, L"\"top\":")) {
						wchar_t* pos = wcsstr(message, L"\"top\":");
						if (pos) {
							long newScroll = _wtoi(pos + 6);
							mScrollTop = newScroll;
						}
					}
					else if (message) {
						std::wstring type = ExtractJsonStringField(message, L"type");
						if (type == L"gt_state") {
							std::wstring text = ExtractJsonStringField(message, L"text");
							UpdateToolbarButtonText(TBB_TRANSLATE, text);
						}
						else if (type == L"linkhover") {
							std::wstring href = ExtractJsonStringField(message, L"href");
							SetStatusText(href.c_str(), 0);
						}
						else if (type == L"linkout") {
							SetStatusText(L"", 0);
						}
					}
					CoTaskMemFree(message);
					return S_OK;
				}).Get(),
			nullptr);

		mWebView->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
					BOOL isSuccess = FALSE;
					args->get_IsSuccess(&isSuccess);
					COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
					args->get_WebErrorStatus(&status);
					mIsWebView2DocumentReady = isSuccess ? true : false;
					char navMsg[128]{};
					sprintf_s(navMsg, "success=%d status=%d documentReady=%d",
						isSuccess ? 1 : 0, (int)status, mIsWebView2DocumentReady ? 1 : 0);
					DebugLog("issue#9:NavigationCompleted", navMsg);
					if (options.flags & OPT_SAVEPOS)
						LoadPosition();
					ReapplyFastFontIfEnabled();
					return S_OK;
				}).Get(),
			nullptr);

		// WebResourceRequested/ResponseReceived removed: no per-request interception — faster load.
		// YouTube proxy removed (issue was geographic restriction, not domain).

		mWebView->add_NavigationStarting(
			Callback<ICoreWebView2NavigationStartingEventHandler>(
				[this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
					LPWSTR uri = nullptr;
					if (FAILED(args->get_Uri(&uri)) || !uri)
						return S_OK;
					std::wstring wUri = uri;

					bool isInternal = IsInternalUri(wUri);

					BOOL isUserInitiated = FALSE;
					args->get_IsUserInitiated(&isUserInitiated);

					if (!isInternal && isUserInitiated) {
						args->put_Cancel(TRUE);
						ShellExecuteW(NULL, L"open", wUri.c_str(), NULL, NULL, SW_SHOWNORMAL);
					}

					CoTaskMemFree(uri);
					return S_OK;
				}).Get(),
			nullptr);

		// Set settings
		CComPtr<ICoreWebView2Settings> settings;
		mWebView->get_Settings(&settings);
		if (settings) {
			settings->put_IsScriptEnabled(TRUE);
			settings->put_AreDefaultContextMenusEnabled(TRUE);
			settings->put_IsStatusBarEnabled(FALSE);

			CComQIPtr<ICoreWebView2Settings2> settings2 = settings;
			if (settings2) {
				settings2->put_UserAgent(
					L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
			}

			// issue #9: search belongs to WebView2. Let Chromium handle Ctrl+F/F3
			// while the plugin suppresses only its own Lister hotkey replay.
			CComQIPtr<ICoreWebView2Settings3> settings3 = settings;
			if (settings3) {
				HRESULT hrAccel = settings3->put_AreBrowserAcceleratorKeysEnabled(TRUE);
				char accelMsg[96]{};
				sprintf_s(accelMsg, "AreBrowserAcceleratorKeysEnabled(TRUE) hr=0x%08X", (unsigned)hrAccel);
				DebugLog("issue#9:CreateBrowser", accelMsg);
			}
		}

		UpdateFolderMapping(mCurrentFolder);

		// Second virtual host for plugin-shipped assets (vendor JS/CSS, fonts, etc.)
		{
			wchar_t pluginDir[MAX_PATH];
			if (GetModuleFileNameW(hinst, pluginDir, MAX_PATH) > 0) {
				PathRemoveFileSpecW(pluginDir);
				CComQIPtr<ICoreWebView2_3> webView3 = mWebView;
				if (webView3) {
					HRESULT hrAssets = webView3->SetVirtualHostNameToFolderMapping(
						L"assets.internal", pluginDir,
						COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
					mAssetsMappingEnabled = SUCCEEDED(hrAssets);
				}
			}
		}

		Resize();

		mIsWebView2Initialized = true;

		if (mPendingHTML.length() > 0) {
			mIsWebView2DocumentReady = false;
			DebugLog("issue#9:CreateBrowser", "NavigateToString pending HTML; documentReady=0");
			HRESULT hr_nav = mWebView->NavigateToString(mPendingHTML.c_str());
			mPendingHTML.clear();
		}
		else if (mPendingURL.Length() > 0) {
			std::wstring url = (wchar_t*)mPendingURL;

			if (url.length() > 2 && url[1] == L':') {
				if (IsTempPreviewFilePath(url)) {
					wchar_t fileUrl[2048];
					DWORD cch = ARRAYSIZE(fileUrl);
					HRESULT hrUrl = UrlCreateFromPathW(url.c_str(), fileUrl, &cch, 0);
					if (SUCCEEDED(hrUrl)) {
						url = fileUrl;
					}
				}
				else {
					CComQIPtr<ICoreWebView2_3> webView3 = mWebView;
					if (webView3) {
						wchar_t folder[MAX_PATH];
						if (SUCCEEDED(StringCchCopyW(folder, ARRAYSIZE(folder), url.c_str())))
						{
							PathRemoveFileSpecW(folder);
							UpdateFolderMapping(folder);
						}
						else
						{
							UpdateFolderMapping(L"");
						}

						if (mFolderMappingEnabled) {
							std::wstring filename = PathFindFileNameW(url.c_str());
							url = L"https://markdown.internal/" + filename;
						}
						else {
							wchar_t fileUrl[2048];
							DWORD cch = ARRAYSIZE(fileUrl);
							HRESULT hrUrl = UrlCreateFromPathW(url.c_str(), fileUrl, &cch, 0);
							if (SUCCEEDED(hrUrl)) {
								url = fileUrl;
							}
						}
					}
					else {
						wchar_t fileUrl[2048];
						DWORD cch = ARRAYSIZE(fileUrl);
						HRESULT hrUrl = UrlCreateFromPathW(url.c_str(), fileUrl, &cch, 0);
						if (SUCCEEDED(hrUrl)) {
							url = fileUrl;
						}
					}
				}
			}

			mIsWebView2DocumentReady = false;
			DebugLog("issue#9:CreateBrowser", "Navigate pending URL; documentReady=0");
			HRESULT hr_nav = mWebView->Navigate(url.c_str());
			mPendingURL.Empty();
		}

		// Завершаем асинхронную инициализацию (балансируем AddRef() в CreateBrowser()).
		Release();
		return S_OK;
	};

	// Используем кэшированный Environment если он уже создан
	if (sSharedEnvironment) {
		HRESULT hr = sSharedEnvironment->CreateCoreWebView2Controller(
			mParentWin,
			Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(onControllerCreated).Get());

		if (FAILED(hr)) {
			Release();
			return false;
		}

		return true;
	}

	// Первый запуск - создаем Environment и кэшируем его
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		nullptr, wUserDataPath.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this, onControllerCreated](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
				if (FAILED(result) || env == nullptr) {
					Release();
					return FAILED(result) ? result : E_FAIL;
				}

				sSharedEnvironment = env;

				HRESULT hr_controller = env->CreateCoreWebView2Controller(
					mParentWin,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(onControllerCreated).Get());

				if (FAILED(hr_controller)) {
					Release();
					return hr_controller;
				}

				return S_OK;
			}).Get());

	if (FAILED(hr)) {
		Release();
		return false;
	}

	return true;
}
void CBrowserHost::GetRect(LPRECT rect)
{
	RECT status_rc;
	::GetClientRect(mParentWin, rect);
	HWND status = (HWND)GetProp(mParentWin, PROP_STATUS);
	if(status)
	{
		GetWindowRect(status, &status_rc);
		rect->bottom -= (status_rc.bottom-status_rc.top);
	}
	HWND toolbar = (HWND)GetProp(mParentWin, PROP_TOOLBAR);
	if(toolbar)
	{
		GetWindowRect(toolbar, &status_rc);
		rect->top += (status_rc.bottom-status_rc.top);
	}
}
void CBrowserHost::Resize()
{
	RECT rect;
	GetRect(&rect);
	if (mWebViewController)
	{
		mWebViewController->put_Bounds(rect);
	}

	if (mWebBrowser)
	{
		CComQIPtr<IOleInPlaceObject> inplace_object(mWebBrowser);
		if (inplace_object)
			inplace_object->SetObjectRects(&rect, &rect);
	}
}

void CBrowserHost::UpdateFolderMapping(const std::wstring& folder)
{
	if (folder.empty())
	{
		mFolderMappingEnabled = false;
		return;
	}

	mCurrentFolder = folder;
	if (!mCurrentFolder.empty() && mCurrentFolder.back() == L'\\') {
		mCurrentFolder.pop_back();
	}

	if (!mWebView)
	{
		mFolderMappingEnabled = false;
		return;
	}

	CComQIPtr<ICoreWebView2_3> webView3 = mWebView;
	if (!webView3)
	{
		mFolderMappingEnabled = false;
		return;
	}

	webView3->ClearVirtualHostNameToFolderMapping(L"markdown.internal");
	HRESULT hr = webView3->SetVirtualHostNameToFolderMapping(
		L"markdown.internal", mCurrentFolder.c_str(),
		COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
	mFolderMappingEnabled = SUCCEEDED(hr);
}

static std::wstring GetPluginFolderW()
{
	wchar_t modulePath[MAX_PATH] = L"";
	GetModuleFileNameW(hinst, modulePath, MAX_PATH);
	wchar_t folder[MAX_PATH];
	wcscpy_s(folder, MAX_PATH, modulePath);
	PathRemoveFileSpecW(folder);
	return std::wstring(folder);
}

std::string CBrowserHost::BuildFontUrl(const char* filename)
{
	if (mAssetsMappingEnabled)
	{
		return std::string("https://assets.internal/css/fonts/") + filename;
	}

	std::wstring pluginFolder = GetPluginFolderW();
	if (pluginFolder.empty())
		return std::string();

	std::wstring full = pluginFolder + L"\\css\\fonts\\" + Utf8ToWide(filename);
	wchar_t fileUrl[2048] = L"";
	DWORD cch = ARRAYSIZE(fileUrl);
	HRESULT hr = UrlCreateFromPathW(full.c_str(), fileUrl, &cch, 0);
	if (SUCCEEDED(hr))
		return WideToUtf8(fileUrl);
	return std::string();
}

void CBrowserHost::ApplyFastFont()
{
	if (!mIsWebView2Initialized || !mWebView)
		return;

	std::string sansUrl = BuildFontUrl("Fast_Sans.ttf");
	if (sansUrl.empty())
		return;

	// Bionic применяется только к обычному тексту. Код (code/pre/kbd/samp/tt) намеренно
	// возвращается к font-family и font-feature-settings из темы — иначе токены кода
	// получают «жирные начальные буквы», что ломает восприятие листингов.
	std::string js =
		"(function(){"
		"var e=document.getElementById('mdv-fast-font');"
		"if(e)e.remove();"
		"var s=document.createElement('style');"
		"s.id='mdv-fast-font';"
		"s.textContent="
		"\"@font-face{font-family:'MDV Fast Sans';src:url('" + sansUrl + "') format('truetype');font-display:swap;}\"+"
		"\"body,body *{font-family:'MDV Fast Sans',sans-serif!important;font-feature-settings:\\\"calt\\\" 1,\\\"liga\\\" 1!important;}\"+"
		"\"code,kbd,samp,tt,pre,pre *,code *\"+"
		"\"{font-family:ui-monospace,SFMono-Regular,Consolas,monospace!important;font-feature-settings:normal!important;}\"+"
		"\"b,strong,b *,strong *,\"+"
		"\"h1,h2,h3,h4,h5,h6,h1 *,h2 *,h3 *,h4 *,h5 *,h6 *,\"+"
		"\"a,a *,\"+"
		"\"font,font *,[color],[color] *,\"+"
		"\"[style*='color:'],[style*='color:'] *,\"+"
		"\"mark,mark *,\"+"
		"\"svg,svg *,.mermaid,.mermaid *\"+"
		"\"{font-family:\\\"Segoe UI\\\",system-ui,-apple-system,sans-serif!important;font-feature-settings:normal!important;}\";"
		"document.head.appendChild(s);"
		"})();";

	std::wstring wjs = Utf8ToWide(js);
	mWebView->ExecuteScript(wjs.c_str(), nullptr);
}

void CBrowserHost::RemoveFastFont()
{
	if (!mIsWebView2Initialized || !mWebView)
		return;
	const wchar_t* js =
		L"(function(){var e=document.getElementById('mdv-fast-font');if(e)e.remove();})();";
	mWebView->ExecuteScript(js, nullptr);
}

void CBrowserHost::ReapplyFastFontIfEnabled()
{
	if (!mParentWin)
		return;
	if (GetProp(mParentWin, PROP_FASTFONT))
		ApplyFastFont();
}

void CBrowserHost::Navigate(const wchar_t* url)
{
	if (mIsWebView2Initialized && mWebView)
	{
        std::wstring finalUrl = url;
        // If it's a local path (starts with X:), check if we can use our mapping
        if (finalUrl.length() > 2 && finalUrl[1] == L':') {
			if (IsTempPreviewFilePath(finalUrl)) {
				wchar_t fileUrl[2048];
				DWORD cch = ARRAYSIZE(fileUrl);
				HRESULT hrUrl = UrlCreateFromPathW(finalUrl.c_str(), fileUrl, &cch, 0);
				if (SUCCEEDED(hrUrl)) {
					finalUrl = fileUrl;
				}
			}
			else {
				wchar_t folder[MAX_PATH];
				if (SUCCEEDED(StringCchCopyW(folder, ARRAYSIZE(folder), finalUrl.c_str())))
					PathRemoveFileSpecW(folder);
				else
					folder[0] = L'\0';

				// VirtualHostNameToFolderMapping доступен только начиная с ICoreWebView2_3.
				// Если интерфейса нет (старый runtime) — уходим на file:// навигацию.
				CComQIPtr<ICoreWebView2_3> webView3 = mWebView;
				if (webView3) {
					UpdateFolderMapping(folder[0] ? folder : L"");

					if (mFolderMappingEnabled) {
						std::wstring filename = PathFindFileNameW(finalUrl.c_str());
						finalUrl = L"https://markdown.internal/" + filename;
					}
					else {
						wchar_t fileUrl[2048];
						DWORD cch = ARRAYSIZE(fileUrl);
						HRESULT hrUrl = UrlCreateFromPathW(finalUrl.c_str(), fileUrl, &cch, 0);
						if (SUCCEEDED(hrUrl)) {
							finalUrl = fileUrl;
						}
					}
				}
				else {
					wchar_t fileUrl[2048];
					DWORD cch = ARRAYSIZE(fileUrl);
					HRESULT hrUrl = UrlCreateFromPathW(finalUrl.c_str(), fileUrl, &cch, 0);
					if (SUCCEEDED(hrUrl)) {
						finalUrl = fileUrl;
					}
				}
			}
        }

		mWebView->Navigate(finalUrl.c_str());
	}
	else if (mWebBrowser)
	{
		mWebBrowser->Navigate(CComBSTR(url), NULL, NULL, NULL, NULL);
	}
	else
	{
		mPendingURL = url;
		mPendingHTML.clear();
	}
}

void CBrowserHost::GoBack()
{
	if (mIsWebView2Initialized && mWebView)
		mWebView->GoBack();
	else if (mWebBrowser)
		mWebBrowser->GoBack();
}

void CBrowserHost::GoForward()
{
	if (mIsWebView2Initialized && mWebView)
		mWebView->GoForward();
	else if (mWebBrowser)
		mWebBrowser->GoForward();
}

void CBrowserHost::Stop()
{
	if (mIsWebView2Initialized && mWebView)
		mWebView->Stop();
	else if (mWebBrowser)
		mWebBrowser->Stop();
}

void CBrowserHost::Print()
{
	if (mIsWebView2Initialized && mWebView)
	{
		// WebView2 print is complex, but we can use window.print() via script
		mWebView->ExecuteScript(L"window.print();", nullptr);
	}
	else if (mWebBrowser)
	{
		CComQIPtr<IOleCommandTarget, &IID_IOleCommandTarget> pCmd = mWebBrowser;
		if (pCmd)
			pCmd->Exec(NULL, OLECMDID_PRINT, OLECMDEXECOPT_DODEFAULT, NULL, NULL);
	}
}

void CBrowserHost::ZoomIn()
{
	if (mIsWebView2Initialized && mWebViewController)
	{
		mZoomFactor *= 1.25;
		if (mZoomFactor > 5.0) mZoomFactor = 5.0;
		mWebViewController->put_ZoomFactor(mZoomFactor);
	}
	else if (mWebBrowser)
	{
		CComQIPtr<IOleCommandTarget, &IID_IOleCommandTarget> pCmd = mWebBrowser;
		if (pCmd)
		{
			VARIANTARG v = { 0 }, vi = { 0 };
			pCmd->Exec(NULL, OLECMDID_OPTICAL_ZOOM, 0, NULL, &v);
			vi.vt = VT_I4;
			vi.intVal = (int)(v.intVal * 1.25);
			pCmd->Exec(NULL, OLECMDID_OPTICAL_ZOOM, 0, &vi, NULL);
		}
	}
}

void CBrowserHost::ZoomOut()
{
	if (mIsWebView2Initialized && mWebViewController)
	{
		mZoomFactor /= 1.25;
		if (mZoomFactor < 0.25) mZoomFactor = 0.25;
		mWebViewController->put_ZoomFactor(mZoomFactor);
	}
	else if (mWebBrowser)
	{
		CComQIPtr<IOleCommandTarget, &IID_IOleCommandTarget> pCmd = mWebBrowser;
		if (pCmd)
		{
			VARIANTARG v = { 0 }, vi = { 0 };
			pCmd->Exec(NULL, OLECMDID_OPTICAL_ZOOM, 0, NULL, &v);
			vi.vt = VT_I4;
			vi.intVal = (int)(v.intVal / 1.25);
			pCmd->Exec(NULL, OLECMDID_OPTICAL_ZOOM, 0, &vi, NULL);
		}
	}
}

void CBrowserHost::ZoomReset()
{
	if (mIsWebView2Initialized && mWebViewController)
	{
		mZoomFactor = 1.0;
		mWebViewController->put_ZoomFactor(mZoomFactor);
	}
	else if (mWebBrowser)
	{
		CComQIPtr<IOleCommandTarget, &IID_IOleCommandTarget> pCmd = mWebBrowser;
		if (pCmd)
		{
			VARIANTARG vi = { 0 };
			vi.vt = VT_I4;
			vi.intVal = 100;
			pCmd->Exec(NULL, OLECMDID_OPTICAL_ZOOM, 0, &vi, NULL);
		}
	}
}

void CBrowserHost::ShowWebFind()
{
	DebugLog("issue#9:ShowWebFind", mIsWebView2Initialized ? "begin WebView2" : "fallback non-WebView2");

	if (mIsWebView2Initialized && mWebViewController)
	{
		Focus();

		INPUT inputs[4]{};
		inputs[0].type = INPUT_KEYBOARD;
		inputs[0].ki.wVk = VK_CONTROL;
		inputs[1].type = INPUT_KEYBOARD;
		inputs[1].ki.wVk = 'F';
		inputs[2].type = INPUT_KEYBOARD;
		inputs[2].ki.wVk = 'F';
		inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
		inputs[3].type = INPUT_KEYBOARD;
		inputs[3].ki.wVk = VK_CONTROL;
		inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
		UINT sent = SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
		char sendMsg[96]{};
		sprintf_s(sendMsg, "SendInput Ctrl+F sent=%u expected=%u lastError=%lu",
			sent, (unsigned)ARRAYSIZE(inputs), GetLastError());
		DebugLog("issue#9:ShowWebFind", sendMsg);
		return;
	}

	FindText(CComBSTR(L""), 0, false);
}

void CBrowserHost::Focus()
{
	if (mIsWebView2Initialized && mWebViewController)
	{
		mWebViewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
		return;
	}

	if (!mWebBrowser) return;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(html_disp)
	{
		CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
		if(html_doc2)
		{
			CComPtr<IHTMLWindow2> html_win;
			html_doc2->get_parentWindow(&html_win);
			if(html_win)
			{
				html_win->focus();
				return;
			}
		}
		CComQIPtr<IHTMLDocument4> html_doc(html_disp);
		if(html_doc)
		{
			html_doc->focus();
			return;
		}
	}
	CComQIPtr<IOleWindow> ole_win(mWebBrowser);
	if(ole_win)
	{
		HWND win;
		ole_win->GetWindow(&win);
		::SetFocus(win);
	}
}

void CBrowserHost::SavePosition()
{
	long position = 0;
	BSTR loc_wide = NULL;

	if (mIsWebView2Initialized && mWebView)
	{
		position = mScrollTop;
		mWebView->get_Source(&loc_wide);
	}
	else if (mWebBrowser)
	{
		CComPtr<IDispatch> html_disp;
		mWebBrowser->get_Document(&html_disp);
		if (html_disp)
		{
			CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
			if (html_doc2)
			{
				CComPtr<IHTMLElement> body_el;
				html_doc2->get_body(&body_el);
				if (body_el)
				{
					CComQIPtr<IHTMLElement2> body_el2(body_el);
					if (body_el2)
						body_el2->get_scrollTop(&position);
				}
			}
		}
		mWebBrowser->get_LocationURL(&loc_wide);
	}

	if (!loc_wide)
		return;

	char loc_char[320];
	WideCharToMultiByte(CP_ACP,NULL,loc_wide,-1,loc_char,320,NULL,FALSE);
	SysFreeString(loc_wide);

	char ini_path[512];
	StringCchCopyA(ini_path, ARRAYSIZE(ini_path), options.IniFileName);
	char* last_slash = strrchr(ini_path, '\\');
	if (last_slash)
	{
		size_t remaining = ARRAYSIZE(ini_path) - (size_t)(last_slash - ini_path);
		StringCchCopyA(last_slash, remaining, "\\positions.ini");
	}
	else
		return;

	char str_pos[16];
	ltoa(position,str_pos,10);
	WritePrivateProfileString("HTML", loc_char, str_pos, ini_path);
}

void CBrowserHost::LoadPosition()
{
	BSTR loc_wide = NULL;
	if (mIsWebView2Initialized && mWebView)
	{
		mWebView->get_Source(&loc_wide);
	}
	else if (mWebBrowser)
	{
		mWebBrowser->get_LocationURL(&loc_wide);
	}

	if (!loc_wide)
		return;

	char loc_char[320];
	WideCharToMultiByte(CP_ACP,NULL,loc_wide,-1,loc_char,320,NULL,FALSE);
	SysFreeString(loc_wide);

	char ini_path[512];
	StringCchCopyA(ini_path, ARRAYSIZE(ini_path), options.IniFileName);
	char* last_slash = strrchr(ini_path, '\\');
	if (last_slash)
	{
		size_t remaining = ARRAYSIZE(ini_path) - (size_t)(last_slash - ini_path);
		StringCchCopyA(last_slash, remaining, "\\positions.ini");
	}
	else
		return;

	long position = GetPrivateProfileInt("HTML", loc_char, 0, ini_path);

	if (mIsWebView2Initialized && mWebView)
	{
		wchar_t script[128];
		swprintf(script, 128, L"window.scrollTo(0, %ld);", position);
		mWebView->ExecuteScript(script, nullptr);
	}
	else if (mWebBrowser)
	{
		CComPtr<IDispatch> html_disp;
		mWebBrowser->get_Document(&html_disp);
		if(!html_disp)
			return;
		CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
		if(!html_doc2)
			return;
		CComPtr<IHTMLElement> body_el;
		html_doc2->get_body( &body_el );
		if(!body_el)
			return;
		CComQIPtr<IHTMLElement2> body_el2(body_el);
		if(!body_el2)
			return;
		body_el2->put_scrollTop( position );
	}
}

void CBrowserHost::SetStatusText(const wchar_t* str, DWORD delay)
{
	HWND status_wnd = (HWND)GetProp(mParentWin, PROP_STATUS);
	SetWindowTextW(status_wnd, str);
	fStatusBarUnlockTime = delay ? GetTickCount()+delay : 0;
}

void CBrowserHost::ExecuteScript(const wchar_t* script)
{
	if (mWebView)
	{
		mWebView->ExecuteScript(script, nullptr);
	}
}

bool CBrowserHost::IsSearchHighlightEnabled()
{
	if(fSearchHighlightMode==0)
		return false;
	else if(fSearchHighlightMode==2)
		return true;

	if (!mWebBrowser) return false;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return false;
	fSearchHighlightMode = options.highlight_all_matches;
	if(options.highlight_all_matches==1)
	{
		VARIANT doc_mod = {0};
		DISPPARAMS params = {0};
		HRESULT hres = html_disp->Invoke(1104, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &params, &doc_mod, NULL, NULL);
		if(hres==S_OK)
			fSearchHighlightMode = 2;
		else if(hres==DISP_E_MEMBERNOTFOUND)
			fSearchHighlightMode = 0;
		return hres==S_OK;
	}
	else if(options.highlight_all_matches==2)
		return true;
	else
		return false;
}

void CBrowserHost::UpdateSearchHighlight()
{
	ClearSearchHighlight();
	HighlightStrings(mLastSearchString, mLastSearchFlags);
}

void CBrowserHost::ClearSearchHighlight()
{
	if (!mWebBrowser)
		return;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	CComQIPtr<IHighlightRenderingServices> hl_services(html_disp);
	if(!hl_services)
		return;
	size_t segm_count = mSearchHighlightSegments.size();
	for(size_t i=0;i<segm_count;++i)
		if(mSearchHighlightSegments[i])
			hl_services->RemoveSegment(mSearchHighlightSegments[i]);
	mSearchHighlightSegments.clear();
	if(mCurrentSearchHighlightSegment)
	{
		hl_services->RemoveSegment(mCurrentSearchHighlightSegment);
		mCurrentSearchHighlightSegment.Release();
	}
}

CAtlStringW GetSearchStatusString(int number, bool finished)
{
	if(number<0)
		return CAtlStringW(finished ? L"" : L"Searching...");
	else if(number==0)
	{
		if(finished)
			return CAtlStringW(L"No occurrences have been found");
		else
			return CAtlStringW(L"Searching... (no occurrences have been found)");
	}
	else if(number==1)
	{
		if(finished)
			return CAtlStringW(L"1 occurrence has been found");
		else
			return CAtlStringW(L"Searching... (1 occurrence has been found)");
	}
	else
	{
		if(finished)
			return CComVariant(number)+CAtlStringW(L" occurrences have been found");
		else
			return CAtlStringW(L"Searching... (")+CComVariant(number)+CAtlStringW(L" occurrences have been found)");
	}
}

bool CBrowserHost::HighlightStrings(CComBSTR search, long search_flags)
{
	if (!mWebBrowser)
		return false;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return false;
	CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
	if(!html_doc2)
		return false;
	CComPtr<IHTMLElement> body_el;
    html_doc2->get_body(&body_el);
	if(!body_el)
		return false;
	CComQIPtr<IHTMLBodyElement> body_element(body_el);
	if(!body_element)
		return false;
	CComPtr<IHTMLTxtRange> txt_range;
	body_element->createTextRange(&txt_range);
	if(!txt_range)
		return false;

	CComQIPtr<IHTMLDocument4> html_doc4(html_disp);
	CComQIPtr<IMarkupServices> markup_services(html_disp);
	CComQIPtr<IDisplayServices> displ_services(html_disp);
	CComQIPtr<IHighlightRenderingServices> hl_services(html_disp);
	if(!html_doc4 || !markup_services || !displ_services || !hl_services)
		return false;

	CComPtr<IHTMLRenderStyle> render_style;
	html_doc4->createRenderStyle(NULL, &render_style);
	render_style->put_defaultTextSelection(CComBSTR(L"false"));
	render_style->put_textColor(CComVariant(CComBSTR(L"transparent")));
	render_style->put_textBackgroundColor(CComVariant(0xFFFF77));

	CComPtr<IMarkupPointer> markup_start;
	CComPtr<IMarkupPointer> markup_end;
	markup_services->CreateMarkupPointer(&markup_start);
	markup_services->CreateMarkupPointer(&markup_end);

	CComPtr<IDisplayPointer> displ_ptr_start;
	CComPtr<IDisplayPointer> displ_ptr_end;
	displ_services->CreateDisplayPointer(&displ_ptr_start);
	displ_services->CreateDisplayPointer(&displ_ptr_end);

	SetStatusText(GetSearchStatusString(-1, false));
	DWORD last_status_update_time = GetTickCount();
	while(true)
	{
		VARIANT_BOOL bFound = VARIANT_FALSE;
		txt_range->findText((BSTR)search, 0x10000000, search_flags, &bFound);
		if(!bFound)
			break;
		markup_services->MovePointersToRange(txt_range, markup_start, markup_end);
		displ_ptr_start->MoveToMarkupPointer(markup_start, NULL);
		displ_ptr_end->MoveToMarkupPointer(markup_end, NULL);
		CComPtr<IHighlightSegment> hl_segment;
		hl_services->AddSegment(displ_ptr_start, displ_ptr_end, render_style, &hl_segment);
		mSearchHighlightSegments.push_back(hl_segment);
		if(GetTickCount()>last_status_update_time+500)
		{
			SetStatusText(GetSearchStatusString((int)mSearchHighlightSegments.size(), false));
			last_status_update_time = GetTickCount();
		}
		long t;
		//txt_range->collapse(VARIANT_TRUE);
		//txt_range->move((BSTR)CComBSTR("character"), 1, &t);
		txt_range->moveStart((BSTR)CComBSTR("character"), 1, &t);
		if(!t)
			break;
	}
	SetStatusText(GetSearchStatusString((int)mSearchHighlightSegments.size(), true), 1000);
	return true;
}

void CBrowserHost::SetCurrentSearchHighlight(IHTMLTxtRange* txt_range)
{
	if (!mWebBrowser)
		return;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return;

	CComQIPtr<IHTMLDocument4> html_doc4(html_disp);
	CComQIPtr<IMarkupServices> markup_services(html_disp);
	CComQIPtr<IDisplayServices> displ_services(html_disp);
	CComQIPtr<IHighlightRenderingServices> hl_services(html_disp);
	if(!html_doc4 || !markup_services || !displ_services || !hl_services)
		return;
	CComPtr<IHTMLRenderStyle> render_style;
	html_doc4->createRenderStyle(NULL, &render_style);

	render_style->put_defaultTextSelection(CComBSTR(L"false"));
	render_style->put_textColor(CComVariant(CComBSTR(L"transparent")));
	render_style->put_textBackgroundColor(CComVariant(0xFF9966));
	render_style->put_renderingPriority(5);

	CComPtr<IMarkupPointer> markup_start;
	CComPtr<IMarkupPointer> markup_end;
	markup_services->CreateMarkupPointer(&markup_start);
	markup_services->CreateMarkupPointer(&markup_end);

	CComPtr<IDisplayPointer> displ_ptr_start;
	CComPtr<IDisplayPointer> displ_ptr_end;
	displ_services->CreateDisplayPointer(&displ_ptr_start);
	displ_services->CreateDisplayPointer(&displ_ptr_end);

	markup_services->MovePointersToRange(txt_range, markup_start, markup_end);

	displ_ptr_start->MoveToMarkupPointer(markup_start, NULL);
	displ_ptr_end->MoveToMarkupPointer(markup_end, NULL);

	if(mCurrentSearchHighlightSegment)
	{
		hl_services->RemoveSegment(mCurrentSearchHighlightSegment);
		mCurrentSearchHighlightSegment.Release();
	}
	hl_services->AddSegment(displ_ptr_start, displ_ptr_end, render_style, &mCurrentSearchHighlightSegment);
}

void CBrowserHost::Search(const wchar_t* text, bool forward, bool matchCase, bool wholeWord)
{
	if (!mIsWebView2Initialized || !mWebView || !text)
		return;

	std::wstring escaped;
	escaped.reserve(wcslen(text) + 8);
	for (const wchar_t* p = text; *p; ++p) {
		switch (*p) {
			case L'\\': escaped += L"\\\\"; break;
			case L'\'': escaped += L"\\'"; break;
			case L'\n': escaped += L"\\n"; break;
			case L'\r': escaped += L"\\r"; break;
			case L'\t': escaped += L"\\t"; break;
			default:    escaped += *p; break;
		}
	}

	std::wstring script;
	script.reserve(escaped.size() + 128);
	script += L"window.find('";
	script += escaped;
	script += L"',";
	script += matchCase ? L"true" : L"false";
	script += L",";
	script += forward ? L"false" : L"true";
	script += L",true,";
	script += wholeWord ? L"true" : L"false";
	script += L",false,false);";

	char searchMsg[160]{};
	sprintf_s(searchMsg, "window.find len=%zu forward=%d matchCase=%d wholeWord=%d",
		wcslen(text), forward ? 1 : 0, matchCase ? 1 : 0, wholeWord ? 1 : 0);
	DebugLog("issue#9:Search", searchMsg);
	mWebView->ExecuteScript(script.c_str(), nullptr);
}

bool CBrowserHost::FindText(CComBSTR search, long search_flags, bool backward)
{
	if (mIsWebView2Initialized && mWebView)
	{
		Search(search, !backward, (search_flags & 4) != 0, (search_flags & 2) != 0);
		return true;
	}

	bool is_new_search = mLastSearchString!=search || mLastSearchFlags!=search_flags;
	if(is_new_search)
		mSearchTxtRange.Release();
	if(mSearchTxtRange)
	{
		CComPtr<IHTMLElement> search_range_elem;
		HRESULT hr = mSearchTxtRange->parentElement(&search_range_elem);
		if(hr!=S_OK || !search_range_elem)
		{
			is_new_search = true;
			mSearchTxtRange.Release();
		}
	}
	mLastSearchString = search;
	mLastSearchFlags = search_flags;
	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return false;
	CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
	if(!html_doc2)
		return false;
	CComPtr<IHTMLElement> body_el;
    html_doc2->get_body( &body_el );
	if(!body_el)
		return false;
	CComQIPtr<IHTMLBodyElement> body_element(body_el);
	if(!body_element)
		return false;
	CComPtr<IHTMLTxtRange> txt_range;
	body_element->createTextRange(&txt_range);
	if(!txt_range)
		return false;

	bool is_highlight_enabled = IsSearchHighlightEnabled();

	long t;
	VARIANT_BOOL bTest = VARIANT_FALSE;
	HRESULT hr;
	if(mSearchTxtRange)
	{
		txt_range->moveStart(CComBSTR("textedit"), 0, &t);
		txt_range->moveEnd(CComBSTR("textedit"), 1, &t);
		hr = txt_range->setEndPoint(backward?CComBSTR("EndToEnd"):CComBSTR("StartToStart"), mSearchTxtRange);
		if(hr==S_OK)
		{
			if(backward)
				txt_range->moveEnd(CComBSTR("character"), -1, &t);
			else
				txt_range->moveStart(CComBSTR("character"), 1, &t);
		}
		else
		{
			is_new_search = true;
			mSearchTxtRange.Release();
		}
	}
	if(!mSearchTxtRange)
	{
		txt_range->moveStart(CComBSTR("textedit"), 0, &t);
		txt_range->moveEnd(CComBSTR("textedit"), 1, &t);
	}

	if(is_highlight_enabled)
		SetStatusText(GetSearchStatusString(-1, false));
	VARIANT_BOOL bFound = VARIANT_FALSE;
	if(backward)
	{
		CComPtr<IHTMLTxtRange> txt_range_curr;
		CComPtr<IHTMLTxtRange> txt_range_found;
		hr = txt_range->duplicate(&txt_range_curr);
		VARIANT_BOOL curr_found = VARIANT_FALSE;
		hr = txt_range_curr->findText(search, -0x10000000, search_flags, &curr_found);
		hr = txt_range_curr->setEndPoint(CComBSTR("EndToEnd"), txt_range);
		if(!curr_found)
		{
			CComPtr<IHTMLTxtRange> txt_range_body;
			body_element->createTextRange(&txt_range_body);
			hr = txt_range_curr->setEndPoint(CComBSTR("StartToStart"), txt_range_body);
		}
		while(true)
		{
			hr = txt_range_curr->findText(search, 0, search_flags, &curr_found);
			if(hr!=S_OK || !curr_found)
				break;
			txt_range_found.Release();
			txt_range_curr->duplicate(&txt_range_found);
			txt_range_curr->setEndPoint(CComBSTR("EndToEnd"), txt_range);
			txt_range_curr->moveStart(CComBSTR("character"), 1, &t);
		}
		if(txt_range_found)
		{
			bFound = VARIANT_TRUE;
			txt_range = txt_range_found;
		}
	}
	else
		hr = txt_range->findText(search, 0x10000000, search_flags, &bFound);
	if(is_highlight_enabled)
		SetStatusText(L"");
	if(bFound)
	{
		txt_range->scrollIntoView(TRUE);
		if(is_highlight_enabled)
		{
			CComPtr<IHTMLSelectionObject> selection;
			html_doc2->get_selection(&selection);
			if(selection)
				selection->empty();
			if(is_new_search || mSearchHighlightSegments.empty())
			{
				SetStatusText(GetSearchStatusString(-1, false));
				UpdateSearchHighlight();
				SetStatusText(GetSearchStatusString((int)mSearchHighlightSegments.size(), true), 1000);
			}
			SetCurrentSearchHighlight(txt_range);
		}
		else
			txt_range->select();
		mSearchTxtRange = txt_range;
		//mSearchTxtRange.Release();
		//txt_range->duplicate(&mSearchTxtRange);
		return true;
	}
	else if(mSearchTxtRange)
	{
		mSearchTxtRange.Release();
		return FindText(search, search_flags, backward);
	}
	else
	{
		if(is_highlight_enabled)
			ClearSearchHighlight();
		MessageBox(mParentWin, "Cannot find the string.", "HTMLView", MB_OK|MB_ICONEXCLAMATION);
		return true;
	}
}
void CBrowserHost::HideShowImagesInHTMLDocument(IHTMLDocument2* lpHtmlDocument, bool remove)
{
	HRESULT hr;

	CComPtr<IHTMLElementCollection> html_elem_coll;
	lpHtmlDocument->get_all(&html_elem_coll);
	if(!html_elem_coll)
		return;
	long elements_len;
	html_elem_coll->get_length(&elements_len);
	VARIANT var;
	var.vt=VT_I4;
	for(var.intVal=0;var.intVal<elements_len;++var.intVal)
	{
		CComPtr<IDispatch> html_elem_disp;
		html_elem_coll->item(var,var,&html_elem_disp);
		if(html_elem_disp)
		{
			CComQIPtr<IHTMLDOMNode> html_dom_node(html_elem_disp);
			if(!html_dom_node)
				continue;
			CComBSTR dom_name;
			html_dom_node->get_nodeName(&dom_name);
			if(dom_name)
			{
				if(dom_name==L"imagedata")
				{
					CComBSTR parent_dom_name;
					CComPtr<IHTMLDOMNode> html_parent_node;
					html_dom_node->get_parentNode(&html_parent_node);
					hr = html_parent_node->get_nodeName(&parent_dom_name);
					if(parent_dom_name==L"shape")
						html_dom_node = html_parent_node;
					else
						html_dom_node.Release();
				}
				else if(dom_name!=L"IMG")
					html_dom_node.Release();
				if(html_dom_node)
				{
					if(remove)
					{
						html_dom_node->removeNode(VARIANT_TRUE, NULL);
						--var.intVal;
					}
					else
					{
						CComQIPtr<IHTMLElement> html_elem(html_dom_node);
						CComPtr<IHTMLStyle> html_style;
						html_elem->get_style(&html_style);
						//html_style->get_visibility(&xx);
						if(mImagesHidden)
							html_style->put_visibility(CComBSTR(L"inherit"));
						else
							html_style->put_visibility(CComBSTR(L"hidden"));
						//html_style->get_visibility(&xx);
					}
				}
			}
		}
	}

	CComPtr<IHTMLFramesCollection2> frames_coll;
	lpHtmlDocument->get_frames(&frames_coll);
	if(!frames_coll)
		return;
	long frames_len;
	frames_coll->get_length(&frames_len);
	if(!frames_len)
		return;
	VARIANT res_var;
	for(var.intVal=0;var.intVal<frames_len;++var.intVal)
		if(frames_coll->item(&var, &res_var) == S_OK)
		{
			CComQIPtr<IHTMLWindow2> html_window(res_var.pdispVal);
			if(!html_window)
				continue;
			CComPtr<IHTMLDocument2> html_doc;
			html_window->get_document(&html_doc);
			if(html_doc)
				HideShowImagesInHTMLDocument(html_doc, remove);
		}
}
void CBrowserHost::HideShowImages(bool remove)
{
	if (!mWebBrowser)
		return;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return;
	CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
	if(!html_doc2)
		return;
	HideShowImagesInHTMLDocument(html_doc2, remove);
	mImagesHidden = !mImagesHidden;
}

void RefreshBrowser(); // defined in main()

void CBrowserHost::ProcessHotkey(UINT Msg, DWORD Key, DWORD Info)
{
	char str_action[80];
	CAtlString full_name = GetFullKeyName((WORD)Key);
	if((full_name=="F3" || full_name=="Shift+F3") && IsSearchHighlightEnabled())
	{
		if (mWebBrowser)
			mWebBrowser->ExecWB(OLECMDID_CLEARSELECTION, OLECMDEXECOPT_DONTPROMPTUSER, NULL, NULL);
	}
	if((Msg==WM_KEYDOWN||Msg==WM_SYSKEYDOWN)&&GetPrivateProfileString("Hotkeys", (const char*)full_name, "", str_action, sizeof(str_action), options.IniFileName))
	{
		if(!strcmpi(str_action,"Back"))
			GoBack();
		else if(!strcmpi(str_action,"Forward"))
			GoForward();
		else if(!strcmpi(str_action,"Stop"))
			Stop();
		else if(!strcmpi(str_action,"Refresh"))
			RefreshBrowser(); // instead of mWebBrowser->Refresh();
		else if(!strcmpi(str_action,"SavePosition"))
			SavePosition();
		else if(!strcmpi(str_action,"LoadPosition"))
			LoadPosition();
		else if(!strcmpi(str_action,"RemoveImages"))
			HideShowImages(true);
		else if(!strcmpi(str_action,"HideShowImages"))
			HideShowImages(false);
		else if(!strnicmp(str_action, "Cmd", 3))
		{
			if(!stricmp(str_action, "CmdFind"))
				ShowWebFind();
			else if(!stricmp(str_action, "CmdPrint"))
				Print();
			else if(!stricmp(str_action, "CmdPrintPreview"))
				Print(); // No preview in WebView2 easy way
			else if(!stricmp(str_action, "CmdZoomIn"))
				ZoomIn();
			else if(!stricmp(str_action, "CmdZoomOut"))
				ZoomOut();
			else if(!stricmp(str_action, "CmdZoomDef"))
				ZoomReset();
		}
	}
}
void CBrowserHost::UpdateTitle()
{
	char buf[1024];
	if(mFocusType!=fctQuickView && GetPrivateProfileString("options", "ListerTitle", "",buf , sizeof(buf), options.IniFileName))
	{
		CAtlStringW atlstr_text = buf;
		BSTR title_wide=NULL;

	if (mIsWebView2Initialized && mWebView)
	{
		mWebView->get_DocumentTitle(&title_wide);
	}
		else if (mWebBrowser)
		{
			CComPtr<IDispatch> html_disp;
			mWebBrowser->get_Document(&html_disp);
			if (html_disp)
			{
				CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
				if (html_doc2)
					html_doc2->get_title(&title_wide);
			}
			if (!title_wide)
				mWebBrowser->get_LocationName(&title_wide);
			else if (!wcslen(title_wide))
			{
				SysFreeString(title_wide);
				mWebBrowser->get_LocationName(&title_wide);
			}
		}

		if(title_wide)
		{
			atlstr_text.Replace(L"%TITLE",title_wide);
			SysFreeString(title_wide);
		}

		BSTR loc_wide=NULL;
	if (mIsWebView2Initialized && mWebView)
	{
		mWebView->get_Source(&loc_wide);
	}
		else if (mWebBrowser)
		{
			mWebBrowser->get_LocationURL(&loc_wide);
		}

		if(loc_wide)
		{
			wchar_t name[262],ext[262],dir[262],drive[5];
			_wsplitpath(loc_wide,drive,dir,name,ext);
			SysFreeString(loc_wide);
			atlstr_text.Replace(L"%DRIVE",drive);
			atlstr_text.Replace(L"%DIR",dir);
			atlstr_text.Replace(L"%NAME",name);
			atlstr_text.Replace(L"%EXT",ext);
		}

		SetWindowTextW(GetParent(mParentWin),atlstr_text);
	}
}
bool CBrowserHost::FormFocused()
{
	if (!mWebBrowser)
		return false;

	CComPtr<IDispatch> html_disp;
	mWebBrowser->get_Document(&html_disp);
	if(!html_disp)
		return false;
	CComQIPtr<IHTMLDocument2> html_doc2(html_disp);
	if(!html_doc2)
		return false;
	CComPtr<IHTMLElement> html_elem;
	html_doc2->get_activeElement(&html_elem);
	if(!html_elem)
		return false;
	//CComQIPtr<IHTMLDOMNode> html_dom_node(html_elem);
	//if(!html_dom_node)
	//	return false;
	BSTR dom_name;
	html_elem->get_tagName(&dom_name);
	if(!dom_name)
		return false;

	bool result = false;
	if(!wcsicmp(dom_name,L"Textarea"))
		result = true;
	else if(!wcsicmp(dom_name,L"Input"))
	{
		VARIANT input_type;
		html_elem->getAttribute(L"Type", 0, &input_type);
		if(input_type.bstrVal)
		{
			if(!wcsicmp(input_type.bstrVal, L"text"))
				result = true;
			SysFreeString(input_type.bstrVal);
		}
		//result = true;
	}
	SysFreeString(dom_name);
	return result;
}

//---------------------------=|  IUnknown  |=---------------------------
STDMETHODIMP CBrowserHost::QueryInterface(REFIID riid, void ** ppvObject)
{
    if(ppvObject == NULL)
		return E_INVALIDARG;
    else if(riid == IID_IUnknown)
        *ppvObject = (IUnknown*)(IDispatch*)this;
    else if(riid == IID_IDispatch)
		*ppvObject = (IDispatch*)this;
	else if(riid == IID_IOleControlSite)
		*ppvObject = (IOleControlSite*)this;
    else if(riid == IID_IOleClientSite)
		*ppvObject = (IOleClientSite*)this;
    else if(riid == IID_IOleInPlaceSite)
		*ppvObject = (IOleInPlaceSite*)this;
    //else if(riid == IID_IDocHostUIHandler)
	//	*ppvObject = (IDocHostUIHandler*)this;
    //else if(riid == IID_IDocHostUIHandler2)
	//	*ppvObject = (IDocHostUIHandler2*)this;
	else
	{
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}
    AddRef();
	return S_OK;
}

ULONG STDMETHODCALLTYPE CBrowserHost::AddRef()
{
    ULONG rc = InterlockedIncrement((LONG*)&mRefCount);
	return rc;
}

ULONG STDMETHODCALLTYPE CBrowserHost::Release()
{
    ULONG rc = InterlockedDecrement((LONG*)&mRefCount);
    if(rc == 0)
        delete this;
    return rc;
}

//---------------------------=|  IOleClientSite  |=---------------------------
STDMETHODIMP CBrowserHost::GetContainer(LPOLECONTAINER FAR* ppContainer) { return E_NOTIMPL; }
STDMETHODIMP CBrowserHost::SaveObject() { return E_NOTIMPL; }
STDMETHODIMP CBrowserHost::GetMoniker(DWORD dwAssign, DWORD dwWhichMoniker, IMoniker ** ppmk) { return E_NOTIMPL; }
STDMETHODIMP CBrowserHost::ShowObject() { return S_OK; }
STDMETHODIMP CBrowserHost::OnShowWindow(BOOL fShow) { return E_NOTIMPL; }
STDMETHODIMP CBrowserHost::RequestNewObjectLayout() { return E_NOTIMPL; }
//---------------------------=|  IOleWindow  |=---------------------------
HRESULT STDMETHODCALLTYPE CBrowserHost::GetWindow(HWND * phwnd)
{
	*phwnd = mParentWin;
	return S_OK;
}
HRESULT STDMETHODCALLTYPE CBrowserHost::ContextSensitiveHelp(BOOL fEnterMode) { return E_NOTIMPL; }
//---------------------------=|  IOleInPlaceSite  |=---------------------------
HRESULT STDMETHODCALLTYPE CBrowserHost::CanInPlaceActivate(void) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnInPlaceActivate(void) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnUIActivate(void) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::GetWindowContext(IOleInPlaceFrame **ppFrame,
                                           IOleInPlaceUIWindow **ppDoc, LPRECT lprcPosRect,
                                           LPRECT lprcClipRect,
                                           LPOLEINPLACEFRAMEINFO lpFrameInfo)
{
	GetRect(lprcPosRect);
	GetRect(lprcClipRect);
	return S_OK;
}
HRESULT STDMETHODCALLTYPE CBrowserHost::Scroll(SIZE scrollExtant) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnUIDeactivate(BOOL fUndoable) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnInPlaceDeactivate(void) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::DiscardUndoState(void) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::DeactivateAndUndo(void) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnPosRectChange(LPCRECT lprcPosRect) { return E_NOTIMPL; }
//---------------------------=|  IOleControlSite  |=---------------------------
HRESULT STDMETHODCALLTYPE CBrowserHost::OnControlInfoChanged(void) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::LockInPlaceActive(BOOL fLock) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::GetExtendedControl(IDispatch **ppDisp) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::TransformCoords(POINTL *pPtlHimetric, POINTF *pPtfContainer, DWORD dwFlags) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::TranslateAccelerator(MSG *pMsg, DWORD grfModifiers) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::OnFocus(BOOL fGotFocus) { return S_OK; }
HRESULT STDMETHODCALLTYPE CBrowserHost::ShowPropertyFrame(void) { return S_OK; }
//---------------------------=|  IDispatch  |=---------------------------
HRESULT STDMETHODCALLTYPE CBrowserHost::GetTypeInfoCount(unsigned int FAR* pctinfo) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::GetTypeInfo(unsigned int iTInfo, LCID  lcid,
                                      ITypeInfo FAR* FAR*  ppTInfo) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CBrowserHost::GetIDsOfNames(REFIID riid, OLECHAR FAR* FAR* rgszNames,
                                        unsigned int cNames, LCID lcid, DISPID FAR* rgDispId) { return E_NOTIMPL; }


struct SNewWindowThread
{
	CComBSTR path;
	bool def_browser;
};

DWORD WINAPI NewWindowThreadFunc(LPVOID param)
{
	SNewWindowThread* thread_struct = (SNewWindowThread*)param;
	if(thread_struct->def_browser)
		ShellExecuteW(NULL, L"open", thread_struct->path, NULL, NULL, SW_SHOW);
	else
	{
	    PROCESS_INFORMATION pi;
		STARTUPINFOW si;
		ZeroMemory(&si, sizeof(si));
		ZeroMemory(&pi, sizeof(pi));
		si.cb = sizeof(si);
		si.dwFlags = 0;
		CreateProcessW(NULL, thread_struct->path, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	}
	delete thread_struct;
	return 0;
}

#include <strsafe.h>

void CBrowserHost::LoadWebBrowserFromStreamWrapper(const BYTE* html, int length)
{
	DebugLog("browserhost.cpp:LoadWebBrowserFromStreamWrapper", (mIsWebView2Initialized && mWebView) ? "WebView2" : (mWebBrowser ? "WebBrowser" : "Pending"));
	std::string s((const char*)html, length);
	std::wstring ws = Utf8ToWide(s);

	if (mIsWebView2Initialized && mWebView)
	{
		mIsWebView2DocumentReady = false;
		DebugLog("issue#9:LoadWebBrowserFromStreamWrapper", "NavigateToString; documentReady=0");
		mWebView->NavigateToString(ws.c_str());
	}
	else if (mWebBrowser)
	{
		IDispatch* pHtmlDoc = NULL;
		mWebBrowser->get_Document(&pHtmlDoc);

		if (pHtmlDoc) {
			IHTMLDocument2* pDoc2 = NULL;
			HRESULT hrDoc2 = pHtmlDoc->QueryInterface(IID_IHTMLDocument2, (void**)&pDoc2);
			if (SUCCEEDED(hrDoc2) && pDoc2)
			{
				SAFEARRAY* psa = SafeArrayCreateVector(VT_VARIANT, 0, 1);
				if (psa)
				{
					VARIANT* v = NULL;
					if (SUCCEEDED(SafeArrayAccessData(psa, (void**)&v)) && v)
					{
						VariantInit(v);
						v->vt = VT_BSTR;
						v->bstrVal = SysAllocString(ws.c_str());
						bool ok = (v->bstrVal != NULL);
						SafeArrayUnaccessData(psa);

						if (ok)
						{
							pDoc2->write(psa);
							pDoc2->close();
						}
					}
					SafeArrayDestroy(psa);
				}

				pDoc2->Release();
			}
			else
			{
				const BYTE utf8Bom[3] = { 0xEF, 0xBB, 0xBF };
				std::vector<BYTE> buf;
				buf.reserve((size_t)length + 3);
				buf.insert(buf.end(), utf8Bom, utf8Bom + 3);
				buf.insert(buf.end(), html, html + length);

				IStream* pStream = SHCreateMemStream(buf.data(), (UINT)buf.size());
				if (pStream)
				{
					IPersistStreamInit* pPersistStreamInit = NULL;
					HRESULT hrQI = pHtmlDoc->QueryInterface(IID_IPersistStreamInit, (void**)&pPersistStreamInit);
					if (SUCCEEDED(hrQI) && pPersistStreamInit) {
						pPersistStreamInit->InitNew();
						pPersistStreamInit->Load(pStream);
						pPersistStreamInit->Release();
					}
					pStream->Release();
				}
			}
			pHtmlDoc->Release();
		}
	}
	else
	{
		mIsWebView2DocumentReady = false;
		DebugLog("issue#9:LoadWebBrowserFromStreamWrapper", "pending HTML; documentReady=0");
		mPendingHTML = ws;
		mPendingURL.Empty();
	}
}

HRESULT STDMETHODCALLTYPE CBrowserHost::Invoke(DISPID dispIdMember, REFIID riid, LCID lcid,
                                                    WORD wFlags, DISPPARAMS FAR* pDispParams,
                                                    VARIANT FAR* pVarResult,
                                                    EXCEPINFO FAR* pExcepInfo,
                                                    unsigned int FAR* puArgErr)
{
	switch (dispIdMember)
	{
		case DISPID_BEFORENAVIGATE:
		case DISPID_DOWNLOADBEGIN:
			if(options.highlight_all_matches)
				ClearSearchHighlight();
			mSearchTxtRange.Release();
			break;
		case DISPID_DOCUMENTCOMPLETE:
		case DISPID_NAVIGATECOMPLETE:
		case DISPID_NAVIGATECOMPLETE2:
			//mImagesHidden = false;
		case 0U-726:
			//ShowWindow(mParentWin,SW_SHOW);
			if ( mFocusType == fctLister )
				Focus();
			//else if(mFocusType == fctQuickView/* && !::GetFocus()*/)
			//	::SetFocus(mFocusWin);
			break;

		case DISPID_NEWWINDOW:
		case DISPID_NEWWINDOW2:
			if (!mWebBrowser) return S_OK;
			READYSTATE m_ReadyState;
			mWebBrowser->get_ReadyState(&m_ReadyState);
			if(m_ReadyState!=READYSTATE_COMPLETE && m_ReadyState!=READYSTATE_INTERACTIVE && !(options.flags&OPT_POPUPS))
				*pDispParams->rgvarg[0].pboolVal=VARIANT_TRUE;
            else if(dispIdMember==DISPID_NEWWINDOW)
			{
				char temp[MAX_PATH+3];
				if(!wcsnicmp(pDispParams->rgvarg[5].bstrVal, L"mk:@msitstore:", 14))
					GetPrivateProfileString("options", "NewWindowCHM", "", (char*)temp, sizeof(temp), options.IniFileName);
				else
					GetPrivateProfileString("options", "NewWindowHTML", "", (char*)temp, sizeof(temp), options.IniFileName);
				if(temp[0]=='\0')
					return S_OK;

				*pDispParams->rgvarg[0].pboolVal=VARIANT_TRUE;
				SNewWindowThread* thread_struct = new SNewWindowThread;
				if(!stricmp(temp, "default"))
				{
					thread_struct->path = pDispParams->rgvarg[5].bstrVal;
					thread_struct->def_browser = true;
				}
				else
				{
					thread_struct->path = temp;
					thread_struct->path += L" \"";
					thread_struct->path += pDispParams->rgvarg[5].bstrVal;
					thread_struct->path += L"\"";
					thread_struct->def_browser = false;
				}
				CreateThread(NULL, 0, NewWindowThreadFunc, (void*)(thread_struct), 0, NULL);
			}
			break;
		case DISPID_STATUSTEXTCHANGE:
			if(fStatusBarUnlockTime==0 || GetTickCount()>fStatusBarUnlockTime)
			{
				SetWindowTextW((HWND)GetProp(mParentWin, PROP_STATUS), pDispParams->rgvarg[0].bstrVal);
				fStatusBarUnlockTime = 0;
			}
			break;
		case DISPID_TITLECHANGE:
			if(options.flags&OPT_TITLE)
				UpdateTitle();
			break;
		case DISPID_COMMANDSTATECHANGE:
			if( options.toolbar && (pDispParams->rgvarg[1].intVal==1 || pDispParams->rgvarg[1].intVal==2))
			{
				HWND toolbar = (HWND)GetProp(mParentWin, PROP_TOOLBAR);
				if(toolbar)
					PostMessage(toolbar, TB_SETSTATE, (pDispParams->rgvarg[1].intVal==1)?TBB_FORWARD:TBB_BACK, (pDispParams->rgvarg[0].boolVal)?TBSTATE_ENABLED:TBSTATE_INDETERMINATE);
			}
			break;
		case DISPID_AMBIENT_DLCONTROL:
			pVarResult->vt = VT_I4;
			pVarResult->lVal =  options.dlcontrol;
			break;
		default:
			return DISP_E_MEMBERNOTFOUND;
	}
	return S_OK;
}
