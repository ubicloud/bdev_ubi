# Source files and object files
SRC_DIR := src
APP_DIR := $(SRC_DIR)/app
LIB_DIR := $(SRC_DIR)/lib
BIN_DIR := bin

ifneq ($(MAKECMDGOALS),format)
PKG_CONFIG_PATH = $(SPDK_PATH)/lib/pkgconfig
SPDK_DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_vhost_blk spdk_event_bdev spdk_event_scheduler spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

# Above pkg-config command adds two instances of "-lrte_net", which results in
# bunch of multiple definition errors. Make sure it's not repeated.
SPDK_DPDK_LIB := $(filter-out -lrte_net,$(SPDK_DPDK_LIB)) -lrte_net
endif

CFLAGS := -D_GNU_SOURCE -Iinclude -Wall -g -O3 -I$(SPDK_PATH)/include
LDFLAGS := -Wl,--whole-archive,-Bstatic $(SPDK_DPDK_LIB) -Wl,--no-whole-archive -luring -Wl,-Bdynamic $(SYS_LIB)

ifeq ($(COVERAGE),true)
    CFLAGS += -fprofile-arcs -ftest-coverage
endif

LIB_SRCS := $(shell find $(LIB_DIR) -name '*.c')
LIB_OBJS := $(LIB_SRCS:%.c=%.o)

APP_TARGETS = $(BIN_DIR)/vhost_ubi

.PHONY: all clean
all: # forward declaration

include src/test/build.mk

all: $(APP_TARGETS) $(TEST_TARGETS)

$(BIN_DIR)/%: $(APP_DIR)/%.c $(LIB_OBJS)
	$(info Building $@ ...)
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

COVERAGE_FILES := $(shell find $(LIB_DIR) -name '*.gc*')
clean:
	@rm -rf $(LIB_OBJS) $(BIN_DIR) coverage.info coverage_report $(COVERAGE_FILES)

format:
	find . -regex '.*\.\(c\|h\)$$' -exec clang-format -i {} \;
