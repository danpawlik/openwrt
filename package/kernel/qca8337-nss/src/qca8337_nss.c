// SPDX-License-Identifier: GPL-2.0-only
/*
 * qca8337_nss.c - re-arm the QCA8337 fabric after qca8k teardown.
 *
 * The VLAN-separated NSS data path ("road 2") needs the switch CPU port
 * to carry plain 802.1q frames: the firmware parses VLAN tags natively
 * (dynamic interface type 17) but cannot parse the 2-byte Atheros header
 * that DSA's tag_qca puts in front of the ethertype. So qca8k is unbound
 * at runtime - after it has done the hard bring-up work (SerDes, clocks,
 * uniphy) - and this module undoes just what its teardown broke:
 *
 *   wake_phys:     the front-panel PHYs are left in BMCR power-down;
 *                  power them back up with autoneg.
 *   switch_fixup:  ports were disabled and the Atheros header left on;
 *                  re-enable MACs, turn the header off everywhere, set
 *                  ports 0-3 to FORWARD with full member masks and
 *                  restore unknown-unicast/multicast/broadcast flooding.
 *
 * Both were measured out on GL-B3000 during the 6.12 bring-up (road 1)
 * and carried over unchanged; the paged-MDIO protocol is copied from
 * qca8k-8xxx.c. VLANs then ride transparently: with the header off the
 * fabric floods tagged frames like any other traffic, so 8021q
 * subinterfaces on the trunk see them intact. Hardware VLAN separation
 * of the front ports (VTU programming) is a follow-up, not needed while
 * every jack belongs to the same test segment.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/phy.h>

static bool switch_fixup = true;
module_param(switch_fixup, bool, 0444);
MODULE_PARM_DESC(switch_fixup, "Re-enable QCA8337 fabric, Atheros header off (default on)");

static char *wake_phys = "90000.mdio-1:00,90000.mdio-1:01,90000.mdio-1:02";
module_param(wake_phys, charp, 0444);

/*
 * VLAN separation of the front ports (the VTU follow-up the header above
 * promised). Format: one block per VLAN, semicolon separated:
 *
 *     vlans=<vid>:<port><t|u>[,<port><t|u>]...[;...]
 *
 * e.g. the GL-B3000 router split - lan1+lan2 on VLAN 1, wan on VLAN 2,
 * the CPU port carrying both tagged:
 *
 *     vlans=1:0t,2u,3u;2:0t,1u
 *
 * A port listed 'u' (untagged egress) also gets the vid as its PVID for
 * ingress. Port isolation follows from the VLAN table: each port's
 * lookup member mask is narrowed to the ports it shares a VLAN with,
 * and the lookup VLAN mode is set to SECURE so ingress is checked
 * against the VTU. Empty string disables the whole feature.
 */
static char *vlans = "";
module_param(vlans, charp, 0444);
MODULE_PARM_DESC(vlans, "VTU program, e.g. '1:0t,2u,3u;2:0t,1u' (empty = off)");
MODULE_PARM_DESC(wake_phys, "Comma list of mdio device names to power up (BMCR)");

static char *bus_via = "90000.mdio-1:01";
module_param(bus_via, charp, 0444);
MODULE_PARM_DESC(bus_via, "Any mdio device on the switch bus, used to reach it");

#define G8_PORT_STATUS(i)	(0x07c + (i) * 4)
#define G8_PORT_HDR_CTRL(i)	(0x9c + (i) * 4)
#define G8_PORT_LOOKUP(i)	(0x660 + (i) * 0xc)
#define G8_GLOBAL_FW_CTRL1	0x624

