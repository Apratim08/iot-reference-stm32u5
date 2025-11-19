/*
 * Cellular PPP Implementation
 *
 * lwIP PPPoS (PPP over Serial) integration for cellular modem
 * Bridges UART <-> lwIP PPP stack
 */

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "cellular_prv.h"

/* lwIP PPP includes */
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/pppapi.h"
#include "lwip/dns.h"

#include <string.h>

/* PPP configuration */
#define CELLULAR_PPP_TASK_PRIORITY       ( tskIDLE_PRIORITY + 3 )
#define CELLULAR_PPP_TASK_STACK_SIZE     ( configMINIMAL_STACK_SIZE * 4 )

/* Forward declarations */
static u32_t prvPppOutputCallback( ppp_pcb * pcb, u8_t * pucData, u32_t ulLen, void * pvCtx );
static void prvPppLinkStatusCallback( ppp_pcb * pcb, int errCode, void * pvCtx );

BaseType_t xCellularPppInit( CellularContext_t * pxCtx )
{
    if( pxCtx == NULL )
    {
        LogError( "Cellular context is NULL" );
        return pdFALSE;
    }

    LogInfo( "Initializing PPP interface..." );
    LogInfo( "netif addr: %p, output callback: %p, status callback: %p, ctx: %p",
             &( pxCtx->xPppNetif ), prvPppOutputCallback, prvPppLinkStatusCallback, pxCtx );

    /* Create PPP control block (thread-safe via pppapi) */
    pxCtx->pxPppPcb = pppapi_pppos_create( &( pxCtx->xPppNetif ),
                                           prvPppOutputCallback,
                                           prvPppLinkStatusCallback,
                                           pxCtx );

    LogInfo( "pppos_create returned: %p", pxCtx->pxPppPcb );

    if( pxCtx->pxPppPcb == NULL )
    {
        LogError( "Failed to create PPP control block - likely out of memory or MEMP_NUM_PPP_PCB too low" );
        return pdFALSE;
    }

    ppp_pcb * pxPppPcb = ( ppp_pcb * ) pxCtx->pxPppPcb;

    /* Set PPP authentication
     * Cellular networks typically don't require credentials (SIM-based auth),
     * but some modems request PAP/CHAP during negotiation with empty credentials.
     * Using PPPAUTHTYPE_ANY allows lwIP to accept whatever the modem requests.
     */
    ppp_set_auth( pxPppPcb, PPPAUTHTYPE_ANY, "", "" );

    /* Configure IPCP to request DNS servers from the peer (cellular modem)
     * This is critical for successful IPCP negotiation with cellular modems.
     */
    pxPppPcb->settings.usepeerdns = 1;

    /* Accept PAP/CHAP authentication if the modem requests it
     * Most cellular modems don't require auth (SIM-based), but accept if requested
     */
    pxPppPcb->settings.refuse_pap = 0;       /* Accept PAP if requested */
    pxPppPcb->settings.refuse_chap = 0;      /* Accept CHAP if requested */

    /* Set default route through this interface */
    ppp_set_default( pxPppPcb );

    LogInfo( "PPP interface initialized" );

    return pdTRUE;
}

BaseType_t xCellularPppStart( CellularContext_t * pxCtx )
{
    if( pxCtx == NULL || pxCtx->pxPppPcb == NULL )
    {
        LogError( "Invalid PPP context" );
        return pdFALSE;
    }

    LogInfo( "Starting PPP connection..." );

    /* Clear exit flag */
    pxCtx->xPppTaskExit = pdFALSE;

    /* Start PPP session (thread-safe via pppapi) */
    err_t err = pppapi_connect( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );
    if( err != ERR_OK )
    {
        LogError( "Failed to start PPP connection: %d", err );
        return pdFALSE;
    }

    /* Create PPP bridge task */
    if( xTaskCreate( vCellularPppTask,
                     "CellularPpp",
                     CELLULAR_PPP_TASK_STACK_SIZE,
                     pxCtx,
                     CELLULAR_PPP_TASK_PRIORITY,
                     &( pxCtx->xPppTaskHandle ) ) != pdPASS )
    {
        LogError( "Failed to create PPP task" );
        ppp_close( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );
        return pdFALSE;
    }

    return pdTRUE;
}

BaseType_t xCellularPppStop( CellularContext_t * pxCtx )
{
    if( pxCtx == NULL || pxCtx->pxPppPcb == NULL )
    {
        return pdFALSE;
    }

    LogInfo( "Stopping PPP connection..." );

    /* Signal PPP task to exit gracefully */
    if( pxCtx->xPppTaskHandle != NULL )
    {
        pxCtx->xPppTaskExit = pdTRUE;

        /* Wait for task to exit (up to 1 second) */
        uint32_t ulWaitCount = 0;
        while( eTaskGetState( pxCtx->xPppTaskHandle ) != eDeleted && ulWaitCount < 100 )
        {
            vTaskDelay( pdMS_TO_TICKS( 10 ) );
            ulWaitCount++;
        }

        if( eTaskGetState( pxCtx->xPppTaskHandle ) != eDeleted )
        {
            LogWarn( "PPP task did not exit gracefully, force deleting" );
            vTaskDelete( pxCtx->xPppTaskHandle );
        }

        pxCtx->xPppTaskHandle = NULL;
    }

    /* Close PPP session (thread-safe via pppapi) */
    pppapi_close( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );

    /* Small delay to allow PPP to terminate cleanly */
    vTaskDelay( pdMS_TO_TICKS( 500 ) );

    return pdTRUE;
}

