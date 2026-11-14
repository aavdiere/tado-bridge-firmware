#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* lwIP 2.2.1 (vendor/lwip, tag STABLE-2_2_1_RELEASE) options for the IB01
 * (#13). Only what differs from lwip/opt.h, or what is set on purpose.
 *
 * Threading: NO_SYS. The net task is the only lwIP context: it reads frames
 * from the ENC28J60, calls netif->input, runs sys_check_timeouts() and, from
 * P4, TLS and the WebSocket client (docs/design.md "Tasks": one medium-priority
 * net task for ENC28J60, lwIP, TLS, WebSocket). Nothing calls lwIP from an
 * interrupt or another task, so no lwIP locking is needed. */

#include <stdint.h>

#define NO_SYS               1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS          1 /* sys_check_timeouts() from the net task loop */
#define LWIP_SOCKET          0
#define LWIP_NETCONN         0

/* Random numbers for the DHCP xid, DNS id and source port, and the first TCP
 * local port: the CTR_DRBG seeded from the PA3 entropy pool (src/rng.h, #14).
 * The net task seeds it before lwip_init(); rng_u32() never returns a weak
 * value (it panics instead). */
uint32_t rng_u32(void);
#define LWIP_RAND() rng_u32()

/* Protocols. IPv4 only: the bridge talks IPv4 to tanoclo (docs/server-api.md);
 * the radio side's IPv6 is handled by our own 6LoWPAN code (docs/design.md
 * "Software stack"). */
#define LWIP_IPV4                1
#define LWIP_IPV6                0
#define LWIP_ARP                 1
#define LWIP_ETHERNET            1
#define LWIP_ICMP                1
#define LWIP_RAW                 0
#define LWIP_UDP                 1
#define LWIP_TCP                 1 /* P4: TLS to tanoclo on port 988 */
#define LWIP_DHCP                1
#define LWIP_DHCP_DOES_ACD_CHECK 1 /* probe the offered address before using it (RFC 5227) */
#define LWIP_DNS                 1
#define LWIP_AUTOIP              0
#define LWIP_IGMP                0 /* ERXFCON passes unicast and broadcast only (src/enc28j60.c) */

/* No fragmentation: TCP keeps to the MSS, DNS answers over UDP stay within
 * 512 bytes (RFC 1035 4.2.1), DHCP within 576. Saves the reassembly buffers. */
#define IP_REASSEMBLY 0
#define IP_FRAG       0

#define LWIP_SINGLE_NETIF   1
#define LWIP_NETIF_HOSTNAME 1 /* DHCP option 12, so the lease shows a name */

/* Memory. Static pools and lwIP's own heap in .bss; nothing comes from
 * newlib malloc or the FreeRTOS heap. MEM_SIZE holds transmit pbufs (TCP
 * segments, DHCP and DNS messages, ARP-queued copies); the pool holds
 * received frames, one full frame per buffer. Frames are processed one at a
 * time in the net task and TCP_QUEUE_OOSEQ is off, so few pool buffers are
 * in use at once; the ENC28J60's own 6.5 KB receive ring absorbs bursts. */
#define MEM_ALIGNMENT    4
#define MEM_SIZE         (6 * 1024)
#define PBUF_POOL_SIZE   6
#define ETH_PAD_SIZE     2 /* align the IP header to 4 bytes after the 14-byte Ethernet header */
#define MEMP_NUM_PBUF    4
#define MEMP_NUM_UDP_PCB 3 /* DHCP, DNS, one spare */
#define MEMP_NUM_TCP_PCB 2 /* the server connection, one spare */
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG        8
#define MEMP_NUM_ARP_QUEUE      4
#define ARP_TABLE_SIZE          4

/* TCP: provisional, to be sized with TLS in P4 (#17). */
#define TCP_MSS          1460
#define TCP_WND          (2 * TCP_MSS)
#define TCP_SND_BUF      (2 * TCP_MSS)
#define TCP_SND_QUEUELEN 8
#define TCP_QUEUE_OOSEQ  0

/* DNS: servers from DHCP option 6. */
#define DNS_TABLE_SIZE      2
#define DNS_MAX_NAME_LENGTH 64
#define DNS_MAX_SERVERS     2

/* Checksums in software. The ENC28J60's DMA checksum is not used (errata
 * #17, docs/datasheets/enc28j60-errata.txt:356-381). */
#define LWIP_CHKSUM_ALGORITHM 3

#define LWIP_STATS 0

#endif
