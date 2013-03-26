// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <deque>
#include <set>

#include <direct.h>
#include <io.h>
#include <sys/locking.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"

#include "env_win_detail/win_all.h"

#include "util/posix_logger.h"


namespace leveldb {

namespace {

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}
static Status IOErrorWinApi(const std::string& context) {
  return Status::IOError(context, GetLastWinApiErrorStr().c_str());
}

class WinSequentialFile: public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  WinSequentialFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) { }
  virtual ~WinSequentialFile() { fclose(file_); }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    Status s;
    size_t r = fread_unlocked(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (feof(file_)) {
        // We leave status as ok if we hit the end of the file
      } else {
        // A partial read with an error: return a non-ok status
        s = IOError(filename_, errno);
      }
    }
    return s;
  }

  virtual Status Skip(uint64_t n) {
    if (fseek(file_, n, SEEK_CUR)) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }
};

// pread() based random-access
class WinRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  HANDLE fd_;

 public:
  WinRandomAccessFile(const std::string& fname, HANDLE fd)
      : filename_(fname), fd_(fd) { }
  virtual ~WinRandomAccessFile() { CHECK_WINAPI_RESULT(CloseHandle(fd_) !=0, "CloseHandle"); }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
	//fd_ is private so other methods will access this file
	LARGE_INTEGER li;
	li.QuadPart = offset;
	bool bOk = CHECK_WINAPI_RESULT(SetFilePointerEx(fd_, li, NULL, FILE_BEGIN) != 0, "SetFilePointerEx");
	DWORD bytesRead;
	if ( bOk )
		bOk = CHECK_WINAPI_RESULT(ReadFile(fd_, scratch, n, &bytesRead, NULL) != 0, "ReadFile");
    *result = Slice(scratch, !bOk ? 0 : bytesRead);
    if (!bOk) {
      // An error: return a non-ok status
      s = IOErrorWinApi(filename_);
    }
    return s;
  }
};

// Helper class to limit mmap file usage so that we do not end up
// running out virtual memory or running into kernel performance
// problems for very large databases.
class MmapLimiter {
 public:
  // Up to 1000 mmaps for 64-bit binaries; none for smaller pointer sizes.
  MmapLimiter() {
    SetAllowed(sizeof(void*) >= 8 ? 1000 : 0);
  }

  // If another mmap slot is available, acquire it and return true.
  // Else return false.
  bool Acquire() {
    if (GetAllowed() <= 0) {
      return false;
    }
    MutexLock l(&mu_);
    intptr_t x = GetAllowed();
    if (x <= 0) {
      return false;
    } else {
      SetAllowed(x - 1);
      return true;
    }
  }

  // Release a slot acquired by a previous call to Acquire() that returned true.
  void Release() {
    MutexLock l(&mu_);
    SetAllowed(GetAllowed() + 1);
  }

 private:
  port::Mutex mu_;
  port::AtomicPointer allowed_;

  intptr_t GetAllowed() const {
    return reinterpret_cast<intptr_t>(allowed_.Acquire_Load());
  }

  // REQUIRES: mu_ must be held
  void SetAllowed(intptr_t v) {
    allowed_.Release_Store(reinterpret_cast<void*>(v));
  }

  MmapLimiter(const MmapLimiter&);
  void operator=(const MmapLimiter&);
};

// mmap() based random-access
class WinMmapReadableFile: public RandomAccessFile {
 private:
  std::string filename_;
  size_t length_;
  MmapLimiter* limiter_;
  MMap mmapresult_;

 public:
  // base[0,length-1] contains the mmapped contents of the file.
  WinMmapReadableFile(const std::string& fname, MMap& mmr, size_t length,
                        MmapLimiter* limiter)
      : filename_(fname), length_(length),
        limiter_(limiter) {
			mmapresult_.moveFrom(mmr);
  }

  virtual ~WinMmapReadableFile() {
    limiter_->Release();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    if (offset + n > length_) {
      *result = Slice();
      s = IOError(filename_, EINVAL);
    } else {
      *result = Slice(reinterpret_cast<char*>(mmapresult_.address()) + offset, n);
    }
    return s;
  }
};

// We preallocate up to an extra megabyte and use memcpy to append new
// data to the file.  This is safe since we either properly close the
// file before reading from it, or for log files, the reading code
// knows enough to skip zero suffixes.
class WinMmapFile : public WritableFile {
 private:
  std::string filename_;
  HANDLE fd_;
  size_t page_size_;
  size_t map_size_;       // How much extra memory to map at a time
  MMap base_;            // The mapped region
  char* limit_;           // Limit of the mapped region
  char* dst_;             // Where to write next  (in range [base_,limit_])
  char* last_sync_;       // Where have we synced up to
  uint64_t file_offset_;  // Offset of base_ in file

