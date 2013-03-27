#include "win_mmap.h"
#include "win_misc.h"

MMap::
	~MMap()
{
	munmap();
}

bool MMap::
	mmap(size_t length, bool bAllowWrite, HANDLE fd, int64_t offset)
{
	handle_ = CHECK_WINAPI_RESULT(
		CreateFileMapping(fd, NULL, bAllowWrite ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL), "CreateFileMapping");

	if ( handle_ != NULL )
	{
		address_ = CHECK_WINAPI_RESULT(MapViewOfFile(handle_, bAllowWrite ? FILE_MAP_WRITE : FILE_MAP_READ,
			DWORD((uint64_t)offset >> 32), DWORD(offset & 0xffffffff), length), "MapViewOfFile");

		if ( address_ == NULL )
		{
			CloseHandle(handle_);
			handle_ = NULL;
		}
	}

	return address_ != NULL;
}

bool MMap::
	msync(void *addr, size_t length)
{
	return CHECK_WINAPI_RESULT(FlushViewOfFile(addr, length) != 0, "FlushViewOfFile");
}

bool MMap::
	munmap()
{
	if ( handle_ == NULL)
		return true;

	bool b1 = CHECK_WINAPI_RESULT(UnmapViewOfFile(address_) != 0, "UnmapViewOfFile");
	address_ = 0;
	bool b2 = CHECK_WINAPI_RESULT(!handle_ || CloseHandle(handle_) != 0 && b1, "CloseHandle");
	handle_ = NULL;

	return b2;
}

void MMap::
	moveFrom(MMap& y)
{
	munmap();
	address_ = y.address_;
	handle_ = y.handle_;
	y.address_ = 0;
	y.handle_ = 0;
}
