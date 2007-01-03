#ifndef _DEBUG

	#pragma comment(linker,"/ENTRY:DllMain")

	#pragma comment(linker,"/NODEFAULTLIB:libc.lib")
	#pragma comment(linker,"/NODEFAULTLIB:libcmt.lib")
	#pragma comment(lib,"msvcrt.lib")

	#define WIN32_LEAN_AND_MEAN

#endif

#include <windows.h>
#include <commctrl.h>
#include <winsock.h>
#include "resource.h"


#define BUFLEN	4096


#define WM_CAPTURED		WM_USER + 100
#define WM_UPDATELIST	WM_USER + 101


typedef struct _Packet
{
	char hostname[16];
	struct in_addr addr;
	unsigned short port;
	int size;
	char data[BUFLEN];

	int delay;
	DWORD threadid;
	HANDLE thread;

	SOCKET sock;

	struct _Packet *next;
} Packet;


static Packet *head = NULL, *tail = NULL;
static Packet captured;

static HMODULE hMod;
static HWND hMain = NULL;

static HWND hDlg = NULL;
static HWND hList;
static BOOL capture = FALSE;


BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,	// handle to DLL module
	DWORD fdwReason,	// reason for calling function
	LPVOID lpvReserved	// reserved
)
{
	hMod = hinstDLL;
	return TRUE;
}


BOOL ReplicatorDestroy(Packet *packet)
{
	Packet *pkt;

	if (packet->sock)
	{
		shutdown(packet->sock, 2);
		closesocket(packet->sock);
	}

	if (TerminateThread(packet->thread, 0))
	{
		if (packet == head)
			head = packet->next;
		else
			for (pkt = head; pkt != NULL; pkt = pkt->next)
				if (pkt->next == packet)
					pkt->next = packet->next;

		VirtualFree(packet, sizeof(Packet)*2, MEM_DECOMMIT); 
		return TRUE;
	}

	return FALSE;
}


DWORD ReplicatorEnd(DWORD dwExitCode)
{
	DWORD ThreadID = GetCurrentThreadId();
	Packet *packet, *prev;

	for (packet = head, prev = head; packet != NULL; packet = packet->next)
	{
		if (packet->threadid == ThreadID)
		{
			if (packet == head)
				head = packet->next;
			else
				prev->next = packet->next;

			VirtualFree(packet, sizeof(Packet)*2, MEM_DECOMMIT); 
			break;
		}

		prev = packet;
	}

	ExitThread(dwExitCode);
	return dwExitCode;
}


