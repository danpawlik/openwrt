# NSS offload on IPQ5018 with the upstream ethernet stack (branch `ipq50xx-nss`)

This branch runs the **NSS packet-processing core of the IPQ5018** on kernel
**6.18** with OpenWrt main's **upstream `stmmac` / `dwmac-ipq5018` ethernet
driver** - no `qca-nss-dp`, no `qca-ssdk`. It is the IPQ50xx counterpart of the
[IPQ807x work](README.md) this tree is layered on: same idea (keep the upstream
driver, attach the firmware through a small glue module), different silicon,
different glue.

Validated on the **GL.iNet GL-B3000** (IPQ5018 + QCA8337 + QCN6122):
routed NAT through the firmware at the ceiling of the single 1 GbE CPU port
(~900 Mbit/s measured, ~911 by arithmetic - see *The single CPU port ceiling*) with the CPU
**above 90 % idle** under load. Wired only: Wi-Fi runs on the host path, for a
firmware reason explained below.

Discussion and test reports: the
[forum thread](https://forum.openwrt.org/t/ipq5018-nss-offload-on-kernel-6-18-with-the-upstream-ethernet-stack-gl-b3000/253014).

## What is in the branch

Ten commits on top of `nss-edma-rework` (base `82693c6350`), in build order:

| Commit | What |
|---|---|
| `qualcommax: ipq50xx: keep the CMN PLL bus clocks enabled on 6.18` | Without this, 6.18 does not boot on IPQ5018 at all - the SoC dies within milliseconds of the CMN PLL probe. Merged into openwrt/main as [86b584bd0994](https://github.com/openwrt/openwrt/commit/86b584bd09949f14231d373c46563cc9); still carried here until the base catches up. |
| `qualcommax: stmmac: add a data-plane claim API for the NSS firmware` | Patch `0956`: lets a module take the data path of a GMAC away from stmmac (TX drained, NAPI off, DMA stopped, `ndo_start_xmit` redirected) and hand it back. phylink, MDIO and the netdev stay with the host. |
| `qualcommax: ipq5018: add the NSS core node and reserved memory` | `ipq5018-nss.dtsi`: the `nss0` node with its clocks and interrupts, plus the reserved-memory region for the firmware. |
| `package: add qca-dwmac-nss, the NSS data-plane glue for IPQ5018` | `kmod-qca-dwmac-nss`: the counterpart of `qca-ppe-nss` for this SoC. Implements the `nss-dp` API `qca-nss-drv` expects on top of the stmmac claim. Arms at runtime through debugfs, never at probe. |
| `package: add qca8337-nss, the switch fabric driver for the NSS trunk` | `kmod-qca8337-nss`: re-arms the QCA8337 as a plain 802.1Q fabric after `qca8k` is unbound (see *Why DSA has to go*). |
| `package: nss-tools: add the ipq50xx bring-up service` | `nss-tools-dwmac`: the `nss` service, the ordering that makes the arm work, the one-shot network-config migration. |
| `qualcommax: ipq5018: enable the NSS core on the GL-B3000` | One line in the board DTS: `#include "ipq5018-nss.dtsi"`. |
| `mac80211: ath11k: NSS fixes found while porting wifili to IPQ5018` | Six real bugs met on the way (REO register layout, ring topology, init flags, L2 update frame padding, non-cacheable rings, and one that matters with the offload *off*: a QCN6122 radio no longer disappears because `ath11k_nss_setup()` returns `-ENOTSUPP` for it). |
| `mac80211: ath11k: wifili investigation tooling (inert by default)` | Twenty-two module parameters and traces, all off by default. Kept for whoever repeats the Wi-Fi investigation. |
| `qualcommax: ipq50xx: debug aids for NSS bring-up work` | `MAGIC_SYSRQ_SERIAL`, `DEVMEM` with `STRICT_DEVMEM` off. **Revert this commit for a build meant to be deployed** - it is one commit precisely so that is easy. |

The companion feed is **[kuncy7/nss-packages](https://github.com/kuncy7/nss-packages/tree/ipq50xx-nss)**,
branch `ipq50xx-nss`: Julius's feed at `e621a63` plus his twelve `qca-nss-drv`
hardening commits (cherry-picked, authorship preserved) plus five of ours - the
12.2 firmware line as a selectable version, the per-target package split that
lets the stack build on ipq50xx, and four driver patches (`0120`, `0121`,
`0136`, `0137`; see the feed README).

## Quick start

```sh
git clone -b ipq50xx-nss https://github.com/kuncy7/openwrt-nss-edma.git
cd openwrt-nss-edma

cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/kuncy7/nss-packages.git;ipq50xx-nss" >> feeds.conf

./scripts/feeds update -a && ./scripts/feeds install -a
./scripts/feeds list -r nss | grep -q qca-nss-drv && echo "nss feed OK"

make menuconfig
make -j$(nproc)
```

The `.config` the validated image was built from, reduced to what matters
(everything the packages depend on is pulled in by `nss-tools-dwmac`):

```
CONFIG_TARGET_qualcommax=y
CONFIG_TARGET_qualcommax_ipq50xx=y
CONFIG_TARGET_qualcommax_ipq50xx_DEVICE_glinet_gl-b3000=y

# the runtime; depends on kmod-qca-dwmac-nss, kmod-qca-nss-drv,
# kmod-qca-nss-ecm, kmod-qca-nss-drv-vlan-mgr, kmod-qca8337-nss
CONFIG_PACKAGE_nss-tools-dwmac=y

# firmware: 12.2-156 is the one that works on IPQ5018 (see below)
CONFIG_PACKAGE_nss-firmware=y
CONFIG_NSS_FIRMWARE_VERSION_12_2=y
CONFIG_NSS_MEM_PROFILE_MEDIUM=y

# qca-nss-drv features the plane uses
CONFIG_NSS_DRV_VLAN_ENABLE=y
CONFIG_NSS_DRV_IPV6_ENABLE=y
CONFIG_NSS_DRV_VIRT_IF_ENABLE=y

# PPPoE: the kernel side builds, but ECM only accelerates PPPoE when this
# connection manager is present - without it flows are tracked and never
# offloaded, with nothing in the log to say so.
CONFIG_PACKAGE_kmod-ppp=y
CONFIG_PACKAGE_kmod-pppoe=y
CONFIG_PACKAGE_kmod-qca-nss-drv-pppoe=y

# ath11k with the NSS patches applied. Not needed for the wired plane -
# Wi-Fi stays on the host path - but this is the validated combination.
CONFIG_PACKAGE_MAC80211_NSS_SUPPORT=y
CONFIG_ATH11K_NSS_SUPPORT=y
CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE=y
CONFIG_NSS_DRV_WIFI_EXT_VDEV_ENABLE=y
```

Do **not** override the ath11k firmware with files from a stock image. The
package's `WLAN.HK.2.7.0.1` is the one that runs stably here; the stock
`2.9.r4` blob makes the internal 2.4 GHz radio's Q6 assert (`PHY0M3`) about
thirteen minutes after the BSS comes up, on this driver, every time.

### Firmware: why 12.2-156

`NSS_FIRMWARE_VERSION_12_2` selects `NSS.FW.12.2-156-MP.R` from the same
tarball the feed already uses. It is the newest firmware published for IPQ5018
(May 2025, nine months after 12.5) and the only line that works here:

- **12.5-210-MP** ignores the host's TX checksum-generation flags - ICMP works,
  every TCP handshake leaves the wire with a bad checksum - and does not answer
  dynamic-interface (VAP) allocation. The glue's `fw_csum` parameter defaults
  to off because of this; leave it.
- **11.4-6** refuses VAP allocation.

## How the plane comes up

A plain reboot is stock OpenWrt on the host stack. The `nss` service
(`START=19`, before netifd) then does, in this order:

1. **Switch to trunk.** `qca8k` has done the hard bring-up (SerDes, clocks,
   uniphy) at boot; the service unbinds it and loads `qca8337-nss` with the VTU
   map from `nss.general.vtu`. From here the CPU port carries plain 802.1Q.
2. Loads `qca-dwmac-nss` and `qca-nss-drv`. Both are inert at this point:
   `qca-nss-drv`'s probe defers until a port is armed.
3. Loads ath11k on the host path (`nss_offload=0` unless
   `nss.general.wifi_offload=1`).
4. Starts `/usr/sbin/nss-dwmac-up` alongside netifd. That script waits until
   `eth0` is up, the `lan` interface is up *in netifd's own view*, and the
   bridge has set the promiscuous flag on the trunk; then it arms the firmware
   (`fw_mask` in debugfs), waits for `phys_if 1: started`, loads
   `qca-nss-vlan`, waits for the bridge, loads ECM with `front_end_selection=1`
   and sets `accel_delay_pkts=1`.

Three orderings that fail *silently* - each measured, each cost days - are the
reason for the waiting:

- **arming on a down interface**: the firmware starts, TX works, and not one
  ingress frame is ever delivered;
- **arming while netifd is still applying config**: netifd bounces the
  interface mid-takeover, and the promiscuous flag the bridge sets afterwards
  never reaches the hardware (the RX filter only reaches the GMAC while the
  host still owns it);
- **loading `qca-nss-vlan` before the arm**: the VLAN manager resolves the
  trunk's NSS interface number at `NETDEV_REGISTER` time; before the arm that
  number does not exist, registration fails silently (the driver's debug
  macros are compiled out at the default log level), and every tagged frame
  disappears. Loaded *after* the arm, its notifier replays `NETDEV_REGISTER`
  for the existing `eth0.<vid>` netdevs, so ordering against netifd stops
  mattering.

If the takeover fails, the script exits and the box keeps running on the host
path **with the same topology**. Every failed experiment during this work ended
with a reachable router; that property is worth more than it sounds.

### Topology

Fixed for both data paths: `eth0` is the switch trunk, netifd builds
`eth0.1` (lan) and `eth0.2` (wan) on it. On first boot a uci-defaults script
migrates an existing DSA-style network config once (`br-lan` ports →
`eth0.1`, `wan`/`wan6` device → `eth0.2`) and leaves a marker
(`nss.general.topology='vlan-trunk'`) so it never runs again.

### `uci` knobs (`/etc/config/nss`, section `general`)

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `0` = stay on the host stack (same topology, no firmware) |
| `wifi_offload` | `0` | `1` = load ath11k with `nss_offload=1`. **Investigation only** - see *Wi-Fi*. |
| `fw_mask` | `0x2` | bitmask of GMACs to hand to the firmware; bit N = GMAC N. Only GMAC1 is validated. |
| `vtu` | `1:0t,2u,3u;2:0t,1u` | VTU program for `qca8337-nss` (B3000 wiring - see *Porting*) |
| `switch_dev` | `90000.mdio-1:11` | the switch's MDIO device, unbound from `qca8k` before the re-arm |
| `switch_args` | *(empty)* | further `qca8337-nss` parameters, passed verbatim (`cpu_port=`, `ports=`, `wake_phys=`, `bus_via=`) |
| `fw_logbuf` | `256` | firmware log ring size, read at `/sys/kernel/debug/qca-nss-drv/logs` |

There is no runtime detach. `/etc/init.d/nss stop` prints how to disable the
plane across reboots; returning to the host path needs a reboot.

### Checking that it works

```sh
cat /sys/kernel/debug/qca-dwmac-nss/status        # phys_if 1: started dev=eth0 ...
logread -e nss                                    # "NSS wired plane + ECM up (Wi-Fi on the host path)"
cat /sys/kernel/debug/ecm/ecm_nss_ipv4/tcp_accelerated_count   # > 0 under traffic
grep -m1 ipv4_rx_pkts /sys/kernel/debug/qca-nss-drv/stats/ipv4  # climbing under traffic
```

Two things that mislead: the `eth0.1` / `eth0.2` **counters do not see
accelerated traffic** - they show the first packet of each flow and then stop
- and `ipv4_rx_byts` in the firmware stats counts **both directions** of a
flow, so read it as roughly double the useful throughput. `top` is the honest
gauge: idle stays above 90 % under a full-rate flow, softirq stays flat.

## Why DSA has to go

The port model on this SoC is the reverse of ipq807x: the NSS `phys_if` is the
GMAC, and the netdev is the switch trunk. The firmware parses 802.1Q natively
(dynamic interface type 17) but cannot parse the two-byte Atheros header that
DSA's `tag_qca` puts where the ethertype should be; and DSA user ports never
get an NSS interface number, so ECM would not try to accelerate them anyway.
On IPQ5018, **DSA user ports and ECM acceleration are mutually exclusive.**

If you are seeing `eth_rx_unknown_l3_protocol` counting most of your frames,
with `iface_count=0` and `accelerated_count=0`, on a QCA8337 board: this is
why, and no ECM patch will fix it.

What works instead: let `qca8k` do the bring-up, unbind it, and program the
fabric directly - MACs on, Atheros header off, VLANs in the VTU with the front
ports untagged in their VLAN and the CPU port carrying all of them tagged.
`qca8337-nss` does exactly that over raw paged MDIO (the protocol is copied from
`qca8k-8xxx.c`). It is not a probe-based driver: nothing binds automatically,
it reaches the switch through *named* MDIO devices given as parameters.

| Parameter | Default | Meaning |
|---|---|---|
| `bus_via` | `90000.mdio-1:01` | any MDIO device on the switch's bus, used to find the bus |
| `wake_phys` | `90000.mdio-1:00,…:01,…:02` | front-panel PHYs to power back up (qca8k's teardown leaves them in BMCR power-down) |
| `vlans` | *(empty = off)* | VTU program, `<vid>:<port><t|u>[,…][;…]`; a port listed `u` also gets the vid as PVID |
| `cpu_port` | `0` | the switch port wired to the SoC GMAC (forced 1G full duplex); `255` = none, for boards whose SoC link enters the switch through one of its PHYs and autonegotiates (AX6000, SPNMX56) |
| `ports` | `0x0f` | bitmask of switch ports to enable, CPU port included |
| `switch_fixup` | `1` | the MAC/header/flooding re-arm; `0` = only wake the PHYs |

Load it with `insmod`, not `modprobe`: kmodloader's `modprobe` drops module
parameters. The service does this for you.

## Porting to another IPQ5018 board

The board side is small. The parts, in order of effort:

1. **DTS**: add `#include "ipq5018-nss.dtsi"` to the board file. That is the
   entire diff between the B3000 DTS and a plain board. Boards already in the
   tree (Linksys MX2000 / MR5500 / MX5500, Xiaomi AX6000, …) need only this
   line to get the NSS node and the reserved memory.
2. **Which GMAC feeds the switch.** On the B3000 it is GMAC1 (`fw_mask=0x2`,
   glue parameter `fw_if=1`, `ifname=eth0`). Only GMAC1 has been armed here;
   GMAC0 (the internal GE PHY) has not been tried.
3. **The switch.** Find your MDIO device names with
   `ls /sys/bus/mdio_bus/devices/` and set `bus_via` / `wake_phys` from them;
   read the port wiring off the `ethernet-switch` node in your DTS and write
   `cpu_port`, `ports` and the `vlans` map from it. All of it goes into uci -
   `nss.general.vtu` for the VTU map, `nss.general.switch_args` for the rest,
   `nss.general.switch_dev` if the switch is not at `90000.mdio-1:11`.

   Two worked examples straight from the DTS files in this tree (untested on
   the boards themselves - the defaults are the only wiring validated here):

   | Board | CPU port | wan | lan | `switch_args` | `vtu` |
   |---|:---:|---|---|---|---|
   | GL-B3000 (default) | 0 | port 1 / PHY 0 | ports 2-3 / PHY 1-2 | *(none)* | `1:0t,2u,3u;2:0t,1u` |
   | Linksys MX2000 | 6 | port 2 / PHY 1 | ports 3-5 / PHY 2-4 | `cpu_port=6 ports=0x7c wake_phys=90000.mdio-1:01,90000.mdio-1:02,90000.mdio-1:03,90000.mdio-1:04` | `1:6t,3u,4u,5u;2:6t,2u` |
   | Linksys MR5500 | 6 | port 5 / PHY 4 | ports 1-4 / PHY 0-3 | `cpu_port=6 ports=0x7e wake_phys=90000.mdio-1:00,90000.mdio-1:01,90000.mdio-1:02,90000.mdio-1:03,90000.mdio-1:04` | `1:6t,1u,2u,3u,4u;2:6t,5u` |

   A board whose CPU port is not 0 was the one thing the fabric re-arm could
   not do until the `cpu_port` / `ports` parameters; with them the module
   carries no board assumption of its own any more.

Also: a board whose WAN is on the internal GE PHY (GMAC0), like the D50, needs
no VTU at all - the switch only carries LANs, `vlans` can stay empty - but then
the network-config migration script (which assumes `eth0.1`/`eth0.2` on one
trunk) does not apply as-is, and arming GMAC0 is untested.

## What is accelerated

Legend as in the [IPQ807x README](README.md): ✅ offloaded & validated ·
🟨 in code, not validated here · ⬜ not carried · ❌ not available.

| Feature | IPQ5018 | Notes |
|---|:---:|---|
| IPv4 NAT / routing | ✅ | ECM; ~900 Mbit/s at the single-CPU-port ceiling, host >90 % idle |
| IPv6 routing | 🟨 | built (`NSS_DRV_IPV6_ENABLE`), not measured |
| 802.1Q VLAN | ✅ | the trunk itself; `qca-nss-vlan` |
| L2 between LAN ports | ✅ | in the switch fabric (same VLAN), never reaches the SoC |
| PPPoE | 🟨 | Builds and links now: kernel patch `0961` gained the lockless `__ppp_hold_channels()` / `__ppp_is_multilink()` that ECM's deadlock fix needs, and `kmod-qca-nss-drv-pppoe` is selected - without that manager ECM tracks PPPoE flows but silently never accelerates them (found by AugustoAmaral, who measured ~950 Mbit/s at 84-95 % idle on an AX6000 once it was in). Not measured here - no PPPoE uplink on this bench. |
| Wi-Fi (wifili) | ❌ | firmware bug, see below; Wi-Fi runs on the host path |
| SQM / NSS qdiscs | ⬜ | not carried for ipq50xx |
| Multicast snooping (`qca-mcs`) | ⬜ | not carried for ipq50xx |
| MAP-T / DS-Lite | 🟨 | `kmod-nat46` staging from the base; untested here |

### The single CPU port ceiling

Every routed flow crosses the trunk twice - in on one VLAN, out on the other -
and the data of one direction shares the wire with the ACKs of the other. With
1500-byte MTU that is `2×1448 / (2×1542 + 94) ≈ 91 %` of 1 GbE: **~911 Mbit/s**
is the most a single-CPU-port board can route, however idle the CPU is. That
matches the measurements, and it is a property of the board layout, not of the
offload.

### Wi-Fi

The wifili data path of the MP firmware line dies on the first RX exception
from an authorized peer: the UBI32 core traps at image address `0x40004918`
and the whole firmware→host direction stops. Ninety-two ath11k NSS patches and
twenty-two investigation knobs later, that is a firmware fault, not a host one -
none of the host-side variants avoids it. So `wifi_offload` defaults to `0`,
ath11k loads on the host path, the wifili node is never registered, and the
firmware never touches the broken path. Wired acceleration is unaffected.

The one host-side lead not yet closed: the stock `qca-wifi` driver sends
`PEER_UPDATE_AUTH_FLAG` from the port-authorized path *after* the 4-way
handshake, while ath11k sends it from `set_key`, ~40 ms after peer create. Real
divergence, unproven cause.

## Two firmware-boot fixes worth knowing about (feed patches)

- **`0136` - park the core before copying the firmware over it.** Warm
  reboots used to leave the NSS core dead one time in two: the old firmware
  was still executing while the new one was copied over it. Holding the core
  in reset around the copy fixed it (4/4 warm reboots). ADCDS's AX3000T port
  documents the same symptom as unresolved; this is the fix.
- **`0137` - map the meminfo block table non-cacheable** (`ioremap_wc`). Found
  first by Adriel Santos for the AX3000T port; carried here with credit.

## Acknowledgements

- [Julius Bairaktaris](https://github.com/JuliusBairaktaris) - the tree this is
  layered on, and the NSS-on-upstream-drivers idea itself.
- [ADCDS](https://github.com/ADCDS/openwrt-xiaomi-ax3000t-rd03v2) - the first
  NSS on IPQ5018 (AX3000T, `qca-nss-dp`, 6.12), and patch `0137`.
- [George Moussalem](https://github.com/georgemoussalem) - the ipq50xx target
  and the upstream stmmac/uniphy conversion this sits on.
- [qosmio](https://github.com/qosmio/openwrt-ipq) - packaging and the firmware
  tarballs.
- MayorBug (Cudy P5), AugustoAmaral (AX6000), Wallys (DR5018S) - the CMN PLL
  test matrix; LS3434 (Exigo D50) - first external user of `qca8337-nss`.

Much of the investigation behind this branch was done with an AI assistant
(Claude) in the loop; every measurement in it was taken on real hardware.