/* VLAN table and per-port VLAN registers (names as in qca8k.h) */
#define G8_PORT_VLAN_CTRL0(i)	(0x420 + (i) * 8)
#define G8_PORT_VLAN_CTRL1(i)	(0x424 + (i) * 8)
#define   G8_OUT_MODE_MASK	GENMASK(13, 12)
#define   G8_OUT_MODE_UNTOUCH	FIELD_PREP(GENMASK(13, 12), 0x3)
#define G8_EGRESS_VLAN(i)	(0x0c70 + 4 * ((i) / 2))
#define G8_VTU_FUNC0		0x610
#define   G8_VTU_VALID		BIT(20)
#define   G8_VTU_IVL_EN		BIT(19)
#define   G8_VTU_EG_SHIFT(i)	(4 + (i) * 2)
#define   G8_VTU_EG_UNTAG	0x1
#define   G8_VTU_EG_TAG		0x2
#define   G8_VTU_EG_NOT		0x3
#define G8_VTU_FUNC1		0x614
#define   G8_VTU_BUSY		BIT(31)
#define   G8_VTU_FULL		BIT(4)
#define   G8_VTU_CMD_LOAD	2
#define   G8_VTU_VID(v)		((u32)(v) << 16)
#define G8_LOOKUP_VLAN_SECURE	FIELD_PREP(GENMASK(9, 8), 0x3)

#define G8_NPORTS		7

static void g8_split(u32 reg, u16 *r1, u16 *r2, u16 *page)
{
	reg >>= 1;
	*r1 = reg & 0x1e;
	reg >>= 5;
	*r2 = reg & 0x7;
	reg >>= 3;
	*page = reg & 0x3ff;
}

static u32 g8_read(struct mii_bus *bus, u32 reg)
{
	u16 r1, r2, page;
	u32 lo, hi;

	g8_split(reg, &r1, &r2, &page);
	mdiobus_write(bus, 0x18, 0, page);
	usleep_range(100, 200);
	lo = mdiobus_read(bus, 0x10 | r2, r1) & 0xffff;
	hi = mdiobus_read(bus, 0x10 | r2, r1 + 1) & 0xffff;
	return (hi << 16) | lo;
}

static void g8_write(struct mii_bus *bus, u32 reg, u32 val)
{
	u16 r1, r2, page;

	g8_split(reg, &r1, &r2, &page);
	mdiobus_write(bus, 0x18, 0, page);
	usleep_range(100, 200);
	mdiobus_write(bus, 0x10 | r2, r1, val & 0xffff);
	mdiobus_write(bus, 0x10 | r2, r1 + 1, val >> 16);
}

static void fixup_vlans(struct mii_bus *bus);

static void fixup_switch(void)
{
	struct device *d;
	struct mii_bus *bus;
	int p;

	d = bus_find_device_by_name(&mdio_bus_type, NULL, bus_via);
	if (!d) {
		pr_err("qca8337-nss: no mdio device '%s' to derive bus\n",
		       bus_via);
		return;
	}
	bus = to_phy_device(d)->mdio.bus;

	pr_info("qca8337-nss: chip id reg0=0x%08x\n", g8_read(bus, 0));

	for (p = 0; p <= 3; p++) {
		u32 lkp;

		/* no Atheros header anywhere */
		g8_write(bus, G8_PORT_HDR_CTRL(p), 0);

		/* MACs on: CPU port forced 1G FD, jacks autoneg */
		if (p == 0)
			g8_write(bus, G8_PORT_STATUS(p),
				 0x2 /*1000*/ | BIT(2) | BIT(3) | BIT(6));
		else
			g8_write(bus, G8_PORT_STATUS(p),
				 BIT(2) | BIT(3) | BIT(9));

		/* lookup: member = all other ports 0-3, state FORWARD(4) */
		lkp = g8_read(bus, G8_PORT_LOOKUP(p));
		lkp &= ~GENMASK(6, 0);
		lkp &= ~GENMASK(18, 16);
		lkp |= (0xf & ~BIT(p));
		lkp |= FIELD_PREP(GENMASK(18, 16), 0x4);
		g8_write(bus, G8_PORT_LOOKUP(p), lkp);
		pr_info("qca8337-nss: port%d status=0x%08x lookup=0x%08x hdr=0x%08x\n",
			p, g8_read(bus, G8_PORT_STATUS(p)),
			g8_read(bus, G8_PORT_LOOKUP(p)),
			g8_read(bus, G8_PORT_HDR_CTRL(p)));
	}

	/* flood unknown UC/MC/BC/IGMP to all ports 0-3 */
	g8_write(bus, G8_GLOBAL_FW_CTRL1,
		 FIELD_PREP(GENMASK(30, 24), 0xf) |
		 FIELD_PREP(GENMASK(22, 16), 0xf) |
		 FIELD_PREP(GENMASK(14, 8), 0xf) |
		 FIELD_PREP(GENMASK(6, 0), 0xf));
	pr_info("qca8337-nss: fw_ctrl1=0x%08x\n",
		g8_read(bus, G8_GLOBAL_FW_CTRL1));

	if (*vlans)
		fixup_vlans(bus);

	put_device(d);
}

