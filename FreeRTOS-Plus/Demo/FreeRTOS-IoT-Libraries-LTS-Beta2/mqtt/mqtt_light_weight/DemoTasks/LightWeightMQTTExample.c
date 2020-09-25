/*
 * FreeRTOS Kernel V10.3.0
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 */

/*
 * Demo for showing use of the MQTT light weight serializer API.
 * The Light weight serializer API lets user to serialize and
 * deserialize MQTT messages into a user provided buffer.
 * This API allows use of a statically allocated buffer.
 *
 * The Example shown below uses this API to create MQTT messages and
 * send them over the connection established using FreeRTOS sockets.
 * The example is single threaded and uses statically allocated memory;
 * it uses QOS0 and therefore does not implement any retransmission
 * mechanism for Publish messages.
 *
 * !!! NOTE !!!
 * This MQTT demo does not authenticate the server or the client.
 * Hence, this demo code is not recommended to be used in production
 * systems requiring secure connections.
 */

/* Demo specific configs. */
#include "demo_config.h"

/* Standard includes. */
#include <string.h>
#include <stdio.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

/* MQTT library includes. */
#include "mqtt_lightweight.h"

/* Retry utilities include. */
#include "retry_utils.h"

/*-----------------------------------------------------------*/

/* Compile time error for undefined configs. */
#ifndef democonfigMQTT_BROKER_ENDPOINT
    #error "Define the config democonfigMQTT_BROKER_ENDPOINT by following the instructions in file demo_config.h."
#endif

/*-----------------------------------------------------------*/

/* Default values for configs. */
#ifndef democonfigCLIENT_IDENTIFIER

/**
 * @brief The MQTT client identifier used in this example.  Each client identifier
 * must be unique, so edit as required to ensure no two clients connecting to the
 * same broker use the same client identifier.
 *
 * @note Appending __TIME__ to the client id string will reduce the possibility of a
 * client id collision in the broker. Note that the appended time is the compilation
 * time. This client id can cause collision, if more than one instance of the same
 * binary is used at the same time to connect to the broker.
 */
    #define democonfigCLIENT_IDENTIFIER    "testClient"__TIME__
#endif

#ifndef democonfigMQTT_BROKER_PORT

/**
 * @brief The port to use for the demo.
 */
    #define democonfigMQTT_BROKER_PORT    ( 1883 )
#endif

/*-----------------------------------------------------------*/

/**
 * @brief The topic to subscribe and publish to in the example.
 *
 * The topic name starts with the client identifier to ensure that each demo
 * interacts with a unique topic name.
 */
#define mqttexampleTOPIC                            democonfigCLIENT_IDENTIFIER "/example/topic"

/**
 * @brief The number of topic filters to subscribe.
 */
#define mqttexampleTOPIC_COUNT                      ( 1 )

/**
 * @brief The MQTT message published in this example.
 */
#define mqttexampleMESSAGE                          "Hello Light Weight MQTT World!"

/**
 * @brief Dimensions a file scope buffer currently used to send and receive MQTT data
 * from a socket.
 */
#define mqttexampleSHARED_BUFFER_SIZE               ( 500U )

/**
 * @brief Time to wait between each cycle of the demo implemented by prvMQTTDemoTask().
 */
#define mqttexampleDELAY_BETWEEN_DEMO_ITERATIONS    ( pdMS_TO_TICKS( 5000U ) )

/**
 * @brief Keep alive time reported to the broker while establishing an MQTT connection.
 *
 * It is the responsibility of the Client to ensure that the interval between
 * Control Packets being sent does not exceed the this Keep Alive value. In the
 * absence of sending any other Control Packets, the Client MUST send a
 * PINGREQ Packet.
 */
#define mqttexampleKEEP_ALIVE_TIMEOUT_SECONDS       ( 10U )

/**
 * @brief Time to wait before sending ping request to keep MQTT connection alive.
 *
 * A PINGREQ is attempted to be sent at every ( #mqttexampleKEEP_ALIVE_TIMEOUT_SECONDS / 4 )
 * seconds. This is to make sure that a PINGREQ is always sent before the timeout
 * expires in broker.
 */
#define mqttexampleKEEP_ALIVE_DELAY                 ( pdMS_TO_TICKS( ( ( mqttexampleKEEP_ALIVE_TIMEOUT_SECONDS / 4 ) * 1000 ) ) )

/**
 * @brief Maximum number of times to call FreeRTOS_recv when initiating a
 * graceful socket shutdown.
 */
#define mqttexampleMAX_SOCKET_SHUTDOWN_LOOPS        ( 3 )

/*-----------------------------------------------------------*/

/**
 * @brief The task used to demonstrate the light weight MQTT API.
 *
 * @param[in] pvParameters Parameters as passed at the time of task creation. Not
 * used in this example.
 */
static void prvMQTTDemoTask( void * pvParameters );

/**
 * @brief Creates a TCP connection to the MQTT broker as specified by
 * democonfigMQTT_BROKER_ENDPOINT and democonfigMQTT_BROKER_PORT defined at the
 * top of this file.
 *
 * @return On success the socket connected to the MQTT broker is returned.
 *
 */
static Socket_t prvCreateTCPConnectionToBroker( void );

/**
 * @brief Connect to MQTT broker with reconnection retries.
 *
 * If connection fails, retry is attempted after a timeout.
 * Timeout value will exponentially increase until maximum
 * timeout value is reached or the number of attempts are exhausted.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker.
 *
 * @return The socket of the final connection attempt.
 */
static Socket_t prvConnectToServerWithBackoffRetries();

/**
 * @brief Sends an MQTT Connect packet over the already connected TCP socket.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker.
 *
 */
