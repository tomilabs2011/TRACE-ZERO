# Tests for the core library and the builder. Run from the repository root:
#   make -C Tests
# Needs a C compiler, zlib and python3 (for the synthetic city).

CC     ?= cc
DEFS   := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS := -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
CORE   := $(wildcard ../Core/*.c)
OUT    := build
FORGE  := ../Forge/tzforge-debug
export ASAN_OPTIONS := detect_leaks=0:abort_on_error=1

all: unit helsinki grid
	@echo "ALL TESTS PASSED"

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_core: test_core.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(DEFS) -o $@ test_core.c $(CORE) -lm

unit: $(OUT)/test_core
	$(OUT)/test_core

$(FORGE):
	$(MAKE) -C ../Forge tzforge-debug

# Real OpenStreetMap data (central Helsinki, © OpenStreetMap contributors, ODbL)
helsinki: $(FORGE) | $(OUT)
	$(FORGE) build data/helsinki.osm.pbf $(OUT)/helsinki.vmap --name Helsinki
	$(FORGE) verify $(OUT)/helsinki.vmap --pairs 500
	$(FORGE) search $(OUT)/helsinki.vmap "aleksanterinkatu 52" | grep -q "Aleksanterinkatu 52"
	$(FORGE) search $(OUT)/helsinki.vmap "stockmann" | grep -q "Department store"
	$(FORGE) route $(OUT)/helsinki.vmap car "60.1712,24.9396" "60.1693,24.9525" | grep -q "Arrive"

# Synthetic 200 x 200 street grid with one-way streets and turn restrictions
grid: $(FORGE) | $(OUT)
	python3 -I make_grid_pbf.py $(OUT)/grid.osm.pbf --size 200
	$(FORGE) build $(OUT)/grid.osm.pbf $(OUT)/grid.vmap --no-search
	$(FORGE) verify $(OUT)/grid.vmap --pairs 100

clean:
	rm -rf $(OUT)

.PHONY: all unit helsinki grid clean
