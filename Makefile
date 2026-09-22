CC ?= gcc
PYTHON ?= python3
CFLAGS ?= -O2 -g

CANN_ROOT ?= $(if $(ASCEND_HOME_PATH),$(ASCEND_HOME_PATH),/usr/local/Ascend/ascend-toolkit/latest)
ARCH ?= $(shell uname -m)
ACL_LIB ?= ascendcl
BUILD_DIR ?= build

SDK_CPPFLAGS ?= -I$(CANN_ROOT)/include -I$(CANN_ROOT)/$(ARCH)-linux/include
SDK_LDFLAGS ?= -L$(CANN_ROOT)/lib64 -L$(CANN_ROOT)/$(ARCH)-linux/lib64
SDK_LDLIBS ?= -lhcomm -lhccl -l$(ACL_LIB)
DEMO_CFLAGS = -std=gnu89 -Wall -Wextra -Werror -Wdeclaration-after-statement
DEPFLAGS = -MMD -MP

DEMO_NAMES = hbm_to_hbm host_to_hbm hbm_to_hbm_bw host_to_hbm_bw
DEMOS = $(addprefix $(BUILD_DIR)/,$(DEMO_NAMES))
COMMON_OBJS = $(BUILD_DIR)/transfer.o $(BUILD_DIR)/control.o $(BUILD_DIR)/bench.o
DEMO_OBJS = $(addsuffix .o,$(DEMOS))
TEST_BIN = $(BUILD_DIR)/control_probe
BENCH_TEST_BIN = $(BUILD_DIR)/bench_test
TEST_OBJS = $(BUILD_DIR)/control_probe.o $(BUILD_DIR)/control.o
OBJECTS = $(sort $(COMMON_OBJS) $(DEMO_OBJS) $(TEST_OBJS) $(BUILD_DIR)/bench_test.o)

.PHONY: all test clean
all: $(DEMOS)

$(DEMOS): $(BUILD_DIR)/%: $(BUILD_DIR)/%.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) $(SDK_LDFLAGS) -o $@ $^ $(SDK_LDLIBS) $(LDLIBS)

$(BUILD_DIR)/transfer.o: transfer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SDK_CPPFLAGS) $(CFLAGS) $(DEMO_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEMO_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/control_probe.o: tests/control_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(DEMO_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/bench_test.o: tests/bench_test.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(DEMO_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BENCH_TEST_BIN): $(BUILD_DIR)/bench_test.o $(BUILD_DIR)/bench.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

# This target builds only the CPU control code; no CANN installation is needed.
test: $(TEST_BIN) $(BENCH_TEST_BIN)
	$(PYTHON) tests/test_control.py $(abspath $(TEST_BIN))
	$(BENCH_TEST_BIN)

clean:
	rm -f $(DEMOS) $(TEST_BIN) $(BENCH_TEST_BIN) $(OBJECTS) $(OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d)
