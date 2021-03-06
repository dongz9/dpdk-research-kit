#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <unistd.h>
#include <getopt.h>
#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_string_fns.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>

#define MAX_LCORE (RTE_MAX_LCORE)

#define MAX_SOCKET (4)

#define MAX_PORT (RTE_MAX_ETHPORTS)

typedef uint8_t  lcoreid_t;
typedef uint8_t  portid_t;
typedef uint16_t queueid_t;

#define DEFAULT_PKT_BURST (32)

struct port_data {
    struct ether_addr ethaddr;
    struct rte_eth_stats stats;
} __rte_cache_aligned;
typedef struct port_data port_data_t;

/* Configuration of Ethernet ports */
portid_t num_ports;
port_data_t *port_data;
uint64_t enabled_port_mask;

struct lcore_rx_queue {
    uint8_t port_id;
    uint8_t queue_id;
} __rte_cache_aligned;
typedef struct lcore_rx_queue lcore_rx_queue_t;

struct mbuf_table {
    struct rte_mbuf *pkts[DEFAULT_PKT_BURST];
    uint16_t num_pkts;
} __rte_cache_aligned;
typedef struct mbuf_table mbuf_table_t;

struct lcore_data {
    struct rte_mempool *mbp;
    uint16_t num_rx_queues;
    lcore_rx_queue_t rx_queue_list[MAX_PORT];
    uint16_t tx_queue_id[MAX_PORT];
    mbuf_table_t tx_mbufs[MAX_PORT];
} __rte_cache_aligned;
typedef struct lcore_data lcore_data_t;

lcoreid_t num_lcores;
lcore_data_t **lcore_data;

uint16_t tx_burst_size;

static inline lcore_data_t *
current_lcore_data(void)
{
    return lcore_data[rte_lcore_id()];
}

static inline void
mbuf_poolname_build(unsigned int sock_id, char* mp_name, int name_size)
{
    snprintf(mp_name, name_size, "mbuf_pool_socket_%u", sock_id);
}

static inline struct rte_mempool *
mbuf_pool_find(unsigned int socket_id)
{
    char pool_name[RTE_MEMPOOL_NAMESIZE];

    mbuf_poolname_build(socket_id, pool_name, sizeof(pool_name));
    return (rte_mempool_lookup((const char *)pool_name));
}

static inline struct rte_mbuf *
alloc_pkt(struct rte_mempool *mp)
{
    struct rte_mbuf *m;

    m = __rte_mbuf_raw_alloc(mp);
    return (m);
}

static inline void
free_pkt(struct rte_mbuf *pkt)
{
    rte_pktmbuf_free(pkt);
}

void
send_pkt(portid_t port_id, struct rte_mbuf *pkt)
{
    lcore_data_t *ld = current_lcore_data();

    queueid_t queue_id = ld->tx_queue_id[port_id];
    mbuf_table_t *mbuf_table = &(ld->tx_mbufs[port_id]);
    uint16_t num_pkts = mbuf_table->num_pkts;

    mbuf_table->pkts[num_pkts++] = pkt;
    if (num_pkts >= tx_burst_size) {
        uint16_t n = rte_eth_tx_burst(
            port_id,
            queue_id,
            mbuf_table->pkts,
            num_pkts);
        if (unlikely(n < num_pkts)) {
            do {
                rte_pktmbuf_free(mbuf_table->pkts[n++]);
            } while (n < num_pkts);
        }
        num_pkts = 0;
    }
    mbuf_table->num_pkts = num_pkts;
}

static inline uint16_t
recv_pkts(portid_t port_id, queueid_t queue_id, struct rte_mbuf **pkts, uint16_t num_pkts)
{
    uint16_t n = rte_eth_rx_burst(
        port_id,
        queue_id,
        pkts,
        num_pkts);
    return n;
}

#define MBUF_SIZE (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MEMPOOL_CACHE_SIZE (256)

