/* SPDX-License-Identifier: GPL-2.0 */
/* Texas Instruments ICSSG Ethernet driver
 *
 * Copyright (C) 2018-2022 Texas Instruments Incorporated - https://www.ti.com/
 *
 */

#ifndef __NET_TI_ICSSG_PRUETH_H
#define __NET_TI_ICSSG_PRUETH_H

#include <linux/bpf.h>
#include <linux/bpf_trace.h>
#include <linux/etherdevice.h>
#include <linux/genalloc.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net_tstamp.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/remoteproc/pruss.h>
#include <linux/pruss_driver.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/remoteproc.h>

#include <linux/dma-mapping.h>
#include <linux/dma/ti-cppi5.h>
#include <linux/dma/k3-udma-glue.h>

#include <net/devlink.h>
#include <net/page_pool.h>

#include "icssg_config.h"
#include "icss_iep.h"
#include "icssg_switch_map.h"
#include "icssg_qos.h"

#define ICSS_SLICE0	0
#define ICSS_SLICE1	1

#define ICSS_FW_PRU	0
#define ICSS_FW_RTU	1

#define ICSSG_MAX_RFLOWS	8	/* per slice */

/* Number of ICSSG related stats */
#define ICSSG_NUM_STATS 70

/* Firmware status codes */
#define ICSS_HS_FW_READY 0x55555555
#define ICSS_HS_FW_DEAD 0xDEAD0000	/* lower 16 bits contain error code */

/* Firmware command codes */
#define ICSS_HS_CMD_BUSY 0x40000000
#define ICSS_HS_CMD_DONE 0x80000000
#define ICSS_HS_CMD_CANCEL 0x10000000

/* Firmware commands */
#define ICSS_CMD_SPAD 0x20
#define ICSS_CMD_RXTX 0x10
#define ICSS_CMD_ADD_FDB 0x1
#define ICSS_CMD_DEL_FDB 0x2
#define ICSS_CMD_ERASE_FDB 0x3
#define ICSS_CMD_SET_RUN 0x4
#define ICSS_CMD_GET_FDB_SLOT 0x5
#define ICSS_CMD_ENABLE_VLAN 0x5
#define ICSS_CMD_DISABLE_VLAN 0x6
#define ICSS_CMD_ADD_FILTER 0x7
#define ICSS_CMD_ADD_MAC 0x8

/* ICSSG IET STATS Base Address */
#define ICSSG_IET_STATS_BASE 0x180

/* In switch mode there are 3 real ports i.e. 3 mac addrs.
 * however Linux sees only the host side port. The other 2 ports
 * are the switch ports.
 * In emac mode there are 2 real ports i.e. 2 mac addrs.
 * Linux sees both the ports.
 */
enum prueth_port {
	PRUETH_PORT_HOST = 0,	/* host side port */
	PRUETH_PORT_MII0,	/* physical port RG/SG MII 0 */
	PRUETH_PORT_MII1,	/* physical port RG/SG MII 1 */
};

enum prueth_mac {
	PRUETH_MAC0 = 0,
	PRUETH_MAC1,
	PRUETH_NUM_MACS,
};

struct prueth_tx_chn {
	struct device *dma_dev;
	struct napi_struct napi_tx;
	struct k3_cppi_desc_pool *desc_pool;
	struct k3_udma_glue_tx_channel *tx_chn;
	struct prueth_emac *emac;
	struct hrtimer tx_hrtimer;
	unsigned long tx_pace_timeout_ns;
	u32 id;
	u32 descs_num;
	unsigned int irq;
	char name[32];
};

struct prueth_rx_chn {
	struct device *dev;
	struct device *dma_dev;
	struct k3_cppi_desc_pool *desc_pool;
	struct k3_udma_glue_rx_channel *rx_chn;
	u32 descs_num;
	unsigned int irq[ICSSG_MAX_RFLOWS];	/* separate irq per flow */
	char name[32];
	struct page_pool *pg_pool;
	struct xdp_rxq_info xdp_rxq;
};

enum prueth_devlink_param_id {
	PRUETH_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	PRUETH_DL_PARAM_SWITCH_MODE,
	PRUETH_DL_PARAM_HSR_OFFLOAD_MODE,
	PRUETH_DL_PARAM_CUT_THRU_EN,
};

struct prueth_devlink {
	struct prueth *prueth;
};

enum prueth_swdata_type {
	PRUETH_SWDATA_INVALID = 0,
	PRUETH_SWDATA_SKB,
	PRUETH_SWDATA_PAGE,
	PRUETH_SWDATA_CMD,
	PRUETH_SWDATA_XDPF,
};

union prueth_data {
	struct sk_buff *skb;
	struct page *page;
	u32 cmd;
	struct xdp_frame *xdpf;
};

struct prueth_swdata {
	union prueth_data data;
	struct prueth_rx_chn *rx_chn;
	enum prueth_swdata_type type;
};

