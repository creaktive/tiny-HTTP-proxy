#ifndef _DEBUG

	#pragma comment(linker,"/ENTRY:main")
	#pragma comment(linker,"/MERGE:.rdata=.text")
	#pragma comment(linker,"/IGNORE:4078")
	#pragma comment(linker,"/OPT:NOWIN98")

	#pragma comment(linker,"/NODEFAULTLIB:libc.lib")
	#pragma comment(linker,"/NODEFAULTLIB:libcmt.lib")
	#pragma comment(lib,"msvcrt.lib")

	#define WIN32_LEAN_AND_MEAN

#endif


#include <windows.h>
#include <commdlg.h>
#include <malloc.h>
#include <shellapi.h>
#include <stdlib.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "resource.h"

#include <winsock.h>

#ifndef _WINSOCK2API_
#define SD_SEND 0x1
#define SD_BOTH 0x2
#endif


#define WM_SHELLNOTIFY	WM_USER + 10


#define LOCALHOST	"127.0.0.1"
#define LOCALPORT	8181
#define MAXCONN		32
#define BUFLEN		512
#define REQLEN		2048


typedef BOOL (*tFilterInit) (HWND hWnd);
typedef BOOL (*tFilterSetup) (HWND hWnd);
typedef BOOL (*tFilterProcess) (SOCKET from, SOCKET to, char *buf, int count, int size, BOOL is_request);
typedef BOOL (*tFilterEnd) (VOID);

typedef struct
{
	SOCKET client, server;
} Tunnel;

typedef struct _Thread
{
	HANDLE handle;
	DWORD id;
	WORD sockn;
	SOCKET sock[16];
	struct _Thread *next;
} Thread;


static HINSTANCE hInst;
static DWORD pID;
static NOTIFYICONDATA tray;
static SOCKET ls;
static HANDLE Proxy = INVALID_HANDLE_VALUE;
static unsigned int in_pkt = 0, in_bytes = 0;
static unsigned int out_pkt = 0, out_bytes = 0;
static char plugin[MAX_PATH];
static HINSTANCE hPlug = NULL;
static BOOL plugin_setup_running = FALSE;

static WORD threadn = 0;
static Thread *head = NULL;
static Thread *tail = NULL;

static tFilterInit		pFilterInit		= NULL;
static tFilterSetup		pFilterSetup	= NULL;
static tFilterProcess	pFilterProcess	= NULL;
static tFilterEnd		pFilterEnd		= NULL;


BOOL ThreadCreate(LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter)
{
	HANDLE hThread;
	DWORD ThreadID;
	Thread *thread;

	hThread = CreateThread(NULL, 0, lpStartAddress, lpParameter, 0, &ThreadID);
	if (hThread == NULL)
		return FALSE;

	thread = (Thread *) malloc(sizeof(Thread));
	if (thread == NULL)
		return FALSE;

	ZeroMemory(thread, sizeof(Thread));
	thread->handle	= hThread;
	thread->id		= ThreadID;
	thread->next	= NULL;

	if (head == NULL)
	{
		head = thread;
		tail = thread;
	}
	else
	{
		tail->next = thread;
		tail = thread;
	}

	threadn++;

	return TRUE;
}


DWORD ThreadExit(DWORD dwExitCode)
{
	DWORD ThreadId = GetCurrentThreadId();
	Thread *thread, *prev;

	for (thread = head, prev = head; thread != NULL; thread = thread->next)
	{
		if (thread->id == ThreadId)
		{
			if (thread == head)
				head = thread->next;
			else
				prev->next = thread->next;
			free(thread);
			break;
		}
		prev = thread;
	}

	threadn--;

	ExitThread(dwExitCode);
	return dwExitCode;
}


void ThreadSock(SOCKET sd)
{
	DWORD ThreadId = GetCurrentThreadId();
	Thread *thread;

	for (thread = head; thread != NULL; thread = thread->next)
		if (thread->id == ThreadId)
		{
			thread->sock[thread->sockn] = sd;
			thread->sockn++;
			break;
		}
}


void KillAllThreads(void)
{
	Thread *thread, *bak;
	WORD i;

	for (thread = head; thread != NULL;)
	{
		for (i = 0; i < thread->sockn; i++)
		{
			shutdown(thread->sock[i], SD_BOTH);
			closesocket(thread->sock[i]);
		}

		TerminateThread(thread->handle, 0);

		bak = thread;
		thread = thread->next;
		free(bak);
	}

	threadn	= 0;
	head	= NULL;
	tail	= NULL;

	return;
}


