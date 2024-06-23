#ifndef SHMSYNC_TEST_UTIL_H__
#define SHMSYNC_TEST_UTIL_H__

// this file contains some utilities for unit tests

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace shmsync;

#define ASSERT_EMPTYSTR(s) ASSERT_EQ(std::string(""), (s))

void* mmap(size_t len) {
  void* p = ::mmap(0, len, PROT_READ|PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
  if (p == MAP_FAILED) {
    std::cerr << "mmap()ing " << len << " bytes fo shared memoery failed - " << strerror(errno) << std::endl;
    abort();
  }
  return p;
}

std::string WaitForChildren(unsigned children) {
  std::ostringstream err;
  unsigned errors = 0;
  while (children > 0 && errors < 50) {
    int wstatus;
    const pid_t pid = ::wait(&wstatus);
    if (pid < 0) {
      if (errno == EINTR) continue;
      err << "ERROR: wait() failed: " << strerror(errno) << ";";
      ++errors;
      continue;
    }
    --children;
    if (!WIFEXITED(wstatus)) {
      err << "child process " << pid << " didn't _exit()" << ";";
      ++errors;
      continue;
    }
    constexpr int success = 0;
    if (WEXITSTATUS(wstatus) != success) {
      err << "child process " << pid << " exited with status " << WEXITSTATUS(wstatus) << " instead of the expected " << success << ";";
    }
  }
  return err.str();

}

class ShmAllocator {
  public:
    explicit ShmAllocator(size_t approx_size) {
      const size_t size = RoundUp(approx_size, (size_t) getpagesize());
      new (this) ShmAllocator((uint8_t*) mmap(size), 0, size);
    }

    void* Alloc(size_t n) {
      n = RoundUp(n, std::max(sizeof(long), sizeof(void*)));
      size_t new_size = offset += n;
      if (new_size > size) {
        std::cerr << "FATAL: attempt to allocate " << size << " more bytes exceeds allocator size " << size << std::endl;
        abort();
      }
      return bytes + new_size - n;
    }

  private:
    ShmAllocator(uint8_t* bytes_, size_t offset_, size_t size_): offset(offset_), size(size_), bytes(bytes_) { }

    static size_t RoundUp(size_t n, size_t by) { n += by - 1; return n - n % by; }

    std::atomic<size_t> offset;
    size_t size;
    uint8_t* bytes;
};

class AllocatorOwner {
  public:
    explicit AllocatorOwner(size_t size): allocator(new ShmAllocator(size)) { }
    AllocatorOwner(): AllocatorOwner(1000 * 1000) { }

    ShmAllocator* const allocator;
};

#endif
