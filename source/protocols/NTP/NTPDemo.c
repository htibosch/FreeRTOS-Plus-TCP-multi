/*
 * FreeRTOS+TCP V2.3.0
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
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
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/*
 * NTPDemo.c
 *
 * An example of how to lookup a domain using DNS
 * And also how to send and receive UDP messages to get the NTP time
 *
 */

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_DNS.h"
#include "FreeRTOS_Stream_Buffer.h"

/* Use the date & time functions from +FAT. */
#if( USE_PLUS_FAT != 0 )
	#include "ff_time.h"
#endif	/* ( USE_PLUS_FAT != 0 ) */

#include "NTPDemo.h"
#include "ntpClient.h"

#include "date_and_time.h"

#if( ipconfigDNS_USE_CALLBACKS == 0 )
	#error ipconfigDNS_USE_CALLBACKS must be 1
#endif

#warning Testing developing
void dump_packet( const uint8_t *pucBuffer, size_t uxLength );

int stime(const time_t *t);

enum EStatus {
	EStatusLookup,
	EStatusAsking,
	EStatusPause,
	EStatusFailed,
};

static struct SNtpPacket xNTPPacket;

BaseType_t xNTPHasTime;
uint32_t ulNTPTime;

#if( ipconfigUSE_CALLBACKS == 0 )
	static char cRecvBuffer[ sizeof( struct SNtpPacket ) + 64 ];
#endif

static enum EStatus xStatus = EStatusLookup;

static BaseType_t xIPFamily = FREERTOS_AF_INET6;
#if( ipconfigMULTI_INTERFACE != 0 )
	struct freertos_addrinfo *pxDNSResult;
#endif

static const char *pcTimeServers[] = {
	"0.asia.pool.ntp.org",
	"0.europe.pool.ntp.org",
	"0.id.pool.ntp.org",
	"0.south-america.pool.ntp.org",
	"0.oceania.pool.ntp.org",
	"0.north-america.pool.ntp.org"
};

static SemaphoreHandle_t xNTPWakeupSem = NULL;
static uint32_t ulIPAddressFound;

#if( ipconfigUSE_IPv6 != 0 )
static IPv6_Address_t xAddressFound;
#endif

static Socket_t xUDPSocket = NULL;
static TaskHandle_t xNTPTaskhandle = NULL;
static TickType_t uxSendTime;

static void prvNTPTask( void *pvParameters );

static void vSignalTask( void )
{
	#if( ipconfigUSE_CALLBACKS == 0 )
	if( xUDPSocket != NULL )
	{
		/* Send a signal to the socket so that the
		FreeRTOS_recvfrom will get interrupted. */
		FreeRTOS_SignalSocket( xUDPSocket );
	}
	else
	#endif
	if( xNTPWakeupSem != NULL )
	{
		xSemaphoreGive( xNTPWakeupSem );
	}
}

