#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(shadow_model, LOG_LEVEL_INF);

/* Builds the shadow structs a connector exchanges with the platform, with no
 * transport underneath, and logs what a sync would send and receive. */
int main(void) {
  /* A connector reads its endpoint and token from Kconfig, not from here. */
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0003",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  int err = pigeon_init(&config);
  if (err) {
    return err;
  }

  /* What a shadow fetch returns. */
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

  /* What a shadow update carries to move current_config toward the target. */
  struct pigeon_shadow_update_request update = {
      .target_config = "{\"report_interval_s\":60}",
  };

  LOG_INF("shadow update request target_config=%s", update.target_config);

  return 0;
}
