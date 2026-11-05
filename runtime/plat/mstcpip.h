/* Linux build: the Winsock extension header net_relay.c / net_minilobby.c include. SIO_UDP_CONNRESET is a Windows
 * ioctl that stops ICMP port-unreachable from failing UDP receives; Linux sockets do not need it. */
#ifndef PLAT_MSTCPIP_H
#define PLAT_MSTCPIP_H
#define SIO_UDP_CONNRESET 0
#define WSAIoctl(...) 0
#endif
