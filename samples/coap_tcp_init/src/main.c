#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net_connect.h"
#include "shadow.h"

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied"; pigeon_init() reads a non-NULL empty string as a zero-length
 * credential that fails every handshake. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

int main(void) {
  /* The pre-shared key is this device's whole authentication: the platform
   * maps the identity to a pigeon and keeps that pigeon's bearer token, so no
   * token is compiled in here. device_id only names the device in its own
   * logs. */
  struct pigeon_config config = {
      .device_id = "pigeon-coap-tcp-sample",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .coap =
                  {
                      .tls_psk_identity = PSK_CONF_OR_NULL(CONFIG_PIGEON_COAP_TLS_PSK_IDENTITY),
                      .tls_psk_secret = PSK_CONF_OR_NULL(CONFIG_PIGEON_COAP_TLS_PSK_SECRET),
                  },
          },
  };

  int err = net_prepare();

  if (err) {
    return err;
  }

  /* Registers the PSK, so it runs before the interface comes up: a modem's
   * credential store only accepts writes while it is offline. */
  err = pigeon_init(&config);
  if (err) {
    return err;
  }

  err = net_connect();
  if (err) {
    return err;
  }

  /* Polls the shadow and reports telemetry until told to reboot. */
  shadow_loop();

  return net_disconnect();
}
