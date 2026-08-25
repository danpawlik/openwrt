/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nss_dp_api_if.h
 *	NSS data-plane glue API for the IPQ5018 dwmac ethernet stack.
 *
 * Drop-in replacement for the header of the same name exported by the
 * removed qca-nss-dp driver, in its IPQ5018 flavour. qca-nss-drv compiles
 * against this header unmodified; the implementation lives in
 * qca-dwmac-nss.ko, which binds the NSS data-plane override to the
 * stmmac netdevs created by dwmac-ipq5018 (and, above them, the DSA user
 * ports of the external QCA8337) instead of nss-dp's own netdevs.
 *
 * Constants and the gmac stats layout below are the IPQ5018 values from
 * qca-nss-dp hal/soc_ops/ipq50xx/nss_ipq50xx.h. They differ from the
 * IPQ807x ones used by qca-ppe-nss in three ways that matter:
 *
 *   - phys_if numbering starts at 0, not 1, and there are 2 ports, not 6;
 *   - struct nss_dp_hal_gmac_stats is the full syn_gmac layout, not an
 *     empty struct: nss-drv's nss_data_plane/hal/nss_ipq50xx.c writes
 *     every field of it in nss_data_plane_hal_stats_sync();
 *   - there is no TX preheader. nss_data_plane_hal_get_mtu_sz() returns
 *     the MTU unchanged on IPQ5018, so no headroom is reserved for it.
 */

#ifndef __NSS_DP_API_IF_H
#define __NSS_DP_API_IF_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>

/*
 * NSS DP status
 */
#define NSS_DP_SUCCESS	0
#define NSS_DP_FAILURE	-1

/*
 * IPQ5018 platform defines (from nss-dp nss_ipq50xx.h)
 */
#define NSS_DP_HAL_MAX_PORTS	2
#define NSS_DP_HAL_START_IFNUM	0
#define NSS_DP_START_IFNUM	NSS_DP_HAL_START_IFNUM
#define NSS_DP_MAX_INTERFACES	(NSS_DP_HAL_MAX_PORTS + NSS_DP_START_IFNUM)

/*
 * NSS PTP service code
 */
#define NSS_PTP_EVENT_SERVICE_CODE	0x9

/**
 * nss_dp_data_plane_ctx
 *	Data plane context base class.
 */
struct nss_dp_data_plane_ctx {
	struct net_device *dev;
};

/**
 * nss_dp_hal_gmac_stats_rx
 *	Per-GMAC Rx statistics.
 */
struct nss_dp_hal_gmac_stats_rx {
	uint64_t rx_bytes;
	uint64_t rx_packets;
	uint64_t rx_errors;
	uint64_t rx_missed;
	uint64_t rx_descriptor_errors;
	uint64_t rx_late_collision_errors;
	uint64_t rx_dribble_bit_errors;
	uint64_t rx_length_errors;
	uint64_t rx_ip_header_errors;
	uint64_t rx_ip_payload_errors;
	uint64_t rx_no_buffer_errors;
	uint64_t rx_transport_csum_bypassed;
	uint64_t rx_fifo_overflows;
	uint64_t rx_overflow_errors;
	uint64_t rx_crc_errors;
	uint64_t rx_skb_alloc_errors;
	uint64_t rx_scatter_packets;
	uint64_t rx_scatter_bytes;
	uint64_t rx_scatter_errors;
};

/**
 * nss_dp_hal_gmac_stats_tx
 *	Per-GMAC Tx statistics.
 */
struct nss_dp_hal_gmac_stats_tx {
	uint64_t tx_bytes;
	uint64_t tx_packets;
	uint64_t tx_collisions;
	uint64_t tx_errors;
	uint64_t tx_jabber_timeout_errors;
	uint64_t tx_frame_flushed_errors;
	uint64_t tx_loss_of_carrier_errors;
	uint64_t tx_no_carrier_errors;
	uint64_t tx_late_collision_errors;
	uint64_t tx_excessive_collision_errors;
	uint64_t tx_excessive_deferral_errors;
	uint64_t tx_underflow_errors;
	uint64_t tx_ip_header_errors;
	uint64_t tx_ip_payload_errors;
	uint64_t tx_dropped;
	uint64_t tx_ts_create_errors;
	uint64_t tx_desc_not_avail;
	uint64_t tx_nr_frags_pkts;
	uint64_t tx_fraglist_pkts;
	uint64_t tx_packets_requeued;
};

