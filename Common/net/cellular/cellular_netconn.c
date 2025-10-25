/*
 * Cellular Network Connection Manager
 *
 * Main state machine for cellular modem initialization, network registration,
 * PPP connection establishment, and connection monitoring/recovery.
 */

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "cellular_netconn.h"
#include "cellular_prv.h"

#include <string.h>

/* Task configuration */
#define CELLULAR_NET_TASK_PRIORITY      ( tskIDLE_PRIORITY + 2 )
#define CELLULAR_NET_TASK_STACK_SIZE    ( configMINIMAL_STACK_SIZE * 3 )

/* Connection state machine states */
typedef enum
{
    CELLULAR_STATE_INIT = 0,
    CELLULAR_STATE_LWIP_INIT,
    CELLULAR_STATE_LWIP_WAIT_READY,
    CELLULAR_STATE_MODEM_POWER_ON,
    CELLULAR_STATE_MODEM_CHECK,
    CELLULAR_STATE_MODEM_CONFIGURE,
    CELLULAR_STATE_SIM_CHECK,
    CELLULAR_STATE_NETWORK_REGISTER,
    CELLULAR_STATE_APN_CONFIG,
    CELLULAR_STATE_PPP_START,
    CELLULAR_STATE_PPP_WAIT_CONNECT,
    CELLULAR_STATE_CONNECTED,
    CELLULAR_STATE_MONITOR,
    CELLULAR_STATE_DISCONNECTED,
    CELLULAR_STATE_ERROR,
    CELLULAR_STATE_RETRY
} CellularState_t;

/* Global cellular context */
static CellularContext_t xCellularContext;

/* Forward declarations */
static void prvCellularNetTask( void * pvParameters );
static void prvLwipReadyCallback( void * pvCtx );
static const char * prvGetStateName( CellularState_t xState );

void cellular_net_main( void * pvParameters )
{
    ( void ) pvParameters;

    LogInfo( "Starting cellular network service..." );

    /* Initialize context */
    memset( &xCellularContext, 0, sizeof( CellularContext_t ) );

    /* Set APN for Sierra Wireless modem */
    strncpy( xCellularContext.pcApn, "data.mono", CELLULAR_APN_MAX_LEN - 1 );

    /* Set task handle for this task */
    xCellularContext.xNetTaskHandle = xTaskGetCurrentTaskHandle();

    /* Initialize lwIP TCP/IP stack */
    LogInfo( "Initializing lwIP TCP/IP stack..." );
    tcpip_init( prvLwipReadyCallback, &xCellularContext );

    /* Initialize UART driver */
    if( xCellularUartInit( &( xCellularContext.xUartCtx ) ) != pdTRUE )
    {
        LogError( "Failed to initialize cellular UART" );
        /* Critical failure - enter infinite error loop */
        for( ; ; )
        {
            vTaskDelay( pdMS_TO_TICKS( 60000 ) );
        }
    }

    LogInfo( "Cellular network service started" );

    /* Run the state machine directly in this task */
    prvCellularNetTask( &xCellularContext );
}

BaseType_t cellular_net_request_reconnect( void )
{
    if( xCellularContext.xNetTaskHandle == NULL )
    {
        return pdFALSE;
    }

    /* Send reconnect request to management task */
    xTaskNotifyIndexed( xCellularContext.xNetTaskHandle,
                       CELLULAR_NET_EVT_IDX,
                       CELLULAR_EVT_RECONNECT_REQ,
                       eSetBits );

    LogInfo( "Reconnection requested" );
    return pdTRUE;
}

const char * cellular_net_get_status( void )
{
    switch( xCellularContext.xStatus )
    {
        case CELLULAR_STATUS_NONE:
            return "Not initialized";
        case CELLULAR_STATUS_INITIALIZING:
            return "Initializing";
        case CELLULAR_STATUS_SIM_READY:
            return "SIM ready";
        case CELLULAR_STATUS_REGISTERED:
            return "Network registered";
        case CELLULAR_STATUS_CONNECTED:
            return "Connecting";
        case CELLULAR_STATUS_PPP_RUNNING:
            return "Connected";
        case CELLULAR_STATUS_DISCONNECTED:
            return "Disconnected";
        case CELLULAR_STATUS_ERROR:
            return "Error";
        default:
            return "Unknown";
    }
}

