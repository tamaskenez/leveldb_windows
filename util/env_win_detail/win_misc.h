#ifndef INCLUDE_GUARD_misc_1364388690
#define INCLUDE_GUARD_misc_1364388690

#include <string>

#include "common.h"

#define fread_unlocked _fread_nolock


bool ftruncate(HANDLE fd, ptrdiff_t length);

void usleep(int usec);
int getpagesize(void);

bool CHECK_WINAPI_RESULT(bool b, const char* function);
HANDLE CHECK_WINAPI_RESULT(HANDLE h, const char* function);

std::string GetLastWinApiErrorStr();

#endif

