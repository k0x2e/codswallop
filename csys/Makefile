CC       = cc
CFLAGS   = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g
SRCS     = gc.c obj.c runtime.c types.c
OBJS     = $(SRCS:.c=.o)

# internals.c uses libm (fmod/pow/trunc); anything linking it (or rom.o,
# which pulls it in transitively via boot.o in the codswallop/test_phase7
# case) needs -lm.
LDLIBS_M = -lm

.PHONY: all test clean

all: test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7 codswallop

test_phase1: gc.o obj.o test_phase1.o
	$(CC) $(CFLAGS) -o $@ $^

test_phase2: gc.o obj.o runtime.o types.o test_phase2.o
	$(CC) $(CFLAGS) -o $@ $^

test_phase3: $(OBJS) test_phase3.o
	$(CC) $(CFLAGS) -o $@ $^

test_phase5: gc.o obj.o runtime.o types.o internals.o rom.o test_phase5.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_M)

test_phase6: gc.o obj.o runtime.o types.o internals.o rom.o test_phase6.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_M)

test_phase7: gc.o obj.o runtime.o types.o internals.o rom.o boot.o test_phase7.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_M)

codswallop: gc.o obj.o runtime.o types.o internals.o rom.o boot.o main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_M)

test: test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7
	./test_phase1
	./test_phase2
	./test_phase3
	./test_phase5
	./test_phase6
	./test_phase7

clean:
	rm -f *.o test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7 codswallop