static void prvCreateMQTTConnectionWithBroker( Socket_t xMQTTSocket );

/**
 * @brief Performs a graceful shutdown and close of the socket passed in as its
 * parameter.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvGracefulShutDown( Socket_t xSocket );

/**
 * @brief Subscribes to the topic as specified in mqttexampleTOPIC at the top of
 * this file.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTSubscribeToTopic( Socket_t xMQTTSocket );

/**
 * @brief Subscribes to the topic as specified in mqttexampleTOPIC at the top of
 * this file. In the case of a Subscribe ACK failure, then subscription is
 * retried using an exponential backoff strategy with jitter.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTSubscribeWithBackoffRetries( Socket_t xMQTTSocket );

/**
 * @brief Function to update variable #xTopicFilterContext with status
 * information from Subscribe ACK. Called by the event callback after processing
 * an incoming SUBACK packet.
 *
 * @param Server response to the subscription request.
 */
static void prvMQTTUpdateSubAckStatus( MQTTPacketInfo_t * pxPacketInfo );

/**
 * @brief  Publishes a message mqttexampleMESSAGE on mqttexampleTOPIC topic.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTPublishToTopic( Socket_t xMQTTSocket );

/**
 * @brief Unsubscribes from the previously subscribed topic as specified
 * in mqttexampleTOPIC.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTUnsubscribeFromTopic( Socket_t xMQTTSocket );

/**
 * @brief Send MQTT Ping Request to the broker.
 * Ping request is used to keep connection to the broker alive.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTKeepAlive( Socket_t xMQTTSocket );

/**
 * @brief Disconnect From the MQTT broker.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTDisconnect( Socket_t xMQTTSocket );

/**
 * @brief Process a response or ack to an MQTT request (PING, SUBSCRIBE
 * or UNSUBSCRIBE). This function processes PING_RESP, SUB_ACK, UNSUB_ACK
 *
 * @param pxIncomingPacket is a pointer to structure containing deserialized
 * MQTT response.
 * @param usPacketId is the packet identifier from the ack received.
 */
static void prvMQTTProcessResponse( MQTTPacketInfo_t * pxIncomingPacket,
                                    uint16_t usPacketId );

/**
 * @brief Process incoming Publish message.
 *
 * @param pxPublishInfo is a pointer to structure containing deserialized
 * Publish message.
 */
static void prvMQTTProcessIncomingPublish( MQTTPublishInfo_t * pxPublishInfo );

/**
 * @brief Receive and validate MQTT packet from the broker, determine the type
 * of the packet and processes the packet based on the type.
 *
 * @param xMQTTSocket is a TCP socket that is connected to an MQTT broker to which
 * an MQTT connection has been established.
 */
static void prvMQTTProcessIncomingPacket( Socket_t xMQTTSocket );

/**
 * @brief The transport receive wrapper function supplied to the MQTT library for
 * receiving type and length of an incoming MQTT packet.
 *
 * @param[in] pxContext Pointer to network context.
 * @param[out] pBuffer Buffer for receiving data.
 * @param[in] bytesToRecv Size of pBuffer.
 *
 * @return Number of bytes received or zero to indicate transportTimeout;
 * negative value on error.
 */
static int32_t prvTransportRecv( NetworkContext_t * pxContext,
                                 void * pvBuffer,
                                 size_t xBytesToRecv );

/**
 * @brief Generate and return monotonically increasing packet identifier.
 *
 * @return The next PacketId.
 *
 * @note This function is not thread safe.
 */
static uint16_t prvGetNextPacketIdentifier( void );

/*-----------------------------------------------------------*/

/* @brief Static buffer used to hold MQTT messages being sent and received. */
static uint8_t ucSharedBuffer[ mqttexampleSHARED_BUFFER_SIZE ];

/**
 * @brief Packet Identifier generated when Subscribe request was sent to the broker;
 * it is used to match received Subscribe ACK to the transmitted subscribe request.
 */
static uint16_t usSubscribePacketIdentifier;

/**
 * @brief Packet Identifier generated when Unsubscribe request was sent to the broker;
 * it is used to match received Unsubscribe response to the transmitted unsubscribe
 * request.
 */
static uint16_t usUnsubscribePacketIdentifier;

/**
 * @brief A pair containing a topic filter and its SUBACK status.
 */
typedef struct topicFilterContext
{
    const char * pcTopicFilter;
    bool xSubAckSuccess;
} topicFilterContext_t;

/**
 * @brief An array containing the context of a SUBACK; the SUBACK status
 * of a filter is updated when a SUBACK packet is processed.
 */
static topicFilterContext_t xTopicFilterContext[ mqttexampleTOPIC_COUNT ] =
{
    { mqttexampleTOPIC, false }
};


/** @brief Static buffer used to hold MQTT messages being sent and received. */
const static MQTTFixedBuffer_t xBuffer =
{
    .pBuffer = ucSharedBuffer,
    .size    = mqttexampleSHARED_BUFFER_SIZE
};

/*-----------------------------------------------------------*/

/**
 * @brief The Network Context implementation. This context is passed to the
 * transport interface functions.
 *
 * This example uses transport interface function only to read the packet type
 * and length of the incoming packet from network.
 */
struct NetworkContext
{
    Socket_t xTcpSocket;
};
/*-----------------------------------------------------------*/

/*
 * @brief Create the task that demonstrates the Light Weight MQTT API Demo.
 * This is the entry function of this demo.
 */
