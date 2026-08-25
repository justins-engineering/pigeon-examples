#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net/wifi_connection_manager.h"
#include "shadow.h"

/*
 * MQTT over TLS to the pigeonhole broker -- pigeon's
 * CONFIG_PIGEON_CONNECTOR_MQTT. One persistent session carries everything:
 * telemetry, shadow reports and log chunks go out as publishes, and the
 * pigeon's target shadow arrives as a retained message rather than being
 * polled for. Two board flavors, two authentication shapes:
 *
 *   native_sim, TLS-PSK       -- the local development loop and the e2e
 *                                driver (scripts/test/native-sim-e2e.sh),
 *                                against a broker built from ~/pigeonhole.
 *   esp32c6_devkitc, cert     -- WiFi, the broker's Let's Encrypt chain
 *                                verified against ISRG Root X2, CONNECT
 *                                password = CONFIG_PIGEON_TOKEN.
 *
 * Either board can run either mode; the confs under boards/ just pick the
 * one that fits (see this repo's README).
 */

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied" -- map that to NULL so pigeon_init() takes its documented
 * absent-PSK path (skip registration; the app owns whatever credential
 * lives under CONFIG_PIGEON_MQTT_SEC_TAG) instead of registering a
 * zero-length credential that fails every handshake. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

LOG_MODULE_REGISTER(main);

int main(void) {
  /*
   * device_id is not decoration on this connector: it is the CONNECT client
   * id and username, and the broker refuses a session whose id is not this
   * pigeon's own 64-hex identifier -- or, on a PSK build, one whose id
   * disagrees with the handshake identity. It therefore comes from
   * CONFIG_MQTT_INIT_PIGEON_ID (prj.local.conf, git-ignored), the same
   * convention the endpoint and the PSK secret follow, rather than being a
   * readable placeholder as in the other samples.
   *
   * The PSK identity is that same string by definition, which is why
   * pigeon's Kconfig has no separate symbol for it: a second place to write
   * one identifier could only ever be a way to get it wrong.
   */
  struct pigeon_config config = {
      .device_id = CONFIG_MQTT_INIT_PIGEON_ID,
      .connector =
          {
              .type = PIGEON_CONNECTOR_MQTT,
              .mqtt =
                  {
#if defined(CONFIG_PIGEON_MQTT_AUTH_PSK)
                      .tls_psk_identity = PSK_CONF_OR_NULL(CONFIG_MQTT_INIT_PIGEON_ID),
                      .tls_psk_secret = PSK_CONF_OR_NULL(CONFIG_PIGEON_MQTT_TLS_PSK_SECRET),
#endif
                  },
          },
  };

  /* Before the link comes up, like coap_dtls_init and for the same reason:
   * on a modem-offloaded board pigeon_init() writes the PSK into the
   * modem's own credential store, which only accepts writes while the modem
   * is offline. Neither board here has a modem, but the ordering that works
   * everywhere is the one worth keeping in a sample. */
  int err = pigeon_init(&config);

  if (err) {
    return err;
  }

  err = wifi_connect();
  if (err) {
    return err;
  }

  /* Starts the session the rest of this sample rides on. Unlike
   * pigeon_ws_start(), there is no polling to fall back to if this fails:
   * on this connector the session IS the transport. */
  err = pigeon_mqtt_start(shadow_event_cb);
  if (err) {
    LOG_ERR("pigeon_mqtt_start() failed: %d", err);
    wifi_disconnect();
    return err;
  }

  /* shadow_loop() does not return under normal operation. */
  shadow_loop();

  pigeon_mqtt_stop();

  return wifi_disconnect();
}