#define DEFAULT_RX_DESC (128)
#define DEFAULT_TX_DESC (512)

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 4 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 36 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = ETH_MQ_RX_RSS,
        .max_rx_pkt_len = ETHER_MAX_LEN,
        .split_hdr_size = 0,
        .header_split   = 0, /**< Header Split disabled */
        .hw_ip_checksum = 1, /**< IP checksum offload enabled */
        .hw_vlan_filter = 0, /**< VLAN filtering disabled */
        .jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
        .hw_strip_crc   = 0, /**< CRC stripped by hardware */
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_IP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

static const struct rte_eth_rxconf rx_conf = {
    .rx_thresh = {
        .pthresh = RX_PTHRESH,
        .hthresh = RX_HTHRESH,
        .wthresh = RX_WTHRESH,
    },
    .rx_free_thresh = 32,
};

static struct rte_eth_txconf tx_conf = {
    .tx_thresh = {
        .pthresh = TX_PTHRESH,
        .hthresh = TX_HTHRESH,
        .wthresh = TX_WTHRESH,
    },
    .tx_free_thresh = 0, /* Use PMD default values */
    .tx_rs_thresh = 0, /* Use PMD default values */
    .txq_flags = (ETH_TXQ_FLAGS_NOMULTSEGS |
                  ETH_TXQ_FLAGS_NOVLANOFFL |
                  ETH_TXQ_FLAGS_NOXSUMSCTP |
                  ETH_TXQ_FLAGS_NOXSUMUDP |
                  ETH_TXQ_FLAGS_NOXSUMTCP)

};

portid_t num_ports;
port_data_t *port_data;
portid_t num_enabled_ports;
uint64_t enabled_port_mask;

lcoreid_t num_lcores;
lcore_data_t **lcore_data;

uint16_t tx_burst_size = 1;

static int promiscuous_on = 0;

static void
mbuf_pool_create(unsigned num_mbuf, unsigned socket_id)
{
    char pool_name[RTE_MEMPOOL_NAMESIZE];
    struct rte_mempool *rte_mp;

    mbuf_poolname_build(socket_id, pool_name, sizeof(pool_name));

    rte_mp = rte_mempool_create(
        pool_name, num_mbuf, MBUF_SIZE, MEMPOOL_CACHE_SIZE,
        sizeof(struct rte_pktmbuf_pool_private),
        rte_pktmbuf_pool_init, NULL,
        rte_pktmbuf_init, NULL,
        socket_id, 0);

    if (rte_mp == NULL)
        rte_exit(EXIT_FAILURE, "Creation of mbuf pool for socket %u failed\n", socket_id);
}

static void
init_config()
{
    lcoreid_t lcore_id;
    unsigned socket_id;
    unsigned num_mbuf_per_pool;

    lcore_data = rte_zmalloc(
        "dpdk_lib: lcore_data",
        sizeof(struct lcore_data *) * MAX_LCORE,
        CACHE_LINE_SIZE);
    if (lcore_data == NULL) {
        rte_exit(EXIT_FAILURE, "rte_zmalloc(%d (struct lcore_data *)) failed\n", num_lcores);
    }
    num_mbuf_per_pool = 8192;
    for (lcore_id = 0; lcore_id < MAX_LCORE; lcore_id++) {
        if (!rte_lcore_is_enabled(lcore_id))
            continue;
        lcore_data[lcore_id] = rte_zmalloc(
            "dpdk_lib: struct lcore_data",
            sizeof(struct lcore_data),
            CACHE_LINE_SIZE);
        if (lcore_data[lcore_id] == NULL) {
            rte_exit(EXIT_FAILURE, "rte_zmalloc(struct lcore_data) failed\n");
        }
        socket_id = rte_lcore_to_socket_id(lcore_id);
        if (!mbuf_pool_find(socket_id))
            mbuf_pool_create(num_mbuf_per_pool, socket_id);
        lcore_data[lcore_id]->mbp = mbuf_pool_find(socket_id);
    }
}

