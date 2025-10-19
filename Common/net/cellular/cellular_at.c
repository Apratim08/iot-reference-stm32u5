/*
 * Cellular AT Command Implementation
 *
 * AT command handler for Sierra Wireless HL7688/HL7680 modem
 */

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "cellular_at.h"
#include "cellular_prv.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* AT command strings for Sierra Wireless HL7688/HL7680 */
#define AT_CMD_TEST              ""                    /* AT - Test command */
#define AT_CMD_GET_IMEI          "+CGSN"               /* Get IMEI */
#define AT_CMD_GET_ICCID         "!ICCID?"             /* Get SIM ICCID (Sierra specific) */
#define AT_CMD_GET_FW_VER        "I"                   /* Get firmware version */
#define AT_CMD_CHECK_SIM         "+CPIN?"              /* Check SIM status */
#define AT_CMD_GET_REG_STATUS    "+CREG?"              /* Get registration status */
#define AT_CMD_SET_REG_URC       "+CREG=1"             /* Enable registration URCs */
#define AT_CMD_GET_SIGNAL        "+CSQ"                /* Get signal quality */
#define AT_CMD_SET_PDP_CONTEXT   "+CGDCONT=1,\"IP\",\"" /* Set PDP context */
#define AT_CMD_ACTIVATE_PDP      "+CGACT=1,1"          /* Activate PDP context */
#define AT_CMD_START_PPP         "D*99#"               /* Start PPP session */
#define AT_CMD_ESCAPE_SEQ        "+++"                 /* Escape sequence (PPP -> AT) */

/* Response strings */
#define AT_RESP_OK              "OK"
#define AT_RESP_ERROR           "ERROR"
#define AT_RESP_CONNECT         "CONNECT"
#define AT_RESP_NO_CARRIER      "NO CARRIER"

/* Helper functions */
static BaseType_t prvWaitForResponse( CellularUartCtx_t * pxUartCtx,
                                      char * pcResponse,
                                      size_t xRespLen,
                                      uint32_t ulTimeoutMs );

static BaseType_t prvCheckResponseOk( const char * pcResponse );

static BaseType_t prvParseRegistrationStatus( const char * pcResponse, CellularRegState_t * pxRegState );

static BaseType_t prvParseSignalQuality( const char * pcResponse, int * plRssi );

BaseType_t xCellularAtInit( CellularNetConnCtx_t * pxCtx )
{
    BaseType_t xResult = pdFALSE;

    if( pxCtx == NULL )
    {
        LogError( "Cellular context is NULL" );
        return pdFALSE;
    }

    LogInfo( "Initializing cellular AT command interface" );

    /* Ensure UART is not in PPP mode */
    vCellularUartSetPppMode( &( pxCtx->xUartCtx ), pdFALSE );

    /* Give modem time to boot if it was just powered on */
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    /* Try to sync with modem */
    for( int i = 0; i < 5; i++ )
    {
        if( xCellularAtCheckModem( pxCtx ) == pdTRUE )
        {
            xResult = pdTRUE;
            break;
        }
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }

    if( xResult == pdTRUE )
    {
        LogInfo( "Cellular modem responding to AT commands" );

        /* Enable registration URCs */
        char pcResponse[ 128 ];
        xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_SET_REG_URC,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS );
    }
    else
    {
        LogError( "Failed to communicate with cellular modem" );
    }

    return xResult;
}

BaseType_t xCellularAtSendCommand( CellularUartCtx_t * pxUartCtx,
                                   const char * pcCmd,
                                   char * pcResponse,
                                   size_t xRespLen,
                                   uint32_t ulTimeoutMs )
{
    BaseType_t xResult = pdFALSE;
    char pcAtCmd[ CELLULAR_AT_CMD_MAX_LEN ];

    if( pxUartCtx == NULL || pcCmd == NULL )
    {
        return pdFALSE;
    }

    /* Build AT command with prefix and CRLF */
    if( strlen( pcCmd ) == 0 )
    {
        /* Just "AT" */
        snprintf( pcAtCmd, sizeof( pcAtCmd ), "AT\r\n" );
    }
    else
    {
        snprintf( pcAtCmd, sizeof( pcAtCmd ), "AT%s\r\n", pcCmd );
    }

    LogDebug( "TX: %s", pcAtCmd );

    /* Send command */
    if( xCellularUartSend( pxUartCtx, ( const uint8_t * ) pcAtCmd, strlen( pcAtCmd ) ) == pdTRUE )
    {
        /* Wait for and read response */
        if( pcResponse != NULL )
        {
            xResult = prvWaitForResponse( pxUartCtx, pcResponse, xRespLen, ulTimeoutMs );

            if( xResult == pdTRUE )
            {
                LogDebug( "RX: %s", pcResponse );
            }
        }
        else
        {
            /* No response expected */
            xResult = pdTRUE;
        }
    }
    else
    {
        LogError( "Failed to send AT command: %s", pcAtCmd );
    }

    return xResult;
}

