# Overview

shmsync is a C++ 11 library that provides synchronization primitives such as mutexes that work beyond threads, for example, across processes, as long as they're stored in a memory area shared between those processes (usually allocated with mmap or with system V shared memory).

Look in the .h files for the documentation of the public API.

# Compilation and testing

To run the unit tests, you'll need Google test.  Here's how to install it on Ubuntu:
```apt-get install cmake googletest-dev
cd /usr/src/googletest
cmake CMakeLists.txt
make```

To compile and test shmsync, just type make.  There's no make install, just copy the .h where you need and link with libshmsync.a.
