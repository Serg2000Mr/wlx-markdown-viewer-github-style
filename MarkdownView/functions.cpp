#ifndef E_BOUNDS
#define E_BOUNDS                         _HRESULT_TYPEDEF_(0x8000000BL)
#endif

#include <mshtmdid.h>
#include "functions.h"
#include <cstdio>
#include <mutex>
#include <strsafe.h>

HINSTANCE hinst = NULL;

SOptions options = {false, 0, 0, 0, 0, 0, "", ""};
static bool gDebugLogEnabled = false;
static bool gIssue9DiagnosticLogAlways = true;
static std::mutex gDebugLogMutex;
static bool gFallbackLogPathInitialized = false;
static char gFallbackLogPath[MAX_PATH] = {};

//					   #--------------------#
//				 	   |		            |
//*********************|  CSmallStringList  |**************************
//					   |		            |
//					   #--------------------#

CSmallStringList::CSmallStringList():data(NULL)
{
}

void CSmallStringList::clear()
{
	if(data)
		delete[] data;
	data = NULL;
}
void CSmallStringList::set_size(size_t size)
{
	clear();
	data = new unsigned char[size];
}
void CSmallStringList::set_data(const unsigned char* buffer, size_t size)
{
	set_size(size);
	memcpy(data, buffer, size);
}
bool CSmallStringList::valid()
{
	return data;
}
void CSmallStringList::load_from_ini(const char* filename, const char* section, const char* key)
{
	char buffer[512];
	GetPrivateProfileString(section, key, "", buffer+1, sizeof(buffer)-1, filename);
	{
		char* s = buffer + 1;
		size_t cap = sizeof(buffer) - 1;
		size_t len = strnlen_s(s, cap);
		if (len + 1 < cap)
		{
			s[len] = ';';
			s[len + 1] = '\0';
		}
		else if (cap >= 2)
		{
			s[cap - 2] = ';';
			s[cap - 1] = '\0';
		}
		else if (cap >= 1)
		{
			s[cap - 1] = '\0';
		}
	}
	char* next_pos;
	char* str_pos = buffer;
	while(true)
	{
		next_pos=strchr(str_pos+1, ';');
		if(next_pos==NULL || next_pos==str_pos)
			break;
		ptrdiff_t seg_len = next_pos - str_pos - 1;
		if (seg_len < 0)
			seg_len = 0;
		if (seg_len > 255)
			seg_len = 255;
		*str_pos = (char)(unsigned char)seg_len;
		str_pos = next_pos;
	}
	*str_pos = 0;
	set_data((unsigned char*)buffer, (size_t)(str_pos - buffer + 1));
}
void CSmallStringList::load_sign_from_ini(const char* filename, const char* section, const char* key)
{
	char buffer[512];
	unsigned char list[512];
	GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), filename);
	{
		char* s = buffer;
		size_t cap = sizeof(buffer);
		size_t len = strnlen_s(s, cap);
		if (len + 1 < cap)
		{
			s[len] = ';';
			s[len + 1] = '\0';
		}
		else if (cap >= 2)
		{
			s[cap - 2] = ';';
			s[cap - 1] = '\0';
		}
		else if (cap >= 1)
		{
			s[cap - 1] = '\0';
		}
	}
	char* next_pos;
	char* str_pos = buffer;
	unsigned char* list_pos = list;
	while(true)
	{
		next_pos = strchr(str_pos, ';');
		if(next_pos==NULL || next_pos==str_pos)
			break;
		if(*str_pos=='\"'&&*(next_pos-1)=='\"')
		{
			ptrdiff_t raw_len = next_pos - str_pos - 2;
			if (raw_len < 0)
				raw_len = 0;
			if (raw_len > 255)
				raw_len = 255;
			*list_pos = (unsigned char)raw_len;
			memcpy(list_pos+1, str_pos+1, *list_pos);
			list_pos += *list_pos+1;
		}
		else
		{
			ptrdiff_t raw_count = (next_pos - str_pos) / 2;
			if (raw_count < 0)
				raw_count = 0;
			if (raw_count > 255)
				raw_count = 255;
			unsigned char byte_count = (unsigned char)raw_count;
			*list_pos = byte_count;
			const char* s = str_pos;
			for (unsigned int i = 0; i < byte_count; i++)
			{
				unsigned int value = 0;
				if (sscanf(s, "%2x", &value) != 1)
					value = 0;
				list_pos[1 + i] = (unsigned char)value;
				s += 2;
			}
			list_pos += byte_count + 1;
		}
		str_pos = next_pos+1;
	}
	*list_pos = 0;
	set_data(list, (size_t)(list_pos - list + 1));
}
bool CSmallStringList::find(const char* str)
{
	if(!valid())
		return false;
	size_t len = strlen(str);
	if (len > 255)
		return false;
	for(const unsigned char* str_pos = data; *str_pos; str_pos+=*str_pos+1)
		if(*str_pos == (unsigned char)len && !memcmp(str, str_pos+1, len))
			return true;
	return false;
}
bool CSmallStringList::check_signature(const char* filename, bool skip_spaces)
{
	if(!valid())
		return false;
	FILE* file = fopen(filename, "rb");
	if(file==NULL)
		return false;
	unsigned char* buf = new unsigned char[256];
	size_t num_read;
	for(const unsigned char* str_pos = data; *str_pos; str_pos+=*str_pos+1)
	{
		fseek(file, 0, SEEK_SET);
		if(skip_spaces)
		{
			char c_skip;
			for(int i=0;i<64;++i)
			{
				fread(&c_skip, sizeof(char), 1, file);
				if(c_skip!=' '&&c_skip!='\r'&&c_skip!='\n')
				{
					fseek(file, -1, SEEK_CUR);
					break;
				}
			}
		}
		num_read = fread(buf, sizeof(unsigned char), *str_pos, file);
		if(num_read == (size_t)*str_pos && !memicmp(buf, str_pos+1, *str_pos))
		{
			fclose(file);
			return true;
		}
	}
	fclose(file);
	return false;
}