BOOL ShutdownConnection(SOCKET sd)
{
	char buf[BUFLEN];
	int nNew;

	if (shutdown(sd, SD_SEND) == SOCKET_ERROR)
		return FALSE;

	while (1)
	{
		nNew = recv(sd, buf, BUFLEN, 0);
		if (nNew == SOCKET_ERROR)
			return FALSE;
		else if (nNew != 0)
			continue;
		else
			break;
	}

	if (closesocket(sd) == SOCKET_ERROR)
		return FALSE;

	return TRUE;
}


unsigned long LookupAddress(const char* host)
{
	unsigned long addr = inet_addr(host);
	HOSTENT *phe;

	if (addr == INADDR_NONE)
	{
		phe = gethostbyname(host);
		if (!phe)
			return INADDR_NONE;
		addr = *((unsigned long *) phe->h_addr_list[0]);
	}

	return addr;
}


int HTTPError(SOCKET sd, int code, const char *str)
{
	char msg[64], html[512];

	ZeroMemory(msg, sizeof(msg));
	ZeroMemory(html, sizeof(html));

	wsprintf(msg, "%d %s", code, str);

	wsprintf(html,
		"HTTP/1.0 %s\r\n"
		"Content-type: text/html\r\n"
		"Connection: close\r\n\r\n"

		"<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 3.2//EN\">\r\n"
		"<html><head><title>%s</title></head><body>"
		"<h1>%s</h1>"
		"<small>Multipurpose HTTP Proxy</small>"
		"</body></html>\r\n"

		, msg, msg, msg
	);

	send(sd, html, lstrlen(html), 0);

	ShutdownConnection(sd);
	return 1;
}


char *TrimCopy(const char *from, char *to, int len)
{
	int i, j;

	ZeroMemory(to, len);

	for (i = 0, j = 0; i < lstrlen(from); i++)
	{
		if (from[i] == '\0')
			break;
		else if (_istspace(from[i]))
		{
			if (!j)
				continue;
			else
				break;
		}
		else if (j < len - 1)
			to[j++] = from[i];
	}

	for (i; i < lstrlen(from); i++)
		if (!_istspace(from[i]))
			break;

	return (char *) from + i;
}


unsigned short HostPort(char *host)
{
	int i;
	int port = 0;

	for (i = 0; i <= lstrlen(host); i++)
		if (host[i] == ':')
		{
			host[i++] = '\0';
			port = atoi(host + i);
			break;
		}

	if (port <= 0 || port >= 65536)
		port = 80;

	return port;
}


BOOL BreakURL(const char *url, char *host, int hostlen, int *port, char *uri, int urilen)
{
	int i, j;
	char tmp[10];

	ZeroMemory(tmp, sizeof(tmp));
	ZeroMemory(host, hostlen);
	ZeroMemory(uri, urilen);

	lstrcpyn(tmp, url, 8);
	if (lstrcmp(tmp, "http://"))
		return FALSE;

	for (i = 7, j = 0; i < lstrlen(url); i++)
		if (url[i] == '/')
			break;
		else
			host[j++] = url[i];

	if (url[i])
		lstrcpyn(uri, url + i, urilen);

	*port = HostPort(host);

	return TRUE;
}


SOCKET OpenProxy(char *host, int port)
{
	SOCKADDR_IN sin;
	SOCKET sd;
	unsigned long addr;

	if ((addr = LookupAddress(host)) == INADDR_NONE)
		return INVALID_SOCKET;

	if ((sd = socket(AF_INET, SOCK_STREAM, 0)) != INVALID_SOCKET)
	{
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = addr;
		sin.sin_port = htons((unsigned short) port);
		if (connect(sd, (struct sockaddr FAR *) &sin, sizeof(sin)) == SOCKET_ERROR)
			return INVALID_SOCKET;
	}

	return sd;
}


#define IS_HEADER(str) \
	!((edit[sizeof(str)] = '\0') || lstrcmpi(edit, str ":"))

#define SKIP_HEADER() \
	for (; (p < end) && (*p!=0xa); p++); \
	continue;


