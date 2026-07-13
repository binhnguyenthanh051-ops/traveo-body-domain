/*
 * prot_config_cm0p.h — CM0+ TCB-isolation wall setup (M4 Seam 5, ADR-0020).
 *
 * The trusted core (CM0+) raises the bus-fabric walls that make ADR-0017
 * reason #2 (TCB isolation) real: an SMPU over the CM0+ image flash (the public
 * key, D3) and fixed PPUs over the CRYPTO block (D4), each allowing only the
 * CM0+ protection context and denying CM4, with the master structs LOCKED so
 * CM4 cannot tear the walls down (D5). It MUST run before Cy_SysEnableCM4()
 * (D2) — the walls have to exist before the untrusted core runs one instruction.
 *
 * TARGET-ONLY (ADR rule 3): this file talks to the PDL Cy_Prot_* API and the
 * device register map, so it lives in the node project, not shared/. The one
 * host-testable slice — the region descriptor math — is shared/prot (D6), whose
 * output this file consumes for the SMPU geometry.
 *
 * Compile guard FBL_M4_SEAM5_PROT: OFF by default so the build is byte-identical
 * to Seam 4 (the body compiles to a no-op) until we enable it at the bench. This
 * is scaffolding with TRM-VERIFY markers on every fact that needs silicon/TRM
 * confirmation before it is trusted (ADR-0020 "To verify").
 */
#ifndef PROT_CONFIG_CM0P_H
#define PROT_CONFIG_CM0P_H

/* Raise the SMPU + PPU walls and lock them. No-op unless FBL_M4_SEAM5_PROT is
 * defined. Call from main_cm0p.c BEFORE Cy_SysEnableCM4 (ADR-0020 D2). */
void cm0p_prot_install_walls(void);

#endif /* PROT_CONFIG_CM0P_H */
