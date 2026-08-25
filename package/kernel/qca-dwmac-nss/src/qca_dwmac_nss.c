// SPDX-License-Identifier: GPL-2.0-only
/*
 * qca_dwmac_nss.c
 *	NSS data-plane glue for the IPQ5018 dwmac ethernet stack.
 *
 * qca-nss-drv is built against the nss-dp API but nss-dp itself was removed
 * from OpenWrt. On IPQ807x, qca-ppe-nss supplies that API on top of the
 * qca_ppe/qca_edma stack. IPQ5018 has neither PPE nor EDMA: it has two
 * Synopsys GMACs driven by dwmac-ipq5018, and the NSS firmware accelerates
 * one by driving its DMA controller directly with firmware-owned rings.
 *
 * The port model is therefore inverted relative to IPQ807x. There, six PPE
 * ports map 1:1 to DSA user ports and the glue overrides each user port,
 * claiming only conduit TX. Here the firmware knows nothing of switch
 * ports: phys_if N is GMAC N itself, so the netdev being overridden is the
 * trunk (on GL-B3000: eth0 = gmac1, the DSA conduit of the QCA8337). DSA
 * stays transparent - tag_qca frames pass through the firmware in both
 * directions (measured at line rate), the conduit remains the stack's
 * transmit device, and user-port demux happens in the DSA receive path
 * exactly as with the host data plane.
 *
 * The takeover sequence differs accordingly: the host DMA must be parked
 * BEFORE the firmware opens the interface (both would drive the same DMA
 * controller), so qca_dwmac_dp_claim() comes first, then mac_addr/mtu/
 * open/link. On IPQ807x the claim is last because EDMA keeps serving the
 * other ports. Unwinding hands the controller back by bouncing the
 * interface, which reinits the host DMA from scratch.
 *
 * Also unlike IPQ807x: phys_if numbering starts at 0 (not 1), there are
 * 2 ports (not 6), and there is no 32-byte TX preheader - although
 * nss-drv's generic dp_ops->init() still adds 32B of needed_headroom,
 * which is tracked and undone like on IPQ807x.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/soc/qcom/qca_dwmac.h>

#include "nss_dp_api_if.h"

#define DWMAC_NSS_GATE_MAX_DEVS 1	/* IPQ5018 has a single UBI32 core */

/*
 * What nss-drv's generic dp_ops->init() adds to needed_headroom (a literal
 * 32 in __nss_data_plane_init). IPQ5018 has no TX preheader, so our header
 * deliberately lacks NSS_DP_PREHEADER_SIZE - this constant exists only to
 * undo that generic bump symmetrically on unwind.
 */
#define DWMAC_NSS_DP_INIT_HEADROOM 32

static char *ifname = "eth0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "netdev backing the armed NSS phys_if (default eth0)");

static int fw_if = 1;
module_param(fw_if, int, 0444);
MODULE_PARM_DESC(fw_if, "NSS phys_if number of that netdev = its GMAC index (default 1, gmac1)");

static bool boot_unarmed;
module_param(boot_unarmed, bool, 0644);
MODULE_PARM_DESC(boot_unarmed,
		 "let qca-nss-drv boot the NSS core with no port armed (debug only)");

static bool fw_csum;
module_param(fw_csum, bool, 0644);
MODULE_PARM_DESC(fw_csum,
		 "trust the NSS firmware to generate TX checksums (default off: measured on fw 12.5-210-MP, the H2N checksum-generation flags are ignored and TCP leaves the wire corrupt - ICMP works, every TCP handshake dies)");

enum dwmac_nss_port_state {
	DWMAC_NSS_PORT_IDLE = 0,
	DWMAC_NSS_PORT_ARMED,		/* netdev resolved and held */
	DWMAC_NSS_PORT_OVERRIDDEN,	/* nss-drv installed dp_ops */
	DWMAC_NSS_PORT_STARTED,		/* host DMA parked, fw owns the GMAC */
};

