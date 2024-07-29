#include "mutex.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <utility>

#include <sstream>

#include <gtest/gtest.h>

#include "futex.h"
#include "test_util.h"

using namespace shmsync;

const char* process_name = "parent";
static volatile const void* state;
static size_t state_size;
static volatile const void* state2;
static size_t state2_size;

constexpr static int ttl = 10;

inline pid_t Fork(const char* child_name) {
  const pid_t pid = fork();
  if (pid == 0) {
    process_name = child_name;
    alarm(ttl);
  }
  return pid;
}

template<class MutexClass> void XLock(MutexClass* m) { m->Lock(); }
template<class MutexClass> void XUnlock(MutexClass* m) { m->Unlock(); }

struct SharedMutexWrapper: public SharedMutex {
  void Lock() { LockExclusive(); }
  void Unlock() { UnlockExclusive(); }
};

// This class implements some basic level of pretty printing that can be used
// from signal handlers, where very few functions can be called safely.
class PrettyPrinterForSignalHandler {
  public:
    PrettyPrinterForSignalHandler& operator<<(const char* s) {
      while ((*end = *s++)) ++end;
      return *this;
    }

    PrettyPrinterForSignalHandler& operator<<(long k) {
      static const char digits[] = "0123456789";
      if (k < 0) { *end++ = '-'; k = -k; }
      const long k10 = k / 10;
      if (k10 != 0) *this =  *this <<k10;
      *end++ = digits[k % 10];
      return *this;
    }

    PrettyPrinterForSignalHandler& operator<<(std::pair<const volatile void*, size_t> x) {
      static const char h[] = "0123456789abcdef";
      for (size_t i = 0; i < x.second; ++i) {
        if (i != 0) {
          *end++ = ' ';
          if (i % 4 == 0) *end++ = ' ';
        }
        const uint8_t b = ((uint8_t*) x.first)[i];
        *end++ = h[b / 16];
        *end++ = h[b % 16];
      }
      return *this;
    }

    bool Print() {
      const ssize_t l = end - buf;
      return write(STDERR_FILENO, buf, l) == l;
    }

  private:
    char buf[1000];
    char* end = buf;
};

static void HandleSignal(int signum) {
  PrettyPrinterForSignalHandler p;
  p << "SIGNAL " << signum << " in process " << process_name << " pid: " << getpid() << " state: " << std::make_pair(state, state_size);
  if (state2 != NULL) p << " state2: " << std::make_pair(state2, state2_size);
  p << "\n";
  _exit(p.Print() ? 88 : 89);
}

static void InstallSignalHandler(int signum, volatile const void* state_, size_t state_size_, volatile const void* state2_ = NULL, size_t state2_size_ = 0) {
  state = state_;
  state_size = state_size_;
  state2 = state2_;
  state2_size = state2_size_;
  signal(signum, HandleSignal);
}

#define FATAL(x) return (std::stringstream() << "in " << __FUNCTION__ << " at line " << __LINE__ << ": " << x).str();

#define MUST(err) if (!err.empty()) FATAL(err);

template<class MutexClass, class RAIILockClass> std::string TestExclusiveLocksFrom2ConcurrentProcesses(uint8_t* shm) {
  if (shm == NULL) FATAL("shm is NULL");
  MutexClass* const mutex = new(shm) MutexClass;
  InstallSignalHandler(SIGALRM, mutex, sizeof(*mutex));
  alarm(ttl);

  for (int k = 0; k < 10; ++k) {
    const pid_t pid = Fork("child");
    if (pid) {  // parent
      if (k % 3 == 0) usleep(1000);
      if (pid <= 0) FATAL("pid " << pid << " " << strerror(errno));
      MUST(WaitForChildren(1));
      RAIILockClass lock(mutex);
    } else {  // child
      if (k % 2 == 0) usleep(1000);
      do { RAIILockClass lock(mutex); } while (false);
      _exit(0);
    }
  }

  volatile int* const i = (int*) (mutex + 1);
  for (int k = 0; k < 5; ++k) {
    *i = 0;
    mutex->Lock();
    const pid_t pid = Fork("child");
    if (pid) {  // parent
      if (pid <= 0) FATAL("pid " << pid << " " << strerror(errno));
      if (*i) FATAL("*i = " << *i);
      *i = 1;
      mutex->Unlock();
      MUST(WaitForChildren(1));
      const int value = *i;
      if (value != 2) FATAL("value = " << value);
    } else {  // child
      mutex->Lock();
      const int value = *i;
      if (value != 1) FATAL("value = " << value);
      *i = 2;
      mutex->Unlock();
      _exit(!(value == 1));
    }
  }

  for (int k = 0; k < 5; ++k) {
    *i = 0;
    mutex->Lock();
    const pid_t pid = Fork("child");
    if (pid) {  // parent
      if (pid <= 0) FATAL("pid " << pid << " " << strerror(errno));
      RAIILockClass lock(mutex);
      const int value = *i;
      if (value != 1) FATAL("*i = " << value);
      MUST(WaitForChildren(1));
    } else {  // child
      const int value = *i;
      if (value != 0) FATAL("value = " << value);
      *i = 1;
      mutex->Unlock();
      _exit(!(value == 0));
    }
  }

  return "";
}

