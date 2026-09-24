/*
 * secoc_key_id.h — the PUBLIC selector for the SecOC MAC key (ADR-0021 D6).
 *
 * This is NOT secret: it is the key_id both the app (which names the key when it
 * calls crypto_mac) and the CM0+ keystore (which holds the bytes) agree on. It
 * is split out from secoc_shared_secret.h on purpose so the application image
 * can reference the key_id WITHOUT ever pulling in the secret bytes
 * (REQ-SECOC-011: the app holds no key material, only the M0+ image does).
 *
 * Must differ from CRYPTO_DEV_KEY_ID (= 1, the ECDSA verify key) and must never
 * be 0 (0 is "no valid key" — ADR-0019 D3 / REQ-SECOC-010).
 */
#ifndef SECOC_KEY_ID_H
#define SECOC_KEY_ID_H

#define SECOC_MAC_KEY_ID   0x5Eu   /* shared A<->B AES-CMAC key (distinct from key 1) */

#endif /* SECOC_KEY_ID_H */