struct dwmac_nss_port {
	int if_num;
	struct net_device *netdev;	/* held while armed */
	enum dwmac_nss_port_state state;
	bool headroom_added;		/* dp_ops->init() added 32B */
	netdev_features_t saved_wanted_features;
	bool features_trimmed;		/* TX csum/TSO cleared while started */
	struct nss_dp_data_plane_ops *dp_ops;
	struct nss_dp_data_plane_ctx *dpc;
	atomic64_t tx_redirect_pkts;
	atomic64_t tx_busy;
	atomic64_t rx_fw_pkts;
	/*
	 * Shadow of what the firmware last acknowledged about the link, so
	 * redundant notifies are skipped and a failed one forces a retry -
	 * same discipline as nss-dp's link-state shadow (and qca-ppe-nss).
	 */
	u8 fw_link_up;
	u8 fw_link_failed;
	atomic_t fw_link_changes;
	atomic_t fw_link_skipped;
};

static struct dwmac_nss_port dwmac_nss_ports[NSS_DP_MAX_INTERFACES];
static DEFINE_MUTEX(dwmac_nss_lock);
static unsigned long dwmac_nss_fw_mask;
static struct dentry *dwmac_nss_dentry;
static atomic_t dwmac_nss_rx_unexpected = ATOMIC_INIT(0);

static void dwmac_nss_gate_kick(void);

/*
 * ===== firmware-side callbacks (claim owner) =====
 */

static netdev_tx_t dwmac_nss_fw_xmit(struct sk_buff *skb, void *ctx)
{
	struct dwmac_nss_port *port = ctx;
	netdev_tx_t ret;

	ret = port->dp_ops->xmit(port->dpc, skb);
	if (unlikely(ret == NETDEV_TX_BUSY))
		atomic64_inc(&port->tx_busy);
	else
		atomic64_inc(&port->tx_redirect_pkts);
	return ret;
}

/*
 * Synchronous fw message; process context only (phylink resolve worker
 * or under rtnl from our own paths). Not called concurrently with claim
 * release for the same port: release stops TX first and the phylink
 * callbacks read the owner pointer once.
 */
static int dwmac_nss_fw_link_state(struct dwmac_nss_port *port, bool up)
{
	int err;

	if (port->fw_link_up == up && !port->fw_link_failed) {
		atomic_inc(&port->fw_link_skipped);
		return 0;
	}

	err = port->dp_ops->link_state(port->dpc, up ? 1 : 0);
	if (err)
		netdev_warn(port->netdev, "qca-dwmac-nss: fw link %s notify failed\n",
			    up ? "up" : "down");

	/* track what the firmware acknowledged, not what we asked of it */
	if (!err)
		port->fw_link_up = up;
	port->fw_link_failed = !!err;
	atomic_inc(&port->fw_link_changes);

	return err;
}

static void dwmac_nss_fw_link_up(void *ctx)
{
	dwmac_nss_fw_link_state(ctx, true);
}

static void dwmac_nss_fw_link_down(void *ctx)
{
	dwmac_nss_fw_link_state(ctx, false);
}

static const struct qca_dwmac_dp_owner dwmac_nss_dp_owner = {
	.xmit		= dwmac_nss_fw_xmit,
	.link_up	= dwmac_nss_fw_link_up,
	.link_down	= dwmac_nss_fw_link_down,
};

/*
 * ===== port state machine =====
 */

static struct dwmac_nss_port *dwmac_nss_port_by_netdev(struct net_device *netdev)
{
	int i;

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++)
		if (dwmac_nss_ports[i].netdev == netdev)
			return &dwmac_nss_ports[i];
	return NULL;
}

/*
 * Set across the restore bounce so the netdev notifier ignores the
 * events our own dev_close/dev_open generate - the caller holds
 * dwmac_nss_lock, and the notifier would deadlock taking it again.
 * Coherent without further locking: everything involved runs under rtnl.
 */
static bool dwmac_nss_in_restore;

/*
 * Reinit the host data path after the firmware owned the GMAC DMA. The
 * close/open cycle is the one supported way to get stmmac's rings and
 * DMA registers back to a known state. Caller holds rtnl and
 * dwmac_nss_lock.
 */
static void dwmac_nss_host_restore(struct net_device *netdev)
{
	int err;

	ASSERT_RTNL();

	dwmac_nss_in_restore = true;
	dev_close(netdev);
	err = dev_open(netdev, NULL);
	dwmac_nss_in_restore = false;
	if (err)
		netdev_warn(netdev, "qca-dwmac-nss: host data plane reopen failed (%d)\n",
			    err);
	else
		netdev_info(netdev, "qca-dwmac-nss: back on host data plane\n");
}

