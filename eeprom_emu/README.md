# eeprom_emu — EEPROM emulation on work-flash

Introduced in **EP.15–EP.16**. Implements wear-levelling and sector rotation over the
`hal_flash_if_t` interface, so the logic is host-tested against a RAM-backed fake flash.

Status: skeleton only. First real code lands with EP.16.
