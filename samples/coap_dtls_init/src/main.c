#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net_connect.h"
#include "shadow.h"

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied"; NULL leaves whatever already sits under the sec tag in place. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

int main(void) {
  /* The endpoint and the PSK come from Kconfig. device_id only names this
   * device in its own logs; the platform identifies it by the PSK identity,
   * which is why this connector needs no bearer token. */
  struct pigeon_config config = {
      .device_id = "pigeon-coap-dtls-sample",
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

  shadow_loop();

  return net_disconnect();
}
