/*
 * Cellular Driver Private Definitions
 *
 * Private header for cellular modem driver implementation.
 * Sierra Wireless HL7688/HL7680 support.
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
 * @brief AT command context
 */
typedef struct
{
    char pcCommand[ CELLULAR_AT_CMD_MAX_LEN ];
    char pcResponse[ CELLULAR_AT_RESP_MAX_LEN ];
    uint32_t ulResponseLen;
    BaseType_t xSuccess;
    uint32_t ulTimeoutMs;
    SemaphoreHandle_t xResponseSemaphore;
} CellularAtCmd_t;

/**
 * @brief Cellular configuration
 */
typedef struct
{
    char pcApn[ CELLULAR_APN_MAX_LEN ];
    char pcUsername[ 64 ];
    char pcPassword[ 64 ];
    BaseType_t xUseAuth;
} CellularConfig_t;

/**
 * @brief Cellular modem information
 */
typedef struct
{
    char pcImei[ CELLULAR_IMEI_LEN + 1 ];
    char pcIccid[ CELLULAR_ICCID_LEN + 1 ];
    char pcFirmwareVersion[ 32 ];
    int lRssi;
    CellularRegState_t xRegState;
} CellularModemInfo_t;

/**
 * @brief UART context for cellular communication
 */
typedef struct
{
    UART_HandleTypeDef * pxUartHandle;
    TaskHandle_t xRxTaskHandle;
    QueueHandle_t xTxQueue;
    MessageBufferHandle_t xRxBuffer;
    SemaphoreHandle_t xTxMutex;
    volatile BaseType_t xPppMode;
} CellularUartCtx_t;

/**
 * @brief PPP context for lwIP integration
 */
typedef struct
{
    struct netif xNetif;
    TaskHandle_t xPppTaskHandle;
    CellularUartCtx_t * pxUartCtx;
    volatile CellularStatus_t xStatus;
    TaskHandle_t xNetTaskHandle;
} CellularPppCtx_t;

/**
 * @brief Main cellular network connection context
 */
typedef struct
{
    CellularConfig_t xConfig;
    CellularModemInfo_t xModemInfo;
    CellularUartCtx_t xUartCtx;
    CellularPppCtx_t xPppCtx;
    TaskHandle_t xNetTaskHandle;
    volatile CellularStatus_t xStatus;
    volatile CellularStatus_t xStatusPrevious;
} CellularNetConnCtx_t;

/* Function declarations for internal use */

/* cellular_uart.c */
BaseType_t xCellularUartInit( CellularUartCtx_t * pxCtx );
BaseType_t xCellularUartDeinit( CellularUartCtx_t * pxCtx );
BaseType_t xCellularUartSend( CellularUartCtx_t * pxCtx, const uint8_t * pucData, size_t xLen );
BaseType_t xCellularUartRecv( CellularUartCtx_t * pxCtx, uint8_t * pucData, size_t xLen, TickType_t xTimeout );
void vCellularUartSetPppMode( CellularUartCtx_t * pxCtx, BaseType_t xEnable );

/* cellular_at.c */
BaseType_t xCellularAtInit( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtSendCommand( CellularUartCtx_t * pxUartCtx,
                                   const char * pcCmd,
                                   char * pcResponse,
                                   size_t xRespLen,
                                   uint32_t ulTimeoutMs );
BaseType_t xCellularAtCheckModem( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtGetModemInfo( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtCheckSim( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtSetApn( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtWaitForRegistration( CellularNetConnCtx_t * pxCtx, uint32_t ulTimeoutMs );
BaseType_t xCellularAtStartPpp( CellularNetConnCtx_t * pxCtx );
BaseType_t xCellularAtStopPpp( CellularNetConnCtx_t * pxCtx );

/* cellular_ppp.c */
BaseType_t xCellularPppInit( CellularPppCtx_t * pxCtx, CellularUartCtx_t * pxUartCtx );
BaseType_t xCellularPppStart( CellularPppCtx_t * pxCtx );
BaseType_t xCellularPppStop( CellularPppCtx_t * pxCtx );
void vCellularPppTask( void * pvParameters );

#ifdef __cplusplus
}
#endif

#endif /* CELLULAR_PRV_H */