//							#----------#
//							|		   |
//**************************|   Init   |**************************
//							|		   |
//							#----------#

void InitOptions()
{
	char modulePath[MAX_PATH]{};
	GetModuleFileName(hinst, modulePath, ARRAYSIZE(modulePath));
	StringCchCopyA(options.IniFileName, ARRAYSIZE(options.IniFileName), modulePath);
	char* dot = strrchr(options.IniFileName, '.');
	if (dot)
	{
		size_t remaining = ARRAYSIZE(options.IniFileName) - (size_t)(dot - options.IniFileName);
		StringCchCopyA(dot, remaining, ".ini");
	}
	else
	{
		StringCchCatA(options.IniFileName, ARRAYSIZE(options.IniFileName), ".ini");
	}

	BOOL ini_exists = PathFileExists(options.IniFileName);
	if(!ini_exists)
	{
		CAtlString message = "File not found: ";
		message += options.IniFileName;
		MessageBox(NULL, message, "HTMLView", MB_OK|MB_ICONWARNING);
	}


	StringCchCopyA(options.LogIniFileName, ARRAYSIZE(options.LogIniFileName), options.IniFileName);
	{
		char* logDot = strrchr(options.LogIniFileName, '.');
		if (logDot)
		{
			size_t remaining = ARRAYSIZE(options.LogIniFileName) - (size_t)(logDot - options.LogIniFileName);
			StringCchCopyA(logDot, remaining, "_Log.ini");
		}
		else
		{
			StringCchCatA(options.LogIniFileName, ARRAYSIZE(options.LogIniFileName), "_Log.ini");
		}
	}

	options.flags = 0;
	char temp[10];
	GetPrivateProfileString("options", "ListerTitle", "", (char*)temp, sizeof(temp), options.IniFileName);
	if(temp[0])
		options.flags |= OPT_TITLE;
	//if(GetPrivateProfileInt("options", "ShowToolbar", 0, options.IniFileName))
	//	options.flags |= OPT_TOOLBAR;
	//if(GetPrivateProfileInt("options", "UseDefaultBrowser", 0, options.IniFileName))
	//	options.flags |= OPT_DEFBROWSER;
	//if(GetPrivateProfileInt("options", "StatusBarInQuickView", 0, options.IniFileName))
	//	options.flags |= OPT_STATUS_QV;
	if(GetPrivateProfileInt("options", "UseSavePosition", 0, options.IniFileName))
		options.flags |= OPT_SAVEPOS;
	if(GetPrivateProfileInt("options", "AllowPopups", 0, options.IniFileName))
		options.flags |= OPT_POPUPS;
	if(GetPrivateProfileInt("options", "ShowDirs", 0, options.IniFileName))
		options.flags |= OPT_DIRS;
	if(GetPrivateProfileInt("Debug", "UseMozillaControl", 0, options.IniFileName))
		options.flags |= OPT_MOZILLA;
	if(GetPrivateProfileInt("Debug", "QiuckQuit", 0, options.IniFileName))
		options.flags |= OPT_QUICKQIUT;
	if(GetPrivateProfileInt("Debug", "GlobalHook", 0, options.IniFileName))
		options.flags |= OPT_GLOBALHOOK;
	if(GetPrivateProfileInt("Extensions", "SignatureSkipSpaces", 0, options.IniFileName))
		options.flags |= OPT_SIGNSKIPSPACES;
	if(GetPrivateProfileInt("Debug", "KeepHookWhenNoWindows", 0, options.IniFileName))
		options.flags |= OPT_KEEPHOOKNOWINDOWS;

	options.toolbar = 3&GetPrivateProfileInt("options", "ShowToolbar", 0, options.IniFileName);
	options.status = 3&GetPrivateProfileInt("options", "ShowStatusbar", 0, options.IniFileName);

	options.toolbar |= (3&GetPrivateProfileInt("Debug", "ToolbarBPP", 2, options.IniFileName))<<2;

	options.highlight_all_matches = GetPrivateProfileInt("options", "HighlightAllMatches", 0, options.IniFileName);

	options.dlcontrol = 0;
	if(!GetPrivateProfileInt("options", "AllowScripting", 0, options.IniFileName))
			options.dlcontrol |= DLCTL_NO_SCRIPTS;
	if(GetPrivateProfileInt("options", "ShowImages", 1, options.IniFileName))
			options.dlcontrol |= DLCTL_DLIMAGES;
	if(GetPrivateProfileInt("options", "ShowVideos", 1, options.IniFileName))
			options.dlcontrol |= DLCTL_VIDEOS;
	if(GetPrivateProfileInt("options", "PlaySounds", 1, options.IniFileName))
			options.dlcontrol |= DLCTL_BGSOUNDS;
	if(!GetPrivateProfileInt("options", "AllowJava", 0, options.IniFileName))
			options.dlcontrol |= DLCTL_NO_JAVA;
	if(!GetPrivateProfileInt("options", "AllowActiveX", 0, options.IniFileName))
			options.dlcontrol |= DLCTL_NO_DLACTIVEXCTLS | DLCTL_NO_RUNACTIVEXCTLS;
	if(GetPrivateProfileInt("options", "ForceOffline", 1, options.IniFileName))
			options.dlcontrol |= DLCTL_OFFLINE | DLCTL_FORCEOFFLINE | DLCTL_OFFLINEIFNOTCONNECTED;
	if(GetPrivateProfileInt("options", "Silent", 1, options.IniFileName))
			options.dlcontrol |= DLCTL_SILENT;
	options.valid = true;

	gDebugLogEnabled = GetPrivateProfileInt("Debug", "Log", 0, options.IniFileName) ? true : false;

	char debugInit[1024]{};
	StringCchPrintfA(
		debugInit, ARRAYSIZE(debugInit),
		"module=\"%s\" ini=\"%s\" logIni=\"%s\" Debug.Log=%d",
		modulePath,
		options.IniFileName,
		options.LogIniFileName,
		gDebugLogEnabled ? 1 : 0
	);
	DebugLog("functions.cpp:InitOptions", debugInit);
}

