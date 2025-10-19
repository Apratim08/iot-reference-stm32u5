/*
 * Cellular AT Command Interface
 *
 * AT command handler for Sierra Wireless HL7688/HL7680 modem
 */

#ifndef CELLULAR_AT_H
#define CELLULAR_AT_H

#include "cellular_prv.h"

/**
 * @brief Initialize AT command interface
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtInit( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Send AT command and wait for response
 *
 * @param pxUartCtx UART context
 * @param pcCmd Command string (without AT prefix or CRLF)
 * @param pcResponse Buffer for response
 * @param xRespLen Response buffer length
 * @param ulTimeoutMs Timeout in milliseconds
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtSendCommand( CellularUartCtx_t * pxUartCtx,
                                   const char * pcCmd,
                                   char * pcResponse,
                                   size_t xRespLen,
                                   uint32_t ulTimeoutMs );

/**
 * @brief Check if modem is responding
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if modem responds, pdFALSE otherwise
 */
BaseType_t xCellularAtCheckModem( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Get modem information (IMEI, firmware version, etc.)
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtGetModemInfo( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Check SIM card status
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if SIM is ready, pdFALSE otherwise
 */
BaseType_t xCellularAtCheckSim( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Set APN configuration
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtSetApn( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Wait for network registration
 *
 * @param pxCtx Cellular network context
 * @param ulTimeoutMs Timeout in milliseconds
 * @return pdTRUE if registered, pdFALSE otherwise
 */
BaseType_t xCellularAtWaitForRegistration( CellularNetConnCtx_t * pxCtx, uint32_t ulTimeoutMs );

/**
 * @brief Start PPP mode (switches modem from AT command mode to PPP)
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtStartPpp( CellularNetConnCtx_t * pxCtx );

/**
 * @brief Stop PPP mode (return to AT command mode)
 *
 * @param pxCtx Cellular network context
 * @return pdTRUE if successful, pdFALSE otherwise
 */
BaseType_t xCellularAtStopPpp( CellularNetConnCtx_t * pxCtx );

#endif /* CELLULAR_AT_H */
