#include "win_misc.h"

const bool bLogErrors = true;


void usleep(int usec)
{
	Sleep((usec + 999)/1000);
}



int getpagesize(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwPageSize;
}

bool ftruncate(HANDLE fd, ptrdiff_t length)
{
	LARGE_INTEGER li, nli;
	li.QuadPart = length;
	bool b = CHECK_WINAPI_RESULT(SetFilePointerEx(fd, li, &nli, FILE_BEGIN) != 0, "SetFilePointerEx");
	if ( !b ) return false;
	return CHECK_WINAPI_RESULT(SetEndOfFile(fd) != 0, "SetEndOfFile");
}

bool CHECK_WINAPI_RESULT(bool b, const char* function)
{
	if ( !b && bLogErrors)
		fprintf(stderr, "Function %s returned winapi error: %s", function, GetLastWinApiErrorStr().c_str());

	return b;
}

HANDLE CHECK_WINAPI_RESULT(HANDLE h, const char* function)
{
	if ( h == NULL && bLogErrors)
		fprintf(stderr, "Function %s returned winapi error: %s", function, GetLastWinApiErrorStr().c_str());

	return h;
}

std::string GetLastWinApiErrorStr()
{
	DWORD le = GetLastError();
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		le,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	std::string errorMsg((char*)lpMsgBuf);

	LocalFree(lpMsgBuf);

	return errorMsg;
}

