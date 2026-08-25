#!/usr/bin/env bash
# End-to-end for the MQTT connector: the mqtt_init sample on native_sim, a
# real pigeonhole broker, and a mock edge -- all on this workstation, no
# platform account and no hardware.
#
# What it proves, and why each step is here rather than in a unit test:
#
#   1. the session authenticates and comes up (the broker's device-socket
#      upgrade is what authenticates it, so a mock that refuses the token
#      would fail here and nowhere else);
#   2. the retained target shadow arrives without being asked for, and the
#      device applies it -- the poll this connector replaces;
#   3. telemetry, a shadow report and a log chunk all reach the platform's
#      own routes with the pigeon's token on them;
#   4. a dashboard-shaped config change pushed mid-session reaches the
#      device and is reported back converged;
#   5. killing the broker under the device reconnects it and the session
#      recovers, which is the failure a bench never gets to schedule.
#
# Usage:
#   scripts/test/native-sim-e2e.sh [--cert] [--keep] [--pigeonhole DIR]
#
#   --cert        certificate mode (the ESP32-C6 shape) instead of TLS-PSK,
#                 verifying the broker against pigeonhole's own dev CA
#   --keep        leave the broker, the mock and the device running
#   --pigeonhole  path to the pigeonhole checkout (default ~/pigeonhole)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
topdir="$(cd "$here/../.." && pwd)"

pigeonhole="${PIGEONHOLE_DIR:-$HOME/pigeonhole}"
mode="psk"
keep=0

while [ $# -gt 0 ]; do
  case "$1" in
    --cert) mode="cert" ;;
    --keep) keep=1 ;;
    --pigeonhole) pigeonhole="$2"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# Test identity only. A pigeon id is 64 lowercase hex, which the broker
# checks before it will talk to anything upstream; these values are fixed and
# fake on purpose, and nothing here should ever be pointed at a real pigeon.
PIGEON_ID="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
DEVICE_TOKEN="mqtt-e2e-device-token"
PSK_SECRET="0123456789abcdef0123456789abcdef"
SERVICE_SECRET="mqtt-e2e-service-secret"

MOCK_PORT=8788
BROKER_PORT=8883

work="$(mktemp -d)"
build="$work/build"
mock_log="$work/mock.log"
broker_log="$work/broker.log"
device_log="$work/device.log"

mock_pid=""
broker_pid=""
device_pid=""

cleanup() {
  if [ "$keep" = "1" ]; then
    echo
    echo "left running (--keep):"
    echo "  mock   pid $mock_pid   log $mock_log"
    echo "  broker pid $broker_pid log $broker_log"
    echo "  device pid $device_pid log $device_log"
    return
  fi
  for pid in "$device_pid" "$broker_pid" "$mock_pid"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

fail() {
  echo
  echo "FAIL: $*" >&2
  echo "--- device log (tail) ---" >&2
  tail -40 "$device_log" 2>/dev/null >&2 || true
  echo "--- broker log (tail) ---" >&2
  tail -40 "$broker_log" 2>/dev/null >&2 || true
  exit 1
}

# Waits for a line to appear in a log, so nothing here sleeps a fixed amount
# and hopes.
await_log() {
  local file="$1" pattern="$2" what="$3" timeout="${4:-60}"
  local deadline=$((SECONDS + timeout))
  while [ $SECONDS -lt $deadline ]; do
    if grep -qF -- "$pattern" "$file" 2>/dev/null; then
      echo "  ok: $what"
      return 0
    fi
    sleep 0.2
  done
  fail "timed out waiting for $what"
}

# Counts something in the mock's recorded state, where `s` is the whole state
# document. One read per poll: the expression is evaluated against a state
# already loaded, so a compound check can look at requests and frames
# together.
await_count() {
  local expr="$1" want="$2" what="$3" timeout="${4:-60}"
  local deadline=$((SECONDS + timeout))
  local have=0
  while [ $SECONDS -lt $deadline ]; do
    have="$(curl -sf "http://127.0.0.1:$MOCK_PORT/_control/state" \
      | python3 -c "import json,sys; s=json.load(sys.stdin); print($expr)" 2>/dev/null || echo 0)"
    if [ "${have:-0}" -ge "$want" ]; then
      echo "  ok: $what ($have)"
      return 0
    fi
    sleep 0.2
  done
  fail "timed out waiting for $what (last count ${have:-0})"
}

echo "== building the broker =="
cargo build --manifest-path "$pigeonhole/Cargo.toml" --quiet
broker_bin="$pigeonhole/target/debug/pigeonhole"
[ -x "$broker_bin" ] || fail "broker binary not found at $broker_bin"

if [ ! -f "$pigeonhole/scripts/dev-cert/server.pem" ]; then
  echo "== issuing a development certificate =="
  "$pigeonhole/scripts/dev-cert.sh" >/dev/null
fi

echo "== starting the mock edge =="
python3 "$here/mock_dovecote.py" \
  --port "$MOCK_PORT" \
  --pigeon-id "$PIGEON_ID" \
  --token "$DEVICE_TOKEN" \
  --psk-secret "$PSK_SECRET" \
  --service-secret "$SERVICE_SECRET" >"$mock_log" 2>&1 &
mock_pid=$!
await_log "$mock_log" "mock dovecote listening" "mock edge listening" 15