  // Have we done an munmap of unsynced data?
  bool pending_sync_;

  // Roundup x to a multiple of y
  static size_t Roundup(size_t x, size_t y) {
    return ((x + y - 1) / y) * y;
  }

  size_t TruncateToPageBoundary(size_t s) {
    s -= (s & (page_size_ - 1));
    assert((s % page_size_) == 0);
    return s;
  }

  bool UnmapCurrentRegion() {
    bool result = true;
	if (base_.valid()) {
      if (last_sync_ < limit_) {
        // Defer syncing this data until next Sync() call, if any
        pending_sync_ = true;
      }
	  file_offset_ += limit_ - (char*)base_.address();
      if (!CHECK_WINAPI_RESULT(base_.munmap(), "munmap")) {
        result = false;
      }
      limit_ = NULL;
      last_sync_ = NULL;
      dst_ = NULL;

      // Increase the amount we map the next time, but capped at 1MB
      if (map_size_ < (1<<20)) {
        map_size_ *= 2;
      }
    }
    return result;
  }

  bool MapNewRegion() {
    assert(!base_.valid());
    if ( !CHECK_WINAPI_RESULT(ftruncate(fd_, file_offset_ + map_size_), "ftruncate")) {
      return false;
    }
	if (!CHECK_WINAPI_RESULT(base_.mmap(map_size_, true, fd_, file_offset_), "mmap")) {
      return false;
    }
	limit_ = (char*)base_.address() + map_size_;
	dst_ = (char*)base_.address();
	last_sync_ = (char*)base_.address();
    return true;
  }

 public:
  WinMmapFile(const std::string& fname, HANDLE fd, size_t page_size)
      : filename_(fname),
        fd_(fd),
        page_size_(page_size),
        map_size_(Roundup(65536, page_size)),
        limit_(NULL),
        dst_(NULL),
        last_sync_(NULL),
        file_offset_(0),
        pending_sync_(false) {
    assert((page_size & (page_size - 1)) == 0);
  }


  ~WinMmapFile() {
    if (fd_ >= 0) {
      WinMmapFile::Close();
    }
  }

  virtual Status Append(const Slice& data) {
    const char* src = data.data();
    size_t left = data.size();
    while (left > 0) {
	  assert(base_.address() <= dst_);
      assert(dst_ <= limit_);
      size_t avail = limit_ - dst_;
      if (avail == 0) {
        if (!UnmapCurrentRegion() ||
            !MapNewRegion()) {
          return IOErrorWinApi(filename_);
        }
      }

      size_t n = (left <= avail) ? left : avail;
      memcpy(dst_, src, n);
      dst_ += n;
      src += n;
      left -= n;
    }
    return Status::OK();
  }

  virtual Status Close() {
    Status s;
    size_t unused = limit_ - dst_;
    if (!UnmapCurrentRegion()) {
      s = IOErrorWinApi(filename_);
    } else if (unused > 0) {
      // Trim the extra space at the end of the file
      if (!CHECK_WINAPI_RESULT(ftruncate(fd_, file_offset_ - unused), "ftruncate")) {
        s = IOErrorWinApi(filename_);
      }
    }

    if (!CHECK_WINAPI_RESULT(CloseHandle(fd_) != 0, "close")) {
      if (s.ok()) {
        s = IOErrorWinApi(filename_);
      }
    }

    fd_ = NULL;
    limit_ = NULL;
    return s;
  }

  virtual Status Flush() {
    return Status::OK();
  }

  virtual Status Sync() {
    Status s;

    if (pending_sync_) {
      // Some unmapped data was not synced
      pending_sync_ = false;
      if (CHECK_WINAPI_RESULT(FlushFileBuffers(fd_) != 0, "FlushFileBuffers")) {
        s = IOErrorWinApi(filename_);
      }
    }

    if (dst_ > last_sync_) {
      // Find the beginnings of the pages that contain the first and last
      // bytes to be synced.
	  size_t p1 = TruncateToPageBoundary(last_sync_ - (char*)base_.address());
      size_t p2 = TruncateToPageBoundary(dst_ - (char*)base_.address() - 1);
      last_sync_ = dst_;
	  if (CHECK_WINAPI_RESULT(base_.msync((char*)base_.address() + p1, p2 - p1 + page_size_) != 0, "msync")) {
        s = IOErrorWinApi(filename_);
      }
    }

    return s;
  }
};

static int LockOrUnlock1024(int fd, bool lock) {
  int r = _lseek(fd, 0, SEEK_SET);
  if ( r != 0 )
	  return r;
  return _locking(fd, lock ? _LK_NBLCK : _LK_UNLCK, 1024);
}

