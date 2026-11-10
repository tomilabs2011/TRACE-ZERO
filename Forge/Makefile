# Builds the `tzforge` command-line tool (macOS or Linux).
#
#   make            -> ./tzforge
#   make debug      -> ./tzforge-debug with address/undefined sanitizers
#   make test       -> runs the core and builder tests in ../Tests
#
# Needs only a C compiler and zlib (both ship with the Xcode command line tools).

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c99 -Wall -Wextra -Wno-unused-parameter
DEFS    := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LIBS    := -lz -lm

CORE    := $(wildcard ../Core/*.c)
SRC     := $(wildcard src/*.c)
HDRS    := $(wildcard ../Core/*.h src/*.h)

tzforge: $(SRC) $(CORE) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) -o $@ $(SRC) $(CORE) $(LIBS)

tzforge-debug: $(SRC) $(CORE) $(HDRS)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) $(DEFS) -o $@ $(SRC) $(CORE) $(LIBS)

debug: tzforge-debug

test: tzforge tzforge-debug
	$(MAKE) -C ../Tests

clean:
	rm -f tzforge tzforge-debug

.PHONY: debug test clean