/*
 * Bring the firmware data plane up for an overridden port. Order matters:
 * the host DMA is parked first (claim), because fw open hands the same
 * DMA controller to the firmware. Caller holds rtnl and dwmac_nss_lock;
 * nss-drv messages are synchronous, so this sleeps.
 */
/*
 * The config messages right after an arm can hit a firmware that is still
 * initializing (pools, wifi pbuf) and come back failed even though the
 * core reports ready - measured at boot: open failed 2.5s after the arm,
 * while the same call an instant later succeeds. Each message is cheap
 * and idempotent, so retry it a few times before giving up. Keep the
 * count low: each synchronous try can eat its own multi-second timeout,
 * and while port_start retries the host DMA is parked - against a truly
 * dead firmware every extra try extends a total network outage (10 tries
 * held eth0 down for 35s before the host fallback kicked in).
 */
#define DWMAC_NSS_MSG_TRIES	3
#define DWMAC_NSS_MSG_DELAY_MS	500

static int dwmac_nss_msg_retry(int (*msg)(void *), void *arg)
{
	int i, ret = -1;

	for (i = 0; i < DWMAC_NSS_MSG_TRIES; i++) {
		ret = msg(arg);
		if (!ret)
			break;
		msleep(DWMAC_NSS_MSG_DELAY_MS);
	}
	return ret;
}

static int dwmac_nss_msg_mac_addr(void *arg)
{
	struct dwmac_nss_port *port = arg;

	return port->dp_ops->mac_addr(port->dpc,
				      (uint8_t *)port->netdev->dev_addr);
}

static int dwmac_nss_msg_mtu(void *arg)
{
	struct dwmac_nss_port *port = arg;

	return port->dp_ops->change_mtu(port->dpc, port->netdev->mtu);
}

static int dwmac_nss_msg_open(void *arg)
{
	struct dwmac_nss_port *port = arg;

	/*
	 * Ring/mode arguments are zero: the MP firmware allocated its GMAC
	 * descriptor rings at boot (visible in meminfo as gmac_tx_desc_*).
	 */
	return port->dp_ops->open(port->dpc, 0, 0, 0);
}

static int dwmac_nss_port_start(struct dwmac_nss_port *port)
{
	struct net_device *netdev = port->netdev;
	int ret;

	ASSERT_RTNL();

	ret = qca_dwmac_dp_claim(netdev, &dwmac_nss_dp_owner, port);
	if (ret) {
		netdev_warn(netdev, "qca-dwmac-nss: data plane claim failed (%d)\n",
			    ret);
		return ret;
	}

	if (dwmac_nss_msg_retry(dwmac_nss_msg_mac_addr, port)) {
		netdev_warn(netdev, "qca-dwmac-nss: fw mac_addr failed\n");
		goto err_release;
	}

	if (dwmac_nss_msg_retry(dwmac_nss_msg_mtu, port)) {
		netdev_warn(netdev, "qca-dwmac-nss: fw change_mtu(%d) failed\n",
			    netdev->mtu);
		goto err_release;
	}

	if (dwmac_nss_msg_retry(dwmac_nss_msg_open, port)) {
		netdev_warn(netdev, "qca-dwmac-nss: fw open failed\n");
		goto err_release;
	}

	/*
	 * The stmmac hardware whose checksum engine backed NETIF_F_HW_CSUM is
	 * parked now, and the firmware doesn't fill in for it (see fw_csum).
	 * Drop TX checksum offload and with it TSO so the kernel computes
	 * checksums in software before packets reach the firmware. Cleared in
	 * wanted_features only: hw_features keeps the bits, so ethtool -K can
	 * re-enable them for experiments (e.g. against another fw release).
	 * Forwarded traffic never needed them; this taxes only local flows.
	 */
	if (!fw_csum) {
		port->saved_wanted_features = netdev->wanted_features;
		netdev->wanted_features &= ~(NETIF_F_CSUM_MASK | NETIF_F_ALL_TSO);
		netdev_update_features(netdev);
		port->features_trimmed = true;
	}

	/*
	 * A freshly opened firmware interface starts with its link down; say
	 * so in the shadow, then feed it the current carrier - phylink only
	 * reports transitions, and this link may already be up.
	 */
	port->fw_link_up = false;
	port->fw_link_failed = 0;
	if (netif_carrier_ok(netdev))
		dwmac_nss_fw_link_state(port, true);

	port->state = DWMAC_NSS_PORT_STARTED;
	netdev_info(netdev, "qca-dwmac-nss: phys_if %d on NSS fw data plane\n",
		    port->if_num);
	return 0;

err_release:
	qca_dwmac_dp_release(netdev);
	dwmac_nss_host_restore(netdev);
	return -EIO;
}