Tunnel *RebuildRequest(SOCKET client, const char *buf, char *sendbuf, int *len)
{
	char *p;
	char method[16];
	char url[1024];
	char host[128];
	int port;
	char uri[1024];
	char version[16];
	Tunnel *request;
	int tmp;
	BOOL is_connect = FALSE;
	char *connect_ok = "HTTP/1.0 200 Connection established\r\n\r\n";
	SOCKET server;
	char *q, edit[64];
	char *end;

	p = TrimCopy(buf, method, sizeof(method));
	if (*p == '\0')
	{
		HTTPError(client, 400, "Bad Request");
		return NULL;
	}
	CharUpperBuff(method, sizeof(method));
	if (
		lstrcmp(method, "HEAD") &&
		lstrcmp(method, "GET") &&
		lstrcmp(method, "POST") &&
		lstrcmp(method, "CONNECT")
	   )
	{
		HTTPError(client, 405, "Method Not Allowed");
		return NULL;
	}

	p = TrimCopy(p, url, sizeof(url));
	if (*p == '\0')
	{
		HTTPError(client, 400, "Bad Request");
		return NULL;
	}
	if (lstrlen(url) == sizeof(url) - 1)
	{
		HTTPError(client, 414, "URI Too Large");
		return NULL;
	}

	p = TrimCopy(p, version, sizeof(version));
	if (version[0] == '\0')
	{
		HTTPError(client, 400, "Bad Request");
		return NULL;
	}
	CharUpperBuff(version, sizeof(version));
	if (
		lstrcmp(version, "HTTP/1.0") &&
		lstrcmp(version, "HTTP/1.1")
	   )
	{
		HTTPError(client, 505, "HTTP Version Unsupported");
		return NULL;
	}

	if (lstrcmp(method, "CONNECT"))
	{
		if (!BreakURL(url, host, sizeof(host), &port, uri, sizeof(uri)))
		{
			HTTPError(client, 501, "Not Implemented");
			return NULL;
		}

		wsprintf(sendbuf, "%s %s %s\r\n", method, uri, "HTTP/1.0");
		end = p + (*len - (p - buf));

		for (q = sendbuf + lstrlen(sendbuf); p < end; p++)
		{
			tmp = end - p;
			ZeroMemory(edit, sizeof(edit));
			lstrcpyn(edit, p, (tmp < sizeof(edit)) ? tmp : sizeof(edit) - 1);

			if (*(p-1)==0xa)
			{
				if (IS_HEADER("Proxy-Connection"))
				{
					SKIP_HEADER();
				}
				else if (IS_HEADER("Keep-Alive"))
				{
					SKIP_HEADER();
				}
			}

			*(q++) = *p;
		}

		*len = q - sendbuf;
	}
	else
	{
		is_connect = TRUE;
		*len = 0;

		port = HostPort(url);
		ZeroMemory(host, sizeof(host));
		lstrcpyn(host, url, sizeof(host));
	}

	if ((server = OpenProxy(host, port)) == INVALID_SOCKET)
	{
		HTTPError(client, 503, "Service Unavailable");
		return NULL;
	}

	request = (Tunnel *) malloc(sizeof(Tunnel));
	request->client = client;
	request->server = server;

	if (is_connect)
		if (send(request->client, connect_ok, lstrlen(connect_ok), 0) == SOCKET_ERROR)
			return NULL;

	return request;
}


DWORD WINAPI Forward(void *request_)
{
	Tunnel request;
	char buf[BUFLEN];
	int nRead, nSent, nTmp;
	int packet = 0;

	memcpy(&request, request_, sizeof(Tunnel));

	ThreadSock(request.client);
	ThreadSock(request.server);

	do
	{
		ZeroMemory(buf, sizeof(buf));
		nRead = recv(request.server, buf, BUFLEN, 0);

		if (nRead > 0)
		{
			if (pFilterProcess)
				if (!pFilterProcess(request.server, request.client, buf, packet++, nRead, FALSE))
					break;

			in_pkt++;
			in_bytes += nRead;

			nSent = 0;
			while (nSent < nRead)
			{
				nTmp = send(request.client, buf + nSent, nRead - nSent, 0);
				if (nTmp > 0)
					nSent += nTmp;
				else if (nTmp == SOCKET_ERROR)
					break;
				else
					break;
			}
		}
		else if (nRead == SOCKET_ERROR)
			break;
	} while (nRead != 0);

	ShutdownConnection(request.client);
	ShutdownConnection(request.server);

	if (pFilterProcess)
		pFilterProcess(0, 0, NULL, packet++, -1, FALSE);

	return ThreadExit(1);
}


