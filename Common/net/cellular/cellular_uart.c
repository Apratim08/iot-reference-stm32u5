/*
 * Cellular UART Driver
 *
 * STM32 HAL-based UART driver for Sierra Wireless modem
 * Supports both AT command mode and PPP data mode
 */

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "cellular_prv.h"
#include "stm32u5xx_hal.h"
#include "stm32u5xx_hal_uart.h"

#include <string.h>

/* UART configuration - which UART peripheral to use */
#ifndef CELLULAR_UART_INSTANCE
    #define CELLULAR_UART_INSTANCE    USART3  /* Arduino D0/D1 on CN13 */
#endif

#ifndef CELLULAR_UART_IRQn
    #define CELLULAR_UART_IRQn        USART3_IRQn
#endif

/* Message buffer size for RX data */
#define CELLULAR_UART_RX_BUFFER_SIZE    2048

/* DMA receive buffer - circular buffer for continuous reception */
#define CELLULAR_UART_DMA_BUFFER_SIZE   512
static uint8_t ucDmaRxBuffer[ CELLULAR_UART_DMA_BUFFER_SIZE ];

/* HAL UART handle */
static UART_HandleTypeDef xHuart;

/* Forward declarations for ISR callbacks */
void vCellularUartRxTask( void * pvParameters );

BaseType_t xCellularUartInit( CellularUartCtx_t * pxCtx )
{
    extern UART_HandleTypeDef * pxHndlUart3;

    if( pxCtx == NULL )
    {
        LogError( "UART context is NULL" );
        return pdFALSE;
    }

    LogInfo( "Initializing cellular UART..." );

    /* Configure UART handle */
    xHuart.Instance = CELLULAR_UART_INSTANCE;
    xHuart.Init.BaudRate = CELLULAR_UART_BAUD_RATE;
    xHuart.Init.WordLength = UART_WORDLENGTH_8B;
    xHuart.Init.StopBits = UART_STOPBITS_1;
    xHuart.Init.Parity = UART_PARITY_NONE;
    xHuart.Init.Mode = UART_MODE_TX_RX;
    xHuart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    xHuart.Init.OverSampling = UART_OVERSAMPLING_16;

    /* Configure advanced features - CRITICAL for STM32U5 reliability */
    xHuart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    /* Configure FIFO mode - Required for reliable DMA operation on STM32U5 */
    xHuart.FifoMode = UART_FIFOMODE_ENABLE;

    /* Initialize HAL UART */
    if( HAL_UART_Init( &xHuart ) != HAL_OK )
    {
        LogError( "Failed to initialize UART HAL" );
        return pdFALSE;
    }

    /* Disable UART RX timeout to prevent spurious errors during DMA */
    __HAL_UART_DISABLE_IT( &xHuart, UART_IT_RTO );

    /* Clear any pending errors from initialization */
    __HAL_UART_CLEAR_FLAG( &xHuart, UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_OREF );
    xHuart.ErrorCode = HAL_UART_ERROR_NONE;

    /* Store handle in context and export globally for interrupt handler */
    pxCtx->pxUartHandle = &xHuart;
    pxHndlUart3 = &xHuart;
    pxCtx->xPppMode = pdFALSE;

    /* Create stream buffer for RX data (used by both AT and PPP modes)
     * Use stream buffer instead of message buffer to allow byte-by-byte reading */
    pxCtx->xRxBuffer = xStreamBufferCreate( CELLULAR_UART_RX_BUFFER_SIZE, 1 );
    if( pxCtx->xRxBuffer == NULL )
    {
        LogError( "Failed to create RX stream buffer" );
        HAL_UART_DeInit( &xHuart );
        return pdFALSE;
    }

    /* Create mutex for TX operations */
    pxCtx->xTxMutex = xSemaphoreCreateMutex();
    if( pxCtx->xTxMutex == NULL )
    {
        LogError( "Failed to create TX mutex" );
        vStreamBufferDelete( pxCtx->xRxBuffer );
        HAL_UART_DeInit( &xHuart );
        return pdFALSE;
    }

    /* Create RX task to handle UART reception */
    if( xTaskCreate( vCellularUartRxTask,
                     "CellularUartRx",
                     configMINIMAL_STACK_SIZE * 2,
                     pxCtx,
                     tskIDLE_PRIORITY + 2,
                     &( pxCtx->xRxTaskHandle ) ) != pdPASS )
    {
        LogError( "Failed to create RX task" );
        vSemaphoreDelete( pxCtx->xTxMutex );
        vStreamBufferDelete( pxCtx->xRxBuffer );
        HAL_UART_DeInit( &xHuart );
        return pdFALSE;
    }

    /* Start DMA reception in circular mode */
    if( HAL_UART_Receive_DMA( &xHuart, ucDmaRxBuffer, CELLULAR_UART_DMA_BUFFER_SIZE ) != HAL_OK )
    {
        LogError( "Failed to start UART DMA reception" );
        vTaskDelete( pxCtx->xRxTaskHandle );
        vSemaphoreDelete( pxCtx->xTxMutex );
        vStreamBufferDelete( pxCtx->xRxBuffer );
        HAL_UART_DeInit( &xHuart );
        return pdFALSE;
    }

    /* Enable UART interrupt */
    HAL_NVIC_SetPriority( CELLULAR_UART_IRQn, 5, 0 );
    HAL_NVIC_EnableIRQ( CELLULAR_UART_IRQn );

    LogInfo( "Cellular UART initialized successfully" );

    return pdTRUE;
}

