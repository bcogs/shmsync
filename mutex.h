#ifndef SHMSYNC_MUTEX_H__
#define SHMSYNC_MUTEX_H__

// This is an implementation of cross-process mutexes that rely on gcc builtin
// atomic operations and linux futex(2).
// It should work cross-thread too, so we use process/thread agnostic
// terminology: a "worker" can be a process or a thread, we don't care.
// It's meant to be efficient: no syscalls are issued when there's no lock
// contention.
//
// If you don't care how the implementation works and you just want to use it,
// you can skip this comment, the API is a classic mutex API, there will be no
// surprise.  But if you want to understand how it works, read on, or you'll get
// a good headache.
//
// The implementation is far from easy to understand for people unfamiliar with
// the matter, so here are the keys to reason about it.
//
// First, what is a futex?  For our purposes, it's a uint32_t that we use with a
// linux syscall for one of two things:
//   1 - waiting until the value of the uint32_t is no longer N (and return
//       immediately if it's already different from N)
//   2 - waking up the workers that are waiting on that futex (either all of
//       them, or up to a caller supplied number of them, often 1)
//
// Second, the key to understand how the state of the locks is used and changed.
// The trick is this: the state of the mutex is primarily a uint32_t that we
// manipulate using atomic exchange operations, to move through a state machine.
// And when the state doesn't allow us to move forward, we use it as a futex to
// wait until another worker wakes us up.
// All the code that waits looks something like the below, trying some possible
// states of the machine, and possibly changing them and/or waiting:
//   while (the state doesn't allow us to acquire the lock) {
//     if (atomically test if the state is X1, and if so, set it to Y1) {
//       wait until the state changes
//     } else if (atomically test if the state is X2, and if so, set it to Y2) {
//       wait until the state changes
//     ...
//   }

#include <stdint.h>

namespace shmsync {

// MutexBase has no attributes, it just encapsulates the atomic operations and
// futex calls that we need, in static methods.
class MutexBase {
  public:
    template<typename T> static T Add(volatile T* addr, T val) {
      return __atomic_add_fetch(addr, val, __ATOMIC_SEQ_CST);
    }

    template<typename T> static T CompareExchange(volatile T* addr, int expected, int wanted) {
      return CompareExchange(addr, (T) expected, (T) wanted);
    }

    template<typename T> static T CompareExchange(volatile T* addr, T expected, T wanted) {
      __atomic_compare_exchange_n(addr, &expected, wanted, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      return expected;
    }

    template<typename T> static T Exchange(volatile T* addr, T wanted) {
      return __atomic_exchange_n(addr, wanted, __ATOMIC_SEQ_CST);
    }

    template<typename T> static T Load(volatile T* addr) {
      return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
    }

    template <typename T> static T Or(volatile T* addr, T val) {
      return __atomic_or_fetch(addr, val, __ATOMIC_SEQ_CST);
    }

    template<typename T> static void Store(volatile T* addr, T wanted) {
      __atomic_store(addr, &wanted, __ATOMIC_SEQ_CST);
    }

    template<typename T> static T Sub(volatile T* addr, T val) {
      return __atomic_sub_fetch(addr, val, __ATOMIC_SEQ_CST);
    }

    // Waits on a futex until awaken by a call to Wake, and *addr is no longer
    // equal to expected.  Note it also checks as soon as called if that is the
    // case, and if so, returns without waiting.
    static void Wait(volatile uint32_t* addr, uint32_t expected);

    // Wake up workers waiting on the futex at address addr.
    // It wakes up up to max_waiters workers, or an unlimited number if it's <0.
    static void Wake(volatile uint32_t* addr, int max_waiters);
};

// Mutex is an exclusive mutex, that works cross-process as long as it's stored
// in memory shared between the processes using it.
class Mutex: public MutexBase {
    enum {  // these are all the possible states of the lock
      FREE,  // unlocked
      LOCKED_NO_WAITERS,  // locked, and no concurrent worker is waiting for it
      LOCKED_WITH_WAITERS  // locked, and concurrent workers are waiting until it's free
    };

  public:
    Mutex() { Store(&state, (uint32_t) FREE); }

    void Lock() {
      uint32_t k = CompareExchange(&state, FREE, LOCKED_NO_WAITERS);
      // wait until state is FREE; meanwhile, set it to LOCKED_WITH_WAITERS
      while (k != FREE) {
        if (k == LOCKED_WITH_WAITERS ||
            CompareExchange(&state, LOCKED_NO_WAITERS, LOCKED_WITH_WAITERS) != FREE) {
          Wait(&state, LOCKED_WITH_WAITERS);
        }
        k = CompareExchange(&state, FREE, LOCKED_WITH_WAITERS);
      }
    }

    void Unlock() { if (Exchange(&state, (uint32_t) FREE) != LOCKED_NO_WAITERS) Wake(&state, 1); }

  private:
    volatile uint32_t state;
};

// RAII class to acquire/release locks on Mutex.
class Lock {
  public:
    explicit Lock(Mutex *f): wrapped_mutex(f) { f->Lock(); }

    ~Lock() { wrapped_mutex->Unlock(); }

