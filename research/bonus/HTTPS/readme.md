# HTTPS Support

## Overview
Add support for HTTPS using TLS encryption. This typically involves integrating OpenSSL to handle the TLS handshake, encryption, and decryption.

## Where to Extend
- **Core Layer**: Wrap the socket creation and client communication (read/write) with OpenSSL functions.
- **Config Layer**: Add new directives for `ssl_certificate` and `ssl_certificate_key`.
- **HTTP Layer**: Transparent to HTTP logic; encryption handled at the socket level.

## UML Suggestion
- Introduce a `TLSSocketWrapper` class that abstracts OpenSSL operations.
- Modify `ClientConnection` to use either a plain socket or a `TLSSocketWrapper`.

## Implementation Tips
- Use `SSL_CTX_new`, `SSL_new`, `SSL_accept`, `SSL_read`, `SSL_write`, and `SSL_shutdown`.
- Ensure certificates and keys are loaded from config.