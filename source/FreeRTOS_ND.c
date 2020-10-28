/*
 * FreeRTOS+TCP V2.3.0
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
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/* The eniire module FreeRTOS_ND.c is skipped when IPv6 is not used. */
#if( ipconfigUSE_IPv6 != 0 )

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>


/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_ARP.h"
#include "FreeRTOS_UDP_IP.h"
#include "FreeRTOS_Routing.h"
#include "FreeRTOS_ND.h"
#if( ipconfigUSE_LLMNR == 1 )
	#include "FreeRTOS_DNS.h"
#endif /* ipconfigUSE_LLMNR */
#include "NetworkBufferManagement.h"

#define ndICMPv6_FLAG_SOLICITED					0x40000000UL
#define ndICMPv6_FLAG_UPDATE					0x20000000UL

/* Possible values of ucFlags in a neighbour advertisement. */
#ifndef _lint
	#define ndFlags_IsRouter		0x8000U
	#define ndFlags_Solicited		0x4000U
	#define ndFlags_Override		0x2000U
#endif

/* A block time of 0 simply means "don't block". */
#define ndDONT_BLOCK				( ( TickType_t ) 0 )

/* The character used to fill ICMP echo requests, and therefore also the
character expected to fill ICMP echo replies. */
#define ndECHO_DATA_FILL_BYTE						'x'

#define ipMULTICAST_MAC_ADDRESS_IPv6_0	0x33U
#define ipMULTICAST_MAC_ADDRESS_IPv6_1	0x33U

/*lint -e754 local struct member not referenced. */
/*lint -e766 Header file pack_struct_end.h' not used in module. */


/* All nodes on the local network segment. */
static const uint8_t pcLOCAL_ALL_NODES_MULTICAST_IP[ ipSIZE_OF_IPv6_ADDRESS ] = { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };	/* ff02:1 */
static const uint8_t pcLOCAL_ALL_NODES_MULTICAST_MAC[ ipMAC_ADDRESS_LENGTH_BYTES ] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 };

/*
 * See if the MAC-address can be resolved because it is a multi-cast address.
 */
static eARPLookupResult_t prvMACResolve( IPv6_Address_t *pxAddressToLookup, MACAddress_t * const pxMACAddress, NetworkEndPoint_t **ppxEndPoint );

/*
 * Lookup an MAC address in the ND cache from the IP address.
 */
static eARPLookupResult_t prvCacheLookup( IPv6_Address_t *pxAddressToLookup, MACAddress_t * const pxMACAddress, NetworkEndPoint_t **ppxEndPoint );

static const char *pcMessageType( BaseType_t xType );

/* The ND cache. */
static NDCacheRow_t xNDCache[ ipconfigND_CACHE_ENTRIES ];
/*-----------------------------------------------------------*/

/*
    ff02::1: All IPv6 devices
    ff02::2: All IPv6 routers
    ff02::5: All OSPFv3 routers
    ff02::a: All EIGRP (IPv6) routers
*/

NetworkEndPoint_t *pxFindLocalEndpoint()
{
NetworkEndPoint_t *pxEndPoint;

	for( pxEndPoint = FreeRTOS_FirstEndPoint( NULL );
		 pxEndPoint != NULL;
		 pxEndPoint = FreeRTOS_NextEndPoint( NULL, pxEndPoint ) )
	{
		
		if( pxEndPoint->bits.bIPv6 == pdTRUE_UNSIGNED )
		{
			break;
		}
	}
	return pxEndPoint;
}

static eARPLookupResult_t prvMACResolve( IPv6_Address_t *pxAddressToLookup, MACAddress_t * const pxMACAddress, NetworkEndPoint_t **ppxEndPoint )
{
eARPLookupResult_t eReturn;

	/* Is it a ff02::2 ( router ) address? */
	if( ( pxAddressToLookup->ucBytes[ 0 ] == 0xffU ) &&
		( pxAddressToLookup->ucBytes[ 1 ] == 0x02U ) )
	{
		pxMACAddress->ucBytes[ 0 ] = 0x33U;
		pxMACAddress->ucBytes[ 1 ] = 0x33U; 
		pxMACAddress->ucBytes[ 2 ] = pxAddressToLookup->ucBytes[ 12 ];
		pxMACAddress->ucBytes[ 3 ] = pxAddressToLookup->ucBytes[ 13 ];
		pxMACAddress->ucBytes[ 4 ] = pxAddressToLookup->ucBytes[ 14 ];
		pxMACAddress->ucBytes[ 5 ] = pxAddressToLookup->ucBytes[ 15 ];
		if( ppxEndPoint != NULL )
		{
			*ppxEndPoint = pxFindLocalEndpoint();
		}
		eReturn = eARPCacheHit;
	}
	else
	{
		/* Not a multicast IP address. */
		eReturn = eARPCacheMiss;
	}

	return eReturn;
}
/*-----------------------------------------------------------*/

