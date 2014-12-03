#include "port_win.h"

#include <Windows.h>

namespace leveldb {
namespace port {

Mutex::Mutex()
#ifndef NDEBUG
    : holding_thread_id_(0)
#endif
{
  static_assert(sizeof(CRITICAL_SECTION) <= kSizeOfCriticalSection, "update Mutex::kSizeOfCriticalSection");
  InitializeCriticalSection(_Out_ (LPCRITICAL_SECTION)cs_);
}

Mutex::~Mutex() {
  DeleteCriticalSection(_Inout_ (LPCRITICAL_SECTION)cs_);
}

void Mutex::Lock() {
  EnterCriticalSection(_Inout_ (LPCRITICAL_SECTION)cs_);
#ifndef NDEBUG
  holding_thread_id_ = GetCurrentThreadId();
#endif
}


void Mutex::Unlock() {
#ifndef NDEBUG
  if ( holding_thread_id_ != 0 )
    AssertHeld();
  holding_thread_id_ = 0;
#endif
  LeaveCriticalSection(_Inout_ (LPCRITICAL_SECTION)cs_);
}

#ifndef NDEBUG
void Mutex::AssertHeld() {
  if ( holding_thread_id_ != GetCurrentThreadId() ) {
    fprintf(stderr, "Mutex::AssertHeld failed, current thread: %d, holding thread: %d\n", GetCurrentThreadId(), holding_thread_id_);
    abort();
  }
}
#endif

CondVar::CondVar(Mutex* mu)
    : mu_(mu) {
  static_assert(sizeof(CONDITION_VARIABLE) >= kSizeOfConditionVariable, "update Mutex::kSizeOfConditionVariable");
  InitializeConditionVariable((PCONDITION_VARIABLE)cv_);
}

CondVar::~CondVar() {
}

void CondVar::Wait() {
  mu_->AssertHeld();
  BOOL b = SleepConditionVariableCS((PCONDITION_VARIABLE)cv_, (PCRITICAL_SECTION)(mu_->cs_), INFINITE);
  if ( !b )
    fprintf(stderr, "CondVar::Wait, SleepConditionVariableCS returned failure");
#ifndef NDEBUG
  mu_->holding_thread_id_ = GetCurrentThreadId();
#endif
}

void CondVar::Signal() {
  WakeConditionVariable((PCONDITION_VARIABLE)cv_);
}

void CondVar::SignalAll() {
  WakeAllConditionVariable((PCONDITION_VARIABLE)cv_);
}

  // Read and return the stored pointer with the guarantee that no
  // later memory access (read or write) by this thread can be
  // reordered ahead of this read.
void* AtomicPointer::Acquire_Load() const {
  //Perform an exchange operation on the condition if the value is 0
  //If it's zero, exchange with zero, return original value (0)
  //If it's nonzero, don't exchange, return original value.
  //So the exchange operation won't change the value, this is a const read operation
  AtomicPointer* mutable_this = const_cast<AtomicPointer*>(this);

  return InterlockedCompareExchangePointerAcquire(&mutable_this->rep_, 0, 0);
}

void AtomicPointer::Release_Store(void* v) {
    PVOID old;
    do {
        old = rep_;
    } while (InterlockedCompareExchangePointerRelease(&rep_, v, old) != old);
}

BOOL CALLBACK InitOnceCallback(
  _Inout_      PINIT_ONCE InitOnce,
  _Inout_opt_  PVOID Parameter,
  _Out_opt_    PVOID *Context
) {
  typedef void (*initializer_fn)();
  ((initializer_fn)Parameter)();
  return TRUE;
}

void InitOnce(port::OnceType* once, void (*initializer)()) {
  INIT_ONCE* real_once = (INIT_ONCE*)once;
  static_assert(sizeof(*real_once) == sizeof(*once), "port::OnceType must be the same as INIT_ONCE");
  BOOL b = InitOnceExecuteOnce(
    real_once,
    InitOnceCallback,
    initializer,
    0);
}

}  // namespace port
}  // namespace leveldb

