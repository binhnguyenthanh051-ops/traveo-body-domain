/*
 * fbl_time.h — real millisecond tick bring-up (M3 Seam 2 prerequisite).
 * Bootloader-internal, same rationale as fbl_can.h (src/, not include/).
 */
#ifndef FBL_TIME_H
#define FBL_TIME_H

/* Start SysTick at 1 kHz. Call once, early in fbl_main(), before anything
 * that relies on fbl_port_now_ms() being real wall-clock time (the knock
 * dwell, ADR-0008 D2; ISO-TP's N_Cr timeout, ADR-0013 D3). */
void fbl_time_init(void);

#endif /* FBL_TIME_H */