eARPLookupResult_t eNDGetCacheEntry( IPv6_Address_t *pxIPAddress, MACAddress_t * const pxMACAddress, struct xNetworkEndPoint **ppxEndPoint )
{
eARPLookupResult_t eReturn;
NetworkEndPoint_t *pxEndPoint;

	/* Multi-cast addresses can be resolved immediately. */
	eReturn = prvMACResolve( pxIPAddress, pxMACAddress, ppxEndPoint );

	if(eReturn == eARPCacheMiss )
	{
		/* See if the IP-address has an entry in the cache. */
		eReturn = prvCacheLookup( pxIPAddress, pxMACAddress, ppxEndPoint );
	}
	if( eReturn == eARPCacheMiss )
	{
		pxEndPoint = FreeRTOS_FindEndPointOnIP_IPv6( pxIPAddress );
		if( pxEndPoint != NULL )
		{
			*( ppxEndPoint ) = pxEndPoint;
			FreeRTOS_printf( ( "eNDGetCacheEntry: FindEndPointOnIP %pip", pxEndPoint->ipv6_settings.xIPAddress.ucBytes ) );
		}
		else
		{
			pxEndPoint = FreeRTOS_FindGateWay( (BaseType_t ) ipTYPE_IPv6 );
			if( pxEndPoint != NULL )
			{
				( void ) memcpy( pxIPAddress->ucBytes, pxEndPoint->ipv6_settings.xGatewayAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );
				FreeRTOS_printf( ( "eNDGetCacheEntry: Using gw %pip", pxIPAddress->ucBytes ) );	/* access to 'ipv6_settings' is checked. */
				FreeRTOS_printf( ( "eNDGetCacheEntry: From addr %pip", pxEndPoint->ipv6_settings.xIPAddress.ucBytes ) );

				/* See if the gateway has an entry in the cache. */
				eReturn = prvCacheLookup( pxIPAddress, pxMACAddress, ppxEndPoint );
				*( ppxEndPoint ) = pxEndPoint;
			}
		}
	}

	return eReturn;
}
/*-----------------------------------------------------------*/

void vNDRefreshCacheEntry( const MACAddress_t * pxMACAddress, const IPv6_Address_t *pxIPAddress, NetworkEndPoint_t *pxEndPoint )
{
BaseType_t x;
BaseType_t xFreeEntry = -1, xEntryFound = -1;
	/* For each entry in the ARP cache table. */
	for( x = 0; x < ipconfigND_CACHE_ENTRIES; x++ )
	{
		if( xNDCache[ x ].ucValid == ( uint8_t )pdFALSE )
		{
			if( xFreeEntry == -1 )
			{
				xFreeEntry = x;
			}
		}
		else if( memcmp( xNDCache[ x ].xIPAddress.ucBytes, pxIPAddress->ucBytes, ipSIZE_OF_IPv6_ADDRESS ) == 0 )
		{
			xEntryFound = x;
			break;
		}
		else
		{
			/* Entry is valid but the IP-address doesn't match. */
		}
	}
	if( xEntryFound < 0 )
	{
		xEntryFound = xFreeEntry;
	}
	if( xEntryFound >= 0 )
	{
		/* Copy the IP-address. */
		( void ) memcpy( xNDCache[ xEntryFound ].xIPAddress.ucBytes, pxIPAddress->ucBytes, ipSIZE_OF_IPv6_ADDRESS );
		/* Copy the MAC-address. */
		( void ) memcpy( xNDCache[ xEntryFound ].xMACAddress.ucBytes, pxMACAddress->ucBytes, sizeof( MACAddress_t ) );
		xNDCache[ xEntryFound ].pxEndPoint = pxEndPoint;
		xNDCache[ xEntryFound ].ucAge = ( uint8_t ) ipconfigMAX_ARP_AGE;
		xNDCache[ xEntryFound ].ucValid = ( uint8_t ) pdTRUE;
		/* The format %pip will print a IPv6 address ( if printf-stdarg.c is included ). */
//		FreeRTOS_printf( ( "vNDRefreshCacheEntry[ %d ] %pip with %02x:%02x:%02x\n",
//			( int ) xEntryFound,
//			pxIPAddress->ucBytes,
//			pxMACAddress->ucBytes[ 3 ],
//			pxMACAddress->ucBytes[ 4 ],
//			pxMACAddress->ucBytes[ 5 ] ) );
	}
	else
	{
		FreeRTOS_printf( ( "vNDRefreshCacheEntry: %pip not found\n", pxIPAddress->ucBytes ) );
	}
}
/*-----------------------------------------------------------*/

void FreeRTOS_ClearND( void )
{
	( void ) memset( xNDCache, 0, sizeof( xNDCache ) );
}
/*-----------------------------------------------------------*/

static eARPLookupResult_t prvCacheLookup( IPv6_Address_t *pxAddressToLookup, MACAddress_t * const pxMACAddress, NetworkEndPoint_t **ppxEndPoint )
{
BaseType_t x;
eARPLookupResult_t eReturn = eARPCacheMiss;
	/* For each entry in the ARP cache table. */
	for( x = 0; x < ipconfigND_CACHE_ENTRIES; x++ )
	{
		if( xNDCache[ x ].ucValid == ( uint8_t ) pdFALSE )
		{
			/* Skip invalid entries. */
		}
		else if( memcmp( xNDCache[ x ].xIPAddress.ucBytes, pxAddressToLookup->ucBytes, ipSIZE_OF_IPv6_ADDRESS ) == 0 )
		{
			( void ) memcpy( pxMACAddress->ucBytes, xNDCache[ x ].xMACAddress.ucBytes, sizeof( MACAddress_t ) );
			eReturn = eARPCacheHit;
			*ppxEndPoint = xNDCache[ x ].pxEndPoint;
			FreeRTOS_printf( ( "prvCacheLookup6[ %d ] %pip with %02x:%02x:%02x:%02x:%02x:%02x\n",
				( int ) x,
				pxAddressToLookup->ucBytes,
				pxMACAddress->ucBytes[ 0 ],
				pxMACAddress->ucBytes[ 1 ],
				pxMACAddress->ucBytes[ 2 ],
				pxMACAddress->ucBytes[ 3 ],
				pxMACAddress->ucBytes[ 4 ],
				pxMACAddress->ucBytes[ 5 ] ) );
			break;
		}
		else
		{
			/* Entry is valid but the MAC-address doesn't match. */
		}
	}
	if( eReturn == eARPCacheMiss )
	{
		FreeRTOS_printf( ( "prvCacheLookup %pip Miss\n", pxAddressToLookup->ucBytes ) );
		*ppxEndPoint = NULL;
	}
	return eReturn;
}
/*-----------------------------------------------------------*/

