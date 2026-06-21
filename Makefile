# Host-side build + test for hardware-independent modules.
# Firmware itself is built with ModusToolbox (see node READMEs), not this Makefile.

CC      ?= gcc
CFLAGS  ?= -std=c17 -Wall -Wextra -Werror -O1 -g -pipe
BUILD   := build

# Unity test framework (vendored as a git submodule).
UNITY_SRC := vendor/unity/src/unity.c
UNITY_INC := -Ivendor/unity/src

# --- messages module ---
MSG_INC  := -Ishared/messages/include
MSG_SRC  := shared/messages/src/body_msgs.c
MSG_TEST := shared/messages/tests/test_body_msgs.c

# --- scheduler module ---
SCHED_INC  := -Ischeduler/include
SCHED_SRC  := scheduler/src/sched.c
SCHED_TEST := scheduler/tests/test_sched.c scheduler/tests/sched_port_fake.c

# --- boot (FBL) module ---
BOOT_INC  := -Ishared/boot/include
BOOT_SRC  := shared/boot/src/boot.c
BOOT_TEST := shared/boot/tests/test_boot.c shared/boot/tests/boot_port_fake.c

.PHONY: test test_messages test_scheduler test_boot clean lint

test: test_messages test_scheduler test_boot

test_messages: $(BUILD)/test_messages
	@echo "== messages =="
	@$(BUILD)/test_messages

test_scheduler: $(BUILD)/test_scheduler
	@echo "== scheduler =="
	@$(BUILD)/test_scheduler

test_boot: $(BUILD)/test_boot
	@echo "== boot =="
	@$(BUILD)/test_boot

# Static analysis. Runs cppcheck over production C (not test harnesses).
# With a licensed MISRA rule-texts file, enable the addon line below for MISRA C:2012.
# cppcheck is installed in CI; locally, install it to run this target.
LINT_SRC := shared/messages/src shared/messages/include \
            shared/hal/include scheduler/src scheduler/include \
            shared/boot/src shared/boot/include \
            shared/can/src shared/can/include shared/diag/src shared/diag/include \
            shared/crypto/src shared/crypto/include shared/secoc/src shared/secoc/include \
            shared/eeprom_emu/src shared/eeprom_emu/include security/src security/include
lint:
	@echo "== static analysis (cppcheck) =="
	cppcheck --error-exitcode=1 --enable=warning,style,portability \
	         --std=c17 --inline-suppr --quiet \
	         -I shared/messages/include -I shared/hal/include -I scheduler/include \
	         -I shared/boot/include \
	         $(LINT_SRC)
	@echo "(MISRA addon: add '--addon=misra.json' once the licensed rule-texts file is in place)"

$(BUILD)/test_messages: $(MSG_SRC) $(MSG_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(MSG_INC) $(UNITY_SRC) $(MSG_SRC) $(MSG_TEST) -o $@

$(BUILD)/test_scheduler: $(SCHED_SRC) $(SCHED_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SCHED_INC) $(UNITY_SRC) $(SCHED_SRC) $(SCHED_TEST) -o $@

$(BUILD)/test_boot: $(BOOT_SRC) $(BOOT_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(BOOT_INC) $(UNITY_SRC) $(BOOT_SRC) $(BOOT_TEST) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
