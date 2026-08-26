#ifndef ECU_LWIPOPTS_H
#define ECU_LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TIMERS                     1

/* The Ethernet zero-copy receive buffers are declared 32-byte aligned.  Keep
   every lwIP pool element at that alignment as well; aligning only the member
   inside RxBuffer is not sufficient when the enclosing object comes from a
   private memp pool. */
#define MEM_ALIGNMENT                   32
#define MEM_SIZE                        (24U * 1024U)
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_SYS_TIMEOUT            8
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               1536
#define LWIP_SUPPORT_CUSTOM_PBUF        1

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        0

#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  0

#define IP_REASSEMBLY                   0
#define IP_FRAG                         0
#define IP_DEFAULT_TTL                  64
#define UDP_TTL                         64
#define IP_SOF_BROADCAST                1
#define IP_SOF_BROADCAST_RECV           1

#define ETHARP_SUPPORT_STATIC_ENTRIES   0
#define ETHARP_TABLE_SIZE               10
#define ETH_PAD_SIZE                    0

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

/* STM32H563 Ethernet MAC checksum offload is enabled by CubeMX. ICMP
   generation remains in software because the HAL setting does not insert it. */
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_ICMP             0

#define LWIP_CHKSUM_ALGORITHM           2

#endif /* ECU_LWIPOPTS_H */