static int g8_vtu_wait(struct mii_bus *bus)
{
	int i;

	for (i = 0; i < 20; i++) {
		if (!(g8_read(bus, G8_VTU_FUNC1) & G8_VTU_BUSY))
			return 0;
		usleep_range(100, 200);
	}
	return -ETIMEDOUT;
}

static int g8_vtu_load(struct mii_bus *bus, u16 vid, u32 func0)
{
	g8_write(bus, G8_VTU_FUNC0, func0);
	g8_write(bus, G8_VTU_FUNC1, G8_VTU_BUSY | G8_VTU_CMD_LOAD | G8_VTU_VID(vid));
	if (g8_vtu_wait(bus))
		return -ETIMEDOUT;
	if (g8_read(bus, G8_VTU_FUNC1) & G8_VTU_FULL)
		return -ENOMEM;
	return 0;
}

static void fixup_vlans(struct mii_bus *bus)
{
	u16 pvid[G8_NPORTS] = {};
	u8 member[G8_NPORTS] = {};
	char *buf, *blk, *cur;
	int p;

	buf = kstrdup(vlans, GFP_KERNEL);
	if (!buf)
		return;

	cur = buf;
	while ((blk = strsep(&cur, ";"))) {
		char *ports, *vidstr, *tok;
		unsigned int vid;
		u8 in_vlan = 0;
		u32 func0;

		if (!*blk)
			continue;
		ports = blk;
		vidstr = strsep(&ports, ":");
		if (!ports || kstrtouint(vidstr, 0, &vid) || !vid || vid > 4094) {
			pr_err("qca8337-nss: vlans: bad block '%s'\n", vidstr);
			continue;
		}

		/* every port not listed egresses NOT_MEMBER */
		func0 = G8_VTU_VALID | G8_VTU_IVL_EN;
		for (p = 0; p < G8_NPORTS; p++)
			func0 |= G8_VTU_EG_NOT << G8_VTU_EG_SHIFT(p);

		while ((tok = strsep(&ports, ","))) {
			unsigned int port;
			char mode;
			size_t n;

			if (!*tok)
				continue;
			n = strlen(tok);
			mode = tok[n - 1];
			tok[n - 1] = '\0';
			if (kstrtouint(tok, 0, &port) || port >= G8_NPORTS ||
			    (mode != 't' && mode != 'u')) {
				pr_err("qca8337-nss: vlans: bad port spec '%s%c' in vid %u\n",
				       tok, mode, vid);
				continue;
			}
			func0 &= ~(0x3u << G8_VTU_EG_SHIFT(port));
			func0 |= (mode == 'u' ? G8_VTU_EG_UNTAG : G8_VTU_EG_TAG)
				 << G8_VTU_EG_SHIFT(port);
			in_vlan |= BIT(port);
			if (mode == 'u')
				pvid[port] = vid;
		}

		if (g8_vtu_load(bus, vid, func0)) {
			pr_err("qca8337-nss: vlans: VTU load failed for vid %u\n", vid);
			continue;
		}

		/* members of one VLAN may reach each other */
		for (p = 0; p < G8_NPORTS; p++)
			if (in_vlan & BIT(p))
				member[p] |= in_vlan & ~BIT(p);

		pr_info("qca8337-nss: vlans: vid %u func0=0x%08x members=0x%02x\n",
			vid, g8_read(bus, G8_VTU_FUNC0), in_vlan);
	}

	/* PVID for untagged ingress + per-port default egress vid */
	for (p = 0; p < G8_NPORTS; p++) {
		u32 reg;

		if (!(member[p] || pvid[p]))
			continue;

		/*
		 * Egress tagging must come from the VTU entry, not the
		 * port-global mode - OUT_MODE=UNTOUCH hands the decision
		 * to the per-VLAN eg bits (the reset default, UNMOD,
		 * forwards frames exactly as they came in, so the CPU
		 * port never saw a tag).
		 */
		reg = g8_read(bus, G8_PORT_VLAN_CTRL1(p));
		reg &= ~G8_OUT_MODE_MASK;
		reg |= G8_OUT_MODE_UNTOUCH;
		g8_write(bus, G8_PORT_VLAN_CTRL1(p), reg);

		if (!pvid[p])
			continue;
		g8_write(bus, G8_PORT_VLAN_CTRL0(p),
			 ((u32)pvid[p] << 16) | pvid[p]);
		reg = g8_read(bus, G8_EGRESS_VLAN(p));
		reg &= ~(GENMASK(11, 0) << (16 * (p % 2)));
		reg |= (u32)pvid[p] << (16 * (p % 2));
		g8_write(bus, G8_EGRESS_VLAN(p), reg);
	}

	/*
	 * Narrow each port's lookup member mask to its VLAN peers and check
	 * ingress against the VTU. Ports in no VLAN are left as fixup_switch
	 * set them - this runs after it, so only listed ports are touched.
	 */
	for (p = 0; p < G8_NPORTS; p++) {
		u32 lkp;

		if (!member[p] && !pvid[p])
			continue;
		lkp = g8_read(bus, G8_PORT_LOOKUP(p));
		lkp &= ~GENMASK(6, 0);
		lkp |= member[p];
		lkp &= ~GENMASK(9, 8);
		lkp |= G8_LOOKUP_VLAN_SECURE;
		g8_write(bus, G8_PORT_LOOKUP(p), lkp);
		pr_info("qca8337-nss: vlans: port%d lookup=0x%08x pvid=%u\n",
			p, g8_read(bus, G8_PORT_LOOKUP(p)), pvid[p]);
	}

	kfree(buf);
}