void vStartSimpleMQTTDemo( void )
{
    /* This example uses a single application task, which in turn is used to
     * connect, subscribe, publish, unsubscribe and disconnect from the MQTT
     * broker. */
    xTaskCreate( prvMQTTDemoTask,          /* Function that implements the task. */
                 "MQTTLWDemo",             /* Text name for the task - only used for debugging. */
                 democonfigDEMO_STACKSIZE, /* Size of stack (in words, not bytes) to allocate for the task. */
                 NULL,                     /* Task parameter - not used in this case. */
                 tskIDLE_PRIORITY,         /* Task priority, must be between 0 and configMAX_PRIORITIES - 1. */
                 NULL );                   /* Used to pass out a handle to the created task - not used in this case. */
}
/*-----------------------------------------------------------*/

static void prvMQTTDemoTask( void * pvParameters )
{
    Socket_t xMQTTSocket;
    uint32_t ulPublishCount = 0U, ulTopicCount = 0U;
    const uint32_t ulMaxPublishCount = 5UL;

    /* Remove compiler warnings about unused parameters. */
    ( void ) pvParameters;

    for( ; ; )
    {
        /****************************** Connect. ******************************/

        /* Attempt to connect to the MQTT broker. If connection fails, retry after
         * a timeout. Timeout value will be exponentially increased until the maximum
         * number of attempts are reached or the maximum timeout value is reached.
         * The function returns a failure status if the TCP connection cannot be established
         * to the broker after the configured number of attempts. */
        xMQTTSocket = prvConnectToServerWithBackoffRetries();
        configASSERT( xMQTTSocket != FREERTOS_INVALID_SOCKET );

        /* Sends an MQTT Connect packet over the already connected TCP socket
         * xMQTTSocket, and waits for connection acknowledgment (CONNACK) packet. */
        LogInfo( ( "Creating an MQTT connection to %s.\r\n", democonfigMQTT_BROKER_ENDPOINT ) );
        prvCreateMQTTConnectionWithBroker( xMQTTSocket );

        /**************************** Subscribe. ******************************/

        /* If server rejected the subscription request, attempt to resubscribe to topic.
         * Attempts are made according to the exponential backoff retry strategy
         * implemented in retryUtils. */
        prvMQTTSubscribeWithBackoffRetries( xMQTTSocket );

        /**************************** Publish and Keep Alive Loop. ******************************/
        /* Publish messages with QoS0, send and process Keep alive messages. */
        for( ulPublishCount = 0; ulPublishCount < ulMaxPublishCount; ulPublishCount++ )
        {
            LogInfo( ( "Publish to the MQTT topic %s.\r\n", mqttexampleTOPIC ) );
            prvMQTTPublishToTopic( xMQTTSocket );

            /* Process incoming publish echo, since application subscribed to the same
             * topic the broker will send publish message back to the application. */
            LogInfo( ( "Attempt to receive publish message from broker.\r\n" ) );
            prvMQTTProcessIncomingPacket( xMQTTSocket );

            /* Leave Connection Idle for some time */
            LogInfo( ( "Keeping Connection Idle.\r\n\r\n" ) );
            vTaskDelay( mqttexampleKEEP_ALIVE_DELAY );

            /* Send Ping request to broker and receive ping response */
            LogInfo( ( "Sending Ping Request to the broker.\r\n" ) );
            prvMQTTKeepAlive( xMQTTSocket );

            /* Process Incoming packet from the broker */
            prvMQTTProcessIncomingPacket( xMQTTSocket );
        }

        /************************ Unsubscribe from the topic. **************************/
        LogInfo( ( "Unsubscribe from the MQTT topic %s.\r\n", mqttexampleTOPIC ) );
        prvMQTTUnsubscribeFromTopic( xMQTTSocket );

        /* Process Incoming packet from the broker. */
        prvMQTTProcessIncomingPacket( xMQTTSocket );

        /**************************** Disconnect. ******************************/

        /* Send an MQTT Disconnect packet over the already connected TCP socket.
         * There is no corresponding response for the disconnect packet. After sending
         * disconnect, client must close the network connection. */
        LogInfo( ( "Disconnecting the MQTT connection with %s.\r\n", democonfigMQTT_BROKER_ENDPOINT ) );
        prvMQTTDisconnect( xMQTTSocket );

        /* Close the network connection.  */
        prvGracefulShutDown( xMQTTSocket );

        /* Reset SUBACK status for each topic iflter after completion of subscription request cycle. */
        for( ulTopicCount = 0; ulTopicCount < mqttexampleTOPIC_COUNT; ulTopicCount++ )
        {
            xTopicFilterContext[ ulTopicCount ].xSubAckSuccess = false;
        }

        /* Wait for some time between two iterations to ensure that we do not
         * bombard the public test mosquitto broker. */
        LogInfo( ( "prvMQTTDemoTask() completed an iteration successfully. Total free heap is %u.\r\n", xPortGetFreeHeapSize() ) );
        LogInfo( ( "Demo completed successfully.\r\n" ) );
        LogInfo( ( "Short delay before starting the next iteration.... \r\n\r\n" ) );
        vTaskDelay( mqttexampleDELAY_BETWEEN_DEMO_ITERATIONS );
    }
}
/*-----------------------------------------------------------*/

static void prvGracefulShutDown( Socket_t xSocket )
{
    uint8_t ucDummy[ 20 ];
    const TickType_t xShortDelay = pdMS_TO_MIN_TICKS( 250 );
    BaseType_t xShutdownLoopCount = 0;

    if( xSocket != ( Socket_t ) 0 )
    {
        if( xSocket != FREERTOS_INVALID_SOCKET )
        {
            /* Initiate graceful shutdown. */
            FreeRTOS_shutdown( xSocket, FREERTOS_SHUT_RDWR );

            /* Wait for the socket to disconnect gracefully (indicated by FreeRTOS_recv()
             * returning a FREERTOS_EINVAL error) before closing the socket. */
            while( FreeRTOS_recv( xSocket, ucDummy, sizeof( ucDummy ), 0 ) >= 0 )
            {
                /* Wait for shutdown to complete.  If a receive block time is used then
                 * this delay will not be necessary as FreeRTOS_recv() will place the RTOS task
                 * into the Blocked state anyway. */
                vTaskDelay( xShortDelay );

                /* Limit the number of FreeRTOS_recv loops to avoid infinite loop. */
                if( ++xShutdownLoopCount >= mqttexampleMAX_SOCKET_SHUTDOWN_LOOPS )
                {
                    break;
                }
            }

            /* The socket has shut down and is safe to close. */
            FreeRTOS_closesocket( xSocket );
        }
    }
}
/*-----------------------------------------------------------*/

