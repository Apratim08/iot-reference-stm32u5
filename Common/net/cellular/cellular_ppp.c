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
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
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

    /* Create PPP control block */
    pxCtx->pxPppPcb = pppos_create( &( pxCtx->xPppNetif ),
                                    prvPppOutputCallback,
                                    prvPppLinkStatusCallback,
                                    pxCtx );

    if( pxCtx->pxPppPcb == NULL )
    {
        LogError( "Failed to create PPP control block" );
        return pdFALSE;
    }

    /* Set PPP authentication (usually not needed for cellular) */
    ppp_set_auth( ( ppp_pcb * ) pxCtx->pxPppPcb, PPPAUTHTYPE_NONE, "", "" );

    /* Set default route through this interface */
    ppp_pcb * pxPppPcb = ( ppp_pcb * ) pxCtx->pxPppPcb;
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

    /* Start PPP session */
    err_t err = ppp_connect( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );
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

    /* Delete PPP task first */
    if( pxCtx->xPppTaskHandle != NULL )
    {
        vTaskDelete( pxCtx->xPppTaskHandle );
        pxCtx->xPppTaskHandle = NULL;
    }

    /* Close PPP session */
    ppp_close( ( ppp_pcb * ) pxCtx->pxPppPcb, 0 );

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

    while( 1 )
    {
        /* Read data from UART (with timeout) */
        size_t xBytesRead = xMessageBufferReceive( pxCtx->xUartCtx.xRxBuffer,
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