static void
print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
    printf("%s%02X:%02X:%02X:%02X:%02X:%02X", name,
           eth_addr->addr_bytes[0],
           eth_addr->addr_bytes[1],
           eth_addr->addr_bytes[2],
           eth_addr->addr_bytes[3],
           eth_addr->addr_bytes[4],
           eth_addr->addr_bytes[5]);
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(portid_t num_ports)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
    uint8_t portid, count, all_ports_up, print_flag = 0;
    struct rte_eth_link link;

    printf("Checking link status");
    fflush(stdout);
    for (count = 0; count <= MAX_CHECK_TIME; count++) {
        all_ports_up = 1;
        for (portid = 0; portid < num_ports; portid++) {
            memset(&link, 0, sizeof(link));
            rte_eth_link_get_nowait(portid, &link);
            /* print link status if flag set */
            if (print_flag == 1) {
                if (link.link_status)
                    printf("Port %d Link Up - speed %u "
                           "Mbps - %s\n", (uint8_t)portid,
                           (unsigned)link.link_speed,
                           (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                           ("full-duplex") : ("half-duplex\n"));
                else
                    printf("Port %d Link Down\n",
                           (uint8_t)portid);
                continue;
            }
            /* clear all_ports_up flag if any link down */
            if (link.link_status == 0) {
                all_ports_up = 0;
                break;
            }
        }
        /* after finally printing all link status, get out */
        if (print_flag == 1)
            break;

        if (all_ports_up == 0) {
            printf(".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL);
        }

        /* set the print_flag if all ports up or timeout */
        if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
            print_flag = 1;
            printf("done\n");
        }
    }
}

