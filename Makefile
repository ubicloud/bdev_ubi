# Source files and object files
SRC_DIR := src
APP_DIR := app
TEST_DIR := test
OBJ_DIR := build/obj
BIN_DIR := build/bin

ifneq ($(MAKECMDGOALS),format)
PKG_CONFIG_PATH = $(SPDK_PATH)/lib/pkgconfig
SPDK_DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_vhost_blk spdk_event_bdev spdk_event_scheduler spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

# Above pkg-config command adds two instances of "-lrte_net", which results in
# bunch of multiple definition errors. Make sure it's not repeated.
SPDK_DPDK_LIB := $(filter-out -lrte_net,$(SPDK_DPDK_LIB))
SPDK_DPDK_LIB += -lrte_net

# Compiler and linker flags
CFLAGS := -D_GNU_SOURCE -Iinclude -Wall -g -O3 -I$(SPDK_PATH)/include
LDFLAGS := -Wl,--whole-archive,-Bstatic $(SPDK_DPDK_LIB) -Wl,--no-whole-archive -luring -Wl,-Bdynamic $(SYS_LIB)
endif

# Automatically generate a list of source files (.c) and object files (.o)
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(BIN_DIR)/vhost_ubi $(BIN_DIR)/test_ubi $(BIN_DIR)/test_image.raw

$(BIN_DIR)/vhost_ubi: $(OBJS) $(OBJ_DIR)/vhost_ubi.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/test_ubi: $(OBJS) $(OBJ_DIR)/test_ubi.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(APP_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/test_image.raw:
	dd if=/dev/random of=$@ bs=1048576 count=40

check:
	sudo ./build/bin/test_ubi --cpumask [0,1,2] --json test/test_conf.json

# Clean up build artifacts
clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR)

# Automatically format source files
format:
	find . -regex '.*\.\(c\|h\)$$' -exec clang-format -i {} \;

.PHONY: all clean
