/** @file wifi_connection_manager.h
 *  @brief WiFi bring-up/teardown for the ESP32-C6 wifi_init sample, mirroring
 *  https_init/coap_tcp_init's connection_manager.h (lte_connect/lte_disconnect)
 *  but over the generic conn_mgr_all_if_*() calls with no LTE modem beneath
 *  them.
 */
#ifndef WIFI_CONNECTION_MANAGER_H
#define WIFI_CONNECTION_MANAGER_H

extern struct k_sem network_connection_sem;

/* Matches connection_manager.h's JES_SEC_TAG=1 convention (both are plain
 * placeholder sec_tag enums, one per sample) -- CONFIG_PIGEON_HTTPS_SEC_TAG
 * defaults to 1, so this is what the CA cert below must be provisioned
 * under for pigeon_https.c's TLS handshake to find it. */
enum tls_sec_tags { NO_SEC_TAG, WIFI_SEC_TAG };

/** @fn int wifi_connect(void)
 *  @brief Brings up the ESP32-C6's WiFi station interface (static
 *  CONFIG_WIFI_CREDENTIALS_STATIC_SSID/_PASSWORD credentials), provisions
 *  the CA cert used for pigeon's HTTPS connector, and blocks until DHCP
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