//						  #-------------#
//						  |		        |
//************************|  Functions  |**************************
//						  |		        |
//						  #-------------#

CAtlString GetKeyName(WORD key)
{
	key &= 0xFF;
	switch (key)
	{
		case VK_CANCEL:		return "Scroll Lock";
		case VK_MBUTTON:	return "";
		case VK_BACK:		return "Backspace";
		case VK_TAB:		return "Tab";
		case VK_CLEAR:		return "Clear";
		case VK_RETURN:		return "Enter";
		case VK_SHIFT:		return "Shift";
		case VK_CONTROL:	return "Ctrl";
		case VK_MENU:		return "Alt";
		case VK_PAUSE:		return "Pause";
		case VK_CAPITAL:	return "Caps Lock";
		case VK_KANA:		return "";
		case VK_JUNJA:		return "";
		case VK_FINAL:		return "";
		case VK_HANJA:		return "";
		case VK_ESCAPE:		return "Esc";
		case VK_CONVERT:	return "";
		case VK_NONCONVERT:	return "";
		case VK_ACCEPT:		return "";
		case VK_MODECHANGE:	return "";
		case VK_SPACE:		return "Space";
		case VK_PRIOR:		return "Page Up";
		case VK_NEXT:		return "Page Down";
		case VK_END:		return "End";
		case VK_HOME:		return "Home";
		case VK_LEFT:		return "Left";
		case VK_UP:			return "Up";
		case VK_RIGHT:		return "Right";
		case VK_DOWN:		return "Down";
		case VK_SELECT:		return "Select";
		case VK_PRINT:		return "Print";
		case VK_EXECUTE:	return "Execute";
		case VK_SNAPSHOT:	return "Print Screen";
		case VK_INSERT:		return "Insert";
		case VK_DELETE:		return "Delete";
		case VK_HELP:		return "Help";
		case VK_LWIN:		return "Windows";
		case VK_RWIN:		return "Right Windows";
		case VK_APPS:		return "Applications";
		case VK_NUMPAD0:	return "Num 0";
		case VK_NUMPAD1:	return "Num 1";
		case VK_NUMPAD2:	return "Num 2";
		case VK_NUMPAD3:	return "Num 3";
		case VK_NUMPAD4:	return "Num 4";
		case VK_NUMPAD5:	return "Num 5";
		case VK_NUMPAD6:	return "Num 6";
		case VK_NUMPAD7:	return "Num 7";
		case VK_NUMPAD8:	return "Num 8";
		case VK_NUMPAD9:	return "Num 9";
		case VK_MULTIPLY:	return "Num *";
		case VK_ADD:		return "Num +";
		case VK_SEPARATOR:	return "Separator";
		case VK_SUBTRACT:	return "Num -";
		case VK_DECIMAL:	return "Num Del";
		case VK_DIVIDE:		return "Num /";
		case VK_F1:			return "F1";
		case VK_F2:			return "F2";
		case VK_F3:			return "F3";
		case VK_F4:			return "F4";
		case VK_F5:			return "F5";
		case VK_F6:			return "F6";
		case VK_F7:			return "F7";
		case VK_F8:			return "F8";
		case VK_F9:			return "F9";
		case VK_F10:		return "F10";
		case VK_F11:		return "F11";
		case VK_F12:		return "F12";
		case VK_F13:		return "F13";
		case VK_F14:		return "F14";
		case VK_F15:		return "F15";
		case VK_F16:		return "F16";
		case VK_F17:		return "F17";
		case VK_F18:		return "F18";
		case VK_F19:		return "F19";
		case VK_F20:		return "F20";
		case VK_F21:		return "F21";
		case VK_F22:		return "F22";
		case VK_F23:		return "F23";
		case VK_F24:		return "F24";
		case VK_NUMLOCK:	return "Num Lock";
		case VK_SCROLL:		return "Scroll Lock";
		case VK_LSHIFT:		return "Left Shift";
		case VK_RSHIFT:		return "Right Shift";
		case VK_LCONTROL:	return "Left Ctrl";
		case VK_RCONTROL:	return "Right Ctrl";
		case VK_LMENU:		return "Left Alt";
		case VK_RMENU:		return "Right Alt";
		case VK_PROCESSKEY:	return "";
		case VK_ATTN:		return "Attn";
		case VK_CRSEL:		return "";
		case VK_EXSEL:		return "";
		case VK_EREOF:		return "";
		case VK_PLAY:		return "Play";
		case VK_ZOOM:		return "Zoom";
		case VK_NONAME:		return "";
		case VK_PA1:		return "";
		case VK_OEM_CLEAR:	return "";
		case VK_OEM_PLUS:	return "+";
	}
	UINT lParam = MapVirtualKey(key, 2);
	if ( (lParam & 0x80000000) == 0 )
		return CAtlString((char)lParam);
	return CAtlString();
}
CAtlString GetFullKeyName(WORD key)
{
	CAtlString result;
	if ( GetKeyState(VK_CONTROL) < 0 )
		result += "Ctrl+";
	if ( GetKeyState(VK_MENU) < 0 )
		result += "Alt+";
	if ( GetKeyState(VK_SHIFT) < 0 )
		result += "Shift+";
	result += GetKeyName(key);
	return result;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) return std::wstring();
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8[0], (int)utf8.size(), NULL, 0);
	std::wstring wstrTo(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, &utf8[0], (int)utf8.size(), &wstrTo[0], size_needed);
	return wstrTo;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wide[0], (int)wide.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wide[0], (int)wide.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static void DebugLogAppendLineLocked(const char* line)
{
	if (!line || !line[0])
		return;

	constexpr long long kMaxBytes = 1024LL * 1024LL;

	auto ensureFallbackPath = []() {
		if (gFallbackLogPathInitialized)
			return;
		gFallbackLogPathInitialized = true;

		char tempDir[MAX_PATH] = {};
		DWORD n = GetTempPathA(ARRAYSIZE(tempDir), tempDir);
		if (n == 0 || n >= ARRAYSIZE(tempDir))
			return;

		StringCchPrintfA(gFallbackLogPath, ARRAYSIZE(gFallbackLogPath), "%sMarkdownViewGitHubStyle_Log.txt", tempDir);
	};

	auto maybeRotate = [kMaxBytes](const char* path) {
		if (!path || !path[0])
			return;
		HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
			return;
		LARGE_INTEGER sz{};
		if (GetFileSizeEx(h, &sz) && sz.QuadPart > kMaxBytes)
		{
			CloseHandle(h);
			DeleteFileA(path);
			return;
		}
		CloseHandle(h);
	};

	const char* primaryPath = options.LogIniFileName[0] ? options.LogIniFileName : nullptr;
	if (primaryPath)
		maybeRotate(primaryPath);

	FILE* f = primaryPath ? fopen(primaryPath, "ab") : nullptr;
	if (!f)
	{
		ensureFallbackPath();
		if (gFallbackLogPath[0])
		{
			maybeRotate(gFallbackLogPath);
			f = fopen(gFallbackLogPath, "ab");
		}
	}
	if (!f)
	{
		CreateDirectoryA("C:\\tmp", NULL);
		const char* fixedFallbackPath = "C:\\tmp\\MarkdownViewGitHubStyle_Log.txt";
		maybeRotate(fixedFallbackPath);
		f = fopen(fixedFallbackPath, "ab");
	}
	if (!f)
		return;
	fwrite(line, 1, strlen(line), f);
	fwrite("\r\n", 1, 2, f);
	fclose(f);
}