void vStartNTPTask( uint16_t usTaskStackSize, UBaseType_t uxTaskPriority )
{
	/* The only public function in this module: start a task to contact
	some NTP server. */

	if( xNTPTaskhandle != NULL )
	{
		switch( xStatus )
		{
		case EStatusPause:
			xStatus = EStatusAsking;
			vSignalTask();
			break;
		case EStatusLookup:
			FreeRTOS_printf( ( "NTP looking up server\n" ) );
			break;
		case EStatusAsking:
			FreeRTOS_printf( ( "NTP still asking\n" ) );
			break;
		case EStatusFailed:
			FreeRTOS_printf( ( "NTP failed somehow\n" ) );
			ulIPAddressFound = 0ul;
			xStatus = EStatusLookup;
			vSignalTask();
			break;
		}
	}
	else
	{
		xUDPSocket = FreeRTOS_socket( FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP );
		if( xUDPSocket != NULL )
		{
		struct freertos_sockaddr xAddress;
		#if( ipconfigUSE_CALLBACKS != 0 )
			BaseType_t xReceiveTimeOut = pdMS_TO_TICKS( 0 );
		#else
			BaseType_t xReceiveTimeOut = pdMS_TO_TICKS( 5000 );
		#endif

			xAddress.sin_addr = 0ul;
			xAddress.sin_port = FreeRTOS_htons( NTP_PORT );

			FreeRTOS_bind( xUDPSocket, &xAddress, sizeof( xAddress ) );
			FreeRTOS_setsockopt( xUDPSocket, 0, FREERTOS_SO_RCVTIMEO, &xReceiveTimeOut, sizeof( xReceiveTimeOut ) );
			xTaskCreate( 	prvNTPTask,						/* The function that implements the task. */
							( const char * ) "NTP client",	/* Just a text name for the task to aid debugging. */
							usTaskStackSize,				/* The stack size is defined in FreeRTOSIPConfig.h. */
							NULL,							/* The task parameter, not used in this case. */
							uxTaskPriority,					/* The priority assigned to the task is defined in FreeRTOSConfig.h. */
							&xNTPTaskhandle );				/* The task handle. */
		}
		else
		{
			FreeRTOS_printf( ( "Creating socket failed\n" ) );
		}
	}
}
/*-----------------------------------------------------------*/

#if( ipconfigUSE_IPv6 != 0 )
static void vDNS_callback( const char *pcName, void *pvSearchID, struct freertos_addrinfo *pxAddrInfo )
{
	xStatus = EStatusAsking;

	( void ) pvSearchID;

	if( pxAddrInfo == NULL )
	{
		FreeRTOS_printf( ( "vDNS_callback: No IPv%c address found\n", ( xIPFamily == FREERTOS_AF_INET4 ) ? '4' : '6' ) );
	}
	else
	{
		switch( pxAddrInfo->ai_family )
		{
		case FREERTOS_AF_INET4:
			{
				char pcBuf[16];
				uint32_t ulIPAddress;

				/* The DNS lookup has a result, or it has reached the time-out. */
				ulIPAddress = pxAddrInfo->ai_addr->sin_addr;
				FreeRTOS_inet_ntoa( ulIPAddress, pcBuf );
				FreeRTOS_printf( ( "vDNS_callback: IP address of %s found: %s\n", pcName, pcBuf ) );
				if( ulIPAddressFound == 0ul )
				{
					ulIPAddressFound = FreeRTOS_inet_addr_quick( 162, 159, 200, 1 );
				}
			}
			break;
		case FREERTOS_AF_INET6:
			{
			const struct freertos_sockaddr6 *pxAddress6 = ( const struct freertos_sockaddr6 * ) pxAddrInfo->ai_addr;

				FreeRTOS_printf( ( "vDNS_callback: IPv6 replied: %pip\n", pxAddress6->sin_addrv6.ucBytes ) );
				memcpy( xAddressFound.ucBytes, pxAddress6->sin_addrv6.ucBytes, sizeof( xAddressFound.ucBytes ) );
				/* Indicate that an address has been found. */
				ulIPAddressFound = 1U;
			}
			break;
		default:
			{
				FreeRTOS_printf( ( "vDNS_callback: address family %02lx not supported\n", pxAddrInfo->ai_family ) );
			}
			break;
		}
	}

	vSignalTask();
}
#else
static void vDNS_callback( const char *pcName, void *pvSearchID, uint32_t ulIPAddress )
{
char pcBuf[16];

	/* The DNS lookup has a result, or it has reached the time-out. */
	FreeRTOS_inet_ntoa( ulIPAddress, pcBuf );
	FreeRTOS_printf( ( "IP address of %s found: %s\n", pcName, pcBuf ) );
	if( ulIPAddressFound == 0ul )
	{
		ulIPAddressFound = ulIPAddress;
	}
	/* For testing: in case DNS doen't respond, still try some NTP server
	with a known IP-address. */
	if( ulIPAddressFound == 0ul )
	{
		ulIPAddressFound = FreeRTOS_inet_addr_quick( 184, 105, 182, 7 );
/*		ulIPAddressFound = FreeRTOS_inet_addr_quick( 103, 242,  70, 4 );	*/
	}
	xStatus = EStatusAsking;

	vSignalTask();
}
#endif
/*-----------------------------------------------------------*/