static int32_t prvTransportRecv( NetworkContext_t * pxContext,
                                 void * pvBuffer,
                                 size_t xBytesToRecv )
{
    int32_t lResult;

    configASSERT( pxContext != NULL );

    /* Receive upto xBytesToRecv from network. */
    lResult = ( int32_t ) FreeRTOS_recv( ( Socket_t ) pxContext->xTcpSocket,
                                         pvBuffer,
                                         xBytesToRecv,
                                         0 );

    return lResult;
}
/*-----------------------------------------------------------*/

static uint16_t prvGetNextPacketIdentifier()
{
    static uint16_t usPacketId = 0;

    usPacketId++;

    /* Since 0 is invalid packet identifier value,
     * take care of it when it rolls over */
    if( usPacketId == 0 )
    {
        usPacketId = 1;
    }

    return usPacketId;
}
/*-----------------------------------------------------------*/

static Socket_t prvCreateTCPConnectionToBroker( void )
{
    Socket_t xMQTTSocket = FREERTOS_INVALID_SOCKET;
    uint32_t ulBrokerIPAddress;
    BaseType_t xStatus = pdFAIL;
    struct freertos_sockaddr xBrokerAddress;

    /* This is the socket used to connect to the MQTT broker. */
    xMQTTSocket = FreeRTOS_socket( FREERTOS_AF_INET,
                                   FREERTOS_SOCK_STREAM,
                                   FREERTOS_IPPROTO_TCP );

    if( xMQTTSocket != FREERTOS_INVALID_SOCKET )
    {
        /* Socket was created.  Locate then connect to the MQTT broker. */
        ulBrokerIPAddress = FreeRTOS_gethostbyname( democonfigMQTT_BROKER_ENDPOINT );

        if( ulBrokerIPAddress != 0 )
        {
            xBrokerAddress.sin_port = FreeRTOS_htons( democonfigMQTT_BROKER_PORT );
            xBrokerAddress.sin_addr = ulBrokerIPAddress;

            if( FreeRTOS_connect( xMQTTSocket, &xBrokerAddress, sizeof( xBrokerAddress ) ) == 0 )
            {
                /* Connection was successful. */
                xStatus = pdPASS;
            }
            else
            {
                LogInfo( ( "Located but could not connect to MQTT broker %s.\r\n\r\n", democonfigMQTT_BROKER_ENDPOINT ) );
            }
        }
        else
        {
            LogInfo( ( "Could not locate MQTT broker %s.\r\n\r\n", democonfigMQTT_BROKER_ENDPOINT ) );
        }
    }
    else
    {
        LogInfo( ( "Could not create TCP socket.\r\n\r\n" ) );
    }

    /* If the socket was created but the connection was not successful then delete
     * the socket again. */
    if( xStatus == pdFAIL )
    {
        if( xMQTTSocket != FREERTOS_INVALID_SOCKET )
        {
            FreeRTOS_closesocket( xMQTTSocket );
            xMQTTSocket = FREERTOS_INVALID_SOCKET;
        }
    }

    return xMQTTSocket;
}
/*-----------------------------------------------------------*/

static Socket_t prvConnectToServerWithBackoffRetries()
{
    Socket_t xSocket;
    RetryUtilsStatus_t xRetryUtilsStatus = RetryUtilsSuccess;
    RetryUtilsParams_t xReconnectParams;

    /* Initialize reconnect attempts and interval. */
    RetryUtils_ParamsReset( &xReconnectParams );
    xReconnectParams.maxRetryAttempts = MAX_RETRY_ATTEMPTS;

    /* Attempt to connect to MQTT broker. If connection fails, retry after
     * a timeout. Timeout value will exponentially increase till maximum
     * attempts are reached.
     */
    do
    {
        /* Establish a TCP connection with the MQTT broker. This example connects to
         * the MQTT broker as specified in democonfigMQTT_BROKER_ENDPOINT and
         * democonfigMQTT_BROKER_PORT at the top of this file. */
        LogInfo( ( "Create a TCP connection to %s:%d.",
                   democonfigMQTT_BROKER_ENDPOINT,
                   democonfigMQTT_BROKER_PORT ) );
        xSocket = prvCreateTCPConnectionToBroker();

        if( xSocket == FREERTOS_INVALID_SOCKET )
        {
            LogWarn( ( "Connection to the broker failed. Retrying connection with backoff and jitter." ) );
            xRetryUtilsStatus = RetryUtils_BackoffAndSleep( &xReconnectParams );
        }

        if( xRetryUtilsStatus == RetryUtilsRetriesExhausted )
        {
            LogError( ( "Connection to the broker failed, all attempts exhausted." ) );
            xSocket = FREERTOS_INVALID_SOCKET;
        }
    } while( ( xSocket == FREERTOS_INVALID_SOCKET ) && ( xRetryUtilsStatus == RetryUtilsSuccess ) );

    return xSocket;
}
/*-----------------------------------------------------------*/

