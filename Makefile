CXXFLAGS := -Wall -Werror -O3 -std=c++11

.PHONY: all
all: build test

.PHONY: build
build: libshmsync.a

libshmsync.a: mutex.o
	ar rcs $@ $^

mutex.o: mutex.cc mutex.h futex.h
	$(CXX) $(CXXFLAGS) -c -o mutex.o mutex.cc

.PHONY: test
test: build *_test.cc test_util.h
	$(CXX) $(CXXFLAGS) -I /usr/src/googletest/googletest/include -L /usr/src/googletest/lib -o mutex_test mutex_test.cc libshmsync.a -lgtest
	./mutex_test

.PHONY: clean
clean:
	rm -f *.o *.a *_test