/*
 * Unwind a port down the state ladder to @target. With @fw_alive the
 * firmware is messaged (link down, close); without, only host-side state
 * is undone - the rmmod path must not message a dead firmware.
 * @restart_host bounces the interface to reinit the host DMA; pass false
 * when the interface is going down anyway (NETDEV_GOING_DOWN, unregister).
 * Caller holds rtnl and dwmac_nss_lock.
 */
static void dwmac_nss_port_unwind(struct dwmac_nss_port *port,
				  enum dwmac_nss_port_state target,
				  bool fw_alive, bool restart_host)
{
	if (port->state == DWMAC_NSS_PORT_STARTED &&
	    target < DWMAC_NSS_PORT_STARTED) {
		if (fw_alive) {
			dwmac_nss_fw_link_state(port, false);
			if (port->dp_ops->close(port->dpc))
				netdev_warn(port->netdev, "qca-dwmac-nss: fw close failed\n");
		}

		/* returns with TX stopped; nothing runs fw_xmit after this */
		qca_dwmac_dp_release(port->netdev);

		if (port->features_trimmed) {
			port->netdev->wanted_features |=
				port->saved_wanted_features &
				(NETIF_F_CSUM_MASK | NETIF_F_ALL_TSO);
			netdev_update_features(port->netdev);
			port->features_trimmed = false;
		}

		if (restart_host)
			dwmac_nss_host_restore(port->netdev);

		port->state = DWMAC_NSS_PORT_OVERRIDDEN;
	}

	if (port->state == DWMAC_NSS_PORT_OVERRIDDEN &&
	    target < DWMAC_NSS_PORT_OVERRIDDEN) {
		if (port->headroom_added) {
			port->netdev->needed_headroom -= DWMAC_NSS_DP_INIT_HEADROOM;
			port->headroom_added = false;
		}
		port->dp_ops = NULL;
		port->dpc = NULL;
		port->state = DWMAC_NSS_PORT_ARMED;
		netdev_info(port->netdev, "qca-dwmac-nss: NSS data plane released on phys_if %d\n",
			    port->if_num);
	}

	if (port->state == DWMAC_NSS_PORT_ARMED &&
	    target < DWMAC_NSS_PORT_ARMED) {
		dev_put(port->netdev);
		port->netdev = NULL;
		port->state = DWMAC_NSS_PORT_IDLE;
	}
}

/*
 * ===== nss-dp API surface (consumed by qca-nss-drv) =====
 */

struct net_device *nss_dp_get_netdev_by_nss_if_num(int if_num)
{
	struct net_device *netdev = NULL;

	if (if_num < NSS_DP_START_IFNUM || if_num >= NSS_DP_MAX_INTERFACES)
		return NULL;

	mutex_lock(&dwmac_nss_lock);
	if (dwmac_nss_ports[if_num].state >= DWMAC_NSS_PORT_ARMED)
		netdev = dwmac_nss_ports[if_num].netdev;
	mutex_unlock(&dwmac_nss_lock);

	return netdev;
}
EXPORT_SYMBOL(nss_dp_get_netdev_by_nss_if_num);

bool nss_dp_is_in_open_state(struct net_device *netdev)
{
	return netif_running(netdev);
}
EXPORT_SYMBOL(nss_dp_is_in_open_state);

int nss_dp_override_data_plane(struct net_device *netdev,
			       struct nss_dp_data_plane_ops *dp_ops,
			       struct nss_dp_data_plane_ctx *dpc)
{
	struct dwmac_nss_port *port;
	int ret = NSS_DP_FAILURE;

	if (!netdev || !dp_ops || !dpc || !dp_ops->init || !dp_ops->open ||
	    !dp_ops->close || !dp_ops->link_state || !dp_ops->mac_addr ||
	    !dp_ops->change_mtu || !dp_ops->xmit)
		return NSS_DP_FAILURE;