BaseType_t xCellularUartDeinit( CellularUartCtx_t * pxCtx )
{
    if( pxCtx == NULL )
    {
        return pdFALSE;
    }

    LogInfo( "Deinitializing cellular UART..." );

    /* Disable interrupt */
    HAL_NVIC_DisableIRQ( CELLULAR_UART_IRQn );

    /* Stop DMA */
    HAL_UART_DMAStop( &xHuart );

    /* Delete task and synchronization objects */
    if( pxCtx->xRxTaskHandle != NULL )
    {
        vTaskDelete( pxCtx->xRxTaskHandle );
        pxCtx->xRxTaskHandle = NULL;
    }

    if( pxCtx->xTxMutex != NULL )
    {
        vSemaphoreDelete( pxCtx->xTxMutex );
        pxCtx->xTxMutex = NULL;
    }

    if( pxCtx->xRxBuffer != NULL )
    {
        vStreamBufferDelete( pxCtx->xRxBuffer );
        pxCtx->xRxBuffer = NULL;
    }

    /* Deinitialize HAL */
    HAL_UART_DeInit( &xHuart );

    return pdTRUE;
}

BaseType_t xCellularUartSend( CellularUartCtx_t * pxCtx, const uint8_t * pucData, size_t xLen )
{
    BaseType_t xResult = pdFALSE;

    if( pxCtx == NULL || pucData == NULL || xLen == 0 )
    {
        return pdFALSE;
    }

    /* Take mutex to ensure atomic transmission */
    if( xSemaphoreTake( pxCtx->xTxMutex, pdMS_TO_TICKS( CELLULAR_UART_TIMEOUT_MS ) ) == pdTRUE )
    {
        /* Use blocking transmit with timeout */
        if( HAL_UART_Transmit( pxCtx->pxUartHandle, ( uint8_t * ) pucData, xLen,
                              CELLULAR_UART_TIMEOUT_MS ) == HAL_OK )
        {
            xResult = pdTRUE;
        }
        else
        {
            LogError( "UART transmit failed" );
        }

        xSemaphoreGive( pxCtx->xTxMutex );
    }
    else
    {
        LogError( "Failed to acquire TX mutex" );
    }

    return xResult;
}

BaseType_t xCellularUartRecv( CellularUartCtx_t * pxCtx, uint8_t * pucData, size_t xLen, TickType_t xTimeout )
{
    size_t xBytesRead;

    if( pxCtx == NULL || pucData == NULL || xLen == 0 )
    {
        return pdFALSE;
    }

    if( pxCtx->xRxBuffer == NULL )
    {
        LogError( "UartRecv: RX buffer is NULL!" );
        return pdFALSE;
    }

    /* Read from stream buffer (populated by RX task) */
    xBytesRead = xStreamBufferReceive( pxCtx->xRxBuffer, pucData, xLen, xTimeout );

    return ( xBytesRead > 0 ) ? pdTRUE : pdFALSE;
}

void vCellularUartSetPppMode( CellularUartCtx_t * pxCtx, BaseType_t xEnable )
{
    if( pxCtx != NULL )
    {
        pxCtx->xPppMode = xEnable;

        if( xEnable )
        {
            /* Switching to PPP mode - clear stream buffer to discard any buffered AT responses */
            if( pxCtx->xRxBuffer != NULL )
            {
                xStreamBufferReset( pxCtx->xRxBuffer );
                LogInfo( "UART mode set to PPP (stream buffer reset)" );
            }
        }
        else
        {
            LogInfo( "UART mode set to AT" );
        }
    }
}

/*
 * RX Task - reads from DMA buffer and pushes to message buffer
 * This task runs continuously and monitors the DMA circular buffer
 */
