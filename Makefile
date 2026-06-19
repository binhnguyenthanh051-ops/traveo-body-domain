# Host-side build + test for hardware-independent modules.
# Firmware itself is built with ModusToolbox (see node READMEs), not this Makefile.

CC      ?= gcc
CFLAGS  ?= -std=c17 -Wall -Wextra -Werror -O1 -g
BUILD   := build

# Each testable module: <name>:<src dirs>:<include dirs>
# Add a line here as new host-testable modules come online (scheduler, eeprom_emu, ...).
MSG_INC := -Icommon/messages/include
MSG_SRC := common/messages/src/body_msgs.c
MSG_TEST:= common/messages/tests/test_body_msgs.c

.PHONY: test clean

test: $(BUILD)/test_messages
	@echo "== running host unit tests =="
	@$(BUILD)/test_messages

$(BUILD)/test_messages: $(MSG_SRC) $(MSG_TEST) | $(BUILD)
	$(CC) $(CFLAGS) $(MSG_INC) $(MSG_SRC) $(MSG_TEST) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