#if( ( ipconfigHAS_PRINTF != 0 ) || ( ipconfigHAS_DEBUG_PRINTF != 0 ) )

	void FreeRTOS_PrintNDCache( void )
	{
	BaseType_t x, xCount = 0;

		/* Loop through each entry in the ARP cache. */
		for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
		{
			if( xNDCache[ x ].ucValid != ( uint8_t ) 0U )
			{
				/* See if the MAC-address also matches, and we're all happy */
				FreeRTOS_printf( ( "ND %2ld: %3u - %pip : %02x:%02x:%02x : %02x:%02x:%02x\n",
					x,
					xNDCache[ x ].ucAge,
					xNDCache[ x ].xIPAddress.ucBytes,
					xNDCache[ x ].xMACAddress.ucBytes[0],
					xNDCache[ x ].xMACAddress.ucBytes[1],
					xNDCache[ x ].xMACAddress.ucBytes[2],
					xNDCache[ x ].xMACAddress.ucBytes[3],
					xNDCache[ x ].xMACAddress.ucBytes[4],
					xNDCache[ x ].xMACAddress.ucBytes[5] ) );
				xCount++;
			}
		}

		FreeRTOS_printf( ( "Arp has %ld entries\n", xCount ) );
	}

#endif /* ( ipconfigHAS_PRINTF != 0 ) || ( ipconfigHAS_DEBUG_PRINTF != 0 ) */
/*-----------------------------------------------------------*/

static void prvReturnICMP_IPv6( NetworkBufferDescriptor_t * const pxNetworkBuffer, size_t uxICMPSize )
{
NetworkEndPoint_t *pxEndPoint = pxNetworkBuffer->pxEndPoint;
ICMPPacket_IPv6_t *pxICMPPacket = ipPOINTER_CAST( ICMPPacket_IPv6_t *, pxNetworkBuffer->pucEthernetBuffer );
ICMPHeader_IPv6_t *pxICMPHeader_IPv6 = ( ICMPHeader_IPv6_t * )&( pxICMPPacket->xICMPHeader_IPv6 );

	configASSERT( pxEndPoint != NULL );
	configASSERT( pxEndPoint->bits.bIPv6 != pdFALSE_UNSIGNED );
	
	( void ) memcpy( pxICMPPacket->xIPHeader.xDestinationAddress.ucBytes, pxICMPPacket->xIPHeader.xSourceAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );
	( void ) memcpy( pxICMPPacket->xIPHeader.xSourceAddress.ucBytes, pxEndPoint->ipv6_settings.xIPAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );
	pxICMPPacket->xIPHeader.usPayloadLength = FreeRTOS_htons( uxICMPSize );

	/* Important: tell NIC driver how many bytes must be sent */
	pxNetworkBuffer->xDataLength = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + uxICMPSize );

	pxICMPHeader_IPv6->usChecksum = 0;
	/* calculate the UDP checksum for outgoing package */
	( void ) usGenerateProtocolChecksum( pxNetworkBuffer->pucEthernetBuffer, pxNetworkBuffer->xDataLength, pdTRUE );

	/* This function will fill in the eth addresses and send the packet */
	vReturnEthernetFrame( pxNetworkBuffer, pdFALSE );
}
/*-----------------------------------------------------------*/

/*
 * Send out an ND request for the IPv6 address contained in pxNetworkBuffer, and
 * add an entry into the ND table that indicates that an ND reply is
 * outstanding so re-transmissions can be generated.
 */