/* There are 4 Tx DMA channels, but the highest priority is CH3 (thread 3)
 * and lower three are lower priority channels or threads.
 */
#define PRUETH_MAX_TX_QUEUES	4

#define PRUETH_MAX_TX_TS_REQUESTS	50	/* Max simultaneous TX_TS requests */

/* XDP BPF state */
#define ICSSG_XDP_PASS           0
#define ICSSG_XDP_CONSUMED       BIT(0)
#define ICSSG_XDP_TX             BIT(1)
#define ICSSG_XDP_REDIR          BIT(2)

/* Minimum coalesce time in usecs for both Tx and Rx */
#define ICSSG_MIN_COALESCE_USECS 20

/* data for each emac port */
struct prueth_emac {
	bool fw_running;
	struct prueth *prueth;
	struct net_device *ndev;
	u8 mac_addr[6];
	struct napi_struct napi_rx;
	u32 msg_enable;

	int link;
	int speed;
	int duplex;

	const char *phy_id;
	struct device_node *phy_node;
	phy_interface_t phy_if;
	enum prueth_port port_id;
	struct icss_iep *iep;
	unsigned int rx_ts_enabled : 1;
	unsigned int tx_ts_enabled : 1;
	unsigned int half_duplex : 1;

	/* DMA related */
	struct prueth_tx_chn tx_chns[PRUETH_MAX_TX_QUEUES];
	struct completion tdown_complete;
	atomic_t tdown_cnt;
	struct prueth_rx_chn rx_chns;
	int rx_flow_id_base;
	int tx_ch_num;

	/* Interrput Pacing Related */
	struct hrtimer rx_hrtimer;
	unsigned long rx_pace_timeout_ns;

	spinlock_t lock;	/* serialize access */

	/* TX HW Timestamping */
	/* TX TS cookie will be index to the tx_ts_skb array */
	struct sk_buff *tx_ts_skb[PRUETH_MAX_TX_TS_REQUESTS];
	atomic_t tx_ts_pending;
	int tx_ts_irq;

	u8 cmd_seq;
	/* shutdown related */
	u32 cmd_data[4];
	struct completion cmd_complete;
	/* Mutex to serialize access to firmware command interface */
	struct mutex cmd_lock;
	struct work_struct rx_mode_work;
	struct workqueue_struct	*cmd_wq;

	struct pruss_mem_region dram;

	u8 cut_thru_queue_map;
	bool offload_fwd_mark;
	struct devlink_port devlink_port;
	int port_vlan;

	struct prueth_qos qos;
	struct work_struct ts_work;
	struct delayed_work stats_work;
	u64 stats[ICSSG_NUM_STATS];

	struct bpf_prog *xdp_prog;
	struct xdp_attachment_info xdpi;
};

/* The buf includes headroom compatible with both skb and xdpf */
#define PRUETH_HEADROOM_NA (max(XDP_PACKET_HEADROOM, NET_SKB_PAD) + NET_IP_ALIGN)
#define PRUETH_HEADROOM  ALIGN(PRUETH_HEADROOM_NA, sizeof(long))

/**
 * struct prueth_pdata - PRUeth platform data
 * @fdqring_mode: Free desc queue mode
 * @quirk_10m_link_issue: 10M link detect errata
 * @switch_mode: switch firmware support
 */
struct prueth_pdata {
	enum k3_ring_mode fdqring_mode;
	u32	quirk_10m_link_issue:1;
	u32	switch_mode:1;
};

/**
 * struct prueth - PRUeth structure
 * @dev: device
 * @pruss: pruss handle
 * @pru: rproc instances of PRUs
 * @rtu: rproc instances of RTUs
 * @txpru: rproc instances of TX_PRUs
 * @shram: PRUSS shared RAM region
 * @sram_pool: MSMC RAM pool for buffers
 * @msmcram: MSMC RAM region
 * @eth_node: DT node for the port
 * @emac: private EMAC data structure
 * @registered_netdevs: list of registered netdevs
 * @fw_data: firmware names to be used with PRU remoteprocs
 * @config: firmware load time configuration per slice
 * @miig_rt: regmap to mii_g_rt block
 * @mii_rt: regmap to mii_g_rt block
 * @pa_stats: regmap to pa_stats block
 * @pru_id: ID for each of the PRUs
 * @pdev: pointer to ICSSG platform device
 * @pdata: pointer to platform data for ICSSG driver
 * @icssg_hwcmdseq: seq counter or HWQ messages
 * @num_emacs_initialized: num of EMACs/ext ports that are up/running
 * @iep0: pointer to IEP0 device
 * @iep1: pointer to IEP1 device
 * @vlan_tbl: VLAN-FID table pointer
 * @hw_bridge_dev: pointer to HW bridge net device
 * @hsr_dev: pointer to the HSR net device
 * @br_members: bitmask of bridge member ports
 * @prueth_netdevice_nb: netdevice notifier block
 * @prueth_switchdevice_nb: switchdev notifier block
 * @prueth_switchdev_bl_nb: switchdev blocking notifier block
 * @is_switch_mode: flag to indicate if device is in Switch mode
 * @is_switchmode_supported: indicates platform support for switch mode
 * @switch_id: ID for mapping switch ports to bridge
 * @default_vlan: Default VLAN for host
 * @devlink: pointer to devlink
 */
