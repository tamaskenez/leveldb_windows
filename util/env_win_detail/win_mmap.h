#ifndef INCLUDE_GUARD_mmap_1364388458
#define INCLUDE_GUARD_mmap_1364388458

#include <stdint.h>

#include "common.h"

class MMap
{
public:
	MMap()
		: address_(0)
		, handle_(0)
	{}
	~MMap();

	bool mmap(size_t length, bool bAllowWrite, HANDLE fd, int64_t offset);
	bool munmap();
	bool msync(void *addr, size_t length);

	void moveFrom(MMap& y);

	void* address() const { return address_; }
	bool valid() { return !address_; }

private:
	MMap(const MMap&);
	void operator=(const MMap&);

	void* address_;
	HANDLE handle_;
};

#endif
