# node_a_gateway/bootloader — Flash Bootloader (FBL)

Deliberately minimal (super-loop / cooperative), small attack surface. Owns: ROM-anchored
secure boot follow-through, app image **verification**, the no-init RAM handshake, the
VTOR/MSP jump to the app, and UDS **programming** services. Built with ModusToolbox.

Pulls from `shared/` (can, diag, crypto, hal) with a bootloader config. See
`docs/architecture/overview.md` §3 and ADR-0004/0006. MVP = milestone M1.
