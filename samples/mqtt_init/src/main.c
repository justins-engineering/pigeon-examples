#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_MODEM_KEY_MGMT)
#include <modem/nrf_modem_lib.h>
#include <nrf_modem.h>
#endif

#include "net_connect.h"
#include "shadow.h"

LOG_MODULE_REGISTER(main);

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
/* The broker's trust anchor. The modem takes PEM; mbedTLS wants it terminated. */
static const char ca_cert[] = {
#include "broker-ca.pem.hex"
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};

BUILD_ASSERT(sizeof(ca_cert) < KB(4), "the modem's credential store caps a certificate at 4 KiB");
#endif

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied"; to pigeon_init() a non-NULL empty string is a zero-length
 * credential that fails every handshake. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

/* Whatever this board's TLS stack needs in place before the link comes up: a
 * modem's credential store only accepts writes while it is offline. */
static int prepare_credentials(void) {
#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
  return net_install_ca(CONFIG_PIGEON_MQTT_SEC_TAG, ca_cert, sizeof(ca_cert));
#elif defined(CONFIG_MODEM_KEY_MGMT)
  /* The modem library has to be running before pigeon_init() writes the PSK
   * over AT, and on this arm only the interface would otherwise start it. */
  if (nrf_modem_is_initialized()) {
    return 0;
  }

  return nrf_modem_lib_init();
#else
  return 0;
#endif
}

int main(void) {
  /* device_id is the CONNECT client id and username, and on a PSK session the
   * handshake identity too; the broker refuses a session whose three copies
   * of it disagree, so it is a credential and lives in prj.local.conf. */
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

  int err = prepare_credentials();

  if (err) {
    LOG_ERR("Credential setup failed: %d", err);
    return err;
  }

  err = pigeon_init(&config);
  if (err) {
    return err;
  }

  err = net_connect();
  if (err) {
    return err;
  }

  /* The session is the transport on this connector, so there is no polling
   * to fall back to when it fails. */
  err = pigeon_mqtt_start(shadow_event_cb);
  if (err) {
    LOG_ERR("pigeon_mqtt_start() failed: %d", err);
    net_disconnect();
    return err;
  }

  /* Applies pushed and periodic shadows until told to reboot. */
  shadow_loop();

  pigeon_mqtt_stop();

  return net_disconnect();
}