	mutex_lock(&dwmac_nss_lock);
	port = dwmac_nss_port_by_netdev(netdev);
	if (!port || port->state < DWMAC_NSS_PORT_ARMED)
		goto out;

	if (port->state >= DWMAC_NSS_PORT_OVERRIDDEN) {
		/* idempotent for nss-drv's per-core loops */
		ret = (port->dpc == dpc) ? NSS_DP_SUCCESS : NSS_DP_FAILURE;
		goto out;
	}

	port->dp_ops = dp_ops;
	port->dpc = dpc;

	/* adds 32B to needed_headroom (generic nss-drv code; no preheader
	 * exists on IPQ5018, but tracking it keeps the unwind symmetric) */
	if (dp_ops->init(dpc)) {
		netdev_warn(netdev, "qca-dwmac-nss: dp init failed\n");
		port->dp_ops = NULL;
		port->dpc = NULL;
		goto out;
	}
	port->headroom_added = true;
	port->state = DWMAC_NSS_PORT_OVERRIDDEN;

	netdev_info(netdev, "qca-dwmac-nss: NSS data plane override on phys_if %d\n",
		    port->if_num);
	ret = NSS_DP_SUCCESS;
out:
	mutex_unlock(&dwmac_nss_lock);
	return ret;
}
EXPORT_SYMBOL(nss_dp_override_data_plane);

/*
 * nss-drv signals "registration done, port was open" here (process
 * context). For a port that is down, the NETDEV_UP notifier does this
 * instead.
 */
void nss_dp_start_data_plane(struct net_device *netdev,
			     struct nss_dp_data_plane_ctx *dpc)
{
	struct dwmac_nss_port *port;

	rtnl_lock();
	mutex_lock(&dwmac_nss_lock);
	port = dwmac_nss_port_by_netdev(netdev);
	if (port && port->state == DWMAC_NSS_PORT_OVERRIDDEN) {
		if (port->dpc != dpc)
			netdev_warn(netdev, "qca-dwmac-nss: start with foreign dpc, ignored\n");
		else
			dwmac_nss_port_start(port);
	}
	mutex_unlock(&dwmac_nss_lock);
	rtnl_unlock();
}
EXPORT_SYMBOL(nss_dp_start_data_plane);

/*
 * nss-drv detaches at rmmod. Its IRQs are already torn down by the time
 * this runs, so firmware messages could only time out: host-side cleanup
 * only, and the interface is bounced to give the host its DMA back.
 */
void nss_dp_restore_data_plane(struct net_device *netdev)
{
	struct dwmac_nss_port *port;

	rtnl_lock();
	mutex_lock(&dwmac_nss_lock);
	port = dwmac_nss_port_by_netdev(netdev);
	if (port && port->state >= DWMAC_NSS_PORT_OVERRIDDEN)
		dwmac_nss_port_unwind(port, DWMAC_NSS_PORT_ARMED, false, true);
	mutex_unlock(&dwmac_nss_lock);
	rtnl_unlock();
}
EXPORT_SYMBOL(nss_dp_restore_data_plane);

/*
 * N2H receive: every packet the firmware delivers for a registered
 * phys_if lands here (nss-drv NAPI). The netdev is the trunk; delivering
 * on it runs the normal conduit receive path, so DSA demuxes tag_qca
 * frames to user ports exactly as if the host DMA had received them.
 */
void nss_dp_receive(struct net_device *netdev, struct sk_buff *skb,
		    struct napi_struct *napi)
{
	struct dwmac_nss_port *port = NULL;

	if (likely(netdev))
		port = dwmac_nss_port_by_netdev(netdev);

	if (unlikely(!port)) {
		atomic_inc(&dwmac_nss_rx_unexpected);
		dev_kfree_skb_any(skb);
		return;
	}

	atomic64_inc(&port->rx_fw_pkts);

	/*
	 * No dev_sw_netstats_rx_add() here, unlike qca-ppe-nss: it requires
	 * per-cpu dev->tstats, which DSA user ports allocate but stmmac does
	 * not (it counts in its own DMA rings, which this path bypasses).
	 * Deref of the absent tstats = panic on first fw-delivered packet
	 * (measured). Firmware-path RX is counted in our debugfs instead.
	 */
	skb->dev = netdev;
	skb->protocol = eth_type_trans(skb, netdev);
	napi_gro_receive(napi, skb);
}
EXPORT_SYMBOL(nss_dp_receive);

