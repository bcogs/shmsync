#include <limits.h>
#include <unistd.h>

#include "futex.h"
#include "mutex.h"

namespace shmsync {

void MutexBase::Wait(volatile uint32_t* addr, uint32_t expected) {
  // waits for a FUTEX_WAKE event unless locked is no longer equal to expected
  FutexSyscall((uint32_t*) addr, FUTEX_WAIT, expected);
}

void MutexBase::Wake(volatile uint32_t* addr, int max_waiters) {
  // TODO: it might be possible to improve performance of shared mutexes with
  // FUTEX_WAKE_BITSET, but the manpage says it's slower, so checking that it's
  // actually an improvement requires benchmarking
  if (max_waiters < 0) max_waiters = INT_MAX;
  FutexSyscall((uint32_t*) addr, FUTEX_WAKE, max_waiters);
  // The linux futex(2) man page doesn't sound like the call can fail, it just
  // says that the return value is the number of awaken waiters.
  // TODO: verify that's true, and/or if it fails, report is somehow
}

}