void vCellularUartRxTask( void * pvParameters )
{
    CellularUartCtx_t * pxCtx = ( CellularUartCtx_t * ) pvParameters;
    size_t xLastPos = 0;
    uint8_t ucTempBuffer[ 256 ];

    LogInfo( "Cellular UART RX task started" );

    while( 1 )
    {
        /* Calculate current DMA position */
        size_t xCurrentPos = CELLULAR_UART_DMA_BUFFER_SIZE -
                             __HAL_DMA_GET_COUNTER( pxCtx->pxUartHandle->hdmarx );

        if( xCurrentPos != xLastPos )
        {
            size_t xBytesAvailable;
            size_t xBytesToRead;

            /* Handle circular buffer wraparound */
            if( xCurrentPos > xLastPos )
            {
                /* No wraparound - simple case */
                xBytesAvailable = xCurrentPos - xLastPos;
                xBytesToRead = ( xBytesAvailable > sizeof( ucTempBuffer ) ) ?
                               sizeof( ucTempBuffer ) : xBytesAvailable;

                memcpy( ucTempBuffer, &ucDmaRxBuffer[ xLastPos ], xBytesToRead );
            }
            else
            {
                /* Wraparound - read to end of buffer first */
                xBytesAvailable = CELLULAR_UART_DMA_BUFFER_SIZE - xLastPos;
                xBytesToRead = ( xBytesAvailable > sizeof( ucTempBuffer ) ) ?
                               sizeof( ucTempBuffer ) : xBytesAvailable;

                memcpy( ucTempBuffer, &ucDmaRxBuffer[ xLastPos ], xBytesToRead );
            }

            /* Send to message buffer for processing */
            if( xBytesToRead > 0 )
            {
                /* Only log hex dump in AT mode - PPP mode generates too much data */
                if( pxCtx->xPppMode == pdFALSE )
                {
                    /* Log received data for debugging - show hex dump */
                    char pcHexDump[ 80 ];
                    size_t xHexPos = 0;
                    for( size_t i = 0; i < xBytesToRead && i < 20; i++ )
                    {
                        xHexPos += snprintf( &pcHexDump[ xHexPos ], sizeof( pcHexDump ) - xHexPos,
                                            "%02X ", ucTempBuffer[ i ] );
                    }
                    LogInfo( "UART RX: %lu bytes [%s]", xBytesToRead, pcHexDump );
                }

                /* Send to stream buffer - non-blocking for now */
                if( pxCtx->xRxBuffer != NULL )
                {
                    size_t xBytesSent = xStreamBufferSend( pxCtx->xRxBuffer, ucTempBuffer, xBytesToRead, 0 );

                    if( xBytesSent != xBytesToRead )
                    {
                        LogError( "StreamBuffer send failed: sent %lu of %lu bytes", xBytesSent, xBytesToRead );
                    }
                }
                xLastPos = ( xLastPos + xBytesToRead ) % CELLULAR_UART_DMA_BUFFER_SIZE;
            }
        }

        /* Small delay to prevent CPU hogging */
        vTaskDelay( pdMS_TO_TICKS( 10 ) );
    }
}

/*
 * UART IRQ Handler - must be called from STM32 interrupt vector
 * This function should be called from the appropriate UARTx_IRQHandler in stm32u5xx_it.c
 */
void vCellularUartIrqHandler( void )
{
    HAL_UART_IRQHandler( &xHuart );
}

/* HAL Callbacks */
void HAL_UART_RxCpltCallback( UART_HandleTypeDef * huart )
{
    /* DMA reception complete - this happens when buffer is full
     * For circular mode, this shouldn't normally trigger */
    if( huart->Instance == CELLULAR_UART_INSTANCE )
    {
        /* RX task will handle the data */
    }
}

void HAL_UART_RxHalfCpltCallback( UART_HandleTypeDef * huart )
{
    /* DMA half-transfer complete callback
     * RX task polls the buffer, so we don't need to do anything here */
    if( huart->Instance == CELLULAR_UART_INSTANCE )
    {
        /* RX task will handle the data */
    }
}

void HAL_UART_ErrorCallback( UART_HandleTypeDef * huart )
{
    if( huart->Instance == CELLULAR_UART_INSTANCE )
    {
        uint32_t ulErrorCode = huart->ErrorCode;

        /* Only log non-framing errors to avoid spam during modem boot */
        if( ( ulErrorCode & ~HAL_UART_ERROR_FE ) != 0 )
        {
            LogError( "UART error detected: 0x%lx", ulErrorCode );
        }

        /* Clear all error flags */
        __HAL_UART_CLEAR_FLAG( huart, UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_OREF );
        huart->ErrorCode = HAL_UART_ERROR_NONE;

        /* Restart DMA reception */
        HAL_UART_Receive_DMA( huart, ucDmaRxBuffer, CELLULAR_UART_DMA_BUFFER_SIZE );
    }
}
