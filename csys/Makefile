CC       = cc
CFLAGS   = -std=c11 -Wall -Wextra -g
SRCS     = gc.c obj.c runtime.c types.c
OBJS     = $(SRCS:.c=.o)

.PHONY: all test clean

all: test_phase1 test_phase2 test_phase3

test_phase1: gc.o obj.o test_phase1.o
	$(CC) $(CFLAGS) -o $@ $^

test_phase2: gc.o obj.o runtime.o types.o test_phase2.o
	$(CC) $(CFLAGS) -o $@ $^

test_phase3: $(OBJS) test_phase3.o
	$(CC) $(CFLAGS) -o $@ $^

test: test_phase1 test_phase2 test_phase3
	./test_phase1
	./test_phase2
	./test_phase3

clean:
	rm -f *.o test_phase1 test_phase2 test_phase3
