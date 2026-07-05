# shared/diag — UDS stack (programming services in FBL; data/DTC services in app). See ADR-0004.

Layered per ADR-0012: `isotp.*` (transport) → `uds_session.*` (session/dispatch) →
`uds_<service>.*` (one file per handler) → operations (reused from `shared/hal`,
`shared/boot`, no local module). ISO-TP and UDS live in this one module rather than split
across `shared/isotp` + `shared/diag` -- the layering is enforced by the interfaces between
files, not by directory boundaries. Config/timing: ADR-0013 (transport), ADR-0014
(session/security).
