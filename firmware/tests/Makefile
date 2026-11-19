# Host-side unit tests for the firmware application code (no ARM toolchain needed).
#   make            build and run both test programs
#   make clean
CC      ?= gcc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fsanitize=address,undefined -fno-sanitize-recover=undefined
INCLUDE  = -Istubs -I. -I../Core/Inc
BUILD    = build
COMMON   = hal_stub.c sd_card_sim.c

all: test

$(BUILD)/test_diskio: test_diskio.c ../drivers/user_diskio.c $(COMMON) $(wildcard *.h stubs/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) test_diskio.c $(COMMON) -lm -o $@

$(BUILD)/test_telemetry: test_telemetry.c ../Core/Src/telemetry.c $(COMMON) $(wildcard *.h stubs/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) test_telemetry.c $(COMMON) -lm -o $@

test: $(BUILD)/test_diskio $(BUILD)/test_telemetry
	$(BUILD)/test_diskio
	$(BUILD)/test_telemetry

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
