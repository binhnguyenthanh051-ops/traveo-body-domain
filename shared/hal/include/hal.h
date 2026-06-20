/*
 * hal.h — hardware abstraction interfaces.
 *
 * Logic modules depend ONLY on these interfaces, never on vendor headers.
 * Target builds provide real implementations (ModusToolbox); host tests provide fakes.
 * Expand as modules need hardware (CAN, flash, crypto, time).
 */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

/* Monotonic millisecond tick — used by the scheduler and timeouts. */
typedef uint32_t (*hal_now_ms_fn)(void);

/* Flash abstraction the EEPROM emulator builds on (see eeprom_emu/). */
typedef struct {
    int  (*read)(uint32_t addr, uint8_t *dst, size_t len);
    int  (*write)(uint32_t addr, const uint8_t *src, size_t len);
    int  (*erase_sector)(uint32_t sector_addr);
    uint32_t sector_size;
} hal_flash_if_t;

/* Crypto service the security layer calls (offloaded to the M0+ on target). */
typedef struct {
    int (*mac_compute)(const uint8_t *data, size_t len,
                       const uint8_t *key, size_t key_len,
                       uint8_t *mac_out, size_t mac_len);
} hal_crypto_if_t;

#endif /* HAL_H */