/**
 * nss_dp_hal_gmac_stats
 *	The per-GMAC statistics structure. nss-drv embeds this by value in
 *	struct nss_data_plane_param, so it must be a complete type with the
 *	exact IPQ5018 field set.
 */
struct nss_dp_hal_gmac_stats {
	struct nss_dp_hal_gmac_stats_rx rx_stats;
	struct nss_dp_hal_gmac_stats_tx tx_stats;
	uint64_t hw_errs[10];
};

struct nss_dp_gmac_stats {
	struct nss_dp_hal_gmac_stats stats;
};

/**
 * nss_dp_data_plane_ops
 *	Per data-plane ops structure, installed by qca-nss-drv via
 *	nss_dp_override_data_plane().
 */
struct nss_dp_data_plane_ops {
	int (*init)(struct nss_dp_data_plane_ctx *dpc);
	int (*open)(struct nss_dp_data_plane_ctx *dpc, uint32_t tx_desc_ring,
		    uint32_t rx_desc_ring, uint32_t mode);
	int (*close)(struct nss_dp_data_plane_ctx *dpc);
	int (*link_state)(struct nss_dp_data_plane_ctx *dpc,
			  uint32_t link_state);
	int (*mac_addr)(struct nss_dp_data_plane_ctx *dpc, uint8_t *addr);
	int (*change_mtu)(struct nss_dp_data_plane_ctx *dpc, uint32_t mtu);
	netdev_tx_t (*xmit)(struct nss_dp_data_plane_ctx *dpc,
			    struct sk_buff *os_buf);
	void (*set_features)(struct nss_dp_data_plane_ctx *dpc);
	int (*pause_on_off)(struct nss_dp_data_plane_ctx *dpc,
			    uint32_t pause_on);
	int (*vsi_assign)(struct nss_dp_data_plane_ctx *dpc, uint32_t vsi);
	int (*vsi_unassign)(struct nss_dp_data_plane_ctx *dpc, uint32_t vsi);
	int (*rx_flow_steer)(struct nss_dp_data_plane_ctx *dpc,
			     struct sk_buff *skb, uint32_t cpu, bool is_add);
	void (*get_stats)(struct nss_dp_data_plane_ctx *dpc,
			  struct nss_dp_gmac_stats *stats);
	int (*deinit)(struct nss_dp_data_plane_ctx *dpc);
};

/*
 * API consumed by qca-nss-drv (see nss_data_plane/nss_data_plane.c)
 */
struct net_device *nss_dp_get_netdev_by_nss_if_num(int if_num);
bool nss_dp_is_in_open_state(struct net_device *netdev);
int nss_dp_override_data_plane(struct net_device *netdev,
			       struct nss_dp_data_plane_ops *dp_ops,
			       struct nss_dp_data_plane_ctx *dpc);
void nss_dp_start_data_plane(struct net_device *netdev,
			     struct nss_dp_data_plane_ctx *dpc);
void nss_dp_restore_data_plane(struct net_device *netdev);
void nss_dp_receive(struct net_device *netdev, struct sk_buff *skb,
		    struct napi_struct *napi);
void nss_phy_tstamp_rx_buf(void *app_data, struct sk_buff *skb);

/*
 * Deferred probe: qca-nss-drv calls this at the top of its platform probe.
 * Returns 0 when the NSS core may boot, or -EPROBE_DEFER while it may not.
 * See qca_dwmac_nss.c for what "may" means at the current bring-up stage.
 */
int nss_dp_probe_gate(struct device *dev);

#endif /* __NSS_DP_API_IF_H */
