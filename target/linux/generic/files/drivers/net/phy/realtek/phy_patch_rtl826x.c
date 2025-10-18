#include "realtek.h"
#include <linux/firmware.h>

#define PHY_PATCH_WAIT_TIMEOUT     10000000

static u16 _phy_rtl826x_mmd_convert(u16 page, u16 addr)
{
    u16 reg = 0;
    if (addr < 16)
    {
        reg = 0xA400 + (page * 2);
    }
    else if (addr < 24)
    {
        reg = (16 * page) + ((addr - 16) * 2);
    }
    else
    {
        reg = 0xA430 + ((addr - 24) * 2);
    }
    return reg;
}

static int _phy_patch_rtl826x_wait(struct phy_device *phydev, u32 mmdAddr, u32 mmdReg, u16 data, u16 mask)
{
    int rData = 0, us_diff;
    struct timespec64 start, now;

    ktime_get_real_ts64(&start);

    do
    {
        if ((rData = phy_read_mmd(phydev, mmdAddr, mmdReg)) < 0)
            return rData;

        if ((rData & mask) == data)
            break;

        mdelay(1);

        ktime_get_real_ts64(&now);
        us_diff = (now.tv_sec - start.tv_sec) * USEC_PER_SEC +
                  (now.tv_nsec - start.tv_nsec) / NSEC_PER_USEC;
    } while (us_diff < PHY_PATCH_WAIT_TIMEOUT);

    if (us_diff > PHY_PATCH_WAIT_TIMEOUT)
    {
        phydev_err(phydev, "826XB patch wait[%u,0x%X,0x%X,0x%X]:0x%X\n", mmdAddr, mmdReg, data, mask, rData);
        return -ETIME;
    }

    return 0;
}

static int _phy_patch_rtl826x_wait_not_equal(struct phy_device *phydev, u32 mmdAddr, u32 mmdReg, u16 data, u16 mask)
{
    int rData = 0, us_diff;
    struct timespec64 start, now;

    ktime_get_real_ts64(&start);

    do
    {
        if ((rData = phy_read_mmd(phydev, mmdAddr, mmdReg)) < 0)
            return rData;

        if ((rData & mask) != data)
            break;

        mdelay(1);

        ktime_get_real_ts64(&now);
        us_diff = (now.tv_sec - start.tv_sec) * USEC_PER_SEC +
                  (now.tv_nsec - start.tv_nsec) / NSEC_PER_USEC;
    } while (us_diff < PHY_PATCH_WAIT_TIMEOUT);

    if (us_diff > PHY_PATCH_WAIT_TIMEOUT)
    {
        phydev_err(phydev, "826xb patch wait[%u,0x%X,0x%X,0x%X]:0x%X\n", mmdAddr, mmdReg, data, mask, rData);
        return -ETIME;
    }

    return 0;
}

static int _phy_patch_rtl826x_sds_get(struct phy_device *phydev, u32 sdsPage, u32 sdsReg)
{
    int ret = 0;
    u32 sdsAddr = 0x8000 + (sdsReg << 6) + sdsPage;

    if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x143, sdsAddr)) < 0)
        return ret;

    if ((ret = _phy_patch_rtl826x_wait(phydev, MDIO_MMD_VEND1, 0x143, 0, BIT(15))) < 0)
        return ret;

    return phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x142);
}

static int _phy_patch_rtl826x_sds_set(struct phy_device *phydev, u32 sdsPage, u32 sdsReg, u32 wData)
{
    int ret = 0;
    u32 sdsAddr = 0x8800 + (sdsReg << 6) + sdsPage;

    if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x141, wData)) < 0)
        return ret;

    if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x143, sdsAddr)) < 0)
        return ret;

    return _phy_patch_rtl826x_wait(phydev, MDIO_MMD_VEND1, 0x143, 0, BIT(15));
}

