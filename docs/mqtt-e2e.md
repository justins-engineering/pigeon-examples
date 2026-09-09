# MQTT end-to-end on one workstation

`scripts/test/native-sim-e2e.sh` runs the whole device-to-platform MQTT path on the build host, with
no platform account and no hardware. It builds the broker from a
[pigeonhole](https://github.com/justins-engineering/pigeonhole) checkout, issues its development
certificate, starts `scripts/test/mock_dovecote.py` as a stand-in for the edge, then builds and runs
`mqtt_init` on `native_sim` against both.

```sh
scripts/test/native-sim-e2e.sh                      # TLS-PSK, the native_sim default
scripts/test/native-sim-e2e.sh --cert               # certificate mode against the dev CA
scripts/test/native-sim-e2e.sh --keep               # leave the broker, the mock and the device up
scripts/test/native-sim-e2e.sh --pigeonhole <dir>   # broker checkout, default ~/pigeonhole
```

It asserts on what arrived at the platform rather than on what the device believes it sent, in this
order:

1. the session authenticates and comes up, which is the broker's device-socket upgrade doing the
   authenticating;
2. the retained target shadow arrives unasked and is applied, the poll this connector replaces;
3. telemetry, a shadow report and a log chunk each reach their own route with a bearer token on
   them;
4. a config change pushed mid-session reaches the device and is reported back converged;
5. the broker is killed under the device and the session recovers.

The mock is a test fixture, not an authorization model. It records device tokens and checks they
are present, never that they are valid; the real platform verifies an Ed25519 signature per
request, which is the whole reason the broker is not a trusted proxy. The identity the script uses
is fixed and fake on purpose. Never point it at real device credentials.

## Two configuration rules this path depends on

**Keep `CONFIG_PSA_WANT_ALG_GCM` alongside CCM in any pre-shared-key configuration.** A build that
offers only `TLS_PSK_WITH_AES_128_CCM_8` cannot connect to a broker whose OpenSSL will not select
it, and that describes every default-configured OpenSSL listener. The failure is a silent handshake
failure rather than a downgrade. `samples/common/boards/tls-psk.conf` wants both, and pins no
ciphersuite, which is what leaves the constrained-device suite on offer at all.

**Batched telemetry is an HTTPS-connector feature.** `CONFIG_PIGEON_TELEMETRY_BATCH` depends on
`PIGEON_CONNECTOR_HTTPS`, so on an MQTT build the assignment is silently dropped and telemetry
leaves as one flat report per flush, which is the shape this connector is specified to send. The
log line is the tell: `Flushed N telemetry key(s) in one report` is the flat path,
`N telemetry reading(s) in one batch` the batched one.