  private:
    Mutex* const wrapped_mutex;
};

// SharedMutex is a shared mutex that works cross-process as long as it's stored
// in memory shared between the processes using it.
// It prioritizes the workers that try to acquire exclusive locks, otherwise,
// in scenarios where readers (that is, workers that want shared locks)
// constantly come, writers could be stuck for long.
class SharedMutex: public MutexBase {
    // Flags whose combinations form the primary state information of the mutex.
    // The possible combinations are:
    //   0, SLOCKED, SLOCKED | WAITERS, ELOCKED, ELOCKED | WAITERS
    //   no other combination is supposed to happen
    enum {
      SLOCKED = 1 << 0,  // a shared lock is acquired
      ELOCKED = 1 << 1,  // an exclusive lock is acquired
      WAITERS = 1 << 2,  // some workers are waiting until the lock is released
    };

    // Flags whose combinations describe whether workers trying to acquire a
    // shared lock should be blocked early to give way to exclusive lockers.
    // Note this blocking mechanism isn't the authoritative thing we use to
    // determine if the workers can safely proceed, it's the job of the
    // SLOCKED/ELOCKED flags.  This blocking is used only to hold the readers
    // when writers are waiting, and it can sometimes let a few extra readers
    // through, as the only consequence of that is that the writers will need to
    // wait a little longer.
    // The possible combinations are:
    //   0, BLOCK, BLOCK | BLOCKED_READERS
    //   no other combination is supposed to happen
    enum {
      BLOCK = 1 << 0,  // block workers that try acquire a shared lock
      BLOCKED_READERS = 1 << 2,  // some workers trying to acquire a shared lock are blocked
    };

  public:
    SharedMutex() {
      Store(&block_readers, (uint32_t) 0);
      Store(&readers, (uint32_t) 0);
      Store(&writers, (uint32_t) 0);
      Store(&state, (uint32_t) 0);
    }

    void LockExclusive() {
      Add(&writers, (uint32_t) 1);
      uint32_t k = CompareExchange(&state, 0, ELOCKED);
      if (k == 0) return;
      Or(&block_readers, (uint32_t) BLOCK);
      // wait until state is 0, meanwhile, set the WAITERS flag
      do {
        if ((k & WAITERS) != 0) {
          Wait(&state, k);
        } else if (CompareExchange(&state, SLOCKED, SLOCKED | WAITERS) == SLOCKED) {
          Wait(&state, SLOCKED | WAITERS);
        } else if (CompareExchange(&state, ELOCKED, ELOCKED | WAITERS) == ELOCKED) {
          Wait(&state, ELOCKED | WAITERS);
        }
        k = CompareExchange(&state, 0, ELOCKED | WAITERS);
      } while (k != 0);
    }

    void UnlockExclusive() {
      if (Exchange(&state, (uint32_t) 0) & WAITERS) Wake(&state, (Load(&readers) <= 1) ? 1 : -1);
      if (Sub(&writers, (uint32_t) 1) == 0 &&
          (Exchange(&block_readers, (uint32_t) 0) & BLOCKED_READERS) != 0) {
        Wake(&block_readers, -1);
      }
    }

    void LockShared() {
      // Wait until writers are no longer queuing, otherwise we could stall them for long.
      uint32_t k = Load(&block_readers);
      while ((k & BLOCK) != 0) {
        if ((k & BLOCKED_READERS) != 0 ||
            CompareExchange(&block_readers, BLOCK, BLOCK | BLOCKED_READERS) == BLOCK) {
          Wait(&block_readers, BLOCK | BLOCKED_READERS);
        }
        k = Load(&block_readers);
      }

      Add(&readers, (uint32_t) 1);
      k = CompareExchange(&state, 0, SLOCKED);
      // wait until (state & SLOCKED) != 0, meanwhile, set the WAITERS flag
      while ((k & SLOCKED) == 0) {
        if (k == (ELOCKED | WAITERS)) {
          Wait(&state, k);
        } else if (CompareExchange(&state, ELOCKED, ELOCKED | WAITERS) == ELOCKED) {

          Wait(&state, ELOCKED | WAITERS);
        }
        k = CompareExchange(&state, 0, SLOCKED | WAITERS);
      }
    }

    void UnlockShared() {
      if (Sub(&readers, (uint32_t) 1) != 0) return;
      if (Exchange(&state, (uint32_t) 0) & WAITERS) Wake(&state, 1);
    }

  private:
    // Combination of BLOCK* flags describing whether workers trying to acquire
    // shared locks should wait because some workers want an exclusive lock.
    // Note this variable isn't the authoritative source of info to know it the
    // lock can safely be acquired.  Trying to use it for this purpose would
    // cause race conditions.
    volatile uint32_t block_readers;
    // number of workers holding or waiting to hold a shared lock
    volatile uint32_t readers;
    // number of workers holding or waiting to hold an exclusive lock
    volatile uint32_t writers;
    // Authoritative state describing if the lock can safely be acquired.
    // Combination of the SLOCKED, ELOCKED, and WAITERS flags describing the
    // current state of the lock.
    volatile uint32_t state;
};

// RAII class to acquire/release an exclusive lock on SharedMutex.
class ExclusiveLock {
  public:
    explicit ExclusiveLock(SharedMutex* sm): wrapped_mutex(sm) { sm->LockExclusive(); }

    ~ExclusiveLock() { wrapped_mutex->UnlockExclusive(); }

  private:
    SharedMutex* const wrapped_mutex;
};

// RAII class to acquire/release a shared lock on SharedMutex.
class SharedLock {
  public:
    explicit SharedLock(SharedMutex* sm): wrapped_mutex(sm) { sm->LockShared(); }

    ~SharedLock() { wrapped_mutex->UnlockShared(); }

  private:
    SharedMutex* const wrapped_mutex;
};

}

#endif
