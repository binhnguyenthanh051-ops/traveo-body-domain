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

# boot in M4 secure-boot mode: same boot.c, built with FBL_DIGEST_ALGO=SHA256 so
# fbl_app_image_valid() delegates to crypto_verify_image (ADR-0016 Seam 4). Links
# the host-tested crypto client + the scripted fake M0+ port (ADR-0012 D6).
BOOT_SECURE_DEFS := -DFBL_DIGEST_ALGO=FBL_DIGEST_SHA256
BOOT_SECURE_SRC  := shared/boot/src/boot.c \
                    shared/crypto/src/crypto_service.c shared/crypto/src/ipc_mailbox.c \
                    shared/crypto/src/crypto_msg.c
BOOT_SECURE_TEST := shared/boot/tests/test_boot_secure.c shared/boot/tests/boot_port_fake.c \
                    shared/crypto/tests/ipc_port_fake.c

# --- Node A app: host-testable logic (bodyctl, reprogram) ---
APP_INC      := -Inode_a_gateway/app/logic/include -Inode_a_gateway/app/include
CAN_INC      := -Ishared/can/include

# --- diag (M3): ISO-TP + UDS session/handlers (ADR-0012/0013/0014) ---
DIAG_INC      := -Ishared/diag/include $(CAN_INC) -Ishared/hal/include
ISOTP_SRC     := shared/diag/src/isotp.c
ISOTP_TEST    := shared/diag/tests/test_isotp.c shared/diag/tests/can_hal_fake.c

UDS_SESSION_SRC  := shared/diag/src/uds_session.c shared/diag/src/uds_session_control.c \
                    shared/diag/src/uds_ecu_reset.c shared/diag/src/uds_security_access.c
UDS_SESSION_TEST := shared/diag/tests/test_uds_session.c shared/diag/tests/isotp_fake.c

UDS_SECURITY_SRC  := shared/diag/src/uds_security_access.c
UDS_SECURITY_TEST := shared/diag/tests/test_uds_security_access.c

UDS_DOWNLOAD_SRC  := shared/diag/src/uds_download.c
UDS_DOWNLOAD_TEST := shared/diag/tests/test_uds_download.c shared/diag/tests/flash_fake.c

# routine_control's "check programmed image" reuses shared/boot's
# fbl_digest()/fbl_app_image_valid() directly (ADR-0012 D8) -- needs BOOT_INC
# and boot.c's own port fake, since boot.c also defines fbl_run_boot() etc.
# which pull in the rest of fbl_port_*.
UDS_ROUTINE_SRC   := shared/diag/src/uds_routine_control.c $(BOOT_SRC) shared/boot/tests/boot_port_fake.c
UDS_ROUTINE_TEST  := shared/diag/tests/test_uds_routine_control.c shared/diag/tests/flash_fake.c

# --- sysmgr (M3): resource & lifecycle manager (ADR-0015) ---
SYSMGR_INC   := -Ishared/sysmgr/include
SYSMGR_SRC   := shared/sysmgr/src/sysmgr.c
SYSMGR_TEST  := shared/sysmgr/tests/test_sysmgr.c

# --- crypto (M4): crypto-service stack + IPC transport (ADR-0016/17/18/19) ---
CRYPTO_INC   := -Ishared/crypto/include

CRYPTO_MSG_SRC   := shared/crypto/src/crypto_msg.c
CRYPTO_MSG_TEST  := shared/crypto/tests/test_crypto_msg.c

IPC_SRC          := shared/crypto/src/ipc_mailbox.c
IPC_TEST         := shared/crypto/tests/test_ipc_mailbox.c shared/crypto/tests/ipc_port_fake.c

# dispatch encodes/decodes envelopes, so it links crypto_msg.c
CRYPTO_DISPATCH_SRC  := shared/crypto/src/crypto_dispatch.c shared/crypto/src/crypto_msg.c
CRYPTO_DISPATCH_TEST := shared/crypto/tests/test_crypto_dispatch.c

CRYPTO_KEYSTORE_SRC  := shared/crypto/src/crypto_keystore.c
CRYPTO_KEYSTORE_TEST := shared/crypto/tests/test_crypto_keystore.c

# the verify client drives the real transport + framing, faking only the port
CRYPTO_VERIFY_SRC    := shared/crypto/src/crypto_service.c shared/crypto/src/ipc_mailbox.c \
                        shared/crypto/src/crypto_msg.c
CRYPTO_VERIFY_TEST   := shared/crypto/tests/test_crypto_verify.c shared/crypto/tests/ipc_port_fake.c

# --- prot (M4 Seam 5): SMPU/PPU region-descriptor math (ADR-0020 D6). The one
# host-testable slice of TCB isolation; enforcement itself is bench-only. ---
PROT_INC     := -Ishared/prot/include
PROT_SRC     := shared/prot/src/prot_region.c
PROT_TEST    := shared/prot/tests/test_prot_region.c

