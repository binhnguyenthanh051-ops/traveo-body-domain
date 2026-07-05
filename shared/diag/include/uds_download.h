/*
 * uds_download.h — 0x34/0x36/0x37 requestDownload / transferData /
 * requestTransferExit (ADR-0012 D1 layer 3, D4, D5).
 *
 * Owns the download-bookkeeping state machine (address, block-sequence
 * counter, remaining size, staged writes) privately -- not folded into
 * session state (D4: a service-specific concern, not a generic one).
 *
 * Interrupted-download safety (D5): requestTransferExit must never finalize
 * on a gap / out-of-order block / incomplete sequence. No new recovery
 * mechanism is needed beyond that -- a partial image is caught by the
 * existing boot-time digest check (ADR-0008 D1 step 5) on the next boot. A
 * fresh requestDownload always restarts cleanly; no resume support in M3.
 */
#ifndef UDS_DOWNLOAD_H
#define UDS_DOWNLOAD_H

#include "uds_handler.h"
#include "hal.h"

/* Bind the flash instance this download writes to (the app-image region,
 * distinct from the eeprom_emu instance -- ADR-0012 D6). Call once at
 * composition time. */
void uds_download_init(const hal_flash_if_t *flash);

const uds_handler_if_t *uds_request_download_handler(void);
const uds_handler_if_t *uds_transfer_data_handler(void);
const uds_handler_if_t *uds_request_transfer_exit_handler(void);

typedef enum {
    UDS_DOWNLOAD_IDLE = 0,
    UDS_DOWNLOAD_ACTIVE,
    UDS_DOWNLOAD_DONE
} uds_download_state_t;

/* For host tests and the routineControl handler's "is a transfer in
 * progress" checks -- not for cross-handler control flow. */
uds_download_state_t uds_download_current_state(void);

#endif /* UDS_DOWNLOAD_H */
