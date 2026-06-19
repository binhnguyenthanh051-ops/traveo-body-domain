# node_a_gateway — the "gateway" firmware (ModusToolbox)

Built with Infineon ModusToolbox, not the host Makefile. Hosts the custom scheduler, UDS
diagnostics, secure boot, crypto offload to the M0+, and signal aggregation. Pulls logic
from `scheduler/`, `common/messages/`, `security/`, `eeprom_emu/`.

To create the actual MTB application: generate a project for `CYT2B7` / `CYTVII-B-E-1M-SK`
here, then add the shared modules to its include/source paths. (EP.01.)
