#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(shadow_model, LOG_LEVEL_INF);

/*
 * Demonstrates building the wire-shaped shadow structs (struct
 * pigeon_shadow_doc / struct pigeon_shadow_update_request, mirroring
 * capsules::PigeonShadow / PigeonShadowUpdateRequest) without a transport.
 * There's no CoAP/HTTPS sync yet (see pigeon's CLAUDE.md), so this only logs
 * the values a real sync would send/receive.
 */
int main(void) {
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0003",
      .connector =
          {
              .type = PIGEON_CONNECTOR_HTTPS,
              .cfg.https =
                  {
                      .endpoint = "https://api.pidgeiot.com/device/pigeons/demo-pigeon-0003",
                      .token = "replace-with-device-jwt",
                  },
          },
  };

  int err = pigeon_init(&config);
  if (err) {
    return err;
  }

  /* As last synced from GET /pigeon/shadow/get. */
  struct pigeon_shadow_doc shadow = {
      .target_version = 2,
      .current_version = 1,
      .target_config = "{\"report_interval_s\":60}",
      .current_config = "{\"report_interval_s\":300}",
      .updated_at = 1751500000,
  };

  LOG_INF(
      "shadow target=v%d current=v%d target_config=%s current_config=%s", shadow.target_version,
      shadow.current_version, shadow.target_config, shadow.current_config
  );

  /* What POST /pigeon/shadow/update would carry to move current_config
   * toward the desired state. */
  struct pigeon_shadow_update_request update = {
      .target_config = "{\"report_interval_s\":60}",
  };

  LOG_INF("shadow update request target_config=%s", update.target_config);

  return 0;
}