BODYCTL_SRC  := node_a_gateway/app/logic/src/bodyctl.c
BODYCTL_TEST := node_a_gateway/app/logic/tests/test_bodyctl.c

# reprogram.c -> boot_handshake_encode (boot.c, needs the FBL port fake) + the
# app_port fake. Drives the M2 App-side .noinit programming-request (ADR-0007 D7).
REPROG_SRC   := node_a_gateway/app/logic/src/reprogram.c shared/boot/src/boot.c \
                shared/boot/tests/boot_port_fake.c \
                node_a_gateway/app/logic/tests/app_port_fake.c
REPROG_TEST  := node_a_gateway/app/logic/tests/test_reprogram.c

.PHONY: test test_messages test_scheduler test_boot test_boot_secure test_bodyctl test_reprogram \
        test_isotp test_uds_session test_uds_security_access test_uds_download \
        test_uds_routine_control test_sysmgr \
        test_crypto_msg test_ipc_mailbox test_crypto_dispatch test_crypto_keystore \
        test_crypto_verify test_prot_region clean lint

test: test_messages test_scheduler test_boot test_boot_secure test_bodyctl test_reprogram \
      test_isotp test_uds_session test_uds_security_access test_uds_download \
      test_uds_routine_control test_sysmgr \
      test_crypto_msg test_ipc_mailbox test_crypto_dispatch test_crypto_keystore \
      test_crypto_verify test_prot_region

test_messages: $(BUILD)/test_messages
	@echo "== messages =="
	@$(BUILD)/test_messages

test_scheduler: $(BUILD)/test_scheduler
	@echo "== scheduler =="
	@$(BUILD)/test_scheduler

test_boot: $(BUILD)/test_boot
	@echo "== boot =="
	@$(BUILD)/test_boot

test_boot_secure: $(BUILD)/test_boot_secure
	@echo "== boot_secure (SHA-256 / M4) =="
	@$(BUILD)/test_boot_secure

test_bodyctl: $(BUILD)/test_bodyctl
	@echo "== bodyctl =="
	@$(BUILD)/test_bodyctl

test_reprogram: $(BUILD)/test_reprogram
	@echo "== reprogram =="
	@$(BUILD)/test_reprogram

test_isotp: $(BUILD)/test_isotp
	@echo "== isotp =="
	@$(BUILD)/test_isotp

test_uds_session: $(BUILD)/test_uds_session
	@echo "== uds_session =="
	@$(BUILD)/test_uds_session

test_uds_security_access: $(BUILD)/test_uds_security_access
	@echo "== uds_security_access =="
	@$(BUILD)/test_uds_security_access

test_uds_download: $(BUILD)/test_uds_download
	@echo "== uds_download =="
	@$(BUILD)/test_uds_download

test_uds_routine_control: $(BUILD)/test_uds_routine_control
	@echo "== uds_routine_control =="
	@$(BUILD)/test_uds_routine_control

test_sysmgr: $(BUILD)/test_sysmgr
	@echo "== sysmgr =="
	@$(BUILD)/test_sysmgr

test_crypto_msg: $(BUILD)/test_crypto_msg
	@echo "== crypto_msg =="
	@$(BUILD)/test_crypto_msg

test_ipc_mailbox: $(BUILD)/test_ipc_mailbox
	@echo "== ipc_mailbox =="
	@$(BUILD)/test_ipc_mailbox

test_crypto_dispatch: $(BUILD)/test_crypto_dispatch
	@echo "== crypto_dispatch =="
	@$(BUILD)/test_crypto_dispatch

test_crypto_keystore: $(BUILD)/test_crypto_keystore
	@echo "== crypto_keystore =="
	@$(BUILD)/test_crypto_keystore

test_crypto_verify: $(BUILD)/test_crypto_verify
	@echo "== crypto_verify =="
	@$(BUILD)/test_crypto_verify

test_prot_region: $(BUILD)/test_prot_region
	@echo "== prot_region (SMPU/PPU geometry / M4 Seam 5) =="
	@$(BUILD)/test_prot_region

# Static analysis. Runs cppcheck over production C (not test harnesses).
# With a licensed MISRA rule-texts file, enable the addon line below for MISRA C:2012.
# cppcheck is installed in CI; locally, install it to run this target.
LINT_SRC := shared/messages/src shared/messages/include \
            shared/hal/include scheduler/src scheduler/include \
            shared/boot/src shared/boot/include \
            shared/can/src shared/can/include shared/diag/src shared/diag/include \
            shared/sysmgr/src shared/sysmgr/include \
            shared/crypto/src shared/crypto/include shared/secoc/src shared/secoc/include \
            shared/prot/src shared/prot/include \
            shared/eeprom_emu/src shared/eeprom_emu/include security/src security/include \
            node_a_gateway/app/logic/src node_a_gateway/app/logic/include \
            node_a_gateway/app/include
