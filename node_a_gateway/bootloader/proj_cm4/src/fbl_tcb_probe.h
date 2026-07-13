/*
 * fbl_tcb_probe.h — CM4 TCB-isolation fault demo (M4 Seam 5, ADR-0020 D6).
 *
 * The BENCH proof that the walls in proj_cm0p/prot_config_cm0p.c are real: a
 * debug-build-only routine that, running as the untrusted CM4 master (PC2),
 * deliberately touches the two walled resources — a byte of the CM0+ image flash
 * (the public key region, Boundary A / SMPU) and a CRYPTO register (Boundary B /
 * PPU). With the walls DOWN the accesses succeed; with them UP the bus fabric
 * refuses the transaction and the CM4 faults, caught by this file's
 * HardFault_Handler and observable over OpenOCD (we debug CM4). That delta is
 * the ADR-0020 claim made demonstrable.
 *
 * NOT part of the boot decision. No-op unless FBL_M4_SEAM5_PROBE is defined.
 */
#ifndef FBL_TCB_PROBE_H
#define FBL_TCB_PROBE_H

/* Attempt the illegal accesses. Returns normally if nothing faulted (walls down);
 * traps in HardFault_Handler if a wall refused the access (walls up). No-op build
 * unless FBL_M4_SEAM5_PROBE is defined. Call early in fbl_main(), after the walls
 * are already up (CM0+ raises them before Cy_SysEnableCM4). */
void fbl_tcb_probe(void);

#endif /* FBL_TCB_PROBE_H */
