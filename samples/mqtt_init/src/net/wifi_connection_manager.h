/** @file wifi_connection_manager.h
 *  @brief WiFi bring-up/teardown for the ESP32-C6 mqtt_init sample, copied
 *  from wifi_init/ws_init (same board, same radio, same conn_mgr flow) with
 *  one difference: what gets provisioned before the link comes up is the
 *  MQTT broker's trust anchor, and only on a certificate build -- see this
 *  repo's README for why these files are copies rather than shared.
 */
#ifndef WIFI_CONNECTION_MANAGER_H
#define WIFI_CONNECTION_MANAGER_H

extern struct k_sem network_connection_sem;

/* The same plain placeholder sec_tag enum the other samples carry, one per
 * sample -- CONFIG_PIGEON_MQTT_SEC_TAG defaults to 1, so this is where a
 * certificate build's trust anchor must be provisioned for the MQTT
 * connector's TLS handshake to find it (and where pigeon registers the PSK
 * itself on a PSK build). */
enum tls_sec_tags { NO_SEC_TAG, BROKER_SEC_TAG };

/** @fn int wifi_connect(void)
 *  @brief Brings up the ESP32-C6's WiFi station interface (static
 *  CONFIG_WIFI_CREDENTIALS_STATIC_SSID/_PASSWORD credentials), provisions
 *  the broker's CA cert on a certificate build, and blocks until DHCP
 *  assigns an address.
 *
 *  A single join attempt is bounded (WIFI_CONNECT_TIMEOUT), but a failed
 *  attempt is retried indefinitely with backoff rather than returned as an
 *  error -- real hardware has shown the join itself to be flaky across
 *  otherwise-identical boots, and a headless device has no operator to
 *  retry it manually. Only returns non-zero for a one-time setup failure
 *  (CA cert provisioning, no WiFi interface found) that a join retry can't
 *  fix.
 */
int wifi_connect(void);

/** @fn int wifi_disconnect(void)
 *  @brief Tears the WiFi interface back down. Unlike lte_disconnect() there
 *  is no modem reset-loop protection to worry about here -- see this
 *  sample's README note -- so this is a plain conn_mgr teardown with no
 *  extra graceful-power-off step.
 */
int wifi_disconnect(void);
#endif