struct prueth {
	struct device *dev;
	struct pruss *pruss;
	struct rproc *pru[PRUSS_NUM_PRUS];
	struct rproc *rtu[PRUSS_NUM_PRUS];
	struct rproc *txpru[PRUSS_NUM_PRUS];
	struct pruss_mem_region shram;
	struct gen_pool *sram_pool;
	struct pruss_mem_region msmcram;

	struct device_node *eth_node[PRUETH_NUM_MACS];
	struct prueth_emac *emac[PRUETH_NUM_MACS];
	struct net_device *registered_netdevs[PRUETH_NUM_MACS];
	const struct prueth_private_data *fw_data;
	struct regmap *miig_rt;
	struct regmap *mii_rt;
	struct regmap *pa_stats;

	enum pruss_pru_id pru_id[PRUSS_NUM_PRUS];
	struct platform_device *pdev;
	struct prueth_pdata pdata;
	u8 icssg_hwcmdseq;
	int num_emacs_initialized;
	struct icss_iep *iep0;
	struct icss_iep *iep1;
	struct prueth_vlan_tbl *vlan_tbl;

	struct net_device *hw_bridge_dev;
	struct net_device *hsr_dev;
	u8 br_members;
	struct notifier_block prueth_netdevice_nb;
	struct notifier_block prueth_switchdev_nb;
	struct notifier_block prueth_switchdev_bl_nb;
	bool is_switch_mode;
	bool is_hsr_offload_mode;
	bool is_switchmode_supported;
	unsigned char switch_id[MAX_PHYS_ITEM_ID_LEN];
	int default_vlan;
	struct devlink *devlink;
	spinlock_t vtbl_lock; /* Lock for vtbl in shared memory */
};

struct emac_tx_ts_response {
	u32 reserved[2];
	u32 cookie;
	u32 lo_ts;
	u32 hi_ts;
};

/* Classifier helpers */
void icssg_class_set_mac_addr(struct regmap *miig_rt, int slice, u8 *mac);
void icssg_class_set_host_mac_addr(struct regmap *miig_rt, const u8 *mac);
void icssg_class_disable(struct regmap *miig_rt, int slice);
void icssg_class_default(struct regmap *miig_rt, int slice, bool allmulti);
void icssg_ft1_set_mac_addr(struct regmap *miig_rt, int slice, u8 *mac_addr);

/* Buffer queue helpers */
int icssg_queue_pop(struct prueth *prueth, u8 queue);
void icssg_queue_push(struct prueth *prueth, int queue, u16 addr);
u32 icssg_queue_level(struct prueth *prueth, int queue);

/* get PRUSS SLICE number from prueth_emac */
static inline int prueth_emac_slice(struct prueth_emac *emac)
{
	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		return ICSS_SLICE0;
	case PRUETH_PORT_MII1:
		return ICSS_SLICE1;
	default:
		return -EINVAL;
	}
}

/* config helpers */
void icssg_config_ipg(struct prueth_emac *emac);
int icssg_config(struct prueth *prueth, struct prueth_emac *emac,
		 int slice);
int emac_set_port_state(struct prueth_emac *emac,
			enum icssg_port_state_cmd state);
void icssg_config_set_speed(struct prueth_emac *emac);
void icssg_config_half_duplex(struct prueth_emac *emac);

int icssg_send_fdb_msg(struct prueth_emac *emac, struct mgmt_cmd *cmd,
		       struct mgmt_cmd_rsp *rsp);
int icssg_fdb_add_del(struct prueth_emac *emac,  const unsigned char *addr,
		      u8 vid, u8 fid_c2, bool add);
int icssg_fdb_lookup(struct prueth_emac *emac, const unsigned char *addr,
		     u8 vid);
void icssg_vtbl_modify(struct prueth_emac *emac, u8 vid, u8 port_mask,
		       u8 untag_mask, bool add);
u16 icssg_get_pvid(struct prueth_emac *emac);
void icssg_set_pvid(struct prueth *prueth, u8 vid, u8 port);
int emac_fdb_erase_all(struct prueth_emac *emac);
int emac_fdb_flush_multicast(struct prueth_emac *emac);
int emac_fdb_flow_id_updated(struct prueth_emac *emac);
#define prueth_napi_to_tx_chn(pnapi) \
	container_of(pnapi, struct prueth_tx_chn, napi_tx)

u64 prueth_iep_gettime(void *clockops_data, struct ptp_system_timestamp *sts);
void emac_stats_work_handler(struct work_struct *work);
#endif /* __NET_TI_ICSSG_PRUETH_H */
