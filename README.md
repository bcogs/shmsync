# Overview

shmsync is a C++ 11 library that provides synchronization primitives such as mutexes that work beyond threads, for example, across processes, as long as they're stored in a memory area shared between those processes (usually allocated with mmap or with system V shared memory).

Look in the .h files for the documentation of the public API.
You'll need to explicitly control where the shmsync objects are placed in memory, which you can do using C++ 11 *placement new*, eg. `new (the address) shmsync::Mutex`.


# Example

```
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>

#include <shmsync/mutex.h>

int main() {
  void* p = ::mmap(0, getpagesize(), PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);
  if (p == MAP_FAILED) abort();
  shmsync::Mutex* const mutex = new (p) shmsync::Mutex;
  switch (fork()) {
    case -1: abort();
    case 0:
      m->Lock();  // you can lock/unlock the mutex explicitly...
      std::cout << "the child acquired the lock" << std::endl;
      m->Unlock();
      return 0;
  }
  shmsync::Lock lock(mutex);  // or you can use RTTI to avoid mistakes
  std::cout << "the parent acquired the lock" << std::endl;
  return 0;
}
```


# Compilation and testing

To run the unit tests, you'll need Google test.  Here's how to install it on Ubuntu:
```
apt-get install cmake googletest-dev
cd /usr/src/googletest
cmake CMakeLists.txt
make
```

To compile and test shmsync, just type `make`.  There's no make install, just copy the .h where you need and link with libshmsync.a.
