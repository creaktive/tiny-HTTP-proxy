#ifndef _DEBUG

	#pragma comment(linker,"/ENTRY:DllMain")

	#pragma comment(linker,"/NODEFAULTLIB:libc.lib")
	#pragma comment(linker,"/NODEFAULTLIB:libcmt.lib")
	#pragma comment(lib,"msvcrt.lib")

	#define WIN32_LEAN_AND_MEAN

#endif

#include <windows.h>
#include <shlobj.h>
#include <winsock.h>
#include "resource.h"


static HMODULE hMod;
static BOOL use_counter = FALSE;
static char output[MAX_PATH];


BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,	// handle to DLL module
	DWORD fdwReason,	// reason for calling function
	LPVOID lpvReserved	// reserved
)
{
	hMod = hinstDLL;
	return TRUE;
}


int CALLBACK BrowseCallbackProc(HWND hWnd, UINT uMsg, LPARAM lp, LPARAM pData)
{
	char szDir[MAX_PATH];

	switch(uMsg)
	{
		case BFFM_INITIALIZED:
		{
			if (GetCurrentDirectory(sizeof(szDir), szDir))
				SendMessage(hWnd, BFFM_SETSELECTION, TRUE, (LPARAM) szDir);
			break;
		}
		case BFFM_SELCHANGED:
		{
			if (SHGetPathFromIDList((LPITEMIDLIST) lp ,szDir))
				SendMessage(hWnd, BFFM_SETSTATUSTEXT, 0, (LPARAM) szDir);
			break;
		}
	}

	return 0;
}


BOOL LogOutputDir(HWND hWnd)
{
	char buf[MAX_PATH];
	BROWSEINFO bi;
	LPITEMIDLIST pidl;

	ZeroMemory(buf, sizeof(buf));

	bi.hwndOwner		= hWnd;
	bi.pidlRoot			= NULL;
	bi.pszDisplayName	= NULL;
	bi.lpszTitle		= "Select directory to output log files:";
	bi.ulFlags			= BIF_EDITBOX | BIF_RETURNONLYFSDIRS | BIF_STATUSTEXT;
	bi.lpfn				= BrowseCallbackProc;
	bi.lParam			= 0;
	bi.iImage			= 0;

	if ((pidl = SHBrowseForFolder(&bi)) == 0)
		return FALSE;

	if (!SHGetPathFromIDList(pidl, buf))
		return FALSE;

	lstrcat(buf, "\\");
	ZeroMemory(output, sizeof(output));
	lstrcpyn(output, buf, sizeof(output) - 1);

	return TRUE;
}


BOOL CALLBACK LogDialog(
	HWND hWnd,		// handle to dialog box
	UINT uMsg,		// message
	WPARAM wParam,	// first message parameter
	LPARAM lParam	// second message parameter
)
{
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			SetDlgItemText(hWnd, IDC_OUTPUT, output);

			if (use_counter)
				goto ISCON;
			else
				goto ISCOFF;

			break;
		}
		case WM_SYSCOMMAND:
		{
			switch((DWORD) wParam)
			{ 
				case SC_CLOSE:
				{
					EndDialog(hWnd, 0);
					return TRUE;
				}
			}

			break; 
		}
		case WM_COMMAND:
		{
			switch (LOWORD((DWORD) wParam))
			{
				case IDC_CON:
				{
					use_counter = TRUE;
ISCON:
					EnableWindow(GetDlgItem(hWnd, IDC_CON), FALSE);
					EnableWindow(GetDlgItem(hWnd, IDC_COFF), TRUE);
					break;
				}
				case IDC_COFF:
				{
					use_counter = FALSE;
ISCOFF:
					EnableWindow(GetDlgItem(hWnd, IDC_CON), TRUE);
					EnableWindow(GetDlgItem(hWnd, IDC_COFF), FALSE);
					break;
				}
				case IDC_SETOUT:
				{
					if (LogOutputDir(hWnd))
						SetDlgItemText(hWnd, IDC_OUTPUT, output);
					break;
				}
			}
		}
	}

	return FALSE;
}


__declspec(dllexport) BOOL FilterInit (HWND hWnd)
{
	ZeroMemory(output, sizeof(output));
	GetCurrentDirectory(sizeof(output) - 1, output);
	lstrcat(output, "\\");

	return TRUE;
}


__declspec(dllexport) BOOL FilterSetup (HWND hWnd)
{
	DialogBox(hMod, MAKEINTRESOURCE(IDD_DIALOG), hWnd, LogDialog);
	return TRUE;
}


char *GetPeerAsStr(SOCKET sd, BOOL filename)
{
	SOCKADDR_IN sin;
	int sinlen = sizeof(sin);
	static char peer[32];

	ZeroMemory(peer, sizeof(peer));

	if (getpeername(sd, (struct sockaddr FAR *) &sin, &sinlen) == SOCKET_ERROR)
		lstrcpy(peer, "UNKNOWN");
	else
	{
		if (filename)
			wsprintf(peer, "%s.%d", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
		else
			wsprintf(peer, "%s:%d", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
	}

	return peer;
}


__declspec(dllexport) BOOL FilterProcess (SOCKET from, SOCKET to, char *buf, int count, int size, BOOL is_request)
{
	char name[MAX_PATH*2];
	char stat[128];
	HANDLE log;
	DWORD nbWrite;

	if (size <= 0)
		return TRUE;

	ZeroMemory(name, sizeof(name));
	lstrcat(name, output);
	lstrcat(name, GetPeerAsStr(from, TRUE));
	lstrcat(name, "-");
	lstrcat(name, GetPeerAsStr(to, TRUE));
	lstrcat(name, ".log");

	log = CreateFile(
		name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL
	);

	if (log == INVALID_HANDLE_VALUE)
		return TRUE;

	if (use_counter)
	{
		ZeroMemory(stat, sizeof(stat));
		wsprintf(stat, "PACKETS #: [%5d]\r\n\r\n", count + 1);
		SetFilePointer(log, 0, 0, FILE_BEGIN);
		WriteFile(log, stat, lstrlen(stat), &nbWrite, NULL);
	}

	SetFilePointer(log, 0, 0, FILE_END);
	WriteFile(log, buf, size, &nbWrite, NULL);
	CloseHandle(log);

	return TRUE;
}


__declspec(dllexport) BOOL FilterEnd (VOID)
{
	return TRUE;
}
