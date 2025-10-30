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

    /* Use lwIP's default IPCP configuration
     * Note: "Could not determine remote IP address: defaulting to 10.64.64.64"
     * is normal - RPi shows the same warning and works fine
     */

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

    /* CRITICAL: Create PPP bridge task BEFORE starting PPP connection
     * The task feeds RX data from UART to lwIP via pppos_input().
     * If we start PPP negotiation before the task exists, we may miss
     * early LCP frames from the modem, causing negotiation to fail.
     */
    if( xTaskCreate( vCellularPppTask,
                     "CellularPpp",
                     CELLULAR_PPP_TASK_STACK_SIZE,
                     pxCtx,
                     CELLULAR_PPP_TASK_PRIORITY,
                     &( pxCtx->xPppTaskHandle ) ) != pdPASS )
    {
        LogError( "Failed to create PPP task" );
        return pdFALSE;
    }

    /* Give the task time to start and begin monitoring RX buffer */
    vTaskDelay( pdMS_TO_TICKS( 50 ) );

    /* Now start PPP session (thread-safe via pppapi) */
    err_t err = pppapi_connect( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );
    if( err != ERR_OK )
    {
        LogError( "Failed to start PPP connection: %d", err );

        /* Clean up the task we just created */
        pxCtx->xPppTaskExit = pdTRUE;
        vTaskDelay( pdMS_TO_TICKS( 100 ) );
        if( pxCtx->xPppTaskHandle != NULL )
        {
            vTaskDelete( pxCtx->xPppTaskHandle );
            pxCtx->xPppTaskHandle = NULL;
        }

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

    while( pxCtx->xPppTaskExit == pdFALSE )
    {
        /* Read data from UART (with timeout) */
        size_t xBytesRead = xStreamBufferReceive( pxCtx->xUartCtx.xRxBuffer,
                                                  ucBuffer,
                                                  sizeof( ucBuffer ),
                                                  pdMS_TO_TICKS( 100 ) );

        if( xBytesRead > 0 )
        {
            /* Feed received data to PPP stack */
            pppos_input( ( ppp_pcb * ) pxCtx->pxPppPcb, ucBuffer, xBytesRead );
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
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvCtx;

    if( pxCtx == NULL || pucData == NULL || ulLen == 0 )
    {
        return 0;
    }

    /* Send data to UART */
    if( xCellularUartSend( &( pxCtx->xUartCtx ), pucData, ulLen ) == pdTRUE )
    {
        return ulLen;
    }

    return 0;
}

/*
 * PPP Link Status Callback - called when PPP connection state changes
 */
static void prvPppLinkStatusCallback( ppp_pcb * pcb, int errCode, void * pvCtx )
{
    CellularContext_t * pxCtx = ( CellularContext_t * ) pvCtx;
    struct netif * pxNetif = ppp_netif( pcb );

    switch( errCode )
    {
        case PPPERR_NONE:
        {
            /* PPP connection is up */
            LogInfo( "PPP connection established" );
            LogInfo( "   Local IP: %s", ip4addr_ntoa( netif_ip4_addr( pxNetif ) ) );
            LogInfo( "   Netmask:  %s", ip4addr_ntoa( netif_ip4_netmask( pxNetif ) ) );
            LogInfo( "   Gateway:  %s", ip4addr_ntoa( netif_ip4_gw( pxNetif ) ) );

            /* Get DNS servers */
            const ip_addr_t * pxDns1 = dns_getserver( 0 );
            const ip_addr_t * pxDns2 = dns_getserver( 1 );

            if( pxDns1 != NULL )
            {
                LogInfo( "   DNS1:     %s", ipaddr_ntoa( pxDns1 ) );
            }
            if( pxDns2 != NULL )
            {
                LogInfo( "   DNS2:     %s", ipaddr_ntoa( pxDns2 ) );
            }

            /* Update status */
            pxCtx->xStatus = CELLULAR_STATUS_PPP_RUNNING;

            /* Notify management task */
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