/*
 * lwIP Ready Callback
 *
 * Called by tcpip_init() when the TCP/IP thread is ready.
 * This signals that lwIP memory pools are initialized and PPP can be created.
 */
static void prvLwipReadyCallback( void * pvCtx )
{
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvCtx;

    LogInfo( "lwIP TCP/IP stack is ready" );

    if( pxCtx->xNetTaskHandle != NULL )
    {
        ( void ) xTaskNotifyIndexed( pxCtx->xNetTaskHandle,
                                     CELLULAR_NET_EVT_IDX,
                                     CELLULAR_EVT_LWIP_READY,
                                     eSetBits );
    }
}

/*
 * Cellular Network Management Task
 *
 * Main state machine that manages the entire connection lifecycle
 */
static void prvCellularNetTask( void * pvParameters )
{
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvParameters;
    CellularState_t xState = CELLULAR_STATE_INIT;
    uint32_t ulRetryCount = 0;
    const uint32_t ulMaxRetries = 3;
    uint32_t ulNotificationValue;

    LogInfo( "Cellular network management task started" );

    while( 1 )
    {
        LogDebug( "State: %s", prvGetStateName( xState ) );

        switch( xState )
        {
            case CELLULAR_STATE_INIT:
                pxCtx->xStatus = CELLULAR_STATUS_INITIALIZING;
                xState = CELLULAR_STATE_LWIP_WAIT_READY;
                break;

            case CELLULAR_STATE_LWIP_WAIT_READY:
                /* Wait for lwIP to be ready (callback from tcpip_init) */
                if( xTaskNotifyWaitIndexed( CELLULAR_NET_EVT_IDX,
                                           0x00,
                                           CELLULAR_EVT_LWIP_READY,
                                           &ulNotificationValue,
                                           portMAX_DELAY ) == pdTRUE )
                {
                    if( ulNotificationValue & CELLULAR_EVT_LWIP_READY )
                    {
                        LogInfo( "lwIP initialized successfully, proceeding to modem initialization" );
                        xState = CELLULAR_STATE_MODEM_POWER_ON;
                    }
                }
                break;

            case CELLULAR_STATE_MODEM_POWER_ON:
                LogInfo( "Powering on modem..." );
                /* Note: GPIO power control would go here if needed */
                /* For HL7688, it auto-powers on when voltage is applied */
                vTaskDelay( pdMS_TO_TICKS( 3000 ) );  /* Allow modem to boot */
                xState = CELLULAR_STATE_MODEM_CHECK;
                break;

            case CELLULAR_STATE_MODEM_CHECK:
                LogInfo( "Checking modem communication..." );
                if( xCellularAtInit( pxCtx ) == pdTRUE )
                {
                    xState = CELLULAR_STATE_MODEM_CONFIGURE;
                    ulRetryCount = 0;
                }
                else
                {
                    LogWarn( "Modem not responding, retry %lu/%lu", ulRetryCount + 1, ulMaxRetries );
                    ulRetryCount++;
                    if( ulRetryCount >= ulMaxRetries )
                    {
                        xState = CELLULAR_STATE_ERROR;
                    }
                    else
                    {
                        vTaskDelay( pdMS_TO_TICKS( 5000 ) );
                    }
                }
                break;

            case CELLULAR_STATE_MODEM_CONFIGURE:
                LogInfo( "Configuring modem..." );
                /* Get modem info (IMEI, firmware version, etc.) */
                xCellularAtGetModemInfo( pxCtx );
                xState = CELLULAR_STATE_SIM_CHECK;
                break;

            case CELLULAR_STATE_SIM_CHECK:
                LogInfo( "Checking SIM card..." );
                if( xCellularAtCheckSim( pxCtx ) == pdTRUE )
                {
                    pxCtx->xStatus = CELLULAR_STATUS_SIM_READY;
                    xState = CELLULAR_STATE_NETWORK_REGISTER;
                    ulRetryCount = 0;
                }
                else
                {
                    LogWarn( "SIM not ready, retry %lu/%lu", ulRetryCount + 1, ulMaxRetries );
                    ulRetryCount++;
                    if( ulRetryCount >= ulMaxRetries )
                    {
                        xState = CELLULAR_STATE_ERROR;
                    }
                    else
                    {
                        vTaskDelay( pdMS_TO_TICKS( 5000 ) );
                    }
                }
                break;

            case CELLULAR_STATE_NETWORK_REGISTER:
                LogInfo( "Waiting for network registration..." );
                if( xCellularAtWaitForRegistration( pxCtx, CELLULAR_REGISTRATION_TIMEOUT_MS ) == pdTRUE )
                {
                    pxCtx->xStatus = CELLULAR_STATUS_REGISTERED;
                    xState = CELLULAR_STATE_APN_CONFIG;
                    ulRetryCount = 0;
                }
                else
                {
                    LogWarn( "Network registration failed, retry %lu/%lu", ulRetryCount + 1, ulMaxRetries );
                    ulRetryCount++;
                    if( ulRetryCount >= ulMaxRetries )
                    {
                        xState = CELLULAR_STATE_ERROR;
                    }
                    else
                    {
                        vTaskDelay( pdMS_TO_TICKS( 5000 ) );
                    }
                }
                break;

            case CELLULAR_STATE_APN_CONFIG:
                LogInfo( "Configuring APN..." );
                if( xCellularAtSetApn( pxCtx ) == pdTRUE )
                {
                    xState = CELLULAR_STATE_PPP_START;
                }
                else
                {
                    LogWarn( "APN configuration failed, continuing anyway" );
                    xState = CELLULAR_STATE_PPP_START;
                }
                break;

            case CELLULAR_STATE_PPP_START:
                LogInfo( "Initializing PPP..." );

                /* Initialize PPP interface */
                if( xCellularPppInit( pxCtx ) != pdTRUE )
                {
                    LogError( "Failed to initialize PPP" );
                    xState = CELLULAR_STATE_ERROR;
                    break;
                }

                /* Start PPP session (AT command) */
                if( xCellularAtStartPpp( pxCtx ) == pdTRUE )
                {
                    pxCtx->xStatus = CELLULAR_STATUS_CONNECTED;

                    /* Start PPP stack */
                    if( xCellularPppStart( pxCtx ) == pdTRUE )
                    {
                        xState = CELLULAR_STATE_PPP_WAIT_CONNECT;
                    }
                    else
                    {
                        LogError( "Failed to start PPP stack" );
                        xState = CELLULAR_STATE_ERROR;
                    }
                }
                else
                {
                    LogError( "Failed to start PPP AT command" );
                    xState = CELLULAR_STATE_ERROR;
                }
                break;

            case CELLULAR_STATE_PPP_WAIT_CONNECT:
                /* Wait for PPP connection event */
                if( xTaskNotifyWaitIndexed( CELLULAR_NET_EVT_IDX,
                                           0x00,
                                           CELLULAR_EVT_CONNECTED | CELLULAR_EVT_DISCONNECTED,
                                           &ulNotificationValue,
                                           pdMS_TO_TICKS( 30000 ) ) == pdTRUE )
                {
                    if( ulNotificationValue & CELLULAR_EVT_CONNECTED )
                    {
                        LogInfo( "PPP connection successful!" );
                        xState = CELLULAR_STATE_MONITOR;
                        ulRetryCount = 0;
                    }
                    else if( ulNotificationValue & CELLULAR_EVT_DISCONNECTED )
                    {
                        LogError( "PPP connection failed" );
                        xState = CELLULAR_STATE_DISCONNECTED;
                    }
                }
                else
                {
                    LogError( "PPP connection timeout" );
                    xState = CELLULAR_STATE_DISCONNECTED;
                }
                break;

            case CELLULAR_STATE_MONITOR:
                /* Wait for events (disconnection, reconnect request) */
                if( xTaskNotifyWaitIndexed( CELLULAR_NET_EVT_IDX,
                                           0x00,
                                           CELLULAR_EVT_DISCONNECTED | CELLULAR_EVT_RECONNECT_REQ,
                                           &ulNotificationValue,
                                           pdMS_TO_TICKS( 10000 ) ) == pdTRUE )
                {
                    if( ulNotificationValue & CELLULAR_EVT_DISCONNECTED )
                    {
                        LogWarn( "Connection lost, will attempt to reconnect" );
                        xState = CELLULAR_STATE_DISCONNECTED;
                    }
                    else if( ulNotificationValue & CELLULAR_EVT_RECONNECT_REQ )
                    {
                        LogInfo( "Manual reconnect requested" );
                        xState = CELLULAR_STATE_DISCONNECTED;
                    }
                }
                /* Else: timeout is normal, just keep monitoring */
                break;

            case CELLULAR_STATE_DISCONNECTED:
                pxCtx->xStatus = CELLULAR_STATUS_DISCONNECTED;

                /* Stop PPP */
                xCellularPppStop( pxCtx );

                /* Return to AT mode */
                xCellularAtStopPpp( pxCtx );

                /* Retry connection */
                xState = CELLULAR_STATE_RETRY;
                break;

            case CELLULAR_STATE_ERROR:
                pxCtx->xStatus = CELLULAR_STATUS_ERROR;
                LogError( "Cellular connection error - will retry in 30 seconds" );
                vTaskDelay( pdMS_TO_TICKS( 30000 ) );
                xState = CELLULAR_STATE_RETRY;
                break;

            case CELLULAR_STATE_RETRY:
                LogInfo( "Retrying cellular connection..." );
                ulRetryCount = 0;
                vTaskDelay( pdMS_TO_TICKS( 5000 ) );
                xState = CELLULAR_STATE_MODEM_CHECK;
                break;

            default:
                LogError( "Unknown state: %d", xState );
                xState = CELLULAR_STATE_ERROR;
                break;
        }

        /* Small yield to prevent task hogging */
        taskYIELD();
    }
}