DWORD WINAPI ProxyHandler(void *sd_)
{
	SOCKET sd = (SOCKET) sd_;
	char buf[REQLEN], sendbuf[REQLEN];
	int nRead, nSent, nTmp;
	int buflen = REQLEN;
	int len;
	Tunnel *request = NULL;
	int packet = 0;

	ThreadSock(sd);

	do
	{
		ZeroMemory(buf, sizeof(buf));
		nRead = recv(sd, buf, buflen, 0);

		if (nRead > 0)
		{
			if (buflen != BUFLEN)
				buflen = BUFLEN;

			len = nRead;
			ZeroMemory(sendbuf, len);

			if (request == NULL)
			{
				if ((request = RebuildRequest(sd, buf, sendbuf, &len)) == NULL)
					return ThreadExit(0);

				ThreadSock(request->server);
				ThreadCreate(Forward, (void *) request);
			}
			else
				memcpy(sendbuf, buf, len);

			if (pFilterProcess)
				if (!pFilterProcess(request->client, request->server, sendbuf, packet++, len, TRUE))
					break;

			out_pkt++;
			out_bytes += len;

			nSent = 0;
			while (nSent < len)
			{
				nTmp = send(request->server, sendbuf + nSent, len - nSent, 0);
				if (nTmp > 0)
					nSent += nTmp;
				else if (nTmp == SOCKET_ERROR)
					break;
				else
					break;
			}
		}
		else if (nRead == SOCKET_ERROR)
			break;
	} while (nRead != 0);

	//ShutdownConnection(request->client);
	//ShutdownConnection(request->server);

	free(request);

	if (pFilterProcess)
		pFilterProcess(0, 0, NULL, packet++, -1, FALSE);

	return ThreadExit(1);
}


SOCKET Listener(const char *host, const int port, const int max)
{
	unsigned long addr;
	SOCKET sd;
	SOCKADDR_IN sin;

	if ((addr = LookupAddress(host)) == INADDR_NONE)
		return INVALID_SOCKET;

	if (addr != INADDR_NONE)
	{
		sd = socket(AF_INET, SOCK_STREAM, 0);
		if (sd != INVALID_SOCKET)
		{
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = addr;
			sin.sin_port = htons((unsigned short) port);
			if (bind(sd, (struct sockaddr FAR *) &sin, sizeof(sin)) != SOCKET_ERROR)
			{
				listen(sd, max);
				return sd;
			}
		}
	}

	return INVALID_SOCKET;
}


DWORD WINAPI ProxyMain(LPVOID lpParameter)
{
	SOCKET sd;
	SOCKADDR_IN sin;
	int sinlen = sizeof(sin);

	while (1)
	{
		sd = accept(ls, (SOCKADDR *) &sin, &sinlen);
		if (sd != INVALID_SOCKET)
			ThreadCreate(ProxyHandler, (void *) sd);
		else
			break;
	}

	return ThreadExit(1);
}


void UnloadPlugin(HWND hWnd)
{
	if (hPlug)
	{
		if (pFilterEnd)
			pFilterEnd();

		FreeLibrary(hPlug);
		hPlug = NULL;
	}

	pFilterInit		= NULL;
	pFilterSetup	= NULL;
	pFilterProcess	= NULL;
	pFilterEnd		= NULL;

	EnableWindow(GetDlgItem(hWnd, IDC_PLUGINSETUP), FALSE);

	return;
}


