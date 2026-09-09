#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net_connect.h"
#include "shadow.h"

/* The platform's root CA, terminated because mbedTLS parses PEM as a string. */
static const char ca_cert[] = {
#include "GTS_Root_R4.crt.hex"
    0x00
};

BUILD_ASSERT(sizeof(ca_cert) < KB(4), "the modem's credential store caps a certificate at 4 KiB");

int main(void) {
  int err = net_prepare();

  if (err) {
    return err;
  }

  err = net_install_ca(CONFIG_PIGEON_HTTPS_SEC_TAG, ca_cert, sizeof(ca_cert));
  if (err) {
    return err;
  }

  err = net_connect();
  if (err) {
    return err;
  }

  /* The endpoint and token come from Kconfig. device_id only names this
   * device in its own logs; the platform identifies it by its token. */
  struct pigeon_config config = {
      .device_id = "pigeon-sample",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    net_disconnect();
    return err;
  }

  /* Polls the shadow and reports telemetry until told to reboot. */
  shadow_loop();

  return net_disconnect();
}