template<class MutexClass, class RAIILockClass> std::string TestExclusiveLocksFromManyConcurrentProcesses(uint8_t* shm) {
  if (shm == NULL) FATAL("shm is NULL");
  volatile long* const i = (long*) shm;
  MutexClass* const mutex = new((void*) (i + 1)) MutexClass;
  InstallSignalHandler(SIGALRM, mutex, sizeof(*mutex));
  alarm(ttl);

  // increments_per_child must be at least 10k to detect bugs most of the time
  constexpr long increments_per_child = 40000;
  const long nchildren = 50;
  for (int k = 0; k < nchildren; ++k) {
    const pid_t pid = Fork("child");
    if (!pid) { // child
      long previous_i = 0;
      for (long k2 = 0; k2 < increments_per_child; ++k2) {
        RAIILockClass lock(mutex);
        const long new_i = 1 + *i;
        *i = new_i;
        if (new_i > previous_i) {
          previous_i = new_i;
          continue;
        }
        _exit(1);
      }
      _exit(0);
    }
    if (pid <= 0) FATAL("pid " << pid << " " << strerror(errno));
  }
  MUST(WaitForChildren(nchildren));
  if (nchildren * increments_per_child != *i) FATAL("nchildren * increments_per_child: " << nchildren * increments_per_child << " *i: " << *i);
  return "";
}

struct SharedLocksTestData {
  SharedLocksTestData(uint32_t nshared_, uint32_t nexcl_): nshared(nshared_), nexcl(nexcl_) { }

  SharedMutex sm;  // keep this first for the signal handler print to work well
  uint32_t nshared;
  uint32_t ishared = 0;
  uint32_t nexcl;
  uint32_t iexcl = 0;
  uint32_t step = 0;
  uint32_t shared_done = 0;
  uint32_t excl_done = 0;
};

