
TEST_DIR := $(SRC_DIR)/test
TEST_BIN_DIR = $(BIN_DIR)/test
DATA_TARGETS = $(TEST_BIN_DIR)/test_image.raw $(TEST_BIN_DIR)/test_disk.raw
TEST_TARGETS = $(TEST_BIN_DIR)/test_ubi $(TEST_BIN_DIR)/memcheck_ubi $(DATA_TARGETS)

TEST_BDEVS := --bdev ubi0 --bdev ubi_nosync --bdev ubi_directio --bdev ubi_copy_on_read

$(TEST_BIN_DIR)/test_image.raw:
	$(info Building $@ ...)
	@mkdir -p $(@D)
	@dd if=/dev/random of=$@ bs=1048576 count=40

$(TEST_BIN_DIR)/test_disk.raw: $(TEST_BIN_DIR)/test_image.raw
	$(info Building $@ ...)
	@mkdir -p $(@D)
	@cp $< $@
	@truncate --size 100M $@

$(TEST_BIN_DIR)/memcheck_ubi: $(TEST_DIR)/memcheck_ubi/*.c $(LIB_OBJS)
	$(info Building $@ ...)
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) $? -o $@ $(LDFLAGS)

$(TEST_BIN_DIR)/test_ubi: $(TEST_DIR)/test_ubi/*.c $(TEST_DIR)/test_ubi/tests/*.c $(LIB_OBJS)
	$(info Building $@ ...)
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -I$(TEST_DIR)/test_ubi $? -o $@ $(LDFLAGS)

check: $(TEST_BIN_DIR)/test_ubi $(DATA_TARGETS)
	sudo $(TEST_BIN_DIR)/test_ubi --cpumask [0,1,2] --json $(TEST_DIR)/test_conf.json \
		--json-ignore-init-errors $(TEST_BDEVS) --free_base_bdev free_base_bdev \
		--image_path $(TEST_BIN_DIR)/test_image.raw

valgrind: $(TEST_BIN_DIR)/memcheck_ubi $(DATA_TARGETS)
	sudo valgrind $(TEST_BIN_DIR)/memcheck_ubi --cpumask [0] \
		--json-ignore-init-errors --json $(TEST_DIR)/test_conf.json $(TEST_BDEVS)

coverage:
	lcov --capture --directory . --exclude=`pwd`/$(TEST_DIR)/'*.c' --no-external --output-file coverage.info > /dev/null
	genhtml coverage.info --output-directory coverage_report
