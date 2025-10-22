/*
 * Cellular Driver Private Definitions
 *
 * Private header for cellular modem driver implementation.
 */

#ifndef CELLULAR_PRV_H
#define CELLULAR_PRV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Private definitions shared between cellular driver files */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "message_buffer.h"

/* lwIP includes */
#include "lwip/netif.h"
#include "lwip/tcpip.h"

/* Constants */
#define CELLULAR_UART_BAUD_RATE           115200
#define CELLULAR_UART_TIMEOUT_MS          1000
#define CELLULAR_DEFAULT_TIMEOUT_MS       5000
#define CELLULAR_CONNECT_TIMEOUT_MS       (120 * 1000)
#define CELLULAR_REGISTRATION_TIMEOUT_MS  (90 * 1000)

#define CELLULAR_AT_CMD_MAX_LEN           256
#define CELLULAR_AT_RESP_MAX_LEN          1024
#define CELLULAR_APN_MAX_LEN              64
#define CELLULAR_IMEI_LEN                 15
#define CELLULAR_ICCID_LEN                20

/* Task notification indices */
#define CELLULAR_NET_EVT_IDX              0x0

/* Network event bits */
#define CELLULAR_EVT_PPP_READY            ( 1 << 0 )
#define CELLULAR_EVT_CONNECTED            ( 1 << 1 )
#define CELLULAR_EVT_DISCONNECTED         ( 1 << 2 )
#define CELLULAR_EVT_IP_ACQUIRED          ( 1 << 3 )
#define CELLULAR_EVT_RECONNECT_REQ        ( 1 << 4 )

/* PPP Buffer sizes */
#define CELLULAR_PPP_TX_QUEUE_LEN         10
#define CELLULAR_PPP_RX_BUFFER_SIZE       2048
#define CELLULAR_PPP_TX_BUFFER_SIZE       2048

/* AT Command response buffer */
#define CELLULAR_AT_RESPONSE_QUEUE_LEN    5
#define CELLULAR_AT_CMD_QUEUE_LEN         5

/**
 * @brief Cellular modem status enumeration
 */
typedef enum
{
    CELLULAR_STATUS_NONE = 0,
    CELLULAR_STATUS_INITIALIZING,
    CELLULAR_STATUS_SIM_READY,
    CELLULAR_STATUS_REGISTERED,
    CELLULAR_STATUS_CONNECTED,
    CELLULAR_STATUS_PPP_RUNNING,
    CELLULAR_STATUS_DISCONNECTED,
    CELLULAR_STATUS_ERROR
} CellularStatus_t;

/**
 * @brief Cellular network registration state
 */
typedef enum
{
    CELLULAR_REG_NONE = 0,
    CELLULAR_REG_HOME = 1,
    CELLULAR_REG_SEARCHING = 2,
    CELLULAR_REG_DENIED = 3,
    CELLULAR_REG_UNKNOWN = 4,
    CELLULAR_REG_ROAMING = 5
} CellularRegState_t;

/**
 * @brief Cellular modem information (optional diagnostics)
 */
typedef struct
{
    char pcImei[ CELLULAR_IMEI_LEN + 1 ];
    char pcFirmwareVersion[ 32 ];
    int lRssi;                      /* Signal strength (0-31, 99=unknown) */
    CellularRegState_t xRegState;
} CellularModemInfo_t;

/**
 * @brief UART context for cellular communication
 */
typedef struct
{
    UART_HandleTypeDef * pxUartHandle;
    TaskHandle_t xRxTaskHandle;
    MessageBufferHandle_t xRxBuffer;    /* Receives data from UART ISR */
    SemaphoreHandle_t xTxMutex;
    volatile BaseType_t xPppMode;       /* pdTRUE=PPP mode, pdFALSE=AT mode */
} CellularUartCtx_t;

/**
 * @brief Main cellular context - simplified for PPP
 */
typedef struct
{
    /* State */
    volatile CellularStatus_t xStatus;

    /* Configuration */
    char pcApn[ CELLULAR_APN_MAX_LEN ];

    /* Hardware */
    CellularUartCtx_t xUartCtx;

    /* lwIP PPP interface */
    struct netif xPppNetif;
    void * pxPppPcb;                    /* PPP control block (ppp_pcb) */
    TaskHandle_t xPppTaskHandle;        /* Task bridging UART <-> lwIP PPP */

    /* Optional diagnostics */
    CellularModemInfo_t xModemInfo;

    /* Management task */
    TaskHandle_t xNetTaskHandle;
} CellularContext_t;

/* Function declarations for internal use */

/* cellular_uart.c */
BaseType_t xCellularUartInit( CellularUartCtx_t * pxCtx );
BaseType_t xCellularUartDeinit( CellularUartCtx_t * pxCtx );
BaseType_t xCellularUartSend( CellularUartCtx_t * pxCtx, const uint8_t * pucData, size_t xLen );
BaseType_t xCellularUartRecv( CellularUartCtx_t * pxCtx, uint8_t * pucData, size_t xLen, TickType_t xTimeout );
void vCellularUartSetPppMode( CellularUartCtx_t * pxCtx, BaseType_t xEnable );

/* cellular_at.c */
BaseType_t xCellularAtInit( CellularContext_t * pxCtx );
BaseType_t xCellularAtSendCommand( CellularUartCtx_t * pxUartCtx,
                                   const char * pcCmd,
                                   char * pcResponse,
                                   size_t xRespLen,
                                   uint32_t ulTimeoutMs );
BaseType_t xCellularAtCheckModem( CellularContext_t * pxCtx );
BaseType_t xCellularAtGetModemInfo( CellularContext_t * pxCtx );
BaseType_t xCellularAtCheckSim( CellularContext_t * pxCtx );
BaseType_t xCellularAtSetApn( CellularContext_t * pxCtx );
BaseType_t xCellularAtWaitForRegistration( CellularContext_t * pxCtx, uint32_t ulTimeoutMs );
BaseType_t xCellularAtStartPpp( CellularContext_t * pxCtx );
BaseType_t xCellularAtStopPpp( CellularContext_t * pxCtx );

/* cellular_ppp.c */
BaseType_t xCellularPppInit( CellularContext_t * pxCtx );
BaseType_t xCellularPppStart( CellularContext_t * pxCtx );
BaseType_t xCellularPppStop( CellularContext_t * pxCtx );
void vCellularPppTask( void * pvParameters );

#ifdef __cplusplus
}
#endif

#endif /* CELLULAR_PRV_H */