static void
start_port()
{
    int ret;
    portid_t port_id;
    queueid_t queue_id;
    lcoreid_t lcore_id;
    unsigned socket_id;

    for (port_id = 0; port_id < num_ports; ++port_id) {
        if (!((enabled_port_mask >> port_id) & 1))
            continue;
                
        ret = rte_eth_dev_configure(
            port_id,
            num_lcores,
            num_lcores,
            &port_conf);
        if (ret < 0)
            rte_exit(
                EXIT_FAILURE, "Cannot configure device: err=%d, port=%d\n",
                ret, port_id);

        queue_id = 0;
        for (lcore_id = 0; lcore_id < MAX_LCORE; ++lcore_id) {
            if (rte_lcore_is_enabled(lcore_id) == 0)
                continue;
            socket_id = rte_lcore_to_socket_id(lcore_id);

            ret = rte_eth_rx_queue_setup(
                port_id,
                queue_id,
                DEFAULT_RX_DESC,
                socket_id,
                &rx_conf,
                lcore_data[lcore_id]->mbp);
            if (ret < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: err=%d, port=%d\n", ret, port_id);
            lcore_data[lcore_id]->rx_queue_list[lcore_data[lcore_id]->num_rx_queues].port_id = port_id;
            lcore_data[lcore_id]->rx_queue_list[lcore_data[lcore_id]->num_rx_queues].queue_id = queue_id;
            ++lcore_data[lcore_id]->num_rx_queues;
                        
            ret = rte_eth_tx_queue_setup(
                port_id,
                queue_id,
                DEFAULT_TX_DESC,
                socket_id,
                &tx_conf);
            if (ret < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d, port=%d\n", ret, port_id);
            lcore_data[lcore_id]->tx_queue_id[port_id] = queue_id;

            queue_id++;
        }

        /* Start device */
        ret = rte_eth_dev_start(port_id);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start: err=%d, port=%d\n", ret, port_id);

        if (promiscuous_on)
            rte_eth_promiscuous_enable(port_id);

        rte_eth_stats_get(port_id, &port_data[port_id].stats);
    }

    check_all_ports_link_status(num_ports);
}

static uint64_t
parse_portmask(const char *s)
{
    char *end = NULL;
    unsigned long long pm;

    /* parse hexadecimal string */
    pm = strtoull(s, &end, 16);
    if ((s[0] == '\0') || (end == NULL) || (*end != '\0'))
        return -1;

    if (pm == 0)
        return -1;

    return (uint64_t)pm;
}

static int
parse_tx_burst_size(const char *s)
{
    char *end = NULL;
    unsigned long tx_burst_size;
    /* parse hexadecimal string */
    tx_burst_size = strtoul(s, &end, 16);
    if ((s[0] == '\0') || (end == NULL) || (*end != '\0'))
        return -1;

    if (tx_burst_size == 0)
        return -1;

    return tx_burst_size;
}

#define CMD_LINE_OPT_TX_BURST_SIZE "tx-burst-size"

static void
parse_args(int argc, char **argv)
{
    int opt, ret;
    char **argvopt;
    int option_index;
    static struct option lgopts[] = {
        {CMD_LINE_OPT_TX_BURST_SIZE, 1, 0, 0},
        {NULL, 0, 0, 0}
    };

    argvopt = argv;
        
    while ((opt = getopt_long(argc, argvopt, "p:", lgopts, &option_index)) != EOF) {
        switch (opt) {
        case 'p':
            enabled_port_mask = parse_portmask(optarg);
            break;

        case 0:
            if (!strncmp(lgopts[option_index].name, CMD_LINE_OPT_TX_BURST_SIZE,
                         sizeof(CMD_LINE_OPT_TX_BURST_SIZE))) {
                ret = parse_tx_burst_size(optarg);
                if ((ret > 0) && (ret < DEFAULT_PKT_BURST)){
                    tx_burst_size = ret;
                } else {
                    printf("invalid TX burst size\n");
                }
            }
            break;
        }
    }
}

#define UDP_SRC_PORT (1024)
#define UDP_DST_PORT (1024)

#define IP_SRC_ADDR ((192U << 24) | (168 << 16) | (0 << 8) | 1)
#define IP_DST_ADDR ((192U << 24) | (168 << 16) | (0 << 8) | 2)

#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

#define ENABLE_FLOW_CONTROL

unsigned flow_limit = 5000;

static void
make_udp_pkt(portid_t port_id, struct rte_mbuf *pkt)
{
    uint16_t pkt_len = 60;
        
    struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    struct ipv4_hdr *ip_hdr = (struct ipv4_hdr *)((char *)eth_hdr + sizeof(struct ether_hdr));
    struct udp_hdr *udp_hdr = (struct udp_hdr *)((char *)ip_hdr + sizeof(struct ipv4_hdr));
    char *payload = (char *)((char *)udp_hdr + sizeof(struct udp_hdr));

    memset(payload, 0, pkt_len - sizeof(struct ether_hdr) - sizeof(struct ipv4_hdr) - sizeof(struct udp_hdr));

    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    memcpy(&eth_hdr->s_addr, &port_data[port_id].ethaddr, sizeof(struct ether_addr));
    eth_hdr->d_addr.addr_bytes[0] = 0x00;
    eth_hdr->d_addr.addr_bytes[1] = 0x0C;
    eth_hdr->d_addr.addr_bytes[2] = 0x29;
    eth_hdr->d_addr.addr_bytes[3] = 0x40;
    eth_hdr->d_addr.addr_bytes[4] = 0x20;
    eth_hdr->d_addr.addr_bytes[5] = 0x59;

    ip_hdr->src_addr = IP_SRC_ADDR;
    ip_hdr->dst_addr = IP_DST_ADDR;
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(pkt_len - sizeof(struct ether_hdr));

    udp_hdr->src_port = UDP_SRC_PORT;
    udp_hdr->dst_port = UDP_DST_PORT;
    udp_hdr->dgram_len = rte_cpu_to_be_16(pkt_len - sizeof(struct ether_hdr) - sizeof(struct ipv4_hdr));
    udp_hdr->dgram_cksum = 0;
        
    uint16_t *ptr16 = (uint16_t *)ip_hdr;
    uint32_t ip_cksum;
    ip_cksum = 0;
    ip_cksum += ptr16[0]; ip_cksum += ptr16[1];
    ip_cksum += ptr16[2]; ip_cksum += ptr16[3];
    ip_cksum += ptr16[4];
    ip_cksum += ptr16[6]; ip_cksum += ptr16[7];
    ip_cksum += ptr16[8]; ip_cksum += ptr16[9];

    /*
     * reduce 32 bit checksum to 16 bits and complement it
     */
    ip_cksum = ((ip_cksum & 0xFFFF0000) >> 16) + (ip_cksum & 0x0000FFFF);
    if (ip_cksum > 65535)
        ip_cksum -= 65535;
    ip_cksum = (~ip_cksum) & 0x00005555;
    if (ip_cksum == 0)
        ip_cksum = 0xFFFF;
    ip_hdr->hdr_checksum = (uint16_t)ip_cksum;

    pkt->pkt.data_len = pkt_len;
    pkt->pkt.next = NULL; /* last segment of packet */
    pkt->pkt.nb_segs = 1;
    pkt->pkt.pkt_len = pkt_len;
    pkt->ol_flags = 0;
}

lcoreid_t master_lcore_id;

static int
pktgen_loop(void *arg)
{
    lcoreid_t lcore_id = rte_lcore_id();
    unsigned socket_id = rte_lcore_to_socket_id(lcore_id);
    portid_t port_id;
    struct rte_mbuf *pkt;
    uint64_t cur_tsc, start_tsc, next_tsc;
    uint64_t tsc_hz = rte_get_tsc_hz();
#ifdef ENABLE_FLOW_CONTROL
    unsigned limit = num_enabled_ports * flow_limit;
#endif

    start_tsc = rte_rdtsc();
    next_tsc = start_tsc + tsc_hz;

    while (1) {
        cur_tsc = rte_rdtsc();

        if (cur_tsc >= next_tsc) {
            if (lcore_id == master_lcore_id) {
                uint64_t thruput = 0;

                for (port_id = 0; port_id < num_ports; ++port_id) {
                    if (!((enabled_port_mask >> port_id) & 1))
                        continue;

                    struct rte_eth_stats stats;
                    rte_eth_stats_get(port_id, &stats);
                    thruput += stats.opackets - port_data[port_id].stats.opackets;
                    port_data[port_id].stats = stats;
                }

                printf("%04llu ", (cur_tsc - start_tsc) / tsc_hz);
                if (thruput >= 100000)
                    printf(" thruput=%.2lfM", ((double)thruput) / 1000000.0);
                else
                    printf(" thruput=%llu", thruput);
                printf("\n");
                fflush(stdout);
            }

            next_tsc += tsc_hz;

#ifdef ENABLE_FLOW_CONTROL
            limit = num_enabled_ports * flow_limit;
#endif
        }

        for (port_id = 0; port_id < num_ports; ++port_id) {
            if (!((enabled_port_mask >> port_id) & 1))
                continue;
                        
#ifdef ENABLE_FLOW_CONTROL
            if (limit > 0)
                --limit;
            else
                break;
#endif
            pkt = alloc_pkt(lcore_data[lcore_id]->mbp);
            make_udp_pkt(port_id, pkt);
            send_pkt(port_id, pkt);
        }               
    }
}

int main(int argc, char **argv)
{
    portid_t port_id;
    int ret, leading = 1;

    rte_set_log_level(RTE_LOG_NOTICE);

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_panic("Cannot init EAL\n");

    num_ports = rte_eth_dev_count();
    port_data = rte_zmalloc(
        "port_data",
        sizeof(struct port_data) * num_ports,
        CACHE_LINE_SIZE);
    printf("Detected %u ports\n", num_ports);
    for (port_id = 0; port_id < num_ports; ++port_id) {
        rte_eth_macaddr_get(port_id, &port_data[port_id].ethaddr);
        printf("Port %d", port_id);
        print_ethaddr(" address ", &port_data[port_id].ethaddr);
        printf("\n");
    }
    enabled_port_mask = ~0;
        
    num_lcores = rte_lcore_count();
    printf("Detected %u lcores\n", num_lcores);

    argc -= ret;
    argv += ret;
    parse_args(argc, argv);

    init_config();
    start_port();

    master_lcore_id = rte_get_master_lcore();
    printf("master_lcore_id=%d\n", master_lcore_id);

    printf("tx_burst_size=%d\n", tx_burst_size);
    printf("enabled_ports=");
    num_enabled_ports = 0;
    for (port_id = 0; port_id < num_ports; ++port_id)
        if ((enabled_port_mask >> port_id) & 1) {
            if (!leading)
                putchar(',');
            leading = 0;
            printf("%u", port_id);
            ++num_enabled_ports;
        }
    printf("\n");

    rte_eal_mp_remote_launch(pktgen_loop, NULL, CALL_MASTER);
    rte_eal_mp_wait_lcore();

    return 0;
}