static void prvCreateMQTTConnectionWithBroker( Socket_t xMQTTSocket )
{
    BaseType_t xStatus;
    size_t xRemainingLength;
    size_t xPacketSize;
    MQTTStatus_t xResult;
    MQTTPacketInfo_t xIncomingPacket;
    MQTTConnectInfo_t xConnectInfo;
    uint16_t usPacketId;
    bool xSessionPresent;
    NetworkContext_t xNetworkContext;

    /***
     * For readability, error handling in this function is restricted to the use of
     * asserts().
     ***/

    /* Many fields not used in this demo so start with everything at 0. */
    memset( ( void * ) &xConnectInfo, 0x00, sizeof( xConnectInfo ) );

    /* Start with a clean session i.e. direct the MQTT broker to discard any
     * previous session data. Also, establishing a connection with clean session
     * will ensure that the broker does not store any data when this client
     * gets disconnected. */
    xConnectInfo.cleanSession = true;

    /* The client identifier is used to uniquely identify this MQTT client to
     * the MQTT broker. In a production device the identifier can be something
     * unique, such as a device serial number. */
    xConnectInfo.pClientIdentifier = democonfigCLIENT_IDENTIFIER;
    xConnectInfo.clientIdentifierLength = ( uint16_t ) strlen( democonfigCLIENT_IDENTIFIER );

    /* Set MQTT keep-alive period. It is the responsibility of the application to ensure
     * that the interval between Control Packets being sent does not exceed the Keep Alive value.
     * In the absence of sending any other Control Packets, the Client MUST send a PINGREQ Packet. */
    xConnectInfo.keepAliveSeconds = mqttexampleKEEP_ALIVE_TIMEOUT_SECONDS;

    /* Get size requirement for the connect packet.
     * Last Will and Testament is not used in this demo. It is passed as NULL. */
    xResult = MQTT_GetConnectPacketSize( &xConnectInfo,
                                         NULL,
                                         &xRemainingLength,
                                         &xPacketSize );

    /* Make sure the packet size is less than static buffer size. */
    configASSERT( xResult == MQTTSuccess );
    configASSERT( xPacketSize < mqttexampleSHARED_BUFFER_SIZE );

    /* Serialize MQTT connect packet into the provided buffer. */
    xResult = MQTT_SerializeConnect( &xConnectInfo,
                                     NULL,
                                     xRemainingLength,
                                     &xBuffer );
    configASSERT( xResult == MQTTSuccess );

    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xPacketSize,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xPacketSize );

    /* Reset all fields of the incoming packet structure. */
    ( void ) memset( ( void * ) &xIncomingPacket, 0x00, sizeof( MQTTPacketInfo_t ) );

    /* Wait for connection acknowledgment.  We cannot assume received data is the
     * connection acknowledgment. Therefore this function reads type and remaining
     * length of the received packet, before processing entire packet - although in
     * this case to keep the example simple error checks are just performed by
     * asserts.
     */
    xNetworkContext.xTcpSocket = xMQTTSocket;

    xResult = MQTT_GetIncomingPacketTypeAndLength( prvTransportRecv,
                                                   &xNetworkContext,
                                                   &xIncomingPacket );

    configASSERT( xResult == MQTTSuccess );
    configASSERT( xIncomingPacket.type == MQTT_PACKET_TYPE_CONNACK );
    configASSERT( xIncomingPacket.remainingLength <= mqttexampleSHARED_BUFFER_SIZE );

    /* Now receive the reset of the packet into the statically allocated buffer. */
    xStatus = FreeRTOS_recv( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xIncomingPacket.remainingLength,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xIncomingPacket.remainingLength );

    xIncomingPacket.pRemainingData = xBuffer.pBuffer;
    xResult = MQTT_DeserializeAck( &xIncomingPacket,
                                   &usPacketId,
                                   &xSessionPresent );
    configASSERT( xResult == MQTTSuccess );

    if( xResult != MQTTSuccess )
    {
        LogInfo( ( "Connection with MQTT broker failed.\r\n" ) );
    }
}
/*-----------------------------------------------------------*/

static void prvMQTTSubscribeToTopic( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    size_t xRemainingLength;
    size_t xPacketSize;
    BaseType_t xStatus;
    MQTTSubscribeInfo_t xMQTTSubscription[ mqttexampleTOPIC_COUNT ];

    /* Some fields not used by this demo so start with everything at 0. */
    ( void ) memset( ( void * ) &xMQTTSubscription, 0x00, sizeof( xMQTTSubscription ) );

    /* Subscribe to the mqttexampleTOPIC topic filter. This example subscribes to
     * only one topic and uses QoS0. */
    xMQTTSubscription[ 0 ].qos = MQTTQoS0;
    xMQTTSubscription[ 0 ].pTopicFilter = mqttexampleTOPIC;
    xMQTTSubscription[ 0 ].topicFilterLength = ( uint16_t ) strlen( mqttexampleTOPIC );

    /***
     * For readability, error handling in this function is restricted to the use of
     * asserts().
     ***/

    xResult = MQTT_GetSubscribePacketSize( xMQTTSubscription,
                                           sizeof( xMQTTSubscription ) / sizeof( MQTTSubscribeInfo_t ),
                                           &xRemainingLength,
                                           &xPacketSize );

    /* Make sure the packet size is less than static buffer size. */
    configASSERT( xResult == MQTTSuccess );
    configASSERT( xPacketSize < mqttexampleSHARED_BUFFER_SIZE );

    /* Get a unique packet id. */
    usSubscribePacketIdentifier = prvGetNextPacketIdentifier();
    /* Make sure the packet id obtained is valid. */
    configASSERT( usSubscribePacketIdentifier != 0 );

    /* Serialize subscribe into statically allocated ucSharedBuffer. */
    xResult = MQTT_SerializeSubscribe( xMQTTSubscription,
                                       sizeof( xMQTTSubscription ) / sizeof( MQTTSubscribeInfo_t ),
                                       usSubscribePacketIdentifier,
                                       xRemainingLength,
                                       &xBuffer );

    configASSERT( xResult == MQTTSuccess );

    /* Send Subscribe request to the broker. */
    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xPacketSize,
                             0 );

    configASSERT( xStatus == ( BaseType_t ) xPacketSize );
}
/*-----------------------------------------------------------*/