static void fixup_wake_phys(void)
{
	char *buf, *tok, *cur;

	buf = kstrdup(wake_phys, GFP_KERNEL);
	if (!buf)
		return;
	cur = buf;
	while ((tok = strsep(&cur, ","))) {
		struct device *d;
		struct phy_device *phydev;
		int bmcr;

		if (!*tok)
			continue;
		d = bus_find_device_by_name(&mdio_bus_type, NULL, tok);
		if (!d) {
			pr_warn("qca8337-nss: wake: no mdio dev %s\n", tok);
			continue;
		}
		phydev = to_phy_device(d);
		bmcr = phy_read(phydev, MII_BMCR);
		pr_info("qca8337-nss: %s BMCR=0x%04x%s\n", tok, bmcr,
			(bmcr & BMCR_PDOWN) ? " (POWER-DOWN)" : "");
		phy_write(phydev, MII_BMCR,
			  (bmcr & ~BMCR_PDOWN) | BMCR_ANENABLE | BMCR_ANRESTART);
		pr_info("qca8337-nss: %s woken, BMCR=0x%04x\n", tok,
			phy_read(phydev, MII_BMCR));
		put_device(d);
	}
	kfree(buf);
}

static int __init qca8337_nss_init(void)
{
	if (*wake_phys)
		fixup_wake_phys();
	if (switch_fixup)
		fixup_switch();
	return 0;
}

static void __exit qca8337_nss_exit(void)
{
}

module_init(qca8337_nss_init);
module_exit(qca8337_nss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QCA8337 fabric re-arm after qca8k teardown (headerless mode)");