static void prvSwapFields( struct SNtpPacket *pxPacket)
{
	/* NTP messages are big-endian */
	pxPacket->rootDelay = FreeRTOS_htonl( pxPacket->rootDelay );
	pxPacket->rootDispersion = FreeRTOS_htonl( pxPacket->rootDispersion );

	pxPacket->referenceTimestamp.seconds = FreeRTOS_htonl( pxPacket->referenceTimestamp.seconds );
	pxPacket->referenceTimestamp.fraction = FreeRTOS_htonl( pxPacket->referenceTimestamp.fraction );

	pxPacket->originateTimestamp.seconds = FreeRTOS_htonl( pxPacket->originateTimestamp.seconds );
	pxPacket->originateTimestamp.fraction = FreeRTOS_htonl( pxPacket->originateTimestamp.fraction );

	pxPacket->receiveTimestamp.seconds = FreeRTOS_htonl( pxPacket->receiveTimestamp.seconds );
	pxPacket->receiveTimestamp.fraction = FreeRTOS_htonl( pxPacket->receiveTimestamp.fraction );

	pxPacket->transmitTimestamp.seconds = FreeRTOS_htonl( pxPacket->transmitTimestamp.seconds );
	pxPacket->transmitTimestamp.fraction = FreeRTOS_htonl( pxPacket->transmitTimestamp.fraction );
}
/*-----------------------------------------------------------*/

static void prvNTPPacketInit( )
{
	memset (&xNTPPacket, '\0', sizeof( xNTPPacket ) );

	xNTPPacket.flags = 0xDB;				/* value 0xDB : mode 3 (client), version 3, leap indicator unknown 3 */
	xNTPPacket.poll = 10;					/* 10 means 1 << 10 = 1024 seconds */
	xNTPPacket.precision = 0xE9;			/* = 250 = 0.015625 seconds */
	xNTPPacket.rootDelay = 0x00001087;		/* 0x5D2E = 23854 or (23854/65535)= 0.3640 sec */
	xNTPPacket.rootDispersion = 0x00091FC7;	/* 0x0008CAC8 = 8.7912  seconds */

	/* use the recorded NTP time */
	time_t uxSecs = FreeRTOS_time( NULL );/* apTime may be NULL, returns seconds */

	xNTPPacket.referenceTimestamp.seconds = uxSecs;	/* Current time */
	xNTPPacket.transmitTimestamp.seconds = uxSecs + 3;

	/* Transform the contents of the fields from native to big endian. */
	prvSwapFields( &xNTPPacket );
}
/*-----------------------------------------------------------*/

