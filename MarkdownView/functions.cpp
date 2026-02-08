#ifndef E_BOUNDS
#define E_BOUNDS                         _HRESULT_TYPEDEF_(0x8000000BL)
#endif

#include <mshtmdid.h>
#include "functions.h"
#include <cstdio>
#include <mutex>

HINSTANCE hinst = NULL;

SOptions options = {false, 0, 0, 0, 0, 0, "", ""};
LARGE_INTEGER LogFirstCount = {0};
static std::mutex gLogMutex;
static std::wstring gLogFilePath;

static bool EndsWithCi(const std::wstring& s, const std::wstring& suffix)
{
	if (s.size() < suffix.size())
		return false;

	size_t start = s.size() - suffix.size();
	for (size_t i = 0; i < suffix.size(); i++)
	{
		wchar_t a = s[start + i];
		wchar_t b = suffix[i];
		if (a >= L'A' && a <= L'Z')
			a = (wchar_t)(a - L'A' + L'a');
		if (b >= L'A' && b <= L'Z')
			b = (wchar_t)(b - L'A' + L'a');
		if (a != b)
			return false;
	}
	return true;
}

static std::wstring DirNameOfPath(const std::wstring& fullPath)
{
	size_t slash = fullPath.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return L"";
	return fullPath.substr(0, slash);
}

static std::wstring GetPrimaryLogRoot()
{
	wchar_t modulePath[MAX_PATH] = {0};
	if (hinst && GetModuleFileNameW(hinst, modulePath, MAX_PATH))
	{
		std::wstring moduleDir = DirNameOfPath(modulePath);
		std::wstring lower = moduleDir;
		for (auto& ch : lower)
			if (ch >= L'A' && ch <= L'Z')
				ch = (wchar_t)(ch - L'A' + L'a');

		const std::wstring binRelease = L"\\bin\\release";
		const std::wstring binDebug = L"\\bin\\debug";
		if (EndsWithCi(lower, binRelease))
		{
			std::wstring root = moduleDir.substr(0, moduleDir.size() - binRelease.size());
			if (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
				root.pop_back();
			return root;
		}
		if (EndsWithCi(lower, binDebug))
		{
			std::wstring root = moduleDir.substr(0, moduleDir.size() - binDebug.size());
			if (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
				root.pop_back();
			return root;
		}

		return moduleDir;
	}

	wchar_t cwd[MAX_PATH] = {0};
	if (GetCurrentDirectoryW(MAX_PATH, cwd))
		return std::wstring(cwd);

	return L".";
}

static std::wstring GetFallbackLogRoot()
{
	wchar_t buf[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
	{
		std::wstring dir = std::wstring(buf) + L"\\TotalCommanderMarkdownViewPlugin";
		CreateDirectoryW(dir.c_str(), NULL);
		return dir;
	}

	wchar_t tmp[MAX_PATH] = {0};
	if (GetTempPathW(MAX_PATH, tmp))
	{
		std::wstring dir = std::wstring(tmp) + L"TotalCommanderMarkdownViewPlugin";
		CreateDirectoryW(dir.c_str(), NULL);
		return dir;
	}

	return L".";
}

static std::wstring EnsureLogFilePath()
{
	if (!gLogFilePath.empty())
		return gLogFilePath;

	std::wstring primary = GetPrimaryLogRoot();
	std::wstring candidate = primary;
	if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/')
		candidate += L"\\";
	candidate += L"mdv_translate.log";

	FILE* f = nullptr;
	if (_wfopen_s(&f, candidate.c_str(), L"ab") == 0 && f)
	{
		fclose(f);
		gLogFilePath = candidate;
		return gLogFilePath;
	}
	if (f)
		fclose(f);

	std::wstring fallback = GetFallbackLogRoot();
	candidate = fallback;
	if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/')
		candidate += L"\\";
	candidate += L"mdv_translate.log";

	gLogFilePath = candidate;
	return gLogFilePath;
}

static void AppendLogUtf8Line(const std::string& lineUtf8)
{
	std::lock_guard<std::mutex> lock(gLogMutex);

	std::wstring path = EnsureLogFilePath();
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f)
		return;

	fwrite(lineUtf8.data(), 1, lineUtf8.size(), f);
	fwrite("\r\n", 1, 2, f);
	fclose(f);
}

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
void CSmallStringList::set_size(int size)
{
	clear();
	data = new unsigned char[size];
}
void CSmallStringList::set_data(const unsigned char* buffer, int size)
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
	strcat(buffer+1, ";");
	char* next_pos;
	char* str_pos = buffer;
	while(true)
	{
		next_pos=strchr(str_pos+1, ';');
		if(next_pos==NULL || next_pos==str_pos)
			break;
		*str_pos = next_pos-str_pos-1;
		str_pos = next_pos;
	}
	*str_pos = 0;
	set_data((unsigned char*)buffer, str_pos-buffer+1);
}
void CSmallStringList::load_sign_from_ini(const char* filename, const char* section, const char* key)
{
	char buffer[512];
	unsigned char list[512];
	GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), filename);
	strcat(buffer, ";");
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
			*list_pos = next_pos-str_pos-2;
			memcpy(list_pos+1, str_pos+1, *list_pos);
			list_pos += *list_pos+1;
		}
		else
		{
			*list_pos = (next_pos-str_pos)/2;
			for ( const char * s = str_pos; s<=next_pos-2; s += 2 )
				sscanf(s, "%2x", ++list_pos);
			++list_pos;
		}
		str_pos = next_pos+1;
	}
	*list_pos = 0;
	set_data(list, list_pos-list+1);
}
bool CSmallStringList::find(const char* str)
{
	if(!valid())
		return false;
	int len = strlen(str);
	for(const unsigned char* str_pos = data; *str_pos; str_pos+=*str_pos+1)
		if(*str_pos==len && !memcmp(str, str_pos+1, len))
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
	int num_read;
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
		if(num_read==*str_pos && !memicmp(buf, str_pos+1, *str_pos))
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
	GetModuleFileName(hinst, options.IniFileName, sizeof(options.IniFileName));
	char* dot = strrchr(options.IniFileName, '.');
	strcpy(dot, ".ini");

	BOOL ini_exists = PathFileExists(options.IniFileName);
	if(!ini_exists)
	{
		CAtlString message = "File not found: ";
		message += options.IniFileName;
		MessageBox(NULL, message, "HTMLView", MB_OK|MB_ICONWARNING);
	}


	strcpy(options.LogIniFileName, options.IniFileName);
	strcpy(options.LogIniFileName+(dot-options.IniFileName), "_Log.ini");

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

HWND GetBrowserHostWnd(HWND child_hwnd)
{
	for(HWND hWnd=child_hwnd;hWnd;hWnd=GetParent(hWnd))
		if(GetProp(hWnd,PROP_BROWSER))
			return hWnd;
	return NULL;
}

void DebugLog(const char* location, const char* message, const char* hypothesisId)
{
	(void)location;
	(void)message;
	(void)hypothesisId;
}

void DebugLogW(const char* location, const wchar_t* message, const char* hypothesisId)
{
	(void)location;
	(void)message;
	(void)hypothesisId;
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

//							#-----------#
//  						|	  	    |
//**************************|    Log    |***************************
//							|		    |
//							#-----------#

int Log(char* Section, char* Text)
{
	(void)Section;
	(void)Text;
	return 0;
}
void LogTimeReset()
{
	return;
}

void LogTime(char* Text)
{
	(void)Text;
}
void LogTime(int number)
{
	(void)number;
}

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
