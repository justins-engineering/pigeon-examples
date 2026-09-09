# CoAP conformance rig

`coap_dtls_init` and `coap_tcp_init` speak to `loft`, the platform's CoAP terminator, in production.
Neither can be pointed at staging: staging runs no terminator, so its `COAP_DEVICE_HOST` is empty
and there is no `coaps://` endpoint to mint.

The peer to develop against instead is [libcoap](https://libcoap.net)'s `coap-server`, which is the
stronger check either way. It is a from-scratch third-party implementation of the same RFCs, rather
than our own two sides agreeing with each other.

## Building the peer

Build libcoap with both secure transports enabled, `-DENABLE_DTLS=ON -DENABLE_TCP=ON`, and its
example programs on. Either the OpenSSL or the mbedTLS backend works; only the mbedTLS one supports
RFC 9146 Connection ID, so a Connection ID claim has to be checked against that build.

Run it with the pre-shared key the device will use. It listens on the port given for plain CoAP and
on the next one up for the secure transports, over UDP and TCP alike:

```sh
./coap-server -p 5683 -k <psk secret> -h <psk identity> -d 20 -v 6
```

Seed a shadow-shaped resource before pointing a device at it. All five wire fields have to decode
or the device rejects the document:

```sh
cat > shadow.json <<'JSON'
{"target_version":3,"current_version":0,"target_config":"{\"log\":true,\"telemetry_interval\":15}",
 "current_config":"{}","updated_at":1789000000}
JSON
./coap-client -m put -k <psk secret> -u <psk identity> -f shadow.json -t application/json \
  "coaps+tcp://127.0.0.1/device/pigeons/smoke/shadow"
```

## Pointing a build at it

Override the endpoint and the key on the command line rather than editing `prj.local.conf`, so the
sample's real credentials stay where they are. Command-line assignments land after that file:

```sh
west build -p always -d build_coap_smoke -b native_sim/native/64 samples/coap_tcp_init -- \
  -DCONFIG_PIGEON_ENDPOINT='"coaps+tcp://127.0.0.1/device/pigeons/smoke"' \
  -DCONFIG_PIGEON_COAP_TLS_PSK_IDENTITY='"<psk identity>"' \
  -DCONFIG_PIGEON_COAP_TLS_PSK_SECRET='"<psk secret>"'
```

`coap_dtls_init` runs the same way with a `coaps://` endpoint. The scheme has to match the transport
the build was compiled with; the library checks it and refuses the endpoint rather than failing
later in a handshake.

## What the rig is good for

- **The full platform cycle.** Shadow GET, JSON decode, apply, report back and a telemetry POST,
  against either libcoap backend.
- **Ciphersuite selection.** The device logs the negotiated suite by code point every session,
  which is worth reading rather than assuming. Pinning the server to one suite is how to confirm
  a constrained-device suite is really on offer, and an OpenSSL-backed server needs
  `@SECLEVEL=0` in its cipher string before it will select `TLS_PSK_WITH_AES_128_CCM_8`, whatever
  its ranking says.
- **Retransmission.** Dropping a datagram in one direction with a scratch UDP proxy is enough to
  watch the device retransmit the identical confirmable message and the exchange complete. DTLS
  record headers are plaintext, so a proxy needs no keys to decide what to drop.
- **Connection ID.** Against the mbedTLS backend the client's records switch to the `tls12_cid`
  content type and a mid-session source-port rebind is survived without a re-handshake. Against
  OpenSSL the same rebind costs a re-handshake, which the device performs on the next poll.
  Do not read the device's own Connection ID status line as the positive signal on a native build:
  `docs/upstream-issues/zephyr-sockets_tls-dtls-cid-status-uninitialized.md` explains why.

## What the server is not

`coap-server` stores whatever is PUT to a resource. A device's shadow report overwrites the seeded
document with the report body, so polls after the first log a shadow that no longer parses. That is
the stand-in server, not the device.

## Ordering on the Feathers

Both CoAP samples initialize the library before bringing the network up. On an nRF91 the pre-shared
key goes into the modem's own credential store, and the modem refuses a write to it while LTE is
active. Each sample's `main.c` calls `net_prepare()`, then `pigeon_init()`, and only then
`net_connect()`.
