#include "realtek.h"

int phy_patch(struct phy_device *phydev)
{
    struct rtl826x_priv *priv = phydev->priv;
    rt_phy_patch_db_t *pPatchDb = priv->patch;
    int ret = 0, i = 0;

    if ((pPatchDb == NULL) || (pPatchDb->fPatch_op == NULL))
    {
        phydev_err(phydev, "phy_patch, db is NULL\n");
        return -EINVAL;
    }

    for (i = 0; i < pPatchDb->size / sizeof(rtk_hwpatch_t); i++)
    {
        ret = pPatchDb->fPatch_op(phydev, &pPatchDb->conf[i]);
        if (ret < 0)
        {
            phydev_err(phydev, "%s failed! %u[%u][0x%X][0x%X][0x%X] ret=0x%X\n", __FUNCTION__,
                       i + 1, pPatchDb->conf[i].patch_op, pPatchDb->conf[i].pagemmd, pPatchDb->conf[i].addr, pPatchDb->conf[i].data, ret);
            return ret;
        }
    }

    return 0;
}