/*
 * PTP timestamp RX delivery, registered by nss-drv as service code 0x9.
 */
void nss_phy_tstamp_rx_buf(void *app_data, struct sk_buff *skb)
{
	struct net_device *ndev = skb->dev;

	if (ndev && phy_has_rxtstamp(ndev->phydev))
		if (phy_rxtstamp(ndev->phydev, skb, 0))
			return;

	netif_receive_skb(skb);
}
EXPORT_SYMBOL(nss_phy_tstamp_rx_buf);

/*
 * ===== nss-drv deferred probe gate =====
 *
 * qca-nss-drv defers its platform probe - and with it the NSS firmware
 * boot - until a port is armed, so a module dependency chain (e.g. from
 * Wi-Fi offload) cannot boot the firmware behind our back. Deferred core
 * devices are recorded and re-attached when fw_mask first goes non-zero.
 */

static DEFINE_SPINLOCK(dwmac_nss_gate_lock);
static struct device *dwmac_nss_gate_devs[DWMAC_NSS_GATE_MAX_DEVS];

int nss_dp_probe_gate(struct device *dev)
{
	bool recorded = false;
	int i;

	spin_lock(&dwmac_nss_gate_lock);
	if (READ_ONCE(dwmac_nss_fw_mask) || boot_unarmed) {
		spin_unlock(&dwmac_nss_gate_lock);
		return 0;
	}
	for (i = 0; i < DWMAC_NSS_GATE_MAX_DEVS; i++) {
		if (dwmac_nss_gate_devs[i] == dev)
			break;
		if (!dwmac_nss_gate_devs[i]) {
			dwmac_nss_gate_devs[i] = get_device(dev);
			recorded = true;
			break;
		}
	}
	spin_unlock(&dwmac_nss_gate_lock);

	/* the driver core retries deferred probes often - log once */
	if (recorded)
		dev_info(dev, "qca-dwmac-nss: deferring NSS core probe until a port is armed\n");
	else if (i == DWMAC_NSS_GATE_MAX_DEVS)
		dev_warn_once(dev,
			      "qca-dwmac-nss: more NSS core devices than cores, this one will stay deferred\n");
	return -EPROBE_DEFER;
}
EXPORT_SYMBOL(nss_dp_probe_gate);

static void dwmac_nss_gate_kick(void)
{
	struct device *devs[DWMAC_NSS_GATE_MAX_DEVS];
	int i;

	spin_lock(&dwmac_nss_gate_lock);
	memcpy(devs, dwmac_nss_gate_devs, sizeof(devs));
	memset(dwmac_nss_gate_devs, 0, sizeof(dwmac_nss_gate_devs));
	spin_unlock(&dwmac_nss_gate_lock);

	for (i = 0; i < DWMAC_NSS_GATE_MAX_DEVS; i++) {
		if (!devs[i])
			continue;
		if (device_attach(devs[i]) < 0)
			dev_warn(devs[i],
				 "qca-dwmac-nss: deferred NSS core attach failed\n");
		put_device(devs[i]);
	}
}

static void dwmac_nss_gate_drop(void)
{
	int i;

	spin_lock(&dwmac_nss_gate_lock);
	for (i = 0; i < DWMAC_NSS_GATE_MAX_DEVS; i++) {
		if (dwmac_nss_gate_devs[i]) {
			put_device(dwmac_nss_gate_devs[i]);
			dwmac_nss_gate_devs[i] = NULL;
		}
	}
	spin_unlock(&dwmac_nss_gate_lock);
}

/*
 * ===== arming (fw_mask) =====
 */

static int dwmac_nss_fw_arm(int if_num)
{
	struct dwmac_nss_port *port = &dwmac_nss_ports[if_num];
	struct net_device *netdev;

	if (port->state >= DWMAC_NSS_PORT_ARMED)
		return 0;

	netdev = dev_get_by_name(&init_net, ifname);
	if (!netdev) {
		pr_warn("qca-dwmac-nss: no netdev '%s' to arm phys_if %d\n",
			ifname, if_num);
		return -ENODEV;
	}

	port->if_num = if_num;
	port->netdev = netdev;
	port->state = DWMAC_NSS_PORT_ARMED;
	netdev_info(netdev, "qca-dwmac-nss: armed as NSS phys_if %d\n", if_num);
	return 0;
}

