// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/locking.h>
#include <io.h>

#include <deque>
#include <set>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "env_win_detail/win_all.h"
#include "util/posix_logger.h"

namespace leveldb {
  bool g_env_win_enable_memory_mapped_files = true;

namespace {

//FILE_SHARED_READ would be sufficient but corruption tests fail if write/delete sharing is not enabled
const DWORD READ_ONLY_FILE_SHARING_ATTRS = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
const DWORD WRITABLE_FILE_SHARING_ATTRS = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}
static Status IOErrorWinApi(const std::string& context) {
  return Status::IOError(context, winapi::GetLastErrorStr().c_str());
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

class WinRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  winapi::File fd_;

 public:
  WinRandomAccessFile(const std::string& fname, winapi::File& fd) //moves out fd
    : filename_(fname) { fd_.moveFrom(fd); }
  virtual ~WinRandomAccessFile() { }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    DWORD bytesRead(0);
    OVERLAPPED ol;
    memset(&ol, 0, sizeof(ol));
    ol.Offset = offset & 0xffffffff;
    ol.OffsetHigh = offset >> 32;
    bool bOk = fd_.readFile(scratch, n, &bytesRead, &ol); //it's not asynchonous since the handle was not opened with FILE_FLAG_OVERLAPPED
    *result = Slice(scratch, !bOk ? 0 : bytesRead);
    if (!bOk) {
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

int fsync_winapi(int fd)
{
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE)
  {
    errno = EBADF;
    return -1;
  }

  if (FlushFileBuffers(h))
    return 0;

  switch (GetLastError())
  {
  case ERROR_ACCESS_DENIED:
    return 0;
  case ERROR_INVALID_HANDLE:
    errno = EINVAL;
    return -1;
  default:
    errno = EIO;
    return -1;
  }
}

class WinWritableFile : public WritableFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  WinWritableFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) { }

  ~WinWritableFile() {
    if (file_ != NULL) {
      // Ignoring any potential errors
      fclose(file_);
    }
  }

  virtual Status Append(const Slice& data) {
    size_t r = _fwrite_nolock(data.data(), 1, data.size(), file_);
    if (r != data.size()) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }

  virtual Status Close() {
    Status result;
    if (fclose(file_) != 0) {
      result = IOError(filename_, errno);
    }
    file_ = NULL;
    return result;
  }

  virtual Status Flush() {
    if (_fflush_nolock(file_) != 0) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }

  virtual Status Sync() {
    //The posix version's SyncDirIfManifest is omitted here
    //since FlushFileBuffers does not work for directories
    Status s;
    if (_fflush_nolock(file_) != 0 ||
        fsync_winapi(fileno(file_)) != 0) {
      s = Status::IOError(filename_, strerror(errno));
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
    char msg[] = "Destroying Env::Default()\n";
    fwrite(msg, 1, sizeof(msg), stderr);
    abort();
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    FILE* f = fopen(fname.c_str(), "rb");
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
    winapi::File fd;
    if (!fd.createFile(fname.c_str(),
              GENERIC_READ, READ_ONLY_FILE_SHARING_ATTRS,
              NULL, OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL)) {
      s = IOErrorWinApi(fname);
    } else if (g_env_win_enable_memory_mapped_files && mmap_limit_.Acquire()) {
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
        MMap base(fd);
        if (base.mmap(size, false, 0)) {
          *result = new WinMmapReadableFile(fname, base, size, &mmap_limit_);
        } else {
        s = IOError(fname, errno);
        }
      }
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
    FILE* f = fopen(fname.c_str(), "wb");
    if (f == NULL) {
      *result = NULL;
      s = IOError(fname, errno);
    } else {
      *result = new WinWritableFile(fname, f);
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
	  std::string dir2(dir);

	  if ( !dir.empty() && dir[dir.size()-1] != '/' && dir[dir.size()-1] != '\\' )
		  dir2 += "/";
	  dir2 += "*";

	  intptr_t d = _findfirst(dir2.c_str(), &entry);

    if (d == -1 )
      return errno == ENOENT ? Status::OK() : IOError(dir, errno);
	  do {
		  if ( strcmp(entry.name, ".") != 0 && strcmp(entry.name, "..") != 0 )
			  result->push_back(entry.name);
	  } while(_findnext(d, &entry) == 0);

	  _findclose(d);

    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (!winapi::DeleteFile(fname.c_str())) {
	    result = IOErrorWinApi(fname);
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
	remove(target.c_str());
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
      const int unbufsize = 100;
      char username[unbufsize];
      DWORD dw = unbufsize;
      GetUserName(username, &dw);
      char buf[200];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%s", username);
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
    FILE* f = fopen(fname.c_str(), "wb");
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
      fprintf(stderr, "windows thread %s: %s\n", label, winapi::GetLastErrorStr().c_str());
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
    : page_size_(getpagesize()),
      started_bgthread_(false),
      bgsignal_(&mu_) {
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