DWORD WINAPI Replicator(Packet *packet)
{
	SOCKADDR_IN sin;
	int nRead, nSent, nTmp;
	char buf[BUFLEN];

	while (1)
	{
		Sleep(packet->delay);

		if ((packet->sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
			break;

		sin.sin_family		= AF_INET;
		sin.sin_addr.s_addr	= packet->addr.s_addr;
		sin.sin_port		= packet->port;

		if (connect(packet->sock, (struct sockaddr FAR *) &sin, sizeof(sin)) == SOCKET_ERROR)
			break;

		nSent = 0;
		while (nSent < packet->size)
		{
			nTmp = send(packet->sock, packet->data + nSent, packet->size - nSent, 0);
			if (nTmp > 0)
				nSent += nTmp;
			else if (nTmp == SOCKET_ERROR)
				break;
			else
				break;
		}

		while (1)
		{
			ZeroMemory(buf, sizeof(buf));
			nRead = recv(packet->sock, buf, sizeof(buf) - 1, 0);
			if (nRead == SOCKET_ERROR)
				break;
			else if (nRead != 0)
				continue;
			else
				break;
		}

		shutdown(packet->sock, 2);
		closesocket(packet->sock);

		packet->sock = 0;
	}

	MessageBox(NULL, "Replicator terminated unexpectingly!", NULL, MB_OK | MB_ICONSTOP | MB_SYSTEMMODAL);
	PostMessage(hDlg, WM_UPDATELIST, 0, 0);
	return ReplicatorEnd(0);
}


Packet *PacketAdd(Packet *packet_data)
{
	Packet *packet;
	HANDLE hThread;
	DWORD ThreadID;

	packet = (Packet *) VirtualAlloc(NULL, sizeof(Packet), MEM_COMMIT, PAGE_READWRITE);
	if (packet == NULL)
		return NULL;

	memcpy(packet, packet_data, sizeof(Packet));

	hThread = CreateThread(NULL, 0, Replicator, packet, CREATE_SUSPENDED, &ThreadID);
	if (hThread == NULL)
	{
		VirtualFree(packet, sizeof(Packet)*2, MEM_DECOMMIT); 
		return NULL;
	}

	packet->thread		= hThread;
	packet->threadid	= ThreadID;
	packet->next		= NULL;

	if (head == NULL)
	{
		head = packet;
		tail = packet;
	}
	else
	{
		tail->next = packet;
		tail = packet;
	}

	ResumeThread(hThread);

	return packet;
}


HWND CreateListView(HWND hWnd)
{
	HWND hWndList;
	LV_COLUMN lvC;

	InitCommonControls();
	hWndList = CreateWindowEx(0L,
		WC_LISTVIEW,
		"",
		WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT |
		    LVS_EDITLABELS | WS_EX_CLIENTEDGE,
		11, 11,
		494, 189,
		hWnd,
		NULL,
		hMod,
		NULL
	);

	if (hWndList == NULL )
		return NULL;

	ZeroMemory(&lvC, sizeof(lvC));

	lvC.mask		= LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	lvC.fmt			= LVCFMT_LEFT;

	lvC.cx			= 120;
	lvC.pszText		= "Remote Hostname";
	if (ListView_InsertColumn(hWndList, 0, &lvC) == -1)
		return NULL;

	lvC.cx			= 270;
	lvC.pszText		= "HTTP Request String";
	if (ListView_InsertColumn(hWndList, 1, &lvC) == -1)
		return NULL;

	lvC.cx			= 80;
	lvC.pszText		= "Period (sec)";
	if (ListView_InsertColumn(hWndList, 2, &lvC) == -1)
		return NULL;

	return hWndList;
}


BOOL ItemAdd(HWND hWndList, Packet *packet)
{
	LV_ITEM lvI;
	char buf[128];
	char *p, *q;
	int i;

	ZeroMemory(&lvI, sizeof(lvI));

	lvI.mask		= LVIF_TEXT | LVIF_PARAM;
	lvI.iItem		= 0; //ListView_GetItemCount(hWndList);
	lvI.iSubItem	= 0;
	lvI.pszText		= LPSTR_TEXTCALLBACK;
	lvI.cchTextMax	= 128;
	lvI.iImage		= 0;
	lvI.lParam		= (LPARAM) packet;

	if (ListView_InsertItem(hWndList, &lvI) == -1)
		return FALSE;

	ZeroMemory(buf, sizeof(buf));
	wsprintf(buf, "%s:%d", packet->hostname, ntohs(packet->port));
	ListView_SetItemText(hWndList, 0, 0, buf);

	ZeroMemory(buf, sizeof(buf));
	p = packet->data;
	q = buf;
	for (i = 0; (i < 126) && !((*p==0xd) || (*p==0xa)); p++, q++, i++)
		*q = *p;
	ListView_SetItemText(hWndList, 0, 1, buf);

	ZeroMemory(buf, sizeof(buf));
	wsprintf(buf, "%d", packet->delay / 1000);
	ListView_SetItemText(hWndList, 0, 2, buf);

	return TRUE;
}


VOID UpdateList(HWND hList)
{
	Packet *packet;
	for (packet = head; packet != NULL; packet = packet->next)
		ItemAdd(hList, packet);
	return;
}


#define STATE(x) \
	EnableWindow(GetDlgItem(hWnd, IDC_CAPTURE), x); \
	EnableWindow(GetDlgItem(hWnd, IDC_DELAY), x); \
	EnableWindow(GetDlgItem(hWnd, IDC_DELAYSET), x); \
	EnableWindow(GetDlgItem(hWnd, IDC_DESTROY), x);

BOOL CALLBACK ReplicatorDialog(
	HWND hWnd,		// handle to dialog box
	UINT uMsg,		// message
	WPARAM wParam,	// first message parameter
	LPARAM lParam	// second message parameter
)
{
	int index;
	LV_ITEM lvI;
	Packet *packet;
	int delay;
	char buf[128];

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			hDlg = hWnd;
			SetDlgItemInt(hWnd, IDC_DELAY, 300, FALSE);
			hList = CreateListView(hWnd);

			UpdateList(hList);

			SetFocus(GetDlgItem(hWnd, IDC_CAPTURE));

			break;
		}
		case WM_CAPTURED:
		{
			if (MessageBox(hWnd, "Add captured packet to resend queue?", "Packet captured!", MB_YESNO | MB_ICONQUESTION | MB_SYSTEMMODAL) == IDYES)
			{
				delay = GetDlgItemInt(hWnd, IDC_DELAY, NULL, FALSE);
				captured.delay = delay * 1000;
				if (packet = PacketAdd(&captured))
					ItemAdd(hList, packet);
				else
					MessageBox(hWnd, "Can't add packet to queue!", NULL, MB_OK | MB_ICONSTOP);
			}

			STATE(TRUE);
			break;
		}
		case WM_UPDATELIST:
		{
			if (!hList)
				break;

			ListView_DeleteAllItems(hList);
			UpdateList(hList);

			break;
		}
		case WM_SYSCOMMAND:
		{
			switch((DWORD) wParam)
			{ 
				case SC_CLOSE:
				{
					if (capture)
						break;

					DestroyWindow(hList);
					hDlg	= NULL;
					hList	= NULL;
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
				case IDC_CAPTURE:
				{
					capture = TRUE;
					STATE(FALSE);
					break;
				}
				case IDC_DELAYSET:
				{
					if ((index = ListView_GetSelectionMark(hList)) == -1)
					{
						MessageBox(hWnd, "Select item to set period!", NULL, MB_OK | MB_ICONEXCLAMATION);
						break;
					}

					ZeroMemory(&lvI, sizeof(lvI));
					lvI.mask	= LVIF_PARAM;
					lvI.iItem	= index;
					if (!ListView_GetItem(hList, &lvI) || !lvI.lParam)
					{
						MessageBox(hWnd, "Unable to locate item", NULL, MB_OK | MB_ICONSTOP);
						break;
					}

					packet = (Packet *) lvI.lParam;

					delay = GetDlgItemInt(hWnd, IDC_DELAY, NULL, FALSE);

					ZeroMemory(buf, sizeof(buf));
					wsprintf(buf, "%d", delay);
					ListView_SetItemText(hList, 0, 2, buf);

					packet->delay = delay * 1000;

					break;
				}
				case IDC_DESTROY:
				{
					if ((index = ListView_GetSelectionMark(hList)) == -1)
					{
						MessageBox(hWnd, "Select item to be destroyed!", NULL, MB_OK | MB_ICONEXCLAMATION);
						break;
					}

					ZeroMemory(&lvI, sizeof(lvI));
					lvI.mask	= LVIF_PARAM;
					lvI.iItem	= index;
					if (!ListView_GetItem(hList, &lvI) || !lvI.lParam)
					{
						MessageBox(hWnd, "Unable to locate item", NULL, MB_OK | MB_ICONSTOP);
						break;
					}

					packet = (Packet *) lvI.lParam;

					if (!ReplicatorDestroy(packet))
					{
						MessageBox(hWnd, "Error destroying item!", NULL, MB_OK | MB_ICONSTOP);
						break;
					}

					ListView_DeleteItem(hList, index);

					break;
				}
			}
		}
	}

	return FALSE;
}


__declspec(dllexport) BOOL FilterInit (HWND hWnd)
{
	WSADATA wsaData;

	hMain = hWnd;

	if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
		return FALSE;

	return TRUE;
}


__declspec(dllexport) BOOL FilterSetup (HWND hWnd)
{
	DialogBox(hMod, MAKEINTRESOURCE(IDD_DIALOG), hWnd, ReplicatorDialog);
	return TRUE;
}


__declspec(dllexport) BOOL FilterProcess (SOCKET from, SOCKET to, char *buf, int count, int size, BOOL is_request)
{
	SOCKADDR_IN sin;
	int sinlen = sizeof(sin);

	if (count || !capture || !is_request || size <= 0)
		return TRUE;

	if (getpeername(to, (struct sockaddr FAR *) &sin, &sinlen) == SOCKET_ERROR)
		return TRUE;

	capture = FALSE;

	ZeroMemory(&captured, sizeof(captured));

	lstrcpy(captured.hostname, inet_ntoa(sin.sin_addr));
	captured.addr = sin.sin_addr;
	captured.port = sin.sin_port;
	captured.size = size < BUFLEN ? size : BUFLEN - 1;
	memcpy(captured.data, buf, captured.size);

	PostMessage(hDlg, WM_CAPTURED, 0, 0);

	return TRUE;
}


__declspec(dllexport) BOOL FilterEnd (VOID)
{
	Packet *packet;

	for (packet = head; packet != NULL; packet = packet->next)
		ReplicatorDestroy(packet);

	WSACleanup();
	hMain = NULL;

	return TRUE;
}