class WinFileLock : public FileLock {
 public:
  int fd_;
  std::string name_;
};

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
class WinLockTable {
 private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;
 public:
  bool Insert(const std::string& fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

class WinEnv : public Env {
 public:
  WinEnv();
  virtual ~WinEnv() {
    fprintf(stderr, "Destroying Env::Default()\n");
    exit(1);
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    FILE* f = fopen(fname.c_str(), "r");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new WinSequentialFile(fname, f);
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) {
    *result = NULL;
    Status s;
    HANDLE fd = CHECK_WINAPI_RESULT(
		CreateFile(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL), "CreateFile");
    if (fd == NULL) {
      s = IOErrorWinApi(fname);
    } else if (mmap_limit_.Acquire()) {
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
		MMap base;
        base.mmap(size, false, fd, 0);
		if (base.valid()) {
          *result = new WinMmapReadableFile(fname, base, size, &mmap_limit_);
        } else {
          s = IOError(fname, errno);
        }
      }
      CHECK_WINAPI_RESULT(CloseHandle(fd) != 0, "CloseHandle");
      if (!s.ok()) {
        mmap_limit_.Release();
      }
    } else {
      *result = new WinRandomAccessFile(fname, fd);
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    Status s;
    HANDLE fd = CHECK_WINAPI_RESULT(
		CreateFile(fname.c_str(),
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL), "CreateFile");
    if (fd == NULL) {
      *result = NULL;
      s = IOErrorWinApi(fname);
    } else {
      *result = new WinMmapFile(fname, fd, page_size_);
    }
    return s;
  }

  virtual bool FileExists(const std::string& fname) {
    return _access(fname.c_str(), 0) == 0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    struct _finddata_t entry;
	intptr_t d = _findfirst(dir.c_str(), &entry);
    if (d == -1 ) {
      return IOError(dir, errno);
    }
	do {
		result->push_back(entry.name);
	} while(_findnext(d, &entry) == 0);

	_findclose(d);

    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = IOError(fname, errno);
    }
    return result;
  };

  virtual Status CreateDir(const std::string& name) {
    Status result;
    if (_mkdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status DeleteDir(const std::string& name) {
    Status result;
    if (_rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    Status s;
    struct __stat64 sbuf;
    if (_stat64(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = NULL;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock1024(fd, true) == -1) {
      result = IOError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      WinFileLock* my_lock = new WinFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    WinFileLock* my_lock = reinterpret_cast<WinFileLock*>(lock);
    Status result;
    if (LockOrUnlock1024(my_lock->fd_, false) == -1) {
      result = IOError("unlock", errno);
    }
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(time(0)));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    winthread_t tid = winthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
    FILE* f = fopen(fname.c_str(), "w");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixLogger(f, &WinEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

 private:
  void WinThreadCall(const char* label, bool bOk) {
    if (!bOk) {
      fprintf(stderr, "windows thread %s: %s\n", label, GetLastWinApiErrorStr().c_str());
      exit(1);
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<WinEnv*>(arg)->BGThread();
    return NULL;
  }

  size_t page_size_;
  leveldb::port::Mutex mu_;
  leveldb::port::CondVar bgsignal_;
  winthread_t bgthread_;
  bool started_bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  WinLockTable locks_;
  MmapLimiter mmap_limit_;
};

WinEnv::WinEnv()
	: page_size_(getpagesize())
    , started_bgthread_(false)
	, bgsignal_(&mu_)
{
}

void WinEnv::Schedule(void (*function)(void*), void* arg) {
  mu_.Lock();

  // Start background thread if necessary
  if (!started_bgthread_) {
    started_bgthread_ = true;
	WinThreadCall("create thread",
        winthread_create(&bgthread_, &WinEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  if (queue_.empty()) {
    bgsignal_.Signal();
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  mu_.Unlock();
}

void WinEnv::BGThread() {
  while (true) {
    // Wait until there is an item that is ready to run
	mu_.Lock();
    while (queue_.empty()) {
      bgsignal_.Wait();
    }

    void (*function)(void*) = queue_.front().function;
    void* arg = queue_.front().arg;
    queue_.pop_front();

	mu_.Unlock();
    (*function)(arg);
  }
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}
static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return NULL;
}

void WinEnv::StartThread(void (*function)(void* arg), void* arg) {
  winthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  WinThreadCall("start thread",
              winthread_create(&t, &StartThreadWrapper, state));
}

}  // namespace

static leveldb::port::OnceType once = LEVELDB_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new WinEnv; }

Env* Env::Default() {
  leveldb::port::InitOnce(&once, InitDefaultEnv);
  return default_env;
}

}  // namespace leveldb