static void prvReadTime( struct SNtpPacket * pxPacket )
{
#if( USE_PLUS_FAT != 0 )
	FF_TimeStruct_t xTimeStruct;
#else
	struct tm  xTimeStruct;;
#endif

	time_t uxPreviousSeconds;
	time_t uxPreviousMS;

	time_t uxCurrentSeconds;
	time_t uxCurrentMS;

	const char *pcTimeUnit;
	int32_t ilDiff;
	TickType_t uxTravelTime;

	uxTravelTime = xTaskGetTickCount() - uxSendTime;

	/* Transform the contents of the fields from big to native endian. */
	prvSwapFields( pxPacket );

	FreeRTOS_printf( ( "Stratum 0x%02x Poll %u Prec %u Dely %u\n",
	  /** Stratum of the clock. */
	  ( int ) pxPacket->stratum,  // value 0 : unspecified
	  /** Maximum interval between successive messages, in log2 seconds. Note that the value is signed. */
	  ( unsigned ) pxPacket->poll,  // 10 means 1 << 10 = 1024 seconds
	  /** Precision of the clock, in log2 seconds. Note that the value is signed. */
	  ( unsigned ) pxPacket->precision, // 0xFA = 250 = 0.015625 seconds
	  /** Round trip time to the primary reference source, in NTP short format. */
	  ( unsigned ) pxPacket->rootDelay ) ); // 0x5D2E = 23854 or (23854/65535)= 0.3640 sec

	FreeRTOS_printf( ( "Time %u frac %u\n",
		( unsigned ) ( pxPacket->receiveTimestamp.seconds - TIME1970 ),
		( unsigned ) ( pxPacket->receiveTimestamp.fraction / 4294967U ) ) );

	uxCurrentSeconds = pxPacket->receiveTimestamp.seconds - TIME1970;
	uxCurrentMS = pxPacket->receiveTimestamp.fraction / 4294967;
	uxCurrentSeconds += uxCurrentMS / 1000;
	uxCurrentMS = uxCurrentMS % 1000;

	// Get the last time recorded
	uxPreviousSeconds = FreeRTOS_get_secs_msec( &uxPreviousMS );

	// Set the new time with precision in msec. */
	FreeRTOS_set_secs_msec( &uxCurrentSeconds, &uxCurrentMS );

	if( uxCurrentSeconds >= uxPreviousSeconds )
	{
		ilDiff = ( int32_t ) ( uxCurrentSeconds - uxPreviousSeconds );
	}
	else
	{
		ilDiff = 0 - ( int32_t ) ( uxPreviousSeconds - uxCurrentSeconds );
	}

	if( ( ilDiff < -5 ) || ( ilDiff > 5 ) )
	{
		/* More than 5 seconds difference. */
		pcTimeUnit = "sec";
	}
	else
	{
		/* Less than or equal to 5 second difference. */
		pcTimeUnit = "ms";
		uint32_t ulLowest = ( uxCurrentSeconds <= uxPreviousSeconds ) ? uxCurrentSeconds : uxPreviousSeconds;
		int32_t iCurMS = 1000 * ( uxCurrentSeconds - ulLowest ) + uxCurrentMS;
		int32_t iPrevMS = 1000 * ( uxPreviousSeconds - ulLowest ) + uxPreviousMS;
		ilDiff = iCurMS - iPrevMS;
	}
	uxCurrentSeconds -= iTimeZone;

#if( USE_PLUS_FAT != 0 )
	FreeRTOS_gmtime_r( &uxCurrentSeconds, &xTimeStruct );
#else
	gmtime_r( &uxCurrentSeconds, &xTimeStruct );
#endif	/* ( USE_PLUS_FAT != 0 ) */

	/*
		378.067 [NTP client] NTP time: 9/11/2015 16:11:19.559 Diff -20 ms (289 ms)
		379.441 [NTP client] NTP time: 9/11/2015 16:11:20.933 Diff 0 ms (263 ms)
	*/

	FreeRTOS_printf( ("NTP time: %d/%d/%02d %2d:%02d:%02d.%03u Diff %d %s (%lu ms)\n",
		xTimeStruct.tm_mday,
		xTimeStruct.tm_mon + 1,
		xTimeStruct.tm_year + 1900,
		xTimeStruct.tm_hour,
		xTimeStruct.tm_min,
		xTimeStruct.tm_sec,
		( unsigned )uxCurrentMS,
		( unsigned )ilDiff,
		pcTimeUnit,
		uxTravelTime ) );

	xNTPHasTime = pdTRUE;
	ulNTPTime = uxCurrentSeconds;
	stime( &uxCurrentSeconds );

	/* Remove compiler warnings in case FreeRTOS_printf() is not used. */
	( void ) pcTimeUnit;
	( void ) uxTravelTime;
}
/*-----------------------------------------------------------*/