lint:
	@echo "== static analysis (cppcheck) =="
	cppcheck --error-exitcode=1 --enable=warning,style,portability \
	         --std=c17 --inline-suppr --quiet \
	         -I shared/messages/include -I shared/hal/include -I scheduler/include \
	         -I shared/boot/include -I shared/can/include -I shared/diag/include \
	         -I shared/sysmgr/include -I shared/prot/include \
	         -I node_a_gateway/app/logic/include -I node_a_gateway/app/include \
	         $(LINT_SRC)
	@echo "(MISRA addon: add '--addon=misra.json' once the licensed rule-texts file is in place)"

$(BUILD)/test_messages: $(MSG_SRC) $(MSG_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(MSG_INC) $(UNITY_SRC) $(MSG_SRC) $(MSG_TEST) -o $@

$(BUILD)/test_scheduler: $(SCHED_SRC) $(SCHED_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SCHED_INC) $(UNITY_SRC) $(SCHED_SRC) $(SCHED_TEST) -o $@

$(BUILD)/test_boot: $(BOOT_SRC) $(BOOT_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(BOOT_INC) $(UNITY_SRC) $(BOOT_SRC) $(BOOT_TEST) -o $@

$(BUILD)/test_boot_secure: $(BOOT_SECURE_SRC) $(BOOT_SECURE_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(BOOT_SECURE_DEFS) $(UNITY_INC) $(BOOT_INC) $(CRYPTO_INC) $(UNITY_SRC) \
	    $(BOOT_SECURE_SRC) $(BOOT_SECURE_TEST) -o $@

$(BUILD)/test_bodyctl: $(BODYCTL_SRC) $(BODYCTL_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(MSG_INC) $(APP_INC) $(UNITY_SRC) $(BODYCTL_SRC) $(BODYCTL_TEST) -o $@

$(BUILD)/test_reprogram: $(REPROG_SRC) $(REPROG_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(BOOT_INC) $(APP_INC) $(UNITY_SRC) $(REPROG_SRC) $(REPROG_TEST) -o $@

$(BUILD)/test_prot_region: $(PROT_SRC) $(PROT_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(PROT_INC) $(UNITY_SRC) $(PROT_SRC) $(PROT_TEST) -o $@

$(BUILD)/test_isotp: $(ISOTP_SRC) $(ISOTP_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(DIAG_INC) $(UNITY_SRC) $(ISOTP_SRC) $(ISOTP_TEST) -o $@

$(BUILD)/test_uds_session: $(UDS_SESSION_SRC) $(UDS_SESSION_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(DIAG_INC) $(UNITY_SRC) $(UDS_SESSION_SRC) $(UDS_SESSION_TEST) -o $@

$(BUILD)/test_uds_security_access: $(UDS_SECURITY_SRC) $(UDS_SECURITY_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(DIAG_INC) $(UNITY_SRC) $(UDS_SECURITY_SRC) $(UDS_SECURITY_TEST) -o $@

$(BUILD)/test_uds_download: $(UDS_DOWNLOAD_SRC) $(UDS_DOWNLOAD_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(DIAG_INC) $(UNITY_SRC) $(UDS_DOWNLOAD_SRC) $(UDS_DOWNLOAD_TEST) -o $@

$(BUILD)/test_uds_routine_control: $(UDS_ROUTINE_SRC) $(UDS_ROUTINE_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(DIAG_INC) $(BOOT_INC) $(UNITY_SRC) $(UDS_ROUTINE_SRC) $(UDS_ROUTINE_TEST) -o $@

$(BUILD)/test_sysmgr: $(SYSMGR_SRC) $(SYSMGR_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SYSMGR_INC) $(UNITY_SRC) $(SYSMGR_SRC) $(SYSMGR_TEST) -o $@

$(BUILD)/test_crypto_msg: $(CRYPTO_MSG_SRC) $(CRYPTO_MSG_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(CRYPTO_MSG_SRC) $(CRYPTO_MSG_TEST) -o $@

$(BUILD)/test_ipc_mailbox: $(IPC_SRC) $(IPC_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(IPC_SRC) $(IPC_TEST) -o $@

$(BUILD)/test_crypto_dispatch: $(CRYPTO_DISPATCH_SRC) $(CRYPTO_DISPATCH_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(CRYPTO_DISPATCH_SRC) $(CRYPTO_DISPATCH_TEST) -o $@

$(BUILD)/test_crypto_keystore: $(CRYPTO_KEYSTORE_SRC) $(CRYPTO_KEYSTORE_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(CRYPTO_KEYSTORE_SRC) $(CRYPTO_KEYSTORE_TEST) -o $@

$(BUILD)/test_crypto_verify: $(CRYPTO_VERIFY_SRC) $(CRYPTO_VERIFY_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(CRYPTO_VERIFY_SRC) $(CRYPTO_VERIFY_TEST) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
