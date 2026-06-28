/*
 * reprogram.h — App-side programming-request (ADR-0007 D7).
 *
 * Pure logic over the app_port seam: encode a PROGRAMMING_REQUESTED handshake
 * into .noinit and trigger a software reset. The FBL then classifies the
 * software reset, reads the request, stays in programming mode, and clears the
 * boot-loop counter (ADR-0007 D4 — the M2-5 interaction). Host-testable with an
 * app_port fake.
 */
#ifndef REPROGRAM_H
#define REPROGRAM_H

/* Write the programming-request and reset. Does not return on target. */
void app_request_reprogram(void);

#endif /* REPROGRAM_H */