static void prvMQTTSubscribeWithBackoffRetries( Socket_t xMQTTSocket )
{
    uint32_t ulTopicCount = 0U;
    RetryUtilsStatus_t xRetryUtilsStatus = RetryUtilsSuccess;
    RetryUtilsParams_t xRetryParams;
    bool xFailedSubscribeToTopic = false;

    /* Initialize retry attempts and interval. */
    RetryUtils_ParamsReset( &xRetryParams );
    xRetryParams.maxRetryAttempts = MAX_RETRY_ATTEMPTS;

    do
    {
        /* The client is now connected to the broker. Subscribe to the topic
         * as specified in mqttexampleTOPIC at the top of this file by sending a
         * subscribe packet then waiting for a subscribe acknowledgment (SUBACK).
         * This client will then publish to the same topic it subscribed to, so it
         * will expect all the messages it sends to the broker to be sent back to it
         * from the broker. This demo uses QOS0 in Subscribe, therefore, the Publish
         * messages received from the broker will have QOS0. */
        LogInfo( ( "Attempt to subscribe to the MQTT topic %s.\r\n", mqttexampleTOPIC ) );
        prvMQTTSubscribeToTopic( xMQTTSocket );

        LogInfo( ( "SUBSCRIBE sent for topic %s to broker.\n\n", mqttexampleTOPIC ) );

        /* Process incoming packet from the broker. After sending the subscribe, the
         * client may receive a publish before it receives a subscribe ack. Therefore,
         * call generic incoming packet processing function. Since this demo is
         * subscribing to the topic to which no one is publishing, probability of
         * receiving Publish message before subscribe ack is zero; but application
         * must be ready to receive any packet.  This demo uses the generic packet
         * processing function everywhere to highlight this fact. */
        prvMQTTProcessIncomingPacket( xMQTTSocket );

        /* Check if recent subscription request has been rejected. #xTopicFilterContext is updated
         * in the event callback to reflect the status of the SUBACK sent by the broker. It represents
         * either the QoS level granted by the server upon subscription, or acknowledgement of
         * server rejection of the subscription request. */
        for( ulTopicCount = 0; ulTopicCount < mqttexampleTOPIC_COUNT; ulTopicCount++ )
        {
            if( xTopicFilterContext[ ulTopicCount ].xSubAckSuccess == false )
            {
                LogWarn( ( "Server rejected subscription request. Attempting to re-subscribe to topic %s.",
                           xTopicFilterContext[ ulTopicCount ].pcTopicFilter ) );
                xFailedSubscribeToTopic = true;
                xRetryUtilsStatus = RetryUtils_BackoffAndSleep( &xRetryParams );
                break;
            }
        }

        configASSERT( xRetryUtilsStatus != RetryUtilsRetriesExhausted );
    } while( ( xFailedSubscribeToTopic == true ) && ( xRetryUtilsStatus == RetryUtilsSuccess ) );
}
/*-----------------------------------------------------------*/

static void prvMQTTUpdateSubAckStatus( MQTTPacketInfo_t * pxPacketInfo )
{
    uint8_t * pucPayload = NULL;
    uint32_t ulTopicCount = 0U;
    size_t ulSize = 0U;

    /* Check if the pxPacketInfo contains a valid SUBACK packet. */
    configASSERT( pxPacketInfo != NULL );
    configASSERT( pxPacketInfo->type == MQTT_PACKET_TYPE_SUBACK );
    configASSERT( pxPacketInfo->pRemainingData != NULL );
    configASSERT( pxPacketInfo->remainingLength >= 3U );

    pucPayload = pxPacketInfo->pRemainingData + ( ( uint16_t ) sizeof( uint16_t ) );
    ulSize = pxPacketInfo->remainingLength - sizeof( uint16_t );

    for( ulTopicCount = 0; ulTopicCount < ulSize; ulTopicCount++ )
    {
        /* 0x80 denotes that the broker rejected subscription to a topic filter.
         * Multiply the index by 2 because the status code consists of two bytes. */
        if( pucPayload[ ulTopicCount * 2 ] & 0x80 )
        {
            xTopicFilterContext[ ulTopicCount ].xSubAckSuccess = false;
        }
        else
        {
            xTopicFilterContext[ ulTopicCount ].xSubAckSuccess = true;
        }
    }
}
/*-----------------------------------------------------------*/

