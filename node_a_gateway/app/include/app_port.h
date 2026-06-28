/*
 * app_port.h — the Node A application's hardware seam (the shared/hal pattern).
 *
 * Mirrors fbl_port.h: the host-testable app logic depends only on these
 * functions; the linker selects the ModusToolbox target implementation or the
 * host fake for tests (ADR-0001). M2 needs just the two hooks the .noinit
 * programming-request path uses (ADR-0007 D7).
 */
#ifndef APP_PORT_H
#define APP_PORT_H

#include "boot_types.h"   /* fbl_handshake_t */

/* Pointer to the pinned .noinit handshake region (ADR-0007 D8). Target: the
 * linker-pinned address; host fake: a static buffer. The FBL primed its ECC, so
 * the app is a pure consumer/writer here (ADR-0007 D9). */
fbl_handshake_t *app_port_noinit(void);

/* Trigger a software reset. Target: a data barrier then NVIC_SystemReset (so the
 * .noinit write lands first); host fake: records the call. Never returns on
 * target. */
void app_port_system_reset(void);

#endif /* APP_PORT_H */
