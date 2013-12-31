#include "comm_core.h"

#include <rte_log.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include "control.h"
#include "comm_log.h"
#include "main.h"
#include "arp.h"
#include "node.h"
#include "../kernel-mod/linux-compat.h"
#include "../kernel-mod/fpproto.h"
#include "../graph-algo/admissible_structures.h"
#include "dpdk-platform.h"

/* number of elements to keep in the pktdesc local core cache */
#define PKTDESC_MEMPOOL_CACHE_SIZE		256
/* should have as many pktdesc objs as number of in-flight packets */
#define PKTDESC_MEMPOOL_SIZE			(FASTPASS_WND_LEN * MAX_NODES + (N_COMM_CORES - 1) * PKTDESC_MEMPOOL_CACHE_SIZE)

/**
 * Information about an end node
 * @conn: connection state (ACKs, RESET, retransmission, etc)
 * @dst_port: the port where outgoing packets should go to
 * @dst_ether: the destination ethernet address for outgoing packets
 * @dst_ip: the destination IP for outgoing packets
 * @controller_ip: the controller IP outgoing packets should use
 */
struct end_node_state {
	struct fpproto_conn conn;
	uint8_t dst_port;
	struct ether_addr dst_ether;
	uint32_t dst_ip;
	uint32_t controller_ip;
};

/* whether we should output verbose debugging */
bool fastpass_debug;

/* bitmap of which nodes have a packet pending */
static struct fp_window triggered_nodes;

/* logs */
struct comm_log comm_core_logs[N_COMM_CORES];

/* per-end-node information */
static struct end_node_state end_nodes[MAX_NODES];

/* fpproto_pktdesc pool */
struct rte_mempool* pktdesc_pool[NB_SOCKETS];

static void handle_reset(void *param);
static void trigger_request(void *param, u64 when);

struct fpproto_ops proto_ops = {
	.handle_reset	= &handle_reset,
	//.handle_ack		= &handle_ack,
	//.handle_neg_ack	= &handle_neg_ack,
	.trigger_request= &trigger_request,
};

void comm_init_global_structs(void)
{
	u32 i;

	fastpass_debug = true;

	for (i = 0; i < MAX_NODES; i++)
		fpproto_init_conn(&end_nodes[i].conn, &proto_ops,&end_nodes[i],
				FASTPASS_RESET_WINDOW_NS, CONTROLLER_SEND_TIMEOUT_NS);

	for (i = 0; i < N_COMM_CORES; i++)
		comm_log_init(&comm_core_logs[i]);

	wnd_reset(&triggered_nodes, 255);
}

/* based on init_mem in main.c */
void comm_init_core(uint16_t lcore_id)
{
	int socketid;
	char s[64];

	socketid = rte_lcore_to_socket_id(lcore_id);

	if (pktdesc_pool[socketid] == NULL) {
		rte_snprintf(s, sizeof(s), "pktdesc_pool_%d", socketid);
		pktdesc_pool[socketid] =
			rte_mempool_create(s,
				PKTDESC_MEMPOOL_SIZE, /* num elements */
				sizeof(struct fpproto_pktdesc), /* element size */
				PKTDESC_MEMPOOL_CACHE_SIZE, /* cache size */
				0, NULL, NULL, NULL, NULL, /* custom initialization, disabled */
				socketid, 0);
		if (pktdesc_pool[socketid] == NULL)
			rte_exit(EXIT_FAILURE,
					"Cannot init pktdesc pool on socket %d: %s\n", socketid,
					rte_strerror(rte_errno));
		else
			printf("Allocated pktdesc pool on socket %d - %llu bufs\n",
					socketid, (u64)PKTDESC_MEMPOOL_SIZE);
	}
}

static void handle_reset(void *param)
{
	(void)param;
	COMM_DEBUG("got reset\n");
}

static void trigger_request(void *param, u64 when)
{
	struct end_node_state *en = (struct end_node_state *)param;
	u32 index = en - end_nodes;
	(void)param; (void)when;
	if (!wnd_is_marked(&triggered_nodes, index))
		wnd_mark(&triggered_nodes, index);
	COMM_DEBUG("trigger_request index=%u\n", index);
}