static void prvMQTTPublishToTopic( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    MQTTPublishInfo_t xMQTTPublishInfo;
    size_t xRemainingLength;
    size_t xPacketSize;
    size_t xHeaderSize;
    BaseType_t xStatus;

    /***
     * For readability, error handling in this function is restricted to the use of
     * asserts().
     ***/

    /* Some fields not used by this demo so start with everything at 0. */
    ( void ) memset( ( void * ) &xMQTTPublishInfo, 0x00, sizeof( xMQTTPublishInfo ) );

    /* This demo uses QoS0. */
    xMQTTPublishInfo.qos = MQTTQoS0;
    xMQTTPublishInfo.retain = false;
    xMQTTPublishInfo.pTopicName = mqttexampleTOPIC;
    xMQTTPublishInfo.topicNameLength = ( uint16_t ) strlen( mqttexampleTOPIC );
    xMQTTPublishInfo.pPayload = mqttexampleMESSAGE;
    xMQTTPublishInfo.payloadLength = strlen( mqttexampleMESSAGE );

    /* Find out length of Publish packet size. */
    xResult = MQTT_GetPublishPacketSize( &xMQTTPublishInfo,
                                         &xRemainingLength,
                                         &xPacketSize );
    configASSERT( xResult == MQTTSuccess );

    /* Make sure the packet size is less than static buffer size. */
    configASSERT( xPacketSize < mqttexampleSHARED_BUFFER_SIZE );

    /* Serialize MQTT Publish packet header. The publish message payload will
     * be sent directly in order to avoid copying it into the buffer.
     * QOS0 does not make use of packet identifier, therefore value of 0 is used */
    xResult = MQTT_SerializePublishHeader( &xMQTTPublishInfo,
                                           0,
                                           xRemainingLength,
                                           &xBuffer,
                                           &xHeaderSize );
    configASSERT( xResult == MQTTSuccess );

    /* Send Publish header to the broker. */
    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xHeaderSize,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xHeaderSize );

    /* Send Publish payload to the broker. */
    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xMQTTPublishInfo.pPayload,
                             xMQTTPublishInfo.payloadLength,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xMQTTPublishInfo.payloadLength );
}
/*-----------------------------------------------------------*/

static void prvMQTTUnsubscribeFromTopic( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    size_t xRemainingLength;
    size_t xPacketSize;
    BaseType_t xStatus;
    MQTTSubscribeInfo_t xMQTTSubscription[ 1 ];

    /* Some fields not used by this demo so start with everything at 0. */
    ( void ) memset( ( void * ) &xMQTTSubscription, 0x00, sizeof( xMQTTSubscription ) );

    /* Subscribe to the mqttexampleTOPIC topic filter. This example subscribes to
     * only one topic and uses QoS0. */
    xMQTTSubscription[ 0 ].qos = MQTTQoS0;
    xMQTTSubscription[ 0 ].pTopicFilter = mqttexampleTOPIC;
    xMQTTSubscription[ 0 ].topicFilterLength = ( uint16_t ) strlen( mqttexampleTOPIC );


    xResult = MQTT_GetUnsubscribePacketSize( xMQTTSubscription,
                                             sizeof( xMQTTSubscription ) / sizeof( MQTTSubscribeInfo_t ),
                                             &xRemainingLength,
                                             &xPacketSize );
    configASSERT( xResult == MQTTSuccess );
    /* Make sure the packet size is less than static buffer size */
    configASSERT( xPacketSize < mqttexampleSHARED_BUFFER_SIZE );

    /* Get next unique packet identifier */
    usUnsubscribePacketIdentifier = prvGetNextPacketIdentifier();
    /* Make sure the packet id obtained is valid. */
    configASSERT( usUnsubscribePacketIdentifier != 0 );

    xResult = MQTT_SerializeUnsubscribe( xMQTTSubscription,
                                         sizeof( xMQTTSubscription ) / sizeof( MQTTSubscribeInfo_t ),
                                         usUnsubscribePacketIdentifier,
                                         xRemainingLength,
                                         &xBuffer );
    configASSERT( xResult == MQTTSuccess );

    /* Send Unsubscribe request to the broker. */
    xStatus = FreeRTOS_send( xMQTTSocket, ( void * ) xBuffer.pBuffer, xPacketSize, 0 );
    configASSERT( xStatus == ( BaseType_t ) xPacketSize );
}
/*-----------------------------------------------------------*/

static void prvMQTTKeepAlive( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    BaseType_t xStatus;
    size_t xPacketSize;

    /* Calculate PING request size. */
    xResult = MQTT_GetPingreqPacketSize( &xPacketSize );
    configASSERT( xResult == MQTTSuccess );
    configASSERT( xPacketSize <= mqttexampleSHARED_BUFFER_SIZE );

    xResult = MQTT_SerializePingreq( &xBuffer );
    configASSERT( xResult == MQTTSuccess );

    /* Send Ping Request to the broker. */
    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xPacketSize,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xPacketSize );
}

/*-----------------------------------------------------------*/

static void prvMQTTDisconnect( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    BaseType_t xStatus;
    size_t xPacketSize;

    /* Calculate DISCONNECT packet size. */
    xResult = MQTT_GetDisconnectPacketSize( &xPacketSize );
    configASSERT( xResult == MQTTSuccess );
    configASSERT( xPacketSize <= mqttexampleSHARED_BUFFER_SIZE );

    xResult = MQTT_SerializeDisconnect( &xBuffer );
    configASSERT( xResult == MQTTSuccess );

    xStatus = FreeRTOS_send( xMQTTSocket,
                             ( void * ) xBuffer.pBuffer,
                             xPacketSize,
                             0 );
    configASSERT( xStatus == ( BaseType_t ) xPacketSize );
}

/*-----------------------------------------------------------*/

