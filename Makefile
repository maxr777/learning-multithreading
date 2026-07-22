CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
LDLIBS ?= -pthread

BUILD := build
LESSONS := \
	01_create_join \
	02_race_mutex \
	03_invariant \
	04_transfers \
	05_bounded_queue \
	06_atomic_claim \
	07_atomic_publish \
	08_false_sharing \
	09_reduction \
	10_barrier \
	11_lane_program
RUN_LESSONS := $(filter-out 08_false_sharing,$(LESSONS))
PROGRAMS := $(addprefix $(BUILD)/,$(LESSONS)) $(BUILD)/showcase

.PHONY: all run showcase check stress debug tsan tsan-check clean help
all: $(PROGRAMS)

$(BUILD):
	mkdir -p $@

$(BUILD)/%: lessons/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)

$(BUILD)/showcase: showcase.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS) -lm

run: all
	@set -e; for p in $(RUN_LESSONS); do \
		echo "== $$p =="; \
		./$(BUILD)/$$p; \
	done

showcase: $(BUILD)/showcase
	./$(BUILD)/showcase --threads 4 --width 640 --height 360 --output showcase.ppm

check: all
	./scripts/check.sh

stress: all
	./scripts/stress.sh

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O0 -g3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow' all

tsan:
	$(MAKE) clean
	$(MAKE) \
		CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=thread -fno-omit-frame-pointer' \
		LDLIBS='-pthread -fsanitize=thread' all
	@echo "TSan build complete. Try: ./build/02_race_mutex --unsafe"

tsan-check: tsan
	./scripts/check.sh

clean:
	rm -rf $(BUILD) showcase.ppm

help:
	@printf '%s\n' \
		'all: build everything (default)' \
		'run: safe lesson tour' \
		'showcase: render showcase.ppm' \
		'check: deterministic checks' \
		'stress: repeat concurrency checks' \
		'debug: rebuild -O0' \
		'tsan: rebuild with ThreadSanitizer (toolchain/platform dependent)' \
		'tsan-check: rebuild with TSan and run all safe checks' \
		'clean: generated files'
