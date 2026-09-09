#include <errno.h>
#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "gnss.h"
#include "net_connect.h"
#include "shadow.h"

LOG_MODULE_REGISTER(main);

/* The platform's root CA, terminated because mbedTLS parses PEM as a string. */
static const char ca_cert[] = {
#include "GTS_Root_R4.crt.hex"
    0x00
};

BUILD_ASSERT(sizeof(ca_cert) < KB(4), "the modem's credential store caps a certificate at 4 KiB");

/* Nothing else recovers an asset that never reaches the network: the watchdog
 * feeds on a successful report, so it never fires, and there is no operator
 * where the device is. A fresh boot clears what a retry often cannot. */
static int connect_with_retry(void) {
  uint32_t backoff_sec = CONFIG_ASSET_TRACKER_CONNECT_BACKOFF_BASE_SEC;

  for (int round = 1; round <= CONFIG_ASSET_TRACKER_CONNECT_MAX_ROUNDS; round++) {
    int err = net_connect();

    if (!err) {
      return 0;
    }

    LOG_ERR(
        "Connect attempt %d/%d failed: %d", round, CONFIG_ASSET_TRACKER_CONNECT_MAX_ROUNDS, err
    );

    if (round == CONFIG_ASSET_TRACKER_CONNECT_MAX_ROUNDS) {
      break;
    }

    LOG_WRN("Retrying in %u s", backoff_sec);
    k_sleep(K_SECONDS(backoff_sec));
    backoff_sec = MIN(backoff_sec * 2, CONFIG_ASSET_TRACKER_CONNECT_BACKOFF_MAX_SEC);
  }

  return -ENOTCONN;
}

int main(void) {
  if (IS_ENABLED(CONFIG_ASSET_TRACKER_SIM_GPS)) {
    LOG_WRN(
        "Simulated position: every fix this build reports is fabricated and "
        "carries gps_fix_quality=%d",
        TRACKER_FIX_SIMULATED
    );
  }

  int err = net_prepare();

  if (err) {
    return err;
  }

  err = net_install_ca(CONFIG_PIGEON_HTTPS_SEC_TAG, ca_cert, sizeof(ca_cert));
  if (err) {
    return err;
  }

  if (connect_with_retry()) {
    pigeon_reboot();
  }

  /* The endpoint and token come from Kconfig. device_id only names this
   * device in its own logs; the platform identifies it by its token. */
  struct pigeon_config config = {
      .device_id = "asset-tracker-sample",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    net_disconnect();
    return err;
  }

  /* A failure here is not fatal: every poll still reports no fix, which is
   * also what a receiver that cannot see the sky reports. */
  err = tracker_gnss_init();
  if (err) {
    LOG_ERR("Position source failed to start: %d", err);
  }

  /* Polls the shadow and reports position until told to reboot. */
  shadow_loop();

  return net_disconnect();
}