start_broker() {
  PIGEONHOLE_LISTEN="127.0.0.1:$BROKER_PORT" \
  PIGEONHOLE_DOVECOTE_URL="http://127.0.0.1:$MOCK_PORT" \
  PIGEONHOLE_SERVICE_SECRET="$SERVICE_SECRET" \
  PIGEONHOLE_TLS_CERT="$pigeonhole/scripts/dev-cert/server.pem" \
  PIGEONHOLE_TLS_KEY="$pigeonhole/scripts/dev-cert/server.key" \
  PIGEONHOLE_LOG="info" \
  "$broker_bin" >>"$broker_log" 2>&1 &
  broker_pid=$!
}

echo "== starting the broker =="
start_broker
await_log "$broker_log" "listening" "broker listening on $BROKER_PORT" 20

echo "== building mqtt_init for native_sim ($mode mode) =="
conf="$work/e2e.conf"
{
  echo "CONFIG_PIGEON_ENDPOINT=\"mqtts://localhost:$BROKER_PORT\""
  echo "CONFIG_MQTT_INIT_PIGEON_ID=\"$PIGEON_ID\""
  # Short enough that the log path is exercised inside this run rather than a
  # minute after it.
  echo "CONFIG_PIGEON_LOG_UPLOAD_MAX_INTERVAL_MS=5000"
  if [ "$mode" = "cert" ]; then
    # The certificate want-list itself lives in the sample's own overlay
    # (added below), so this file stays credentials-only.
    echo "CONFIG_PIGEON_TOKEN=\"$DEVICE_TOKEN\""
  else
    echo "CONFIG_PIGEON_MQTT_AUTH_PSK=y"
    echo "CONFIG_PIGEON_MQTT_TLS_PSK_SECRET=\"$PSK_SECRET\""
  fi
} >"$conf"

conf_files="$conf"
cmake_args=()
if [ "$mode" = "cert" ]; then
  # native_sim's board conf is TLS-PSK; the X.509 want-list and the broker's
  # own development CA come in on top.
  conf_files="$topdir/samples/mqtt_init/overlay-cert-native-tls.conf;$conf"
  cmake_args+=("-DPIGEON_MQTT_CA_FILE=$pigeonhole/scripts/dev-cert/ca.pem")
fi

(cd "$topdir" && west build -p -d "$build" -b native_sim/native/64 samples/mqtt_init \
  -- "-DEXTRA_CONF_FILE=$conf_files" "${cmake_args[@]}") >"$work/build.log" 2>&1 \
  || { tail -30 "$work/build.log" >&2; fail "sample build failed"; }
echo "  ok: built"

echo "== running the device =="
"$build/zephyr/zephyr.exe" >"$device_log" 2>&1 &
device_pid=$!

await_log "$device_log" "MQTT session up" "device session up" 60
await_log "$device_log" "Target shadow received" "retained target shadow delivered" 30
await_log "$device_log" "Applied shadow v1" "device applied the retained target" 30

# The first pass reports telemetry and, because it just applied a new
# target, a shadow report. Both are asserted at the platform's own routes,
# not from the device's own log: a device that believes it published proves
# nothing about what arrived. QoS 0 telemetry rides the held device socket
# as a frame instead of a POST, which is why either is accepted here.
await_count "sum(1 for r in s['requests'] if r['leaf']=='shadow')" 1 \
  "shadow report reached the platform" 60
await_count "sum(1 for r in s['requests'] if r['leaf']=='telemetry') + sum(1 for f in s['frames'] if 'telemetry' in f['text'])" 1 \
  "telemetry reached the platform" 60
await_count "sum(1 for r in s['requests'] if r['leaf']=='logs')" 1 \
  "device log chunk reached the platform" 60

echo "== pushing a config change (the dashboard's half) =="
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_control/shadow" \
  -H 'Content-Type: application/json' \
  -d '{"target_version":2,"target_config":"{\"telemetry_interval\":20,\"log\":true}"}' \
  >/dev/null || fail "control push failed"

await_log "$device_log" "Applied shadow v2" "pushed config applied on the device" 30
await_count "sum(1 for r in s['requests'] if r['leaf']=='shadow' and '\"current_version\":2' in r['body'].get('text',''))" 1 \
  "device reported convergence at v2" 30

echo "== restarting the broker under the device =="
before="$(grep -c "MQTT session up" "$device_log" || true)"
kill "$broker_pid" 2>/dev/null || true
wait "$broker_pid" 2>/dev/null || true
sleep 1
start_broker
await_log "$broker_log" "listening" "broker back up" 20

deadline=$((SECONDS + 90))
while [ $SECONDS -lt $deadline ]; do
  now="$(grep -c "MQTT session up" "$device_log" || true)"
  if [ "$now" -gt "$before" ]; then
    echo "  ok: device reconnected after the broker restart"
    break
  fi
  sleep 0.5
done
[ "${now:-0}" -gt "$before" ] || fail "device did not reconnect after the broker restart"

echo "== summary =="
curl -sf "http://127.0.0.1:$MOCK_PORT/_control/state" | python3 -c '
import json, sys
state = json.load(sys.stdin)
leaves = {}
for r in state["requests"]:
    leaves[r["leaf"]] = leaves.get(r["leaf"], 0) + 1
print("  upgrades accepted:", state["upgrades"], " refused:", state["refusals"])
print("  device-route calls:", leaves or "none")
print("  device-socket frames:", len(state["frames"]))
print("  shadow: target_version", state["shadow"]["target_version"],
      "current_version", state["shadow"]["current_version"])
'
echo "  logs: $work"

echo
echo "PASS ($mode mode)"
