# security — secure boot helpers, crypto service, message authentication

Introduced across **EP.12 (secure boot + flash protection)**, **EP.13 (crypto/HSM on the
M0+)**, and **EP.18 (authenticated CAN, SecOC-style)**.

The message-authentication logic is host-testable against a fake `hal_crypto_if_t`; key
storage and the real secure-boot chain are target-only.

Status: skeleton only.