BOOL CALLBACK ProxyDialog(
	HWND hWnd,		// handle to dialog box
	UINT uMsg,		// message
	WPARAM wParam,	// first message parameter
	LPARAM lParam	// second message parameter
)
{
	char host[64];
	int port, maxconn;
	DWORD ThreadID;
	DWORD ExitCode;
	OPENFILENAME ofn;
	char buf[MAX_PATH];

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			ZeroMemory(&tray, sizeof(NOTIFYICONDATA));
			tray.cbSize		= sizeof(NOTIFYICONDATA);
			tray.hWnd		= hWnd;
			tray.uID		= IDI_ICON;
			tray.uFlags		= NIF_ICON | NIF_MESSAGE | NIF_TIP;
			tray.uCallbackMessage = WM_SHELLNOTIFY;
			tray.hIcon		= LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICON));
			Shell_NotifyIcon(NIM_ADD, &tray);

			SendMessage(hWnd, WM_SETICON, 0, (DWORD) tray.hIcon);

			SetTimer(hWnd, 0, 250, NULL);

			SetDlgItemText(hWnd, IDC_HOST, LOCALHOST);
			SetDlgItemInt(hWnd, IDC_PORT, LOCALPORT, FALSE);
			SetDlgItemInt(hWnd, IDC_MAXCONN, MAXCONN, FALSE);
			SetDlgItemInt(hWnd, IDC_THREADS, 0, FALSE);

			SetDlgItemInt(hWnd, IDC_IN_PKT, 0, FALSE);
			SetDlgItemInt(hWnd, IDC_IN_BYTES, 0, FALSE);
			SetDlgItemInt(hWnd, IDC_OUT_PKT, 0, FALSE);
			SetDlgItemInt(hWnd, IDC_OUT_BYTES, 0, FALSE);

			CheckDlgButton(hWnd, IDC_PLUGINON, BST_CHECKED);
			SetDlgItemText(hWnd, IDC_PLUGINFILE, plugin);
			EnableWindow(GetDlgItem(hWnd, IDC_PLUGINSETUP), FALSE);

			EnableWindow(GetDlgItem(hWnd, IDC_START), TRUE);
			EnableWindow(GetDlgItem(hWnd, IDC_STOP), FALSE);

			SetFocus(GetDlgItem(hWnd, IDC_START));

			break;
		}
		case WM_SYSCOMMAND:
		{
			switch((DWORD) wParam)
			{ 
				case SC_CLOSE:
				{
					ShowWindow(hWnd, SW_HIDE);
					break;
				}
			}

			break; 
		}
		case WM_TIMER:
		{
			if (Proxy != INVALID_HANDLE_VALUE && (!GetExitCodeThread(Proxy, &ExitCode) || ExitCode != STILL_ACTIVE))
			{
				MessageBox(hWnd, "Proxy died unexpectingly!", NULL, MB_OK | MB_ICONEXCLAMATION);
				goto STOP;
			}

			SetDlgItemInt(hWnd, IDC_THREADS, threadn, FALSE);

			wsprintf(tray.szTip, "Active connections: %d", threadn);
			Shell_NotifyIcon(NIM_MODIFY, &tray);

			SetDlgItemInt(hWnd, IDC_IN_PKT, in_pkt, FALSE);
			SetDlgItemInt(hWnd, IDC_IN_BYTES, in_bytes, FALSE);
			SetDlgItemInt(hWnd, IDC_OUT_PKT, out_pkt, FALSE);
			SetDlgItemInt(hWnd, IDC_OUT_BYTES, out_bytes, FALSE);

			break;
		}
		case WM_SHELLNOTIFY:
		{
			switch (lParam)
			{
				case WM_LBUTTONDOWN:
				case WM_MBUTTONDOWN:
				{
					ShowWindow(hWnd, SW_SHOW);
					SetForegroundWindow(hWnd);

					break;
				}
				case WM_RBUTTONDOWN:
				{
					if (plugin_setup_running || !pFilterSetup)
						break;

					plugin_setup_running = TRUE;
					pFilterSetup(hWnd);
					plugin_setup_running = FALSE;

					break;
				}
			}

			break;
		}
		case WM_COMMAND:
		{
			switch (LOWORD((DWORD) wParam))
			{
				case IDC_START:
				{
					ZeroMemory(host, sizeof(host));
					GetDlgItemText(hWnd, IDC_HOST, host, sizeof(host) - 1);
					port = GetDlgItemInt(hWnd, IDC_PORT, NULL, FALSE);
					maxconn = GetDlgItemInt(hWnd, IDC_MAXCONN, NULL, FALSE);

					if (
						(*host == '\0') ||
						(port <= 0) || (port >= 65536) ||
						(maxconn <= 0) || (maxconn > 128)
					   )
					{
						MessageBox(hWnd, "Parameters out of range!", NULL, MB_OK | MB_ICONEXCLAMATION);
						break;
					}

					if (IsDlgButtonChecked(hWnd, IDC_PLUGINON))
					{
						if ((hPlug = LoadLibrary(plugin)) == NULL)
						{
							UnloadPlugin(hWnd);
							MessageBox(hWnd, "Can't load plugin!", NULL, MB_OK | MB_ICONSTOP);
							break;
						}

						pFilterInit		= (tFilterInit) GetProcAddress(hPlug, "FilterInit");
						pFilterSetup	= (tFilterSetup) GetProcAddress(hPlug, "FilterSetup");
						pFilterProcess	= (tFilterProcess) GetProcAddress(hPlug, "FilterProcess");
						pFilterEnd		= (tFilterEnd) GetProcAddress(hPlug, "FilterEnd");

						if (!(pFilterInit && pFilterSetup && pFilterProcess && pFilterEnd))
						{
							UnloadPlugin(hWnd);
							MessageBox(hWnd, "Not a valid plugin!", NULL, MB_OK | MB_ICONSTOP);
							break;
						}

						pFilterInit(hWnd);
						EnableWindow(GetDlgItem(hWnd, IDC_PLUGINSETUP), TRUE);
					}
					else
						UnloadPlugin(hWnd);

				    if ((ls = Listener(host, port, maxconn)) != INVALID_SOCKET)
					{
						Proxy = CreateThread(NULL, 0, ProxyMain, NULL, 0, &ThreadID);

						EnableWindow(GetDlgItem(hWnd, IDC_HOST), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_PORT), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_MAXCONN), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_PLUGINON), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_PLUGINLOAD), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_START), FALSE);
						EnableWindow(GetDlgItem(hWnd, IDC_STOP), TRUE);

						SetFocus(GetDlgItem(hWnd, IDC_STOP));
					}
					else
						MessageBox(hWnd, "Can't start proxy!", NULL, MB_OK | MB_ICONSTOP);

					break;
				}
				case IDC_STOP:
				{
STOP:
					if (TerminateThread(Proxy, 0))
					{
						closesocket(ls);
						KillAllThreads();
						Proxy = INVALID_HANDLE_VALUE;

						EnableWindow(GetDlgItem(hWnd, IDC_HOST), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_PORT), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_MAXCONN), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_PLUGINON), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_PLUGINLOAD), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_START), TRUE);
						EnableWindow(GetDlgItem(hWnd, IDC_STOP), FALSE);

						SetFocus(GetDlgItem(hWnd, IDC_START));

						UnloadPlugin(hWnd);
						break;
					}
					else
						MessageBox(hWnd, "Can't stop proxy!", NULL, MB_OK | MB_ICONSTOP);

					break;
				}
				case IDC_QUIT:
				{
					UnloadPlugin(hWnd);
					EndDialog(hWnd, 0);
					return TRUE;
				}
				case IDC_PLUGINLOAD:
				{
					ZeroMemory(buf, sizeof(buf));
					lstrcpyn(buf, plugin, sizeof(buf) - 1);

					ZeroMemory(&ofn, sizeof(ofn));
					ofn.lStructSize		= sizeof(ofn);
					ofn.hwndOwner		= hWnd;
					ofn.hInstance		= hInst;
					ofn.lpstrFilter		= "Dynamic Loadable Library (*.dll)\0*.dll\0\0";
					ofn.nFilterIndex	= 1;
					ofn.lpstrFile		= buf;
					ofn.nMaxFile		= MAX_PATH - 1;
					ofn.lpstrInitialDir	= NULL;
					ofn.lpstrTitle		= "Select HTTP Proxy Plugin";
					ofn.Flags			= OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_READONLY;
					ofn.lpstrDefExt		= "dll";

					if (GetOpenFileName(&ofn))
					{
						lstrcpyn(plugin, buf, sizeof(plugin) - 1);
						SetDlgItemText(hWnd, IDC_PLUGINFILE, plugin);
					}

					break;
				}
				case IDC_PLUGINSETUP:
				{
					plugin_setup_running = TRUE;
					pFilterSetup(hWnd);
					plugin_setup_running = FALSE;

					break;
				}
			}
		}
	}

	return FALSE;
}