static inline struct rte_mbuf *
make_packet(struct end_node_state *en, struct fpproto_pktdesc *pd)
{
	const unsigned int socket_id = rte_socket_id();
	struct rte_mbuf *m;
	struct ether_hdr *eth_hdr;
	struct ipv4_hdr *ipv4_hdr;
	unsigned char *payload_ptr;
	uint32_t ipv4_length;
	uint32_t data_len;

	// Allocate packet on the current socket
	m = rte_pktmbuf_alloc(tx_pktmbuf_pool[socket_id]);
	if(m == NULL) {
		comm_log_tx_cannot_allocate_mbuf(en->dst_ip);
		return NULL;
	}

	eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);

	ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(m, unsigned char *)
			     + sizeof(struct ether_hdr));

	payload_ptr = (rte_pktmbuf_mtod(m, unsigned char *)
			     + sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr));

	/* dst addr according to destination */
	ether_addr_copy(&en->dst_ether, &eth_hdr->d_addr);
	/* src addr according to output port*/
	ether_addr_copy(&port_info[en->dst_port].eth_addr, &eth_hdr->s_addr);
	/* ethernet payload is IPv4 */
	eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

	/* ipv4 header */
	ipv4_hdr->version_ihl = 0x45; // Version=4, IHL=5
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->packet_id = 0;
	ipv4_hdr->fragment_offset = 0;
	ipv4_hdr->time_to_live = 77;
	ipv4_hdr->next_proto_id = IPPROTO_FASTPASS;
	// ipv4_hdr->hdr_checksum will be calculated in HW
	ipv4_hdr->src_addr = en->controller_ip;
	ipv4_hdr->dst_addr = en->dst_ip;

	/* encode fastpass payload */
	data_len = fpproto_encode_packet(&en->conn, pd, payload_ptr,
			FASTPASS_MAX_PAYLOAD, en->controller_ip, en->dst_ip);

	/* adjust packet size */
	ipv4_length = sizeof(struct ipv4_hdr) + data_len;
	// ipv4_length = RTE_MAX(46u, ipv4_length);
	rte_pktmbuf_append(m, ETHER_HDR_LEN + ipv4_length);
	ipv4_hdr->total_length = rte_cpu_to_be_16(ipv4_length);

	// Activate IP checksum offload for packet
	m->ol_flags |= PKT_TX_IP_CKSUM;
	m->pkt.vlan_macip.f.l2_len = sizeof(struct ether_hdr);
	m->pkt.vlan_macip.f.l3_len = sizeof(struct ipv4_hdr);
	ipv4_hdr->hdr_checksum = 0;

	return m;
}

/**
 * \brief Performs an allocation for a single request packet, sends
 * 		a reply to the requester
 *
 * Takes ownership of mbuf memory - either sends it or frees it.
 * @param portid: the port out of which to send the packet
 */
static inline void
comm_rx(struct rte_mbuf *m, uint8_t portid)
{
	struct ether_hdr *eth_hdr;
	struct ipv4_hdr *ipv4_hdr;
	u8 *req_pkt;
	uint32_t req_src;
	struct end_node_state *en;
	uint16_t ether_type;

	eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(m, unsigned char *)
			     + sizeof(struct ether_hdr));
	req_pkt = (rte_pktmbuf_mtod(m, unsigned char *)
			     + sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr));

	ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);

	if (unlikely(ether_type == ETHER_TYPE_ARP)) {
		print_arp(m, portid);
		send_gratuitous_arp(portid, controller_ip(portid));
		goto cleanup; // Disregard ARP
	}

	if (unlikely(ether_type != ETHER_TYPE_IPv4)) {
		comm_log_rx_non_ipv4_packet(portid);
		goto cleanup;
	}

	if (unlikely(ipv4_hdr->next_proto_id != IPPROTO_FASTPASS)) {
		comm_log_rx_ip_non_fastpass_pkt(portid);
		goto cleanup;
	}

	req_src = node_from_node_ip(rte_be_to_cpu_32(ipv4_hdr->src_addr));
	en = &end_nodes[req_src];

	/* copy most recent ethernet and IP addresses, for return packets */
	ether_addr_copy(&eth_hdr->s_addr, &en->dst_ether);
	en->dst_ip = ipv4_hdr->src_addr;
	en->controller_ip = ipv4_hdr->dst_addr;


	RTE_LOG(INFO, BENCHAPP, "at %lu controller got packet src_ip=0x%"PRIx32
			"src_node=%u dst=0x%"PRIx32"\n", rte_get_timer_cycles(),
			ipv4_hdr->src_addr, req_src, ipv4_hdr->dst_addr);

	if (req_src < MAX_NODES) {
		fpproto_handle_rx_packet(&end_nodes[req_src].conn, req_pkt,
				rte_pktmbuf_data_len(m) - (sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr)),
				ipv4_hdr->src_addr, ipv4_hdr->dst_addr);
	}

