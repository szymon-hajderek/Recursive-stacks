CC = gcc
CFLAGS = -Wall -Wextra -Wno-implicit-fallthrough -std=gnu23 -fPIC -Werror=return-type -Werror=shadow
LFLAGS = -shared -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=reallocarray -Wl,--wrap=free -Wl,--wrap=strdup -Wl,--wrap=strndup

rstack_debug.o: rstack.c rstack.h
	$(CC) rstack.c -c $(CFLAGS) -g -o rstack_debug.o

memory_tests_debug.o: memory_tests.c memory_tests.h
	$(CC) memory_tests.c -c $(CFLAGS) -g -o memory_tests_debug.o

librstack_debug.so: rstack_debug.o memory_tests_debug.o
	$(CC) -g rstack_debug.o memory_tests_debug.o $(LFLAGS) -o librstack_debug.so

rstack_example: rstack_example.c librstack_debug.so
	$(CC) -g rstack_example.c librstack_debug.so $(CFLAGS) -g -Wl,-rpath=. -o rstack_example

rstack_example_rel: rstack_example.c librstack.so
	$(CC) -g rstack_example.c librstack.so $(CFLAGS) -O2 -Wl,-rpath=. -o rstack_example_rel

rstack.o: rstack.c rstack.h
	$(CC) rstack.c -c $(CFLAGS) -O2 -o rstack.o

memory_tests.o: memory_tests.c memory_tests.h
	$(CC) memory_tests.c -c $(CFLAGS) -O2 -o memory_tests.o

librstack.so: rstack.o memory_tests.o
	$(CC) rstack.o memory_tests.o  $(LFLAGS) -o librstack.so

sh479226.tgz: makefile rstack.c rstack.h memory_tests.c memory_tests.h
	tar -czvf sh479226.tgz --transform 's,^,sh479226/,' makefile rstack.c rstack.h

.PHONY: clean
clean:
	rm -f *.o *.out *.so rstack rstack_example sh479226.tgz rstack_example_rel