static const char * prvGetStateName( CellularState_t xState )
{
    switch( xState )
    {
        case CELLULAR_STATE_INIT:                return "INIT";
        case CELLULAR_STATE_LWIP_INIT:           return "LWIP_INIT";
        case CELLULAR_STATE_LWIP_WAIT_READY:     return "LWIP_WAIT_READY";
        case CELLULAR_STATE_MODEM_POWER_ON:      return "MODEM_POWER_ON";
        case CELLULAR_STATE_MODEM_CHECK:         return "MODEM_CHECK";
        case CELLULAR_STATE_MODEM_CONFIGURE:     return "MODEM_CONFIGURE";
        case CELLULAR_STATE_SIM_CHECK:           return "SIM_CHECK";
        case CELLULAR_STATE_NETWORK_REGISTER:    return "NETWORK_REGISTER";
        case CELLULAR_STATE_APN_CONFIG:          return "APN_CONFIG";
        case CELLULAR_STATE_PPP_START:           return "PPP_START";
        case CELLULAR_STATE_PPP_WAIT_CONNECT:    return "PPP_WAIT_CONNECT";
        case CELLULAR_STATE_CONNECTED:           return "CONNECTED";
        case CELLULAR_STATE_MONITOR:             return "MONITOR";
        case CELLULAR_STATE_DISCONNECTED:        return "DISCONNECTED";
        case CELLULAR_STATE_ERROR:               return "ERROR";
        case CELLULAR_STATE_RETRY:               return "RETRY";
        default:                                 return "UNKNOWN";
    }
}
