/** @file connection_manager.h
 *  @brief Macros and function defines for the zephyr connection manager.
 */
#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

extern struct k_sem network_connection_sem;

/** @fn int lte_connect(void)
 *  @brief Brings the network interface up and blocks until connected (or the
 *  connect attempt times out).
 */
int lte_connect(void);

/** @fn int lte_disconnect(void)
 *  @brief Tears the network interface down and gracefully powers the modem
 *  off (CFUN=0).
 */
int lte_disconnect(void);
#endif