static int dwmac_nss_fw_mask_apply(unsigned long mask)
{
	unsigned long old = dwmac_nss_fw_mask;
	int i, ret = 0;

	/* only the configured GMAC can be armed on this board */
	if (mask & ~BIT(fw_if)) {
		pr_warn("qca-dwmac-nss: fw_mask %#lx outside supported bit %d\n",
			mask, fw_if);
		return -EINVAL;
	}

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		bool want = !!(mask & BIT(i));
		bool have = !!(old & BIT(i));

		if (want && !have)
			ret = dwmac_nss_fw_arm(i);
		else if (!want && have)
			dwmac_nss_port_unwind(&dwmac_nss_ports[i],
					      DWMAC_NSS_PORT_IDLE, true, true);
	}

	if (!ret) {
		WRITE_ONCE(dwmac_nss_fw_mask, mask);
		if (mask && !old)
			dwmac_nss_gate_kick();
	}
	return ret;
}

static ssize_t dwmac_nss_fw_mask_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	unsigned long mask;
	char buf[24];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtoul(strim(buf), 0, &mask);
	if (ret)
		return ret;

	rtnl_lock();
	mutex_lock(&dwmac_nss_lock);
	ret = dwmac_nss_fw_mask_apply(mask);
	mutex_unlock(&dwmac_nss_lock);
	rtnl_unlock();

	return ret ? ret : count;
}

static int dwmac_nss_fw_mask_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%#lx\n", READ_ONCE(dwmac_nss_fw_mask));
	return 0;
}

static int dwmac_nss_fw_mask_open(struct inode *inode, struct file *file)
{
	return single_open(file, dwmac_nss_fw_mask_show, NULL);
}

static const struct file_operations dwmac_nss_fw_mask_fops = {
	.owner		= THIS_MODULE,
	.open		= dwmac_nss_fw_mask_open,
	.read		= seq_read,
	.write		= dwmac_nss_fw_mask_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * ===== netdev events =====
 */

static int dwmac_nss_netdev_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	struct dwmac_nss_port *port;

	/*
	 * Filter BEFORE taking the lock: events we don't handle can be fired
	 * synchronously from paths that already hold it. The concrete case:
	 * port_start's netdev_update_features() raises NETDEV_FEAT_CHANGE
	 * with dwmac_nss_lock (and rtnl) held - taking the lock here again
	 * deadlocked the nss-drv registration workqueue with rtnl pinned,
	 * freezing all network configuration (measured).
	 */
	switch (event) {
	case NETDEV_UP:
	case NETDEV_GOING_DOWN:
	case NETDEV_CHANGEMTU:
	case NETDEV_CHANGEADDR:
	case NETDEV_UNREGISTER:
		break;
	default:
		return NOTIFY_DONE;
	}

	/* our own restore bounce; the lock is already held by its caller */
	if (dwmac_nss_in_restore)
		return NOTIFY_DONE;

	mutex_lock(&dwmac_nss_lock);
	port = dwmac_nss_port_by_netdev(netdev);
	if (!port) {
		mutex_unlock(&dwmac_nss_lock);
		return NOTIFY_DONE;
	}

	switch (event) {
	case NETDEV_UP:
		if (port->state == DWMAC_NSS_PORT_OVERRIDDEN)
			dwmac_nss_port_start(port);
		break;
	case NETDEV_GOING_DOWN:
		/*
		 * stmmac_release would tear the driver down around a
		 * firmware-owned DMA; hand the port back first. No bounce -
		 * the interface is closing anyway, and the next NETDEV_UP
		 * restarts the firmware path over the fresh host state.
		 */
		if (port->state == DWMAC_NSS_PORT_STARTED)
			dwmac_nss_port_unwind(port, DWMAC_NSS_PORT_OVERRIDDEN,
					      true, false);
		break;
	case NETDEV_CHANGEMTU:
		if (port->state == DWMAC_NSS_PORT_STARTED &&
		    port->dp_ops->change_mtu(port->dpc, netdev->mtu))
			netdev_warn(netdev, "qca-dwmac-nss: fw change_mtu(%d) failed\n",
				    netdev->mtu);
		break;
	case NETDEV_CHANGEADDR:
		if (port->state == DWMAC_NSS_PORT_STARTED &&
		    port->dp_ops->mac_addr(port->dpc, (uint8_t *)netdev->dev_addr))
			netdev_warn(netdev, "qca-dwmac-nss: fw mac_addr failed\n");
		break;
	case NETDEV_UNREGISTER:
		dwmac_nss_port_unwind(port, DWMAC_NSS_PORT_IDLE,
				      port->state == DWMAC_NSS_PORT_STARTED,
				      false);
		WRITE_ONCE(dwmac_nss_fw_mask,
			   dwmac_nss_fw_mask & ~BIT(port->if_num));
		break;
	}
	mutex_unlock(&dwmac_nss_lock);
	return NOTIFY_DONE;
}