cleanup:
	/* free the request packet */
	rte_pktmbuf_free(m);
}

void exec_comm_core(struct comm_core_cmd * cmd)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	int i, j, nb_rx;
	uint8_t portid, queueid;
	uint64_t rx_time;
	struct lcore_conf *qconf;

	qconf = &lcore_conf[rte_lcore_id()];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, BENCHAPP, "lcore %u has nothing to do\n", rte_lcore_id());
		while(1);
	}

	for (i = 0; i < qconf->n_rx_queue; i++) {
		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		RTE_LOG(INFO, BENCHAPP, "comm_core -- lcoreid=%u portid=%hhu rxqueueid=%hhu\n",
				rte_lcore_id(), portid, queueid);
		send_gratuitous_arp(portid, controller_ip(i));
	}

	while (rte_get_timer_cycles() < cmd->start_time);

	for (i = 0; i < qconf->n_rx_queue; i++) {
		portid = qconf->rx_queue_list[i].port_id;
		send_gratuitous_arp(portid, controller_ip(i));
	}

	while (rte_get_timer_cycles() < cmd->end_time) {
		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; ++i) {

			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;
			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst, MAX_PKT_BURST);
			rx_time = rte_get_timer_cycles();

			/* Prefetch first packets */
			for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++) {
				rte_prefetch0(rte_pktmbuf_mtod(
						pkts_burst[j], void *));
			}

			/* Prefetch and handle already prefetched packets */
			for (j = 0; j < (nb_rx - PREFETCH_OFFSET); j++) {
				rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[
						j + PREFETCH_OFFSET], void *));
				comm_rx(pkts_burst[j], portid);
			}

			/* handle remaining prefetched packets */
			for (; j < nb_rx; j++) {
				comm_rx(pkts_burst[j], portid);
			}

			comm_log_processed_batch(nb_rx, rx_time);
		}

		/* TODO: Process allocated timeslots and send allocation packets */
		for (i = 0; i < MAX_PKT_BURST; i++) {
			uint32_t node_ind;
			struct rte_mbuf *out_pkt;
			struct end_node_state *en;
			struct fpproto_pktdesc *pd;
			u64 now;

			if (wnd_empty(&triggered_nodes))
				break;

			node_ind = wnd_earliest_marked(&triggered_nodes);
			en = &end_nodes[node_ind];


			/* prepare to send */
			fpproto_prepare_to_send(&en->conn);

			/* allocate pktdesc */
			pd = fpproto_pktdesc_alloc();
			if (unlikely(pd == NULL)) {
				comm_log_pktdesc_alloc_failed(node_ind);
				break;
			}

			/* make the packet */
			out_pkt = make_packet(en, pd);
			if (unlikely(out_pkt == NULL)) {
				fpproto_pktdesc_free(pd);
				break;
			}

			/* we want this packet's reliability to be tracked */
			now = fp_get_time_ns();
			fpproto_commit_packet(&en->conn, pd, now);

			/* send on port */
			send_packet_via_queue(out_pkt, en->dst_port);

			/* log sent packet */
			comm_log_tx_pkt(node_ind, now);

			/* clear the trigger */
			wnd_clear(&triggered_nodes, node_ind);
		}

		/* Flush queued packets */
		for (i = 0; i < n_enabled_port; i++)
			send_queued_packets(enabled_port[i]);

	}
}
