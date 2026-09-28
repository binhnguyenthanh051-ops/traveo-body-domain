# Stage 2 + 4.1 — CMAC known-answer test, 2026-09-28

Images built with `CRYPTO_BRINGUP_KAT=1`; values read from RAM with the debugger after boot.
Pass = `g_kat_result == 7`. Memory view shows 32-bit words (little-endian); tags below are in byte order.

## Node A — CYT2B7 CM0+ (FBL image)
| Variable | Addr | Value |
|---|---|---|
| g_kat_result | 0x08000F54 | 7 |
| g_kat_tag_16b | 0x08000F34 | 070a16b4 6b4d4144 … (first 8 B read) |
| g_kat_shared_tag | 0x08000F24 | 6ca65e17 e56e15f7 f52497d0 1aae5ea4 (= Node B ✓, Stage 4.1) |

## Node B — CYT4BF CM0+ (flashed via `program all CRYPTO_BRINGUP_KAT=1`)
Raw memory view:
```
0x28001250  175EA66C F7156EE5 D09724F5 A45EAE1A B4160A07 44414D6B 9DDD9BF7 7C284AD0 29691DBB 283759E9 127DA37F
0x2800127C  4667759B 00000007 00000000
```
| Variable | Addr | Value (byte order) | Expected |
|---|---|---|---|
| g_kat_shared_tag | 0x28001250 | 6ca65e17 e56e15f7 f52497d0 1aae5ea4 | = Node A |
| g_kat_tag_16b | 0x28001260 | 070a16b4 6b4d4144 f79bdd9d d04a287c | NIST D.1 ex. 2 ✓ |
| g_kat_tag_empty | 0x28001270 | bb1d6929 e9593728 7fa37d12 9b756746 | NIST D.1 ex. 1 ✓ |
| g_kat_result | 0x28001280 | 7 | 7 ✓ |

The first Node B attempt read the normal image (stale merged hex), see runbook finding #11.