void vNDSendNeighbourSolicitation( NetworkBufferDescriptor_t * const pxNetworkBuffer, IPv6_Address_t *pxIPAddress )
{
ICMPPacket_IPv6_t *pxICMPPacket;
ICMPHeader_IPv6_t *xICMPHeader_IPv6;
NetworkEndPoint_t *pxEndPoint = pxNetworkBuffer->pxEndPoint;
size_t uxNeededSize;
IPv6_Address_t xTargetIPAddress;
MACAddress_t xMultiCastMacAddress;
NetworkBufferDescriptor_t *pxDescriptor = pxNetworkBuffer;

	configASSERT( pxEndPoint != NULL );
	configASSERT( pxEndPoint->bits.bIPv6 != pdFALSE_UNSIGNED );

	uxNeededSize = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + sizeof( ICMPHeader_IPv6_t ) );
	if( pxDescriptor->xDataLength < uxNeededSize )
	{
		pxDescriptor = pxDuplicateNetworkBufferWithDescriptor( pxDescriptor, uxNeededSize );
		if( pxDescriptor == NULL )
		{
			return;	/*lint !e904 Return statement before end of function [MISRA 2012 Rule 15.5, advisory]. */
		}
	}
	pxICMPPacket = ipPOINTER_CAST( ICMPPacket_IPv6_t *, pxDescriptor->pucEthernetBuffer );
	xICMPHeader_IPv6 = ipPOINTER_CAST( ICMPHeader_IPv6_t *, &( pxICMPPacket->xICMPHeader_IPv6 ) );

	pxDescriptor->xDataLength = uxNeededSize;

	/* Set the multi-cast MAC-address. */
	xMultiCastMacAddress.ucBytes[ 0 ] = 0x33;
	xMultiCastMacAddress.ucBytes[ 1 ] = 0x33;
	xMultiCastMacAddress.ucBytes[ 2 ] = 0xff;
	xMultiCastMacAddress.ucBytes[ 3 ] = pxIPAddress->ucBytes[ 13 ];
	xMultiCastMacAddress.ucBytes[ 4 ] = pxIPAddress->ucBytes[ 14 ];
	xMultiCastMacAddress.ucBytes[ 5 ] = pxIPAddress->ucBytes[ 15 ];

	/* Set Ethernet header. Source and Destination will be swapped. */
	( void ) memcpy( pxICMPPacket->xEthernetHeader.xSourceAddress.ucBytes, xMultiCastMacAddress.ucBytes, ipMAC_ADDRESS_LENGTH_BYTES );
	( void ) memcpy( pxICMPPacket->xEthernetHeader.xDestinationAddress.ucBytes, pxEndPoint->xMACAddress.ucBytes, ipMAC_ADDRESS_LENGTH_BYTES );
	pxICMPPacket->xEthernetHeader.usFrameType = ipIPv6_FRAME_TYPE;

	/* Set IP-header. */
	pxICMPPacket->xIPHeader.ucVersionTrafficClass = 0x60;
	pxICMPPacket->xIPHeader.ucTrafficClassFlow = 0;
	pxICMPPacket->xIPHeader.usFlowLabel = 0;
	pxICMPPacket->xIPHeader.usPayloadLength = FreeRTOS_htons( 32U );/*lint !e845: (Info -- The right argument to operator '|' is certain to be 0. */
	pxICMPPacket->xIPHeader.ucNextHeader = ipPROTOCOL_ICMP_IPv6;
	pxICMPPacket->xIPHeader.ucHopLimit = 255;

	/* Source address "fe80::1" */
	( void ) memset( pxICMPPacket->xIPHeader.xSourceAddress.ucBytes, 0, sizeof pxICMPPacket->xIPHeader.xSourceAddress.ucBytes );
	pxICMPPacket->xIPHeader.xSourceAddress.ucBytes[  0 ] = 0xfeU;
	pxICMPPacket->xIPHeader.xSourceAddress.ucBytes[  1 ] = 0x80U;
	pxICMPPacket->xIPHeader.xSourceAddress.ucBytes[ 15 ] = 0x01U;

	//ff02::1:ff5a:afe7
	( void ) memset( xTargetIPAddress.ucBytes, 0, sizeof( xTargetIPAddress.ucBytes ) );
	xTargetIPAddress.ucBytes[  0 ] = 0xff;
	xTargetIPAddress.ucBytes[  1 ] = 0x02;
	xTargetIPAddress.ucBytes[ 11 ] = 0x01;
	xTargetIPAddress.ucBytes[ 12 ] = 0xff;
	xTargetIPAddress.ucBytes[ 13 ] = pxIPAddress->ucBytes[ 13 ];
	xTargetIPAddress.ucBytes[ 14 ] = pxIPAddress->ucBytes[ 14 ];
	xTargetIPAddress.ucBytes[ 15 ] = pxIPAddress->ucBytes[ 15 ];
	( void ) memcpy( pxICMPPacket->xIPHeader.xDestinationAddress.ucBytes, xTargetIPAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );

	/* Set ICMP header. */
	( void ) memset( xICMPHeader_IPv6, 0, sizeof( *xICMPHeader_IPv6 ) );
	xICMPHeader_IPv6->ucTypeOfMessage = ipICMP_NEIGHBOR_SOLICITATION_IPv6;
	( void ) memcpy( xICMPHeader_IPv6->xIPv6_Address.ucBytes, pxIPAddress->ucBytes, ipSIZE_OF_IPv6_ADDRESS );
	xICMPHeader_IPv6->ucOptionType = ndICMP_SOURCE_LINK_LAYER_ADDRESS;
	xICMPHeader_IPv6->ucOptionLength = 1;	/* times 8 bytes. */
	( void ) memcpy( xICMPHeader_IPv6->ucOptionBytes, pxEndPoint->xMACAddress.ucBytes, ipMAC_ADDRESS_LENGTH_BYTES );

	/* Checmsums. */
	xICMPHeader_IPv6->usChecksum = 0;
	/* calculate the UDP checksum for outgoing package */
	( void ) usGenerateProtocolChecksum( pxDescriptor->pucEthernetBuffer, pxDescriptor->xDataLength, pdTRUE );

	/* This function will fill in the eth addresses and send the packet */
	vReturnEthernetFrame( pxDescriptor, pdTRUE );
}
/*-----------------------------------------------------------*/