int WINAPI WinMain(
	HINSTANCE hInstance,		// handle to current instance
	HINSTANCE hPrevInstance,	// handle to previous instance
	LPSTR lpCmdLine,			// pointer to command line
	int nCmdShow				// show state of window
)
{
	WSADATA wsaData;
	char *p;

	if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
		return 0;

	hInst = hInstance;
	pID = GetCurrentProcessId();
	
	p = lpCmdLine;
	if (*p == '"')
	{
		for (p++; (*p!='\0')&&(*p!='"'); p++);
		if (*p == '"')
			for (p++; *p==' '; p++);
	}
	else
	{
		for (; (*p!='\0') && (*p!=' '); p++);
		if (*p) p++;
	}

	ZeroMemory(plugin, sizeof(plugin));
	if (*p)
		lstrcpyn(plugin, p, sizeof(plugin) - 1);
	else
	{
		GetCurrentDirectory(sizeof(plugin) - 16, plugin);
		lstrcat(plugin, "\\logger.dll");
	}

	DialogBox(hInstance, MAKEINTRESOURCE(IDD_DIALOG), NULL, ProxyDialog);

	Shell_NotifyIcon(NIM_DELETE, &tray);
	WSACleanup();
	return 1;
}


void main(void)
{
	ExitProcess(WinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOWDEFAULT));
}
