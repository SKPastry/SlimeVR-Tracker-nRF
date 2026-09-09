CC ?= cc
CFLAGS ?= -O1 -g -std=c11 -Wall -Wextra -Werror
SANITIZER_FLAGS ?= -fsanitize=undefined -fno-sanitize-recover=undefined

SYSTEM_DIR := ../../../src/system
BUILD_DIR := build
SYNC_HEADER := $(SYSTEM_DIR)/sk_led_sync.h

.PHONY: check clean

check: $(BUILD_DIR)/test_sk_led_sync $(BUILD_DIR)/test_sk_led_sync_disabled
	./$(BUILD_DIR)/test_sk_led_sync
	./$(BUILD_DIR)/test_sk_led_sync_disabled

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/test_sk_led_sync: test_sk_led_sync.c $(SYSTEM_DIR)/sk_led_sync.c $(SYNC_HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_FLAGS) -DCONFIG_SK_LED_SYNC=1 -I$(SYSTEM_DIR) \
		-o $@ test_sk_led_sync.c $(SYSTEM_DIR)/sk_led_sync.c $(LDFLAGS) $(LDLIBS)

# No module or clock mock is linked: disabled builds must be self-contained.
$(BUILD_DIR)/test_sk_led_sync_disabled: test_sk_led_sync_disabled.c $(SYNC_HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_FLAGS) -I$(SYSTEM_DIR) \
		-o $@ test_sk_led_sync_disabled.c $(LDFLAGS) $(LDLIBS)

clean:
	$(RM) $(BUILD_DIR)/test_sk_led_sync $(BUILD_DIR)/test_sk_led_sync_disabled
