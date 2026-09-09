#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net_connect.h"
#include "shadow.h"

#if defined(CONFIG_MODEM_KEY_MGMT)
#include <modem/nrf_modem_lib.h>
#include <nrf_modem.h>
#endif

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied"; NULL leaves whatever already sits under the sec tag in place. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

/* The modem reaches its credential store over AT commands, so the library has
 * to be running before the PSK is written, and the interface has to stay down
 * because the store only accepts writes while the modem is offline. */
static int credential_store_ready(void) {
#if defined(CONFIG_MODEM_KEY_MGMT)
  if (!nrf_modem_is_initialized()) {
    return nrf_modem_lib_init();
  }
#endif

  return 0;
}

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

  int err = credential_store_ready();

  if (err) {
    return err;
  }

  /* Registers the PSK, so it runs before the link comes up. */
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
