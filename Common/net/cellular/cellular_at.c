/*
 * Cellular AT Command Implementation
 *
 * AT command handler for SIM7600G modem with Simplex SIM
 */

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "cellular_prv.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* AT commands*/
#define AT_CMD_TEST              ""                    /* AT - Test command */
#define AT_CMD_ECHO_OFF          "E0"                  /* Disable echo */
#define AT_CMD_GET_IMEI          "+CGSN"               /* Get IMEI */
#define AT_CMD_GET_FW_VER        "I"                   /* Get firmware version */
#define AT_CMD_CHECK_SIM         "+CPIN?"              /* Check SIM status */
#define AT_CMD_GET_REG_STATUS    "+CREG?"              /* Get registration status */
#define AT_CMD_GET_SIGNAL        "+CSQ"                /* Get signal quality */
#define AT_CMD_SET_PDP_CONTEXT   "+CGDCONT=1,\"IPV4V6\",\"" /* Set PDP context (matches RPi exactly) */
#define AT_CMD_START_PPP         "DT*99#"              /* Start PPP session (matches RPi5) */
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

BaseType_t xCellularAtInit( CellularContext_t * pxCtx )
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

    /* Flush any stale data from the stream buffer (especially important on retries) */
    uint8_t ucDummy[ 256 ];
    size_t xFlushed = 0;
    while( xCellularUartRecv( &( pxCtx->xUartCtx ), ucDummy, sizeof( ucDummy ), pdMS_TO_TICKS( 100 ) ) == pdTRUE )
    {
        xFlushed += sizeof( ucDummy );
        if( xFlushed > 4096 )  /* Safety limit to prevent infinite loop */
        {
            LogWarn( "Flushed %lu bytes from buffer - modem may be stuck", xFlushed );
            break;
        }
    }
    if( xFlushed > 0 )
    {
        LogInfo( "Flushed %lu stale bytes from RX buffer", xFlushed );
        /* Give the modem a moment to finish any pending output */
        vTaskDelay( pdMS_TO_TICKS( 500 ) );
    }

    /* Give modem time to boot if it was just powered on */
    vTaskDelay( pdMS_TO_TICKS( 2000 ) );

    char pcResponse[ 128 ];

    /* CRITICAL: Reset modem with ATZ (matches RPi5 chat script)
     * This clears the modem's internal message buffer, including boot URCs
     * (RDY, +CPIN: READY) that would otherwise be dumped during PPP transition.
     */
    LogInfo( "Resetting modem (ATZ) to clear internal buffers..." );
    for( int i = 0; i < 5; i++ )
    {
        if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                    "Z",  /* ATZ command */
                                    pcResponse,
                                    sizeof( pcResponse ),
                                    CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
        {
            if( strstr( pcResponse, "OK" ) != NULL )
            {
                LogInfo( "Modem reset successful" );
                xResult = pdTRUE;
                break;
            }
        }
        vTaskDelay( pdMS_TO_TICKS( 500 ) );
    }

    if( xResult == pdFALSE )
    {
        LogWarn( "ATZ failed, continuing anyway" );
    }

    /* Wait for modem to complete reset */
    vTaskDelay( pdMS_TO_TICKS( 1000 ) );

    /* CRITICAL: Flush boot URCs that arrived during ATZ/modem boot
     * The modem sends boot URCs (RDY, +CPIN: READY) when it finishes booting.
     * These may arrive AFTER we sent ATZ but BEFORE it responded.
     * They're sitting in our stream buffer and will corrupt PPP if not flushed.
     */
    LogInfo( "Flushing boot URCs after ATZ..." );
    uint8_t ucBootFlush[ 256 ];
    size_t xBootFlushed = 0;
    while( xCellularUartRecv( &( pxCtx->xUartCtx ), ucBootFlush, sizeof( ucBootFlush ), pdMS_TO_TICKS( 200 ) ) == pdTRUE )
    {
        xBootFlushed += sizeof( ucBootFlush );
        if( xBootFlushed > 2048 )
        {
            LogWarn( "Flushed %lu bytes after ATZ - modem may be chatty", xBootFlushed );
            break;
        }
    }
    if( xBootFlushed > 0 )
    {
        LogInfo( "Flushed %lu bytes of boot URCs after ATZ", xBootFlushed );
    }

    /* Now disable echo (RPi5 uses ATE1, we use ATE0)
     * CRITICAL: Disable echo IMMEDIATELY without any ready check!
     * Any command sent before ATE0 will be echoed and buffered by the modem,
     * causing PPP corruption later. Just keep sending ATE0 until it works.
     */
    LogInfo( "Disabling echo (no sync first to avoid buffer corruption)..." );
    for( int i = 0; i < 10; i++ )
    {
        if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                    AT_CMD_ECHO_OFF,
                                    pcResponse,
                                    sizeof( pcResponse ),
                                    CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
        {
            if( strstr( pcResponse, "OK" ) != NULL )
            {
                LogInfo( "Modem echo disabled" );
                xResult = pdTRUE;
                break;
            }
        }
        vTaskDelay( pdMS_TO_TICKS( 500 ) );
    }

    if( xResult == pdFALSE )
    {
        LogError( "Failed to disable echo" );
        return pdFALSE;
    }

    /* Enable radio functionality (critical for SIM7600G) */
    LogInfo( "Enabling radio functionality..." );
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                "+CFUN=1",
                                pcResponse,
                                sizeof( pcResponse ),
                                5000 ) == pdTRUE )
    {
        if( prvCheckResponseOk( pcResponse ) )
        {
            LogInfo( "Radio enabled" );
        }
    }
    vTaskDelay( pdMS_TO_TICKS( 1000 ) );  /* Allow radio to stabilize */

    LogInfo( "Cellular modem initialized (minimal sequence)" );
    return pdTRUE;
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
            else
            {
                LogError( "No response or timeout for command: %s", pcAtCmd );
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

BaseType_t xCellularAtCheckModem( CellularContext_t * pxCtx )
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

BaseType_t xCellularAtGetModemInfo( CellularContext_t * pxCtx )
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

BaseType_t xCellularAtCheckSim( CellularContext_t * pxCtx )
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

BaseType_t xCellularAtSetApn( CellularContext_t * pxCtx )
{
    char pcCmd[ CELLULAR_AT_CMD_MAX_LEN ];
    char pcResponse[ 128 ];

    if( strlen( pxCtx->pcApn ) == 0 )
    {
        LogWarn( "APN not configured, using default" );
        /* Many carriers auto-detect APN */
        snprintf( pcCmd, sizeof( pcCmd ), "%s\"", AT_CMD_SET_PDP_CONTEXT );
    }
    else
    {
        snprintf( pcCmd, sizeof( pcCmd ), "%s%s\"",
                  AT_CMD_SET_PDP_CONTEXT,
                  pxCtx->pcApn );
    }

    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                pcCmd,
                                pcResponse,
                                sizeof( pcResponse ),
                                CELLULAR_DEFAULT_TIMEOUT_MS ) == pdTRUE )
    {
        if( prvCheckResponseOk( pcResponse ) )
        {
            LogInfo( "APN configured: %s", pxCtx->pcApn );
            return pdTRUE;
        }
    }

    return pdFALSE;
}

