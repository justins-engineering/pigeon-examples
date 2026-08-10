# DRAFT — not posted. For Justin to review and file at
# https://github.com/zephyrproject-rtos/zephyr/issues

**Title suggestion:** `subsys/net/lib/sockets/sockets_tls.c`:
`TLS_DTLS_CID_STATUS` getsockopt reads uninitialized stack when the peer
did not negotiate a Connection ID — can spuriously report
`TLS_DTLS_CID_STATUS_UPLINK`/`_BIDIRECTIONAL`

## Confidence note (read first)

Both halves of this are high confidence: the code path is short and the
uninitialized read is visible from the two functions' contracts alone, and
the wrong answer was reproduced against a real DTLS server that provably
(wire capture) never negotiated CID. What was NOT explored is how often the
garbage happens to be zero (i.e. how often the bug hides) — on our build it
reproduced 100% of the time with the same wrong answer, but that's an
artifact of whatever happened to be on that stack, not something to rely on
in either direction.

## Summary

`tls_opt_dtls_connection_id_status_get()` (sockets_tls.c) declares its
scratch on the stack without initialization:

```c
	struct tls_dtls_cid cid;
	...
	ret = mbedtls_ssl_get_peer_cid(&session_ctx->ssl, &enabled,
				       cid.cid, &cid.cid_len);
```

but `mbedtls_ssl_get_peer_cid()` (mbedTLS `library/ssl_tls.c`) returns `0`
**without writing any of the out-parameters except `*enabled`** when the
CID extension was not used (or was negotiated with empty CIDs on both
sides):

```c
    /* We report MBEDTLS_SSL_CID_DISABLED in case the CID extensions
     * were used, but client and server requested the empty CID.
     * This is indistinguishable from not using the CID extension
     * in the first place. */
    if (ssl->transform_in->in_cid_len  == 0 &&
        ssl->transform_in->out_cid_len == 0) {
        return 0;
    }
```

`cid.cid_len` is then consumed as `have_peer_cid`:

```c
	have_peer_cid = (cid.cid_len != 0);
	...
	} else if (have_peer_cid) {
		val = ZSOCK_TLS_DTLS_CID_STATUS_UPLINK;
```

so whenever the application enabled CID on its side
(`context->options.dtls_cid.enabled`, e.g. via `TLS_DTLS_CID =
TLS_DTLS_CID_SUPPORTED`) but the **server never negotiated it**, the
reported status is whatever stack garbage `cid.cid_len` holds — on our
build, consistently `TLS_DTLS_CID_STATUS_UPLINK` ("CID is in use by peer")
for a connection to a server with zero CID support.

Note the function even computes `cid.enabled = (enabled ==
MBEDTLS_SSL_CID_ENABLED)` from the one out-parameter mbedTLS *does* always
write — and then never uses it in the status decision.

## Reproduction (how we hit it)

Zephyr v4.4.1, `native_sim/native/64`, NSOS offloaded sockets, DTLS 1.2
PSK client socket with `CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID=y`;
`setsockopt(TLS_DTLS_CID, &(int){TLS_DTLS_CID_SUPPORTED})` before
`connect()`, then `getsockopt(TLS_DTLS_CID_STATUS)` after — against
libcoap's `coap-server` built with the **OpenSSL** backend, which does not
support RFC 9146 at all (`coap_dtls_cid_is_supported()` returns 0 there;
its own startup banner prints "no CID support").

Result: `TLS_DTLS_CID_STATUS_UPLINK` every time. Ground truth from a UDP
proxy in the path logging DTLS record content types (plaintext first byte
of every datagram): **every** post-handshake record in both directions is
type 23 (`application_data`), never 25 (`tls12_cid`) — no CID on the wire
in either direction.

Control experiment: the identical client against libcoap built with the
**mbedTLS** backend (which enables server CID) reports the same "uplink"
status — but there the proxy shows client→server records of type 25, and
the session genuinely survives a mid-session source-port rebind with no
re-handshake. Same reported status, opposite reality — i.e. the status is
currently not usable to distinguish exactly the situation it exists to
distinguish.

## Suggested fix

Zero-initialize the scratch (`struct tls_dtls_cid cid = { 0 };`) — or
honor the `enabled` out-parameter mbedTLS guarantees:

```c
	if (ret || enabled != MBEDTLS_SSL_CID_ENABLED) {
		cid.cid_len = 0;
	}
```

Either makes the not-negotiated case report
`TLS_DTLS_CID_STATUS_DISABLED` as documented.

## Impact on this project

`pigeon`'s CoAP DTLS/UDP transport logs the negotiated CID status after
each handshake so an operator can tell whether a session will survive
carrier-NAT rebinds/PSM sleeps without a re-handshake. Until this is fixed
upstream, that log line can claim "uplink" against a server that never
negotiated CID — the log is documented as unreliable-when-positive on
native-stack builds, and behavioral verification (rebind survival) is the
authoritative check. nRF91 modem-offloaded builds don't run this code path
(the modem implements `NRF_SO_SEC_DTLS_CID_STATUS` itself).
