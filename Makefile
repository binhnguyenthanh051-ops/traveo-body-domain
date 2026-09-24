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

# MAC-op framing (ADR-0021 D7) + the AES-secret key type (D6). Links crypto_msg
# (MAC helpers, currently stubbed => red) and the real keystore selection.
CRYPTO_MAC_SRC   := shared/crypto/src/crypto_msg.c shared/crypto/src/crypto_keystore.c
CRYPTO_MAC_TEST  := shared/crypto/tests/test_crypto_mac.c

# --- secoc (M5): SecOC frame + freshness + resync (ADR-0021). Host-testable
# core; the M0+ oracle and eeprom_emu backing are faked in tests/. secoc.c and
# secoc_freshness.c are stubs now => the first four suites are red; the store
# fake is the reference impl so test_freshness_store is green (pins the port). ---
SECOC_INC   := -Ishared/secoc/include -Ishared/secoc/tests

SECOC_SRC   := shared/secoc/src/secoc.c shared/secoc/tests/secoc_mac_fake.c
SECOC_TEST  := shared/secoc/tests/test_secoc.c

SECOC_FRESH_SRC  := shared/secoc/src/secoc_freshness.c shared/secoc/tests/secoc_freshness_store_fake.c
SECOC_FRESH_TEST := shared/secoc/tests/test_secoc_freshness.c

SECOC_RESYNC_SRC  := shared/secoc/src/secoc_freshness.c shared/secoc/tests/secoc_freshness_store_fake.c
SECOC_RESYNC_TEST := shared/secoc/tests/test_secoc_resync.c

FRESH_STORE_SRC  := shared/secoc/tests/secoc_freshness_store_fake.c
FRESH_STORE_TEST := shared/secoc/tests/test_freshness_store.c

# The composed RX verdict seam (ADR-0023 D11): MAC + freshness + accept with the
# per-reason counters and the LOG_EVT_SECOC_* events emitted together. It is the
# one secoc suite that links shared/log — that dependency IS the decision, so the
# link line is where a reviewer sees it.
SECOC_RX_SRC  := shared/secoc/src/secoc.c shared/secoc/src/secoc_freshness.c \
                 shared/secoc/src/secoc_rx.c shared/log/src/log.c \
                 shared/secoc/tests/secoc_mac_fake.c \
                 shared/secoc/tests/secoc_freshness_store_fake.c \
                 shared/log/tests/log_port_fake.c
SECOC_RX_TEST := shared/secoc/tests/test_secoc_rx.c

# --- log (M5): target logging channel (ADR-0023, REQ-LOG-001..014). Structured
# event records over UART, decoded on the host. log.c is a stub now => this
# suite is red; it pins the contract before the body exists. The fake port IS
# the reference behaviour of log_port.h, so tests can drive the UART, the clock
# and the interrupt mask.
LOG_INC   := -Ishared/log/include -Ishared/log/tests
LOG_SRC   := shared/log/src/log.c
LOG_TEST  := shared/log/tests/test_log.c shared/log/tests/log_port_fake.c

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
        test_crypto_verify test_crypto_mac test_prot_region \
        test_secoc test_secoc_freshness test_secoc_resync test_freshness_store \
        test_secoc_rx test_log clean lint

test: test_messages test_scheduler test_boot test_boot_secure test_bodyctl test_reprogram \
      test_isotp test_uds_session test_uds_security_access test_uds_download \
      test_uds_routine_control test_sysmgr \
      test_crypto_msg test_ipc_mailbox test_crypto_dispatch test_crypto_keystore \
      test_crypto_verify test_crypto_mac test_prot_region \
      test_secoc test_secoc_freshness test_secoc_resync test_freshness_store \
      test_secoc_rx test_log

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

test_crypto_mac: $(BUILD)/test_crypto_mac
	@echo "== crypto_mac (MAC-op framing + AES key type / M5) =="
	@$(BUILD)/test_crypto_mac

test_secoc: $(BUILD)/test_secoc
	@echo "== secoc (secured frame / M5) =="
	@$(BUILD)/test_secoc

test_secoc_freshness: $(BUILD)/test_secoc_freshness
	@echo "== secoc_freshness (per-ID counter + accept rule / M5) =="
	@$(BUILD)/test_secoc_freshness

test_secoc_resync: $(BUILD)/test_secoc_resync
	@echo "== secoc_resync (receiver-reset resync / M5) =="
	@$(BUILD)/test_secoc_resync

test_freshness_store: $(BUILD)/test_freshness_store
	@echo "== freshness_store (persistence port contract / M5) =="
	@$(BUILD)/test_freshness_store

test_secoc_rx: $(BUILD)/test_secoc_rx
	@echo "== secoc_rx (RX verdict + SecOC events / M5 Step 7) =="
	@$(BUILD)/test_secoc_rx

test_log: $(BUILD)/test_log
	@echo "== log (structured event channel / M5) =="
	@$(BUILD)/test_log

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
            shared/log/src shared/log/include \
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
	         -I shared/sysmgr/include -I shared/prot/include -I shared/log/include \
	         -I shared/secoc/include -I shared/crypto/include \
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

$(BUILD)/test_secoc_rx: $(SECOC_RX_SRC) $(SECOC_RX_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SECOC_INC) $(LOG_INC) $(UNITY_SRC) $(SECOC_RX_SRC) $(SECOC_RX_TEST) -o $@

$(BUILD)/test_log: $(LOG_SRC) $(LOG_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(LOG_INC) $(UNITY_SRC) $(LOG_SRC) $(LOG_TEST) -o $@

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

$(BUILD)/test_crypto_mac: $(CRYPTO_MAC_SRC) $(CRYPTO_MAC_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(CRYPTO_INC) $(UNITY_SRC) $(CRYPTO_MAC_SRC) $(CRYPTO_MAC_TEST) -o $@

$(BUILD)/test_secoc: $(SECOC_SRC) $(SECOC_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SECOC_INC) $(UNITY_SRC) $(SECOC_SRC) $(SECOC_TEST) -o $@

$(BUILD)/test_secoc_freshness: $(SECOC_FRESH_SRC) $(SECOC_FRESH_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SECOC_INC) $(UNITY_SRC) $(SECOC_FRESH_SRC) $(SECOC_FRESH_TEST) -o $@

$(BUILD)/test_secoc_resync: $(SECOC_RESYNC_SRC) $(SECOC_RESYNC_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SECOC_INC) $(UNITY_SRC) $(SECOC_RESYNC_SRC) $(SECOC_RESYNC_TEST) -o $@

$(BUILD)/test_freshness_store: $(FRESH_STORE_SRC) $(FRESH_STORE_TEST) $(UNITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(UNITY_INC) $(SECOC_INC) $(UNITY_SRC) $(FRESH_STORE_SRC) $(FRESH_STORE_TEST) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