#if ( ipconfigSUPPORT_OUTGOING_PINGS == 1 )

	BaseType_t FreeRTOS_SendPingRequestIPv6( IPv6_Address_t *pxIPAddress, size_t uxNumberOfBytesToSend, TickType_t uxBlockTimeTicks )
	{
	NetworkBufferDescriptor_t *pxNetworkBuffer;
	EthernetHeader_t *pxEthernetHeader;
	ICMPPacket_IPv6_t *pxICMPPacket;
	ICMPEcho_IPv6_t *pxICMPHeader;
	BaseType_t xReturn = pdFAIL;
	static uint16_t usSequenceNumber = 0;
	uint8_t *pucChar;
	IPStackEvent_t xStackTxEvent = { eStackTxEvent, NULL };
	NetworkEndPoint_t *pxEndPoint;
	size_t uxPacketLength;
	
		pxEndPoint = FreeRTOS_FindEndPointOnIP_IPv6( pxIPAddress );
		if( pxEndPoint == NULL )
		{
			pxEndPoint = FreeRTOS_FindGateWay( ( BaseType_t ) ipTYPE_IPv6 );
			if( pxEndPoint == NULL )
			{
				FreeRTOS_printf( ( "SendPingRequestIPv6: No routing/gw found" ) );
				return 0;	/*lint !e904 Return statement before end of function [MISRA 2012 Rule 15.5, advisory]. */
			}
			FreeRTOS_printf( ( "SendPingRequestIPv6: Using gw %pip", pxEndPoint->ipv6_settings.xGatewayAddress.ucBytes ) );	/* access to 'ipv6_settings' is checked. */
		}
		uxPacketLength = sizeof( EthernetHeader_t ) + sizeof( IPHeader_IPv6_t ) + sizeof( ICMPEcho_IPv6_t ) + uxNumberOfBytesToSend;
		pxNetworkBuffer = pxGetNetworkBufferWithDescriptor( uxPacketLength, uxBlockTimeTicks );

		if( pxNetworkBuffer != NULL )
		{
		BaseType_t xEnoughSpace;

			/* Probably not necessary to clear the buffer. */
			( void ) memset( pxNetworkBuffer->pucEthernetBuffer, 0, pxNetworkBuffer->xDataLength);

			pxNetworkBuffer->pxEndPoint = ipPOINTER_CAST( struct xNetworkEndPoint *, pxEndPoint );
			pxICMPPacket = ipPOINTER_CAST( ICMPPacket_IPv6_t *, pxNetworkBuffer->pucEthernetBuffer );
			if( uxNumberOfBytesToSend < ( ( ipconfigNETWORK_MTU - sizeof( IPHeader_IPv6_t ) ) - sizeof( ICMPEcho_IPv6_t ) ) )
			{
				xEnoughSpace = pdTRUE;
			}
			else
			{
				xEnoughSpace = pdFALSE;
			}
			if( ( uxGetNumberOfFreeNetworkBuffers() >= 3U ) && ( uxNumberOfBytesToSend >= 1U ) && ( xEnoughSpace != pdFALSE ) )
			{
				pxICMPHeader = ipPOINTER_CAST( ICMPEcho_IPv6_t *, &( pxICMPPacket->xICMPHeader_IPv6 ) );
				usSequenceNumber++;

				pxICMPPacket->xEthernetHeader.usFrameType = ipIPv6_FRAME_TYPE;

				configASSERT( pxEndPoint != NULL );
				configASSERT( pxEndPoint->bits.bIPv6 != pdFALSE_UNSIGNED );

				pxICMPPacket->xIPHeader.usPayloadLength = FreeRTOS_htons( sizeof( ICMPEcho_IPv6_t ) + uxNumberOfBytesToSend );
				( void ) memcpy( pxICMPPacket->xIPHeader.xDestinationAddress.ucBytes, pxIPAddress->ucBytes, ipSIZE_OF_IPv6_ADDRESS );
				( void ) memcpy( pxICMPPacket->xIPHeader.xSourceAddress.ucBytes, pxEndPoint->ipv6_settings.xIPAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );

				/* Fill in the basic header information. */
				pxICMPHeader->ucTypeOfMessage = ipICMP_PING_REQUEST_IPv6;
				pxICMPHeader->ucTypeOfService = 0;
				pxICMPHeader->usIdentifier = usSequenceNumber;
				pxICMPHeader->usSequenceNumber = usSequenceNumber;

				/* Find the start of the data. */
				pucChar = ( uint8_t * ) pxICMPHeader;
				pucChar = &( pucChar[ sizeof( ICMPEcho_IPv6_t ) ] );

				/* Just memset the data to a fixed value. */
				( void ) memset( pucChar, ( int ) ndECHO_DATA_FILL_BYTE, uxNumberOfBytesToSend );

				/* The message is complete, IP and checksum's are handled by
				vProcessGeneratedUDPPacket */
				pxNetworkBuffer->pucEthernetBuffer[ ipSOCKET_OPTIONS_OFFSET ] = FREERTOS_SO_UDPCKSUM_OUT;
				pxNetworkBuffer->ulIPAddress = 0UL;
				( void ) memcpy( pxNetworkBuffer->xIPv6_Address.ucBytes, pxIPAddress->ucBytes, ipSIZE_OF_IPv6_ADDRESS );
				/* Let vProcessGeneratedUDPPacket() know that this is an ICMP packet. */
				pxNetworkBuffer->usPort = ipPACKET_CONTAINS_ICMP_DATA;
				pxNetworkBuffer->xDataLength = uxPacketLength;

				pxEthernetHeader = ipPOINTER_CAST( EthernetHeader_t *, pxNetworkBuffer->pucEthernetBuffer );
				pxEthernetHeader->usFrameType = ipIPv6_FRAME_TYPE;

				/* Send to the stack. */
				xStackTxEvent.pvData = pxNetworkBuffer;

				if( xSendEventStructToIPTask( &xStackTxEvent, uxBlockTimeTicks) != pdPASS )
				{
					vReleaseNetworkBufferAndDescriptor( pxNetworkBuffer );
					iptraceSTACK_TX_EVENT_LOST( ipSTACK_TX_EVENT );
				}
				else
				{
					xReturn = ( BaseType_t ) usSequenceNumber;
				}
			}
		}
		else
		{
			/* The requested number of bytes will not fit in the available space
			in the network buffer. */
		}

		return xReturn;
	}