BaseType_t xCellularAtCheckModem( CellularNetConnCtx_t * pxCtx )
{
    char pcResponse[ 64 ];

    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_TEST,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        return prvCheckResponseOk( pcResponse );
    }

    return pdFALSE;
}

BaseType_t xCellularAtGetModemInfo( CellularNetConnCtx_t * pxCtx )
{
    char pcResponse[ 256 ];

    /* Get IMEI */
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_GET_IMEI,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        /* Parse IMEI from response */
        char * pcImeiStart = strstr( pcResponse, "\r\n" );
        if( pcImeiStart != NULL )
        {
            pcImeiStart += 2;
            char * pcImeiEnd = strstr( pcImeiStart, "\r\n" );
            if( pcImeiEnd != NULL )
            {
                size_t xLen = pcImeiEnd - pcImeiStart;
                if( xLen <= CELLULAR_IMEI_LEN )
                {
                    memcpy( pxCtx->xModemInfo.pcImei, pcImeiStart, xLen );
                    pxCtx->xModemInfo.pcImei[ xLen ] = '\0';
                    LogInfo( "Modem IMEI: %s", pxCtx->xModemInfo.pcImei );
                }
            }
        }
    }

    /* Get firmware version */
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_GET_FW_VER,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        /* Parse first line of version info */
        char * pcVerStart = strstr( pcResponse, "\r\n" );
        if( pcVerStart != NULL )
        {
            pcVerStart += 2;
            char * pcVerEnd = strstr( pcVerStart, "\r\n" );
            if( pcVerEnd != NULL )
            {
                size_t xLen = pcVerEnd - pcVerStart;
                if( xLen < sizeof( pxCtx->xModemInfo.pcFirmwareVersion ) )
                {
                    memcpy( pxCtx->xModemInfo.pcFirmwareVersion, pcVerStart, xLen );
                    pxCtx->xModemInfo.pcFirmwareVersion[ xLen ] = '\0';
                    LogInfo( "Modem FW: %s", pxCtx->xModemInfo.pcFirmwareVersion );
                }
            }
        }
    }

    return pdTRUE;
}

BaseType_t xCellularAtCheckSim( CellularNetConnCtx_t * pxCtx )
{
    char pcResponse[ 128 ];

    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_CHECK_SIM,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        /* Look for "+CPIN: READY" */
        if( strstr( pcResponse, "READY" ) != NULL )
        {
            LogInfo( "SIM card is ready" );
            return pdTRUE;
        }
        else
        {
            LogWarn( "SIM card not ready: %s", pcResponse );
        }
    }

    return pdFALSE;
}

BaseType_t xCellularAtSetApn( CellularNetConnCtx_t * pxCtx )
{
    char pcCmd[ CELLULAR_AT_CMD_MAX_LEN ];
    char pcResponse[ 128 ];

    if( strlen( pxCtx->xConfig.pcApn ) == 0 )
    {
        LogWarn( "APN not configured, using default" );
        /* Many carriers auto-detect APN */
        snprintf( pcCmd, sizeof( pcCmd ), "%s\"", AT_CMD_SET_PDP_CONTEXT );
    }
    else
    {
        snprintf( pcCmd, sizeof( pcCmd ), "%s%s\"",
                  AT_CMD_SET_PDP_CONTEXT,
                  pxCtx->xConfig.pcApn );
    }

    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                pcCmd,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        if( prvCheckResponseOk( pcResponse ) )
        {
            LogInfo( "APN configured: %s", pxCtx->xConfig.pcApn );
            return pdTRUE;
        }
    }

    return pdFALSE;
}

BaseType_t xCellularAtWaitForRegistration( CellularNetConnCtx_t * pxCtx, uint32_t ulTimeoutMs )
{
    TickType_t xStartTime = xTaskGetTickCount();
    TickType_t xTimeout = pdMS_TO_TICKS( ulTimeoutMs );
    char pcResponse[ 128 ];
    CellularRegState_t xRegState = CELLULAR_REG_NONE;

    LogInfo( "Waiting for network registration..." );

    while( ( xTaskGetTickCount() - xStartTime ) < xTimeout )
    {
        if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                    AT_CMD_GET_REG_STATUS,
                                    pcResponse,
                                    sizeof( pcResponse ),
                                    CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
        {
            if( prvParseRegistrationStatus( pcResponse, &xRegState ) == pdTRUE )
            {
                pxCtx->xModemInfo.xRegState = xRegState;

                if( xRegState == CELLULAR_REG_HOME || xRegState == CELLULAR_REG_ROAMING )
                {
                    LogInfo( "Network registered (state: %d)", xRegState );

                    /* Get signal quality */
                    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                                AT_CMD_GET_SIGNAL,
                                                pcResponse,
                                                sizeof( pcResponse ),
                                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
                    {
                        int lRssi = 0;
                        if( prvParseSignalQuality( pcResponse, &lRssi ) == pdTRUE )
                        {
                            pxCtx->xModemInfo.lRssi = lRssi;
                            LogInfo( "Signal strength: %d", lRssi );
                        }
                    }

                    return pdTRUE;
                }
                else if( xRegState == CELLULAR_REG_DENIED )
                {
                    LogError( "Network registration denied" );
                    return pdFALSE;
                }
            }
        }

        /* Wait before retry */
        vTaskDelay( pdMS_TO_TICKS( 2000 ) );
    }

    LogError( "Network registration timeout" );
    return pdFALSE;
}

