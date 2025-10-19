/*
 * Cellular Network Connection Interface
 *
 * This file provides the public API for cellular network connectivity
 * for the STM32U5 IoT Reference project.
 */

#ifndef CELLULAR_NETCONN_H
#define CELLULAR_NETCONN_H

#include "FreeRTOS.h"

/**
 * @brief Main cellular network task entry point.
 *
 * This task handles cellular modem initialization, network registration,
 * PPP connection establishment, and connection monitoring.
 *
 * @param pvParameters Task parameters (unused)
 */
void cellular_net_main( void * pvParameters );

/**
 * @brief Request cellular network reconnection.
 *
 * This function can be called to request the cellular task to attempt
 * to reconnect to the network.
 *
 * @return pdTRUE if reconnection request was accepted, pdFALSE otherwise
 */
BaseType_t cellular_net_request_reconnect( void );

/**
 * @brief Get cellular network status.
 *
 * @return Network status string for display
 */
const char * cellular_net_get_status( void );

#endif /* CELLULAR_NETCONN_H */