/*
 * PPP Task - bridges UART RX data to lwIP PPP stack
 *
 * This task continuously reads data from the UART and feeds it to lwIP's PPP stack.
 * Data from PPP to UART is handled via the output callback.
 */
void vCellularPppTask( void * pvParameters )
{
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvParameters;
    uint8_t ucBuffer[ CELLULAR_PPP_RX_BUFFER_SIZE ];

    LogInfo( "PPP bridge task started" );

    static uint32_t ulRxCount = 0;

    while( pxCtx->xPppTaskExit == pdFALSE )
    {
        /* Read data from UART (with timeout) */
        size_t xBytesRead = xStreamBufferReceive( pxCtx->xUartCtx.xRxBuffer,
                                                  ucBuffer,
                                                  sizeof( ucBuffer ),
                                                  pdMS_TO_TICKS( 100 ) );

        if( xBytesRead > 0 )
        {
            /* Log packet reception for debugging (keep minimal to avoid spam) */
            if( ( ulRxCount % 10 ) == 0 || xBytesRead < 200 )
            {
                /* Log small packets (like DNS responses) or every 10th packet */
                LogInfo( "PPP RX: %lu bytes (total: %lu)", xBytesRead, ulRxCount );
            }
            ulRxCount++;

            /* Feed received data to PPP stack */
            pppos_input( ( ppp_pcb * ) pxCtx->pxPppPcb, ucBuffer, ( int ) xBytesRead );
        }

        /* Yield to other tasks periodically */
        taskYIELD();
    }

    LogInfo( "PPP bridge task exiting" );

    /* Task will self-delete */
    vTaskDelete( NULL );
}

/*
 * PPP Output Callback - called by lwIP when PPP needs to send data
 *
 * This function is called from lwIP context to send PPP frames over UART
 */
static u32_t prvPppOutputCallback( ppp_pcb * pcb, u8_t * pucData, u32_t ulLen, void * pvCtx )
{
    ( void ) pcb;  /* Unused parameter */
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvCtx;

    if( pxCtx == NULL || pucData == NULL || ulLen == 0 )
    {
        return 0;
    }

    /* Log packet transmission for debugging (keep minimal to avoid spam) */
    static uint32_t ulTxCount = 0;
    if( ( ulTxCount % 10 ) == 0 || ulLen < 200 )
    {
        /* Log small packets (like DNS queries) or every 10th packet */
        LogInfo( "PPP TX: %lu bytes (total: %lu)", ulLen, ulTxCount );
    }
    ulTxCount++;

    /* Send data to UART */
    if( xCellularUartSend( &( pxCtx->xUartCtx ), pucData, ulLen ) == pdTRUE )
    {
        return ulLen;
    }

    LogError( "PPP TX failed: UART send error" );
    return 0;
}

/*
 * PPP Link Status Callback - called when PPP connection state changes
 */
static void prvPppLinkStatusCallback( ppp_pcb * pcb, int errCode, void * pvCtx )
{
    ( void ) pcb;  /* Unused - minimize processing in lwIP callback */
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvCtx;

    switch( errCode )
    {
        case PPPERR_NONE:
        {
            /* PPP connection is up
             * IMPORTANT: This callback runs in lwIP's tcpip thread context.
             * Keep processing minimal - just update state and notify.
             * Defer logging to avoid potential deadlocks with LWIP_TCPIP_CORE_LOCKING.
             */

            /* Update status */
            pxCtx->xStatus = CELLULAR_STATUS_PPP_RUNNING;

            /* Notify management task - this will handle logging in its own context */
            if( pxCtx->xNetTaskHandle != NULL )
            {
                xTaskNotifyIndexed( pxCtx->xNetTaskHandle,
                                   CELLULAR_NET_EVT_IDX,
                                   CELLULAR_EVT_CONNECTED | CELLULAR_EVT_IP_ACQUIRED,
                                   eSetBits );
            }
            break;
        }

        case PPPERR_PARAM:
            LogError( "PPP error: Invalid parameter" );
            break;

        case PPPERR_OPEN:
            LogError( "PPP error: Unable to open PPP session" );
            break;

        case PPPERR_DEVICE:
            LogError( "PPP error: Invalid I/O device" );
            break;

        case PPPERR_ALLOC:
            LogError( "PPP error: Unable to allocate resources" );
            break;

        case PPPERR_USER:
            LogInfo( "PPP connection terminated by user" );
            break;

        case PPPERR_CONNECT:
            LogError( "PPP error: Connection lost" );
            break;

        case PPPERR_AUTHFAIL:
            LogError( "PPP error: Authentication failed" );
            break;

        case PPPERR_PROTOCOL:
            LogError( "PPP error: Protocol error" );
            break;

        case PPPERR_PEERDEAD:
            LogError( "PPP error: Peer is dead" );
            break;

        case PPPERR_IDLETIMEOUT:
            LogInfo( "PPP connection idle timeout" );
            break;

        case PPPERR_CONNECTTIME:
            LogError( "PPP error: Connect time limit reached" );
            break;

        case PPPERR_LOOPBACK:
            LogError( "PPP error: Loopback detected" );
            break;

        default:
            LogError( "PPP unknown error: %d", errCode );
            break;
    }

    /* Handle disconnection */
    if( errCode != PPPERR_NONE )
    {
        pxCtx->xStatus = CELLULAR_STATUS_DISCONNECTED;

        /* Notify management task */
        if( pxCtx->xNetTaskHandle != NULL )
        {
            xTaskNotifyIndexed( pxCtx->xNetTaskHandle,
                               CELLULAR_NET_EVT_IDX,
                               CELLULAR_EVT_DISCONNECTED,
                               eSetBits );
        }
    }
}