#if( ipconfigUSE_CALLBACKS != 0 )

	static BaseType_t xOnUDPReceive( Socket_t xSocket, void * pvData, size_t xLength,
		const struct freertos_sockaddr *pxFrom, const struct freertos_sockaddr *pxDest )
	{
		( void ) xSocket;
		( void ) pxDest;

		if( xLength >= sizeof( xNTPPacket ) )
		{
FreeRTOS_printf( ( "Recv NTP from %xip:%u\n", ( unsigned ) FreeRTOS_ntohl( pxFrom->sin_addr ), FreeRTOS_ntohs( pxFrom->sin_port ) ) );
dump_packet( ( const uint8_t * )pvData, xLength );

			prvReadTime( ( struct SNtpPacket *)pvData );
			if( xStatus != EStatusPause )
			{
				xStatus = EStatusPause;
			}
		}
		vSignalTask();
		/* Tell the driver not to store the RX data */
		return 1;
	}
	/*-----------------------------------------------------------*/

#endif	/* ipconfigUSE_CALLBACKS != 0 */

static void prvNTPTask( void *pvParameters )
{
BaseType_t xServerIndex = 3;
struct freertos_sockaddr xAddress;
#if( ipconfigUSE_CALLBACKS != 0 )
	F_TCP_UDP_Handler_t xHandler;
#endif /* ipconfigUSE_CALLBACKS != 0 */

	( void ) pvParameters;

	xStatus = EStatusLookup;
	#if( ipconfigSOCKET_HAS_USER_SEMAPHORE != 0 ) || ( ipconfigUSE_CALLBACKS != 0 )
	{
		xNTPWakeupSem = xSemaphoreCreateBinary();
	}
	#endif

	#if( ipconfigUSE_CALLBACKS != 0 )
	{
		memset( &xHandler, '\0', sizeof( xHandler ) );
		xHandler.pxOnUDPReceive = xOnUDPReceive;
		FreeRTOS_setsockopt( xUDPSocket, 0, FREERTOS_SO_UDP_RECV_HANDLER, ( void * ) &xHandler, sizeof( xHandler ) );
	}
	#endif
	#if( ipconfigSOCKET_HAS_USER_SEMAPHORE != 0 )
	{
		FreeRTOS_setsockopt( xUDPSocket, 0, FREERTOS_SO_SET_SEMAPHORE, ( void * ) &xNTPWakeupSem, sizeof( xNTPWakeupSem ) );
	}
	#endif
	for( ; ; )
	{
		switch( xStatus )
		{
		case EStatusLookup:
			if( ( ulIPAddressFound == 0ul ) || ( ulIPAddressFound == ~0ul ) )
			{
			char pcHostName[ 65 ];
				snprintf( pcHostName, sizeof pcHostName, "%s", pcTimeServers[ xServerIndex ] );

				if( ++xServerIndex == sizeof( pcTimeServers ) / sizeof( pcTimeServers[ 0 ] ) )
				{
					xServerIndex = 0;
					if( xIPFamily == FREERTOS_AF_INET6 )
					{
						xIPFamily = FREERTOS_AF_INET4;
					}
					else
					{
						xIPFamily = FREERTOS_AF_INET6;
					}
				}
				if( xIPFamily == FREERTOS_AF_INET6 )
				{
					pcHostName[ 0 ] = '2';
				}
				FreeRTOS_printf( ( "Looking up server '%s' with IPv%c\n", pcTimeServers[ xServerIndex ], ( xIPFamily == FREERTOS_AF_INET4 ) ? '4' : '6' ) );
				/* The asynchronous verions of FreeRTOS_getaddrinfo(). */
			#if( ipconfigMULTI_INTERFACE != 0 )
				{
				struct freertos_addrinfo xHints;

					memset( &( xHints ) , 0, sizeof( xHints ) );
					xHints.ai_family = xIPFamily;
					FreeRTOS_getaddrinfo_a( pcHostName,			/* The name of the node or device */
											NULL,				/* pcService: Ignored for now. */
											&( xHints ),		/* If not NULL: preferences. */
											&( pxDNSResult ),	/* An allocated struct, containing the results. */
											vDNS_callback,
											NULL,				/* void *pvSearchID, */
											3000U );			/* Time-out. */
				}
			#else
				{
					FreeRTOS_gethostbyname_a( pcHostName, vDNS_callback, (void *)NULL, 3000U );
				}
			#endif /* ( ipconfigMULTI_INTERFACE != 0 ) */
			}
			else
			{
FreeRTOS_printf( ( "Server found at %xip\n", ( unsigned ) ulIPAddressFound ) );
				xStatus = EStatusAsking;
			}
			break;

		case EStatusAsking:
			{
				prvNTPPacketInit( );
				memset( &( xAddress ), 0, sizeof ( xAddress ) );
				#if( ipconfigUSE_IPv6 != 0 )
				if( xIPFamily == FREERTOS_AF_INET6 )
				{
					struct freertos_sockaddr6 *pxAddress6 = ( struct freertos_sockaddr6 * ) &( xAddress );
					pxAddress6->sin_port = FreeRTOS_htons( NTP_PORT );
					memcpy( pxAddress6->sin_addrv6.ucBytes, xAddressFound.ucBytes, sizeof  pxAddress6->sin_addrv6.ucBytes );
					pxAddress6->sin_family = FREERTOS_AF_INET6;
					FreeRTOS_printf( ( "Sending UDP message to %pip port %u\n",
						pxAddress6->sin_addrv6.ucBytes,
						( unsigned ) FreeRTOS_ntohs( pxAddress6->sin_port ) ) );
				}
				else
				#endif
				{
					xAddress.sin_addr = ulIPAddressFound;
					xAddress.sin_port = FreeRTOS_htons( NTP_PORT );
					xAddress.sin_family = FREERTOS_AF_INET;
					FreeRTOS_printf( ( "Sending UDP message to %xip port %u\n",
						( unsigned ) FreeRTOS_ntohl( ulIPAddressFound ),
						( unsigned ) FreeRTOS_ntohs( xAddress.sin_port ) ) );
				}

				uxSendTime = xTaskGetTickCount( );
FreeRTOS_printf( ( "Send NTP \n" ) );
dump_packet( ( const uint8_t * )&xNTPPacket, sizeof( xNTPPacket ) );
				FreeRTOS_sendto( xUDPSocket, ( void * )&xNTPPacket, sizeof( xNTPPacket ), 0, &xAddress, sizeof( xAddress ) );
			}
			break;

		case EStatusPause:
			break;

		case EStatusFailed:
			break;
		}

		#if( ipconfigUSE_CALLBACKS != 0 )
		{
			xSemaphoreTake( xNTPWakeupSem, 5000 );
		}
		#else
		{
		uint32_t xAddressSize;
		BaseType_t xReturned;

			xAddressSize = sizeof( xAddress );
			xReturned = FreeRTOS_recvfrom( xUDPSocket, ( void * ) cRecvBuffer, sizeof( cRecvBuffer ), 0, &xAddress, &xAddressSize );
			switch( xReturned )
			{
			case 0:
			case -pdFREERTOS_ERRNO_EAGAIN:
			case -pdFREERTOS_ERRNO_EINTR:
				break;
			default:
				if( xReturned < sizeof( xNTPPacket ) )
				{
					FreeRTOS_printf( ( "FreeRTOS_recvfrom: returns %ld\n", xReturned ) );
				}
				else
				{
					prvReadTime( ( struct SNtpPacket *)cRecvBuffer );
					if( xStatus != EStatusPause )
					{
						xStatus = EStatusPause;
					}
				}
				break;
			}
		}
		#endif
	}
}
/*-----------------------------------------------------------*/
