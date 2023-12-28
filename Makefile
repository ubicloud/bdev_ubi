# Source files and object files
SRC_DIR := src
APP_DIR := app
OBJ_DIR := build/obj
BIN_DIR := build/bin

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

# Automatically generate a list of source files (.c) and object files (.o)
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(BIN_DIR)/vhost_ubi

# Link object files to create the final executable, and place it in build/bin
$(BIN_DIR)/vhost_ubi: $(OBJS) $(OBJ_DIR)/vhost_ubi.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile .c to .o, place object files in build/obj
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(APP_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR)

# Automatically format source files
format:
	find . -regex '.*\.\(c\|h\)$$' -exec clang-format -i {} \;

.PHONY: all clean
