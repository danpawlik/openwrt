#ifndef __PHY_PATCH_H__
#define __PHY_PATCH_H__

#include <linux/types.h>
#include <linux/phy.h>

typedef enum rtk_phypatch_op_e
{
    RTK_PATCH_OP_PHY,
    RTK_PATCH_OP_PHY_WAIT,
    RTK_PATCH_OP_PHY_WAIT_NOT,
    RTK_PATCH_OP_PHYOCP,
    RTK_PATCH_OP_PHYOCP_BC62,
    RTK_PATCH_OP_TOP,
    RTK_PATCH_OP_TOPOCP,
    RTK_PATCH_OP_PSDS0,
    RTK_PATCH_OP_PSDS1,
    RTK_PATCH_OP_MSDS,
    RTK_PATCH_OP_MAC,
    RTK_PATCH_OP_DELAY_MS
} rtk_phypatch_op_t;

typedef struct rtk_hwpatch_s
{
    u8 patch_op;
    u8 portmask;
    u16 pagemmd;
    u16 addr;
    u8 msb;
    u8 lsb;
    u16 data;
} __attribute__((packed)) rtk_hwpatch_t;

typedef struct rt_phy_patch_db_s
{
    int (*fPatch_op)(struct phy_device *phydev, rtk_hwpatch_t *pPatch_data);
    rtk_hwpatch_t *conf;
    u32 size;
} rt_phy_patch_db_t;

int phy_patch(struct phy_device *phydev);

#endif