#endif /* ipconfigSUPPORT_OUTGOING_PINGS == 1 */
/*-----------------------------------------------------------*/

static const char *pcMessageType( BaseType_t xType )
{
	const char *pcReturn;
	switch( ( uint8_t ) xType )
	{
		case ipICMP_DEST_UNREACHABLE_IPv6:        pcReturn = "DEST_UNREACHABLE"; break;
		case ipICMP_PACKET_TOO_BIG_IPv6:          pcReturn = "PACKET_TOO_BIG"; break;
		case ipICMP_TIME_EXEEDED_IPv6:            pcReturn = "TIME_EXEEDED"; break;
		case ipICMP_PARAMETER_PROBLEM_IPv6:       pcReturn = "PARAMETER_PROBLEM"; break;
		case ipICMP_PING_REQUEST_IPv6:            pcReturn = "PING_REQUEST"; break;
		case ipICMP_PING_REPLY_IPv6:              pcReturn = "PING_REPLY"; break;
		case ipICMP_ROUTER_SOLICITATION_IPv6:     pcReturn = "ROUTER_SOL"; break;
		case ipICMP_ROUTER_ADVERTISEMENT_IPv6:    pcReturn = "ROUTER_ADV"; break;
		case ipICMP_NEIGHBOR_SOLICITATION_IPv6:   pcReturn = "NEIGHBOR_SOL"; break;
		case ipICMP_NEIGHBOR_ADVERTISEMENT_IPv6:  pcReturn = "NEIGHBOR_ADV"; break;
		default:                                  pcReturn = "UNKNOWN ICMP"; break;
	}
	return pcReturn;
}
/*-----------------------------------------------------------*/

eFrameProcessingResult_t prvProcessICMPMessage_IPv6( NetworkBufferDescriptor_t * const pxNetworkBuffer )
{
ICMPPacket_IPv6_t *pxICMPPacket = ipPOINTER_CAST( ICMPPacket_IPv6_t *, pxNetworkBuffer->pucEthernetBuffer );
ICMPHeader_IPv6_t *xICMPHeader_IPv6 = ipPOINTER_CAST( ICMPHeader_IPv6_t *, &( pxICMPPacket->xICMPHeader_IPv6 ) );
NetworkEndPoint_t *pxEndPoint = pxNetworkBuffer->pxEndPoint;
size_t uxNeededSize;

	if( xICMPHeader_IPv6->ucTypeOfMessage != ipICMP_PING_REQUEST_IPv6 )
	{
		FreeRTOS_printf( ( "ICMPv6 %d (%s) from %pip to %pip end-point = %d\n",
			xICMPHeader_IPv6->ucTypeOfMessage,
			pcMessageType( ( BaseType_t ) xICMPHeader_IPv6->ucTypeOfMessage ),
			pxICMPPacket->xIPHeader.xSourceAddress.ucBytes,
			pxICMPPacket->xIPHeader.xDestinationAddress.ucBytes,
			( ( pxEndPoint != NULL ) && ( pxEndPoint->bits.bIPv6 != pdFALSE_UNSIGNED ) ) ? 1 : 0 ) );
	}
	if( ( pxEndPoint != NULL ) && ( pxEndPoint->bits.bIPv6 != pdFALSE_UNSIGNED ) )
	{
		switch( xICMPHeader_IPv6->ucTypeOfMessage )
		{
			default:
			case ipICMP_DEST_UNREACHABLE_IPv6:
			case ipICMP_PACKET_TOO_BIG_IPv6:
			case ipICMP_TIME_EXEEDED_IPv6:
			case ipICMP_PARAMETER_PROBLEM_IPv6:
				/* These message types are not implemented. They are logged here above. */
				break;
			case ipICMP_PING_REQUEST_IPv6 :
				{
				size_t uxICMPSize;
				uint16_t usICMPSize;

					/* Lint would complain about casting '()' immediately. */
					usICMPSize = FreeRTOS_ntohs( pxICMPPacket->xIPHeader.usPayloadLength );
					uxICMPSize = ( size_t ) usICMPSize;
					uxNeededSize = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + uxICMPSize );
					if( uxNeededSize > pxNetworkBuffer->xDataLength )
					{
						FreeRTOS_printf( ("Too small\n" ) );
						break;
					}

					xICMPHeader_IPv6->ucTypeOfMessage = ipICMP_PING_REPLY_IPv6;
					prvReturnICMP_IPv6( pxNetworkBuffer, uxICMPSize );
				}
				break;
			#if( ipconfigSUPPORT_OUTGOING_PINGS != 0 )
			case ipICMP_PING_REPLY_IPv6:
				{
				ePingReplyStatus_t eStatus = eSuccess;
				ICMPEcho_IPv6_t * pxICMPEchoHeader = ipPOINTER_CAST( ICMPEcho_IPv6_t *, xICMPHeader_IPv6 );
				size_t uxDataLength, uxCount;
				const uint8_t * pucByte;

					/* Find the total length of the IP packet. */
					uxDataLength = ipNUMERIC_CAST( size_t, FreeRTOS_ntohs( pxICMPPacket->xIPHeader.usPayloadLength ) );
					uxDataLength = uxDataLength - sizeof( *pxICMPEchoHeader );

					/* Find the first byte of the data within the ICMP packet. */
					pucByte = ( const uint8_t * ) pxICMPEchoHeader;
					pucByte = &( pucByte[ sizeof( *pxICMPEchoHeader ) ] );

					/* Check each byte. */
					for( uxCount = 0; uxCount < uxDataLength; uxCount++ )
					{
						if( *pucByte != ( uint8_t ) ipECHO_DATA_FILL_BYTE )
						{
							eStatus = eInvalidData;
							break;
						}

						pucByte++;
					}
					/* Call back into the application to pass it the result. */
					vApplicationPingReplyHook( eStatus, pxICMPEchoHeader->usIdentifier );
				}
				break;
			#endif	/* ( ipconfigSUPPORT_OUTGOING_PINGS != 0 ) */
			case ipICMP_NEIGHBOR_SOLICITATION_IPv6 :
				{
				size_t uxICMPSize;

					uxICMPSize = sizeof( ICMPHeader_IPv6_t );
					uxNeededSize = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + uxICMPSize );
					if( uxNeededSize > pxNetworkBuffer->xDataLength )
					{
						FreeRTOS_printf( ("Too small\n" ) );
						break;
					}
					xICMPHeader_IPv6->ucTypeOfMessage = ipICMP_NEIGHBOR_ADVERTISEMENT_IPv6;
					xICMPHeader_IPv6->ucTypeOfService = 0;
					xICMPHeader_IPv6->ulReserved = ndICMPv6_FLAG_SOLICITED | ndICMPv6_FLAG_UPDATE;
					xICMPHeader_IPv6->ulReserved = FreeRTOS_htonl( xICMPHeader_IPv6->ulReserved );

					/* Type of option. */
					xICMPHeader_IPv6->ucOptionType = ndICMP_TARGET_LINK_LAYER_ADDRESS;
					/* Length of option in units of 8 bytes. */
					xICMPHeader_IPv6->ucOptionLength = 1;
					( void ) memcpy( xICMPHeader_IPv6->ucOptionBytes, pxEndPoint->xMACAddress.ucBytes, sizeof( MACAddress_t ) );
					pxICMPPacket->xIPHeader.ucHopLimit = 255;
					( void ) memcpy( xICMPHeader_IPv6->xIPv6_Address.ucBytes, pxEndPoint->ipv6_settings.xIPAddress.ucBytes, sizeof( xICMPHeader_IPv6->xIPv6_Address.ucBytes ) );

					prvReturnICMP_IPv6( pxNetworkBuffer, uxICMPSize );
				}
				break;
			case ipICMP_NEIGHBOR_ADVERTISEMENT_IPv6:
				{
					vNDRefreshCacheEntry( ipPOINTER_CAST( MACAddress_t *, xICMPHeader_IPv6->ucOptionBytes ),
										  &( xICMPHeader_IPv6->xIPv6_Address ),
										  pxEndPoint );
					FreeRTOS_printf( ( "NA from %pip\n",
									   xICMPHeader_IPv6->xIPv6_Address.ucBytes ) );

				#if( ipconfigUSE_RA != 0 )
					{
						vReceiveNA( pxNetworkBuffer );
					}
				#endif	/* ( ipconfigUSE_RA != 0 ) */
				}
				break;
			case ipICMP_ROUTER_SOLICITATION_IPv6:
				{
				}
				break;
			#if( ipconfigUSE_RA != 0 )
			case ipICMP_ROUTER_ADVERTISEMENT_IPv6:
				{
					vReceiveRA( pxNetworkBuffer );
				}
				break;
			#endif	/* ( ipconfigUSE_RA != 0 ) */
		}	/* switch( xICMPHeader_IPv6->ucTypeOfMessage ) */
	}	/* if( pxEndPoint != NULL ) */

	return eReleaseBuffer;
