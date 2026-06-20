# Host-side build + test for hardware-independent modules.
# Firmware itself is built with ModusToolbox (see node READMEs), not this Makefile.

CC      ?= gcc
CFLAGS  ?= -std=c17 -Wall -Wextra -Werror -O1 -g
BUILD   := build

# Each testable module: <name>:<src dirs>:<include dirs>
# Add a line here as new host-testable modules come online (scheduler, eeprom_emu, ...).
MSG_INC := -Ishared/messages/include
MSG_SRC := shared/messages/src/body_msgs.c
MSG_TEST:= shared/messages/tests/test_body_msgs.c

.PHONY: test clean lint

test: $(BUILD)/test_messages
	@echo "== running host unit tests =="
	@$(BUILD)/test_messages

# Static analysis. Runs cppcheck over production C (not test harnesses).
# With a licensed MISRA rule-texts file, enable the addon line below for MISRA C:2012.
# cppcheck is installed in CI; locally, install it to run this target.
LINT_SRC := shared/messages/src shared/messages/include \
            shared/hal/include scheduler/src scheduler/include \
            shared/can/src shared/can/include shared/diag/src shared/diag/include \
            shared/crypto/src shared/crypto/include shared/secoc/src shared/secoc/include \
            shared/eeprom_emu/src shared/eeprom_emu/include security/src security/include
lint:
	@echo "== static analysis (cppcheck) =="
	cppcheck --error-exitcode=1 --enable=warning,style,portability \
	         --std=c17 --inline-suppr --quiet \
	         -I shared/messages/include -I shared/hal/include \
	         $(LINT_SRC)
	@echo "(MISRA addon: add '--addon=misra.json' once the licensed rule-texts file is in place)"

$(BUILD)/test_messages: $(MSG_SRC) $(MSG_TEST) | $(BUILD)
	$(CC) $(CFLAGS) $(MSG_INC) $(MSG_SRC) $(MSG_TEST) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
