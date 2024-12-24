#ifndef SHMSYNC_FUTEX_H__
#define SHMSYNC_FUTEX_H__

#include <linux/futex.h>
#include <stdint.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace shmsync {

inline int FutexSyscall(volatile uint32_t* addr, int futex_op, uint32_t val) {
// When compiling with -DCAUTIOUS_MUTEX, we pass a short timeout to the futex
// operations, so that any bug that causes a worker to wait when no other worker
// will wake it up is worked around.
#ifndef CAUTIOUS_MUTEX
  return syscall(SYS_futex, addr, futex_op, val, 0, NULL, 0);
#else
  timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 100 * 1000;
  return syscall(SYS_futex, addr, futex_op, val, &timeout, NULL, 0);
#endif
}

}

#endif