void DebugLog(const char* location, const char* message)
{
	if (!gDebugLogEnabled && !gIssue9DiagnosticLogAlways)
		return;

	SYSTEMTIME st{};
	GetLocalTime(&st);
	DWORD pid = GetCurrentProcessId();
	DWORD tid = GetCurrentThreadId();

	char line[2048]{};
	StringCchPrintfA(
		line, ARRAYSIZE(line),
		"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu %s | %s",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		(unsigned long)pid, (unsigned long)tid,
		location ? location : "",
		message ? message : ""
	);

	std::lock_guard<std::mutex> lock(gDebugLogMutex);
	OutputDebugStringA(line);
	OutputDebugStringA("\r\n");
	DebugLogAppendLineLocked(line);
}

void DebugLogW(const char* location, const wchar_t* message)
{
	std::string utf8 = message ? WideToUtf8(std::wstring(message)) : std::string();
	DebugLog(location, utf8.c_str());
}

void DebugLogBytes(const char* location, const void* data, size_t len)
{
	if (!gDebugLogEnabled)
		return;

	(void)data;
	char msg[64]{};
	StringCchPrintfA(msg, ARRAYSIZE(msg), "bytes len=%u", (unsigned int)len);
	DebugLog(location, msg);
}

HWND GetBrowserHostWnd(HWND child_hwnd)
{
	for(HWND hWnd=child_hwnd;hWnd;hWnd=GetParent(hWnd))
		if(GetProp(hWnd,PROP_BROWSER))
			return hWnd;
	return NULL;
}
/*
CBrowserHost* GetBrowserHost(HWND child_hwnd)
{
	void* browser_host;
	for(HWND hWnd=child_hwnd;hWnd;hWnd=GetParent(hWnd))
	{
		browser_host = GetProp(hWnd, PROP_BROWSER);
		if(browser_host)
			return (CBrowserHost*)browser_host;
	}
	return NULL;
}
*/

void DisplayLastError(void)
{
	LPVOID lpMessageBuffer;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR) &lpMessageBuffer,
		0,NULL );
	MessageBox(0,(LPCSTR)lpMessageBuffer,"Error",0);
	LocalFree( lpMessageBuffer );
}
