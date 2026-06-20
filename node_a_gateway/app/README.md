# node_a_gateway/app — Gateway application

Runs on **FreeRTOS**. CAN aggregation, UDS data/DTC services, EEPROM, fail-safe (watchdog /
safe-state), SecOC-authenticated control toward Node B, crypto offload to the M0+. Built with
ModusToolbox. Pulls from `shared/` with an app config. MVP = milestone M2.