#if 0
#include <iostream>
#define DEBUG(x) do { std::stringstream ss_; ss_ << x << std::endl; std::cerr << ss_.str(); } while (0)
#else
#define DEBUG(x)
#endif
std::string TestSharedLocksFromManyConcurrentProcesses(uint8_t* shm, long nexcl,long nshared, long increments_per_shared) {
  if (shm == NULL) FATAL("shm is NULL");
  SharedLocksTestData* const x = new(shm) SharedLocksTestData(nshared, nexcl);

  InstallSignalHandler(SIGALRM, &x->sm, sizeof(x->sm), sizeof(x->sm) + (char*) &x->sm, sizeof(*x) - sizeof(x->sm));

  if (nshared <= 0) __atomic_store_n(&x->shared_done, 1, __ATOMIC_SEQ_CST);
  for (long shared = 0; shared < nshared; ++shared) {
    const pid_t pid = Fork("child-shared");
    if (pid < 0) FATAL("pid " << pid << " " << strerror(errno));
    if (!pid) { // child
      for (long step = 0; step < increments_per_shared; ++step) {
        while (__atomic_load_n(&x->step, __ATOMIC_SEQ_CST) <= step) {
          if (!FutexSyscall(&x->step, FUTEX_WAIT, step)) _exit(61);
        }
        DEBUG("shared end of futex wait &x->step " << step);
        SharedLock lock(&x->sm);
        DEBUG("shared lock acquired " << step);
        if (__atomic_add_fetch(&x->ishared, 1, __ATOMIC_SEQ_CST) >= nshared) {
          __atomic_store_n(&x->ishared, 0, __ATOMIC_SEQ_CST);
          __atomic_store_n(&x->shared_done, 1, __ATOMIC_SEQ_CST);
          DEBUG("shared_done = 1");
          DEBUG("futex wake &x->shared_done");
          if (!FutexSyscall(&x->shared_done, FUTEX_WAKE, 1)) _exit(62);
        }
      }
      _exit(0);
    }
  }

  if (nexcl <= 0) __atomic_store_n(&x->excl_done, 1, __ATOMIC_SEQ_CST);
  for (long excl = 0; excl < nexcl; ++excl) {
    const pid_t pid = Fork("child-exclusive");
    if (!pid) {  // child
    if (pid < 0) FATAL("pid " << pid << " " << strerror(errno));
      for (long step = 0; step < increments_per_shared; ++step) {
        while (__atomic_load_n(&x->step, __ATOMIC_SEQ_CST) <= step) {
          if (!FutexSyscall(&x->step, FUTEX_WAIT, step)) _exit(63);
        }
        DEBUG("exclusive end of futex wait &x->step " << step);
        ExclusiveLock lock(&x->sm);
        DEBUG("x->iexcl = " << x->iexcl << " nexcl = " << nexcl);
        if (++(x->iexcl) >= nexcl) {
          x->iexcl = 0;
          __atomic_store_n(&x->excl_done, 1, __ATOMIC_SEQ_CST);
          DEBUG("excl_done = 1");
          DEBUG("futex wake &x->excl_done");
          if (!FutexSyscall(&x->excl_done, FUTEX_WAKE, 1)) _exit(64);
        }
      }
      _exit(0);
    }
  }

  for (long step = 1; step <= increments_per_shared; ++step) {
    alarm(ttl);
    __atomic_store_n(&x->step, step, __ATOMIC_SEQ_CST);
    DEBUG("============== step = " << step);
    DEBUG("futex wake &x->step");
    if (!FutexSyscall(&x->step, FUTEX_WAKE, nexcl + nshared)) FATAL("futex wake failed: " << strerror(errno));
    while (__atomic_load_n(&x->shared_done, __ATOMIC_SEQ_CST) == 0) {
      if (!FutexSyscall(&x->shared_done, FUTEX_WAIT, 0)) FATAL("futex wait failed: " << strerror(errno));
    }
    DEBUG("end of futex wait &x->shared_done");
    while (__atomic_load_n(&x->excl_done, __ATOMIC_SEQ_CST) == 0) {
      if (!FutexSyscall(&x->excl_done, FUTEX_WAIT, 0)) FATAL("futex wait failed: " << strerror(errno));
    }
    DEBUG("end of futex wait &x->excl_done");
    if (nshared != 0) __atomic_store_n(&x->shared_done, 0, __ATOMIC_SEQ_CST);
    if (nexcl != 0) __atomic_store_n(&x->excl_done, 0, __ATOMIC_SEQ_CST);
  }

  MUST(WaitForChildren(nexcl + nshared));
  return "";
}

class TestMutex : public testing::Test {
  public:
    TestMutex() { shm = (uint8_t*) mmap(getpagesize()); }

    ~TestMutex() { munmap(shm, getpagesize()); }


  protected:
    uint8_t* shm = NULL;
};

TEST_F(TestMutex, locks_from_2_concurrent_processes) {
  const auto s(TestExclusiveLocksFrom2ConcurrentProcesses<Mutex, Lock>(shm));
  ASSERT_EMPTYSTR(s);
}

TEST_F(TestMutex, locks_from_many_concurrent_processes) {
  const auto s(TestExclusiveLocksFromManyConcurrentProcesses<Mutex, Lock>(shm));
  ASSERT_EMPTYSTR(s);
}

class TestSharedMutex : public TestMutex { };

TEST_F(TestSharedMutex, exclusive_locks_from_2_concurrent_processes) {
  const auto s(TestExclusiveLocksFrom2ConcurrentProcesses<SharedMutexWrapper, ExclusiveLock>(shm));
  ASSERT_EMPTYSTR(s);
}

TEST_F(TestSharedMutex, exclusive_locks_from_many_concurrent_processes) {
  const auto s(TestExclusiveLocksFromManyConcurrentProcesses<SharedMutexWrapper, ExclusiveLock>(shm));
  ASSERT_EMPTYSTR(s);
}

TEST_F(TestSharedMutex, shared_locks_only_readers) {
  ASSERT_EMPTYSTR(TestSharedLocksFromManyConcurrentProcesses(shm, 0, 5, 4000));
}

TEST_F(TestSharedMutex, shared_locks_many_readers) {
  ASSERT_EMPTYSTR(TestSharedLocksFromManyConcurrentProcesses(shm, 2, 50, 4000));
}

TEST_F(TestSharedMutex, shared_locks_many_writers) {
  ASSERT_EMPTYSTR(TestSharedLocksFromManyConcurrentProcesses(shm, 50, 2, 4000));
}

TEST_F(TestSharedMutex, shared_locks_many_everything) {
  // we don't use as many workers as in the other unit tests, otherwise it's too slow
  ASSERT_EMPTYSTR(TestSharedLocksFromManyConcurrentProcesses(shm, 10, 10, 4000));
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