static void prvMQTTProcessResponse( MQTTPacketInfo_t * pxIncomingPacket,
                                    uint16_t usPacketId )
{
    uint32_t ulTopicCount = 0U;

    switch( pxIncomingPacket->type )
    {
        case MQTT_PACKET_TYPE_SUBACK:

            /* Check if recent subscription request has been accepted. #xTopicFilterContext is updated
             * in #prvMQTTProcessIncomingPacket to reflect the status of the SUBACK sent by the broker. */
            for( ulTopicCount = 0; ulTopicCount < mqttexampleTOPIC_COUNT; ulTopicCount++ )
            {
                if( xTopicFilterContext[ ulTopicCount ].xSubAckSuccess != false )
                {
                    LogInfo( ( "Subscribed to the topic %s with maximum QoS %u.\r\n",
                               xTopicFilterContext[ ulTopicCount ].pcTopicFilter,
                               xTopicFilterContext[ ulTopicCount ].xSubAckSuccess ) );
                }
            }

            /* Make sure ACK packet identifier matches with Request packet identifier. */
            configASSERT( usSubscribePacketIdentifier == usPacketId );
            break;

        case MQTT_PACKET_TYPE_UNSUBACK:
            LogInfo( ( "Unsubscribed from the topic %s.\r\n", mqttexampleTOPIC ) );
            /* Make sure ACK packet identifier matches with Request packet identifier. */
            configASSERT( usUnsubscribePacketIdentifier == usPacketId );
            break;

        case MQTT_PACKET_TYPE_PINGRESP:
            LogInfo( ( "Ping Response successfully received.\r\n" ) );
            break;

        /* Any other packet type is invalid. */
        default:
            LogWarn( ( "prvMQTTProcessResponse() called with unknown packet type:(%02X).\r\n",
                       pxIncomingPacket->type ) );
    }
}

/*-----------------------------------------------------------*/

static void prvMQTTProcessIncomingPublish( MQTTPublishInfo_t * pxPublishInfo )
{
    configASSERT( pxPublishInfo != NULL );

    /* Process incoming Publish. */
    LogInfo( ( "Incoming QoS : %d\n", pxPublishInfo->qos ) );

    /* Verify the received publish is for the we have subscribed to. */
    if( ( pxPublishInfo->topicNameLength == strlen( mqttexampleTOPIC ) ) &&
        ( 0 == strncmp( mqttexampleTOPIC,
                        pxPublishInfo->pTopicName,
                        pxPublishInfo->topicNameLength ) ) )
    {
        LogInfo( ( "\r\nIncoming Publish Topic Name: %.*s matches subscribed topic.\r\n"
                   "Incoming Publish Message : %.*s\r\n",
                   pxPublishInfo->topicNameLength,
                   pxPublishInfo->pTopicName,
                   pxPublishInfo->payloadLength,
                   pxPublishInfo->pPayload ) );
    }
    else
    {
        LogInfo( ( "Incoming Publish Topic Name: %.*s does not match subscribed topic.\r\n",
                   pxPublishInfo->topicNameLength,
                   pxPublishInfo->pTopicName ) );
    }
}

/*-----------------------------------------------------------*/

static void prvMQTTProcessIncomingPacket( Socket_t xMQTTSocket )
{
    MQTTStatus_t xResult;
    MQTTPacketInfo_t xIncomingPacket;
    BaseType_t xStatus;
    MQTTPublishInfo_t xPublishInfo;
    uint16_t usPacketId;
    NetworkContext_t xNetworkContext;

    /***
     * For readability, error handling in this function is restricted to the use of
     * asserts().
     ***/

    ( void ) memset( ( void * ) &xIncomingPacket, 0x00, sizeof( MQTTPacketInfo_t ) );

    /* Determine incoming packet type and remaining length. */
    xNetworkContext.xTcpSocket = xMQTTSocket;

    xResult = MQTT_GetIncomingPacketTypeAndLength( prvTransportRecv,
                                                   &xNetworkContext,
                                                   &xIncomingPacket );

    if( xResult != MQTTNoDataAvailable )
    {
        configASSERT( xResult == MQTTSuccess );
        configASSERT( xIncomingPacket.remainingLength <= mqttexampleSHARED_BUFFER_SIZE );

        /* Current implementation expects an incoming Publish and three different
         * responses ( SUBACK, PINGRESP and UNSUBACK ). */

        /* Receive the remaining bytes. In case of PINGRESP, remaining length will be zero.
         * Skip reading from network for remaining length zero. */
        if( xIncomingPacket.remainingLength > 0 )
        {
            xStatus = FreeRTOS_recv( xMQTTSocket,
                                     ( void * ) xBuffer.pBuffer,
                                     xIncomingPacket.remainingLength, 0 );
            configASSERT( xStatus == ( BaseType_t ) xIncomingPacket.remainingLength );
            xIncomingPacket.pRemainingData = xBuffer.pBuffer;
        }

        /* Check if the incoming packet is a publish packet. */
        if( ( xIncomingPacket.type & 0xf0 ) == MQTT_PACKET_TYPE_PUBLISH )
        {
            xResult = MQTT_DeserializePublish( &xIncomingPacket, &usPacketId, &xPublishInfo );
            configASSERT( xResult == MQTTSuccess );

            /* Process incoming Publish message. */
            prvMQTTProcessIncomingPublish( &xPublishInfo );
        }
        else
        {
            /* If the received packet is not a Publish message, then it is an ACK for one
             * of the messages we sent out, verify that the ACK packet is a valid MQTT
             * packet. Session present is only valid for a CONNACK. CONNACK is not
             * expected to be received here. Hence pass NULL for pointer to session
             * present. */
            xResult = MQTT_DeserializeAck( &xIncomingPacket, &usPacketId, NULL );
            configASSERT( xResult == MQTTSuccess );

            if( xIncomingPacket.type == MQTT_PACKET_TYPE_SUBACK )
            {
                prvMQTTUpdateSubAckStatus( &xIncomingPacket );

                /* #MQTTServerRefused is returned when the broker refuses the client
                 * to subscribe to a specific topic filter. */
                configASSERT( xResult == MQTTSuccess || xResult == MQTTServerRefused );
            }
            else
            {
                configASSERT( xResult == MQTTSuccess );
            }

            /* Process the response. */
            prvMQTTProcessResponse( &xIncomingPacket, usPacketId );
        }
    }
}

/*-----------------------------------------------------------*/