BaseType_t xCellularAtStartPpp( CellularNetConnCtx_t * pxCtx )
{
    char pcResponse[ 128 ];

    LogInfo( "Starting PPP session..." );

    /* Activate PDP context first */
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_ACTIVATE_PDP,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        if( !prvCheckResponseOk( pcResponse ) )
        {
            LogWarn( "PDP activation failed, continuing anyway" );
        }
    }

    /* Start PPP with ATD*99# */
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_START_PPP,
                                pcResponse,
                                sizeof( pcResponse ),
                                10000 ) == pdTRUE )  /* Longer timeout for CONNECT */
    {
        /* Look for CONNECT response */
        if( strstr( pcResponse, AT_RESP_CONNECT ) != NULL )
        {
            LogInfo( "PPP session started - switching UART to PPP mode" );

            /* Switch UART to PPP mode - no more AT commands! */
            vCellularUartSetPppMode( &( pxCtx->xUartCtx ), pdTRUE );

            return pdTRUE;
        }
        else
        {
            LogError( "Failed to start PPP: %s", pcResponse );
        }
    }

    return pdFALSE;
}

BaseType_t xCellularAtStopPpp( CellularNetConnCtx_t * pxCtx )
{
    LogInfo( "Stopping PPP session..." );

    /* Send escape sequence to exit PPP mode */
    vTaskDelay( pdMS_TO_TICKS( 1000 ) );  /* Guard time before +++ */

    if( xCellularUartSend( &( pxCtx->xUartCtx ),
                          ( const uint8_t * ) AT_CMD_ESCAPE_SEQ,
                          3 ) == pdTRUE )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );  /* Guard time after +++ */

        /* Switch back to AT command mode */
        vCellularUartSetPppMode( &( pxCtx->xUartCtx ), pdFALSE );

        /* Verify we're back in AT mode */
        if( xCellularAtCheckModem( pxCtx ) == pdTRUE )
        {
            LogInfo( "Returned to AT command mode" );
            return pdTRUE;
        }
    }

    LogError( "Failed to exit PPP mode" );
    return pdFALSE;
}

/* Helper function implementations */

static BaseType_t prvWaitForResponse( CellularUartCtx_t * pxUartCtx,
                                      char * pcResponse,
                                      size_t xRespLen,
                                      uint32_t ulTimeoutMs )
{
    size_t xBytesRead = 0;
    TickType_t xTimeout = pdMS_TO_TICKS( ulTimeoutMs );
    TickType_t xStartTime = xTaskGetTickCount();

    memset( pcResponse, 0, xRespLen );

    while( ( xTaskGetTickCount() - xStartTime ) < xTimeout && xBytesRead < ( xRespLen - 1 ) )
    {
        uint8_t ucByte;
        TickType_t xRemaining = xTimeout - ( xTaskGetTickCount() - xStartTime );

        if( xCellularUartRecv( pxUartCtx, &ucByte, 1, xRemaining ) == pdTRUE )
        {
            pcResponse[ xBytesRead++ ] = ( char ) ucByte;
            pcResponse[ xBytesRead ] = '\0';

            /* Check if we got a complete response (ends with OK, ERROR, or CONNECT) */
            if( strstr( pcResponse, "\r\nOK\r\n" ) != NULL ||
                strstr( pcResponse, "\r\nERROR\r\n" ) != NULL ||
                strstr( pcResponse, "CONNECT" ) != NULL )
            {
                return pdTRUE;
            }
        }
    }

    return ( xBytesRead > 0 ) ? pdTRUE : pdFALSE;
}

static BaseType_t prvCheckResponseOk( const char * pcResponse )
{
    return ( strstr( pcResponse, AT_RESP_OK ) != NULL ) ? pdTRUE : pdFALSE;
}

static BaseType_t prvParseRegistrationStatus( const char * pcResponse, CellularRegState_t * pxRegState )
{
    /* Parse "+CREG: n,stat" response */
    const char * pcCregStart = strstr( pcResponse, "+CREG:" );

    if( pcCregStart != NULL )
    {
        int n, stat;
        if( sscanf( pcCregStart, "+CREG: %d,%d", &n, &stat ) == 2 )
        {
            *pxRegState = ( CellularRegState_t ) stat;
            return pdTRUE;
        }
    }

    return pdFALSE;
}

static BaseType_t prvParseSignalQuality( const char * pcResponse, int * plRssi )
{
    /* Parse "+CSQ: rssi,ber" response */
    const char * pcCsqStart = strstr( pcResponse, "+CSQ:" );

    if( pcCsqStart != NULL )
    {
        int rssi, ber;
        if( sscanf( pcCsqStart, "+CSQ: %d,%d", &rssi, &ber ) == 2 )
        {
            *plRssi = rssi;
            return pdTRUE;
        }
    }

    return pdFALSE;
}