BaseType_t xCellularAtWaitForRegistration( CellularContext_t * pxCtx, uint32_t ulTimeoutMs )
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

BaseType_t xCellularAtStartPpp( CellularContext_t * pxCtx )
{
    char pcResponse[ 128 ];

    LogInfo( "Starting PPP session..." );

    /* Minimal commands - match working manual sequence
     * No URC disables, no diagnostics, just start PPP with ATD*99# */
    if( xCellularAtSendCommand( &( pxCtx->xUartCtx ),
                                AT_CMD_START_PPP,
                                pcResponse,
                                sizeof( pcResponse ),
                                10000 ) == pdTRUE )  /* Longer timeout for CONNECT */
    {
        /* Look for CONNECT response */
        if( strstr( pcResponse, AT_RESP_CONNECT ) != NULL )
        {
            LogInfo( "CONNECT received - modem in PPP data mode" );

            /* Flush any residual AT response data from stream buffer.
             * The SIM7600G dumps its internal message buffer ~120ms after CONNECT.
             * We must wait for this dump and flush it before starting PPP.
             */
            uint8_t ucFlushBuf[ 256 ];
            size_t xFlushed;
            size_t xTotalFlushed = 0;

            /* Wait for modem to dump its internal buffer (empirically ~120ms) */
            vTaskDelay( pdMS_TO_TICKS( 200 ) );

            /* Aggressive flush loop to catch all buffered data */
            do {
                xFlushed = xStreamBufferReceive( pxCtx->xUartCtx.xRxBuffer,
                                                  ucFlushBuf,
                                                  sizeof( ucFlushBuf ),
                                                  pdMS_TO_TICKS( 300 ) );
                if( xFlushed > 0 )
                {
                    xTotalFlushed += xFlushed;

                    /* Log for debugging */
                    char pcHexDump[ 80 ];
                    size_t xHexPos = 0;
                    for( size_t i = 0; i < xFlushed && i < 20; i++ )
                    {
                        xHexPos += snprintf( &pcHexDump[ xHexPos ],
                                            sizeof( pcHexDump ) - xHexPos,
                                            "%02X ", ucFlushBuf[ i ] );
                    }
                    LogWarn( "Flushed %lu bytes: [%s]%s", xFlushed, pcHexDump,
                             xFlushed > 20 ? "..." : "" );
                }
            } while( xFlushed > 0 && xTotalFlushed < 2048 );

            LogInfo( "Flushed %lu total bytes before PPP", xTotalFlushed );

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

BaseType_t xCellularAtStopPpp( CellularContext_t * pxCtx )
{
    LogInfo( "Stopping PPP session..." );

    /* CRITICAL: Switch to AT mode BEFORE sending escape sequence
     * The escape sequence (++++) requires 1 second of silence before/after.
     * If the PPP task is still feeding data during guard time, the modem
     * won't recognize the escape sequence. We must stop PPP traffic first.
     */
    vCellularUartSetPppMode( &( pxCtx->xUartCtx ), pdFALSE );

    /* Wait for any in-flight PPP data to finish transmitting */
    vTaskDelay( pdMS_TO_TICKS( 200 ) );

    /* Flush stream buffer to discard any PPP frames */
    uint8_t ucFlushBuf[ 256 ];
    size_t xFlushed = 0;
    while( xStreamBufferReceive( pxCtx->xUartCtx.xRxBuffer,
                                  ucFlushBuf,
                                  sizeof( ucFlushBuf ),
                                  pdMS_TO_TICKS( 100 ) ) > 0 )
    {
        xFlushed += sizeof( ucFlushBuf );
        if( xFlushed > 2048 )
        {
            break;
        }
    }

    if( xFlushed > 0 )
    {
        LogInfo( "Flushed %lu bytes of PPP data before escape", xFlushed );
    }

    /* Send escape sequence to exit PPP mode */
    vTaskDelay( pdMS_TO_TICKS( 1000 ) );  /* Guard time before +++ */

    if( xCellularUartSend( &( pxCtx->xUartCtx ),
                          ( const uint8_t * ) AT_CMD_ESCAPE_SEQ,
                          3 ) == pdTRUE )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );  /* Guard time after +++ */

        /* Verify we're back in AT mode */
        if( xCellularAtCheckModem( pxCtx ) == pdTRUE )
        {
            LogInfo( "Returned to AT command mode" );
            return pdTRUE;
        }
        else
        {
            LogWarn( "Modem not responding after escape sequence" );
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
    BaseType_t xFirstByteReceived = pdFALSE;

    memset( pcResponse, 0, xRespLen );

    while( ( xTaskGetTickCount() - xStartTime ) < xTimeout && xBytesRead < ( xRespLen - 1 ) )
    {
        uint8_t ucByte;
        TickType_t xRemaining = xTimeout - ( xTaskGetTickCount() - xStartTime );

        if( xCellularUartRecv( pxUartCtx, &ucByte, 1, xRemaining ) == pdTRUE )
        {
            pcResponse[ xBytesRead++ ] = ( char ) ucByte;
            pcResponse[ xBytesRead ] = '\0';

            /* Check if we got a complete response (ends with OK, ERROR, or CONNECT)
             * Handle both with and without echo (e.g., "AT\r\r\nOK\r\n" or "\r\nOK\r\n") */
            if( strstr( pcResponse, "OK\r\n" ) != NULL ||
                strstr( pcResponse, "ERROR\r\n" ) != NULL ||
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