static int _phy_patch_rtl826x_sds_modify(struct phy_device *phydev, u32 sdsPage, u32 sdsReg, u32 mask, u32 wData)
{
    int ret = 0;

    if (mask != GENMASK(15, 0))
    {
        if ((ret = _phy_patch_rtl826x_sds_get(phydev, sdsPage, sdsReg)) < 0)
            return ret;
    }

    return _phy_patch_rtl826x_sds_set(phydev, sdsPage, sdsReg, (ret & ~mask) | wData);
}

static int phy_patch_rtl826x_op(struct phy_device *phydev, rtk_hwpatch_t *pPatch_data)
{
    int ret = 0, rData, cnt;
    u16 reg = 0;
    u32 mask = GENMASK(pPatch_data->msb, pPatch_data->lsb);

    phydev_dbg(phydev, "patch_op: %u\n", pPatch_data->patch_op);

    switch (pPatch_data->patch_op)
    {
        case RTK_PATCH_OP_PHY:
            reg = _phy_rtl826x_mmd_convert(pPatch_data->pagemmd, pPatch_data->addr);
            if (mask != GENMASK(15, 0))
            {
                if ((ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, reg, mask, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            else
            {
                if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, reg, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            break;

        case RTK_PATCH_OP_PHY_WAIT:
            reg = _phy_rtl826x_mmd_convert(pPatch_data->pagemmd, pPatch_data->addr);
            if ((ret = _phy_patch_rtl826x_wait(phydev, MDIO_MMD_VEND2, reg, pPatch_data->data << pPatch_data->lsb, mask)) < 0)
                return ret;
            break;

        case RTK_PATCH_OP_PHY_WAIT_NOT:
            reg = _phy_rtl826x_mmd_convert(pPatch_data->pagemmd, pPatch_data->addr);
            if ((ret = _phy_patch_rtl826x_wait_not_equal(phydev, MDIO_MMD_VEND2, reg, pPatch_data->data << pPatch_data->lsb, mask)) < 0)
                return ret;
            break;

        case RTK_PATCH_OP_PHYOCP:
            if (mask != GENMASK(15, 0))
            {
                if ((ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, pPatch_data->addr, mask, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            else
            {
                if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, pPatch_data->addr, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            break;

        case RTK_PATCH_OP_PHYOCP_BC62:
            if ((rData = phy_read_mmd(phydev, MDIO_MMD_VEND2, 0xbc62)) < 0)
                return rData;

            rData = (rData >> 8) & 0x1f;
            for (cnt = 0; cnt <= rData; cnt++) {
                if ((ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0xbc62, GENMASK(12, 8), cnt << 8)) < 0)
                    return ret;
            }
            break;

        case RTK_PATCH_OP_TOP:
            reg = (pPatch_data->pagemmd * 8) + (pPatch_data->addr - 16);
            if (mask != GENMASK(15, 0))
            {
                if ((ret = phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg, mask, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            else
            {
                if ((ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, reg, pPatch_data->data << pPatch_data->lsb)) < 0)
                    return ret;
            }
            break;

        case RTK_PATCH_OP_PSDS0:
            if ((ret = _phy_patch_rtl826x_sds_modify(phydev, pPatch_data->pagemmd, pPatch_data->addr, mask, pPatch_data->data << pPatch_data->lsb)) < 0)
                return ret;
            break;

        case RTK_PATCH_OP_DELAY_MS:
            mdelay(pPatch_data->data);
            break;

        default:
            phydev_err(phydev, "patch_op:%u not implemented yet!\n", pPatch_data->patch_op);
            return -EINVAL;
    }

    return ret;
}

int phy_patch_rtl8264b_db_init(struct phy_device *phydev)
{
    int ret = 0;
    struct device *dev = &phydev->mdio.dev;
    struct rtl826x_priv *priv = phydev->priv;
    rt_phy_patch_db_t *patch_db = NULL;
    const struct firmware *fw = NULL;

    ret = request_firmware(&fw, "rtl8264b.bin", dev);
    if (ret)
        goto out;

    patch_db = kmalloc(sizeof(rt_phy_patch_db_t), GFP_ATOMIC);
    if (!patch_db)
    {
        ret = -ENOMEM;
        goto out;
    }

    memset(patch_db, 0x0, sizeof(rt_phy_patch_db_t));

    patch_db->fPatch_op = phy_patch_rtl826x_op;

    patch_db->size = fw->size;
    if (priv->patch_rtk_serdes)
        patch_db->size += 2 * sizeof(rtk_hwpatch_t);

    patch_db->conf = kmalloc(patch_db->size, GFP_ATOMIC);
    if (!patch_db->conf)
    {
        ret = -ENOMEM;
        goto out;
    }

    memcpy(patch_db->conf, fw->data, fw->size);

    if (priv->patch_rtk_serdes)
    {
        patch_db->conf[fw->size].patch_op = RTK_PATCH_OP_PSDS0;
        patch_db->conf[fw->size].portmask = 0xff;
        patch_db->conf[fw->size].pagemmd = 0x07;
        patch_db->conf[fw->size].addr = 0x10;
        patch_db->conf[fw->size].msb = 15;
        patch_db->conf[fw->size].lsb = 0;
        patch_db->conf[fw->size].data = 0x80aa;

        patch_db->conf[fw->size + 1].patch_op = RTK_PATCH_OP_PSDS0;
        patch_db->conf[fw->size + 1].portmask = 0xff;
        patch_db->conf[fw->size + 1].pagemmd = 0x06;
        patch_db->conf[fw->size + 1].addr = 0x12;
        patch_db->conf[fw->size + 1].msb = 15;
        patch_db->conf[fw->size + 1].lsb = 0;
        patch_db->conf[fw->size + 1].data = 0x5078;
    }

    priv->patch = patch_db;

out:
    if (fw && !IS_ERR(fw))
        release_firmware(fw);

    return ret;
}

int phy_patch_rtl8261n_db_init(struct phy_device *phydev)
{
    int ret = 0;
    struct device *dev = &phydev->mdio.dev;
    struct rtl826x_priv *priv = phydev->priv;
    rt_phy_patch_db_t *patch_db = NULL;
    const struct firmware *fw = NULL;

    ret = request_firmware(&fw, "rtl8261n.bin", dev);
    if (ret)
        goto out;

    patch_db = kmalloc(sizeof(rt_phy_patch_db_t), GFP_ATOMIC);
    if (!patch_db)
    {
        ret = -ENOMEM;
        goto out;
    }

    memset(patch_db, 0x0, sizeof(rt_phy_patch_db_t));

    patch_db->fPatch_op = phy_patch_rtl826x_op;

    patch_db->size = fw->size;
    if (priv->patch_rtk_serdes)
        patch_db->size += 2 * sizeof(rtk_hwpatch_t);

    patch_db->conf = kmalloc(patch_db->size, GFP_ATOMIC);
    if (!patch_db->conf)
    {
        ret = -ENOMEM;
        goto out;
    }

    memcpy(patch_db->conf, fw->data, fw->size);

    if (priv->patch_rtk_serdes)
    {
        patch_db->conf[fw->size].patch_op = RTK_PATCH_OP_PSDS0;
        patch_db->conf[fw->size].portmask = 0xff;
        patch_db->conf[fw->size].pagemmd = 0x07;
        patch_db->conf[fw->size].addr = 0x10;
        patch_db->conf[fw->size].msb = 15;
        patch_db->conf[fw->size].lsb = 0;
        patch_db->conf[fw->size].data = 0x80aa;

        patch_db->conf[fw->size + 1].patch_op = RTK_PATCH_OP_PSDS0;
        patch_db->conf[fw->size + 1].portmask = 0xff;
        patch_db->conf[fw->size + 1].pagemmd = 0x06;
        patch_db->conf[fw->size + 1].addr = 0x12;
        patch_db->conf[fw->size + 1].msb = 15;
        patch_db->conf[fw->size + 1].lsb = 0;
        patch_db->conf[fw->size + 1].data = 0x5078;
    }

    priv->patch = patch_db;

out:
    if (fw && !IS_ERR(fw))
        release_firmware(fw);

    return ret;
}