//	return eFrameConsumed;
//	return eReturnEthernetFrame;
}
/*-----------------------------------------------------------*/

void FreeRTOS_OutputAdvertiseIPv6( NetworkEndPoint_t *pxEndPoint )
{
NetworkBufferDescriptor_t *pxNetworkBuffer;
ICMPPacket_IPv6_t *pxICMPPacket;
NetworkInterface_t *pxInterface;
ICMPHeader_IPv6_t *pxICMPHeader_IPv6;
size_t uxICMPSize;
size_t xPacketSize;

	xPacketSize = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + sizeof( ICMPHeader_IPv6_t ) );

	/* This is called from the context of the IP event task, so a block time
	must not be used. */
	pxNetworkBuffer = pxGetNetworkBufferWithDescriptor( xPacketSize, ndDONT_BLOCK );
	if( pxNetworkBuffer != NULL )
	{
		pxNetworkBuffer->ulIPAddress = 0;
		pxNetworkBuffer->pxEndPoint = pxEndPoint;

		pxInterface = pxEndPoint->pxNetworkInterface;

		configASSERT( pxInterface != NULL );

		pxICMPPacket = ipPOINTER_CAST( ICMPPacket_IPv6_t *, pxNetworkBuffer->pucEthernetBuffer );
		pxICMPHeader_IPv6 = ipPOINTER_CAST( ICMPHeader_IPv6_t *, &( pxICMPPacket->xICMPHeader_IPv6 ) );

		( void ) memcpy( pxICMPPacket->xEthernetHeader.xDestinationAddress.ucBytes, pcLOCAL_ALL_NODES_MULTICAST_MAC, ipMAC_ADDRESS_LENGTH_BYTES );
		( void ) memcpy( pxICMPPacket->xEthernetHeader.xSourceAddress.ucBytes, pxEndPoint->xMACAddress.ucBytes, ipMAC_ADDRESS_LENGTH_BYTES );
		pxICMPPacket->xEthernetHeader.usFrameType = ipIPv6_FRAME_TYPE;              /* 12 + 2 = 14 */

		pxICMPPacket->xIPHeader.ucVersionTrafficClass = 0x60;
		pxICMPPacket->xIPHeader.ucTrafficClassFlow = 0;
		pxICMPPacket->xIPHeader.usFlowLabel = 0;

		pxICMPPacket->xIPHeader.usPayloadLength = FreeRTOS_htons( sizeof( ICMPHeader_IPv6_t ) );/*lint !e845: (Info -- The right argument to operator '|' is certain to be 0. */
		pxICMPPacket->xIPHeader.ucNextHeader = ipPROTOCOL_ICMP_IPv6;
		pxICMPPacket->xIPHeader.ucHopLimit = 255;
		( void ) memcpy( pxICMPPacket->xIPHeader.xSourceAddress.ucBytes, pxEndPoint->ipv6_settings.xIPAddress.ucBytes, ipSIZE_OF_IPv6_ADDRESS );
		( void ) memcpy( pxICMPPacket->xIPHeader.xDestinationAddress.ucBytes, pcLOCAL_ALL_NODES_MULTICAST_IP, ipSIZE_OF_IPv6_ADDRESS );

		uxICMPSize = sizeof( ICMPHeader_IPv6_t );
		pxICMPHeader_IPv6->ucTypeOfMessage = ipICMP_NEIGHBOR_ADVERTISEMENT_IPv6;
		pxICMPHeader_IPv6->ucTypeOfService = 0;
		pxICMPHeader_IPv6->ulReserved = ndICMPv6_FLAG_SOLICITED | ndICMPv6_FLAG_UPDATE;
		pxICMPHeader_IPv6->ulReserved = FreeRTOS_htonl( pxICMPHeader_IPv6->ulReserved );

		/* Type of option. */
		pxICMPHeader_IPv6->ucOptionType = ndICMP_TARGET_LINK_LAYER_ADDRESS;
		/* Length of option in units of 8 bytes. */
		pxICMPHeader_IPv6->ucOptionLength = 1;
		( void ) memcpy( pxICMPHeader_IPv6->ucOptionBytes, pxEndPoint->xMACAddress.ucBytes, sizeof( MACAddress_t ) );
		pxICMPPacket->xIPHeader.ucHopLimit = 255;
		( void ) memcpy( pxICMPHeader_IPv6->xIPv6_Address.ucBytes, pxEndPoint->ipv6_settings.xIPAddress.ucBytes, sizeof( pxICMPHeader_IPv6->xIPv6_Address.ucBytes ) );

		/* Important: tell NIC driver how many bytes must be sent */
		pxNetworkBuffer->xDataLength = ( size_t ) ( ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER + uxICMPSize );

		pxICMPHeader_IPv6->usChecksum = 0;
		/* calculate the UDP checksum for outgoing package */
		( void ) usGenerateProtocolChecksum( pxNetworkBuffer->pucEthernetBuffer, pxNetworkBuffer->xDataLength, pdTRUE );

		/* Set the parameter 'bReleaseAfterSend'. */
		( void ) pxInterface->pfOutput( pxInterface, pxNetworkBuffer, pdTRUE );
	}
}
/*-----------------------------------------------------------*/