static struct notifier_block dwmac_nss_netdev_nb = {
	.notifier_call = dwmac_nss_netdev_event,
};

/*
 * ===== debugfs status =====
 */

static const char * const dwmac_nss_state_names[] = {
	"idle", "armed", "overridden", "started",
};

static int dwmac_nss_status_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "fw_mask: %#lx\n", READ_ONCE(dwmac_nss_fw_mask));
	seq_printf(m, "rx_unexpected: %d\n",
		   atomic_read(&dwmac_nss_rx_unexpected));

	mutex_lock(&dwmac_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct dwmac_nss_port *port = &dwmac_nss_ports[i];

		if (port->state == DWMAC_NSS_PORT_IDLE)
			continue;
		seq_printf(m, "phys_if %d: %s dev=%s fw_link=%s%s\n",
			   i, dwmac_nss_state_names[port->state],
			   netdev_name(port->netdev),
			   port->fw_link_up ? "up" : "down",
			   port->fw_link_failed ? " (notify-failed)" : "");
		seq_printf(m, "  tx_redirect=%lld tx_busy=%lld rx_fw=%lld link_changes=%d link_skipped=%d\n",
			   (long long)atomic64_read(&port->tx_redirect_pkts),
			   (long long)atomic64_read(&port->tx_busy),
			   (long long)atomic64_read(&port->rx_fw_pkts),
			   atomic_read(&port->fw_link_changes),
			   atomic_read(&port->fw_link_skipped));
	}
	mutex_unlock(&dwmac_nss_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dwmac_nss_status);

static int __init qca_dwmac_nss_init(void)
{
	int ret;

	if (fw_if < NSS_DP_START_IFNUM || fw_if >= NSS_DP_MAX_INTERFACES) {
		pr_err("qca-dwmac-nss: fw_if %d out of range 0..%d\n",
		       fw_if, NSS_DP_MAX_INTERFACES - 1);
		return -EINVAL;
	}

	ret = register_netdevice_notifier(&dwmac_nss_netdev_nb);
	if (ret)
		return ret;

	dwmac_nss_dentry = debugfs_create_dir("qca-dwmac-nss", NULL);
	debugfs_create_file("status", 0444, dwmac_nss_dentry, NULL,
			    &dwmac_nss_status_fops);
	debugfs_create_file("fw_mask", 0644, dwmac_nss_dentry, NULL,
			    &dwmac_nss_fw_mask_fops);

	pr_info("qca-dwmac-nss: nss-dp glue for IPQ5018 loaded (%s = phys_if %d)\n",
		ifname, fw_if);
	return 0;
}

static void __exit qca_dwmac_nss_exit(void)
{
	int i;

	/*
	 * nss-drv holds symbol references on this module, so it is gone by
	 * the time rmmod succeeds and every port is at most ARMED.
	 */
	rtnl_lock();
	mutex_lock(&dwmac_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++)
		dwmac_nss_port_unwind(&dwmac_nss_ports[i],
				      DWMAC_NSS_PORT_IDLE, false, false);
	mutex_unlock(&dwmac_nss_lock);
	rtnl_unlock();

	debugfs_remove_recursive(dwmac_nss_dentry);
	unregister_netdevice_notifier(&dwmac_nss_netdev_nb);
	dwmac_nss_gate_drop();
}

module_init(qca_dwmac_nss_init);
module_exit(qca_dwmac_nss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("NSS data-plane glue for the IPQ5018 dwmac stack");