BaseType_t FreeRTOS_CreateIPv6Address( IPv6_Address_t *pxIPAddress, const IPv6_Address_t *pxPrefix, size_t uxPrefixLength, BaseType_t xDoRandom )
{
uint32_t pulRandom[ 4 ];
uint8_t *pucSource;
BaseType_t xIndex, xResult = pdPASS;

	if( xDoRandom != pdFALSE )
	{
		/* Create an IP-address, based on a net prefix and a random host address. */
		for( xIndex = 0; xIndex < ARRAY_SIZE( pulRandom ); xIndex++ )
		{
			if( xApplicationGetRandomNumber( &( pulRandom[ xIndex ] ) ) == pdFAIL )
			{
				xResult = pdFAIL;
				break;
			}
		}
	}
	else
	{
		( void ) memset( pulRandom, 0, sizeof pulRandom );
	}
	if( xResult == pdPASS )
	{
		configASSERT( ( uxPrefixLength > 0U ) && ( uxPrefixLength < ( 8U * ipSIZE_OF_IPv6_ADDRESS ) ) );
		( void ) memcpy( pxIPAddress->ucBytes, pxPrefix->ucBytes, ( uxPrefixLength + 7U ) / 8U );
		pucSource = ipPOINTER_CAST( uint8_t *, pulRandom );
		size_t uxIndex = uxPrefixLength / 8U;
		if( ( uxPrefixLength % 8U ) != 0U )
		{
		/* uxHostLen is between 1 and 7 bits long. */
		size_t uxHostLen = 8U - ( uxPrefixLength % 8U );
		uint32_t uxHostMask = ( ( ( uint32_t ) 1U ) << uxHostLen ) - 1U;
		uint8_t ucNetMask = ( uint8_t ) ~( uxHostMask );

			pxIPAddress->ucBytes[ uxIndex ] &= ucNetMask;
			pxIPAddress->ucBytes[ uxIndex ] |= ( pucSource[ 0 ] & ( ( uint8_t ) uxHostMask ) );
			pucSource = &( pucSource[ 1 ] );
			uxIndex++;
		}
		if( uxIndex < ipSIZE_OF_IPv6_ADDRESS )
		{
			( void ) memcpy( &( pxIPAddress->ucBytes[ uxIndex ] ), pucSource, ipSIZE_OF_IPv6_ADDRESS - uxIndex );
		}
	}
	return xResult;
}
/*-----------------------------------------------------------*/

#endif /* ipconfigUSE_IPv6 */

