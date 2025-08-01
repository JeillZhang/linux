// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek MT7530 DSA Switch driver
 * Copyright (C) 2017 Sean Wang <sean.wang@mediatek.com>
 */
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <net/dsa.h>
#include <net/pkt_cls.h>

#include "mt7530.h"

static struct mt753x_pcs *pcs_to_mt753x_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct mt753x_pcs, pcs);
}

/* String, offset, and register size in bytes if different from 4 bytes */
static const struct mt7530_mib_desc mt7530_mib[] = {
	MIB_DESC(1, MT7530_PORT_MIB_TX_DROP, "TxDrop"),
	MIB_DESC(1, MT7530_PORT_MIB_TX_CRC_ERR, "TxCrcErr"),
	MIB_DESC(1, MT7530_PORT_MIB_TX_COLLISION, "TxCollision"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_DROP, "RxDrop"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_FILTERING, "RxFiltering"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_CRC_ERR, "RxCrcErr"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_CTRL_DROP, "RxCtrlDrop"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_INGRESS_DROP, "RxIngressDrop"),
	MIB_DESC(1, MT7530_PORT_MIB_RX_ARL_DROP, "RxArlDrop"),
};

static void
mt7530_mutex_lock(struct mt7530_priv *priv)
{
	if (priv->bus)
		mutex_lock_nested(&priv->bus->mdio_lock, MDIO_MUTEX_NESTED);
}

static void
mt7530_mutex_unlock(struct mt7530_priv *priv)
{
	if (priv->bus)
		mutex_unlock(&priv->bus->mdio_lock);
}

static void
core_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;
	int ret;

	mt7530_mutex_lock(priv);

	/* Write the desired MMD Devad */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_CTRL, MDIO_MMD_VEND2);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_DATA, reg);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_CTRL, MDIO_MMD_VEND2 | MII_MMD_CTRL_NOINCR);
	if (ret < 0)
		goto err;

	/* Write the data into MMD's selected register */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_DATA, val);
err:
	if (ret < 0)
		dev_err(&bus->dev, "failed to write mmd register\n");

	mt7530_mutex_unlock(priv);
}

static void
core_rmw(struct mt7530_priv *priv, u32 reg, u32 mask, u32 set)
{
	struct mii_bus *bus = priv->bus;
	u32 val;
	int ret;

	mt7530_mutex_lock(priv);

	/* Write the desired MMD Devad */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_CTRL, MDIO_MMD_VEND2);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_DATA, reg);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_CTRL, MDIO_MMD_VEND2 | MII_MMD_CTRL_NOINCR);
	if (ret < 0)
		goto err;

	/* Read the content of the MMD's selected register */
	val = bus->read(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			MII_MMD_DATA);
	val &= ~mask;
	val |= set;
	/* Write the data into MMD's selected register */
	ret = bus->write(bus, MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
			 MII_MMD_DATA, val);
err:
	if (ret < 0)
		dev_err(&bus->dev, "failed to write mmd register\n");

	mt7530_mutex_unlock(priv);
}

static void
core_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, 0, val);
}

static void
core_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, val, 0);
}

static int
mt7530_mii_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);

	if (ret < 0)
		dev_err(priv->dev,
			"failed to write mt7530 register\n");

	return ret;
}

static u32
mt7530_mii_read(struct mt7530_priv *priv, u32 reg)
{
	int ret;
	u32 val;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		WARN_ON_ONCE(1);
		dev_err(priv->dev,
			"failed to read mt7530 register\n");
		return 0;
	}

	return val;
}

static void
mt7530_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_mutex_lock(priv);

	mt7530_mii_write(priv, reg, val);

	mt7530_mutex_unlock(priv);
}

static u32
_mt7530_unlocked_read(struct mt7530_dummy_poll *p)
{
	return mt7530_mii_read(p->priv, p->reg);
}

static u32
_mt7530_read(struct mt7530_dummy_poll *p)
{
	u32 val;

	mt7530_mutex_lock(p->priv);

	val = mt7530_mii_read(p->priv, p->reg);

	mt7530_mutex_unlock(p->priv);

	return val;
}

static u32
mt7530_read(struct mt7530_priv *priv, u32 reg)
{
	struct mt7530_dummy_poll p;

	INIT_MT7530_DUMMY_POLL(&p, priv, reg);
	return _mt7530_read(&p);
}

static void
mt7530_rmw(struct mt7530_priv *priv, u32 reg,
	   u32 mask, u32 set)
{
	mt7530_mutex_lock(priv);

	regmap_update_bits(priv->regmap, reg, mask, set);

	mt7530_mutex_unlock(priv);
}

static void
mt7530_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, val, val);
}

static void
mt7530_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, val, 0);
}

static int
mt7530_fdb_cmd(struct mt7530_priv *priv, enum mt7530_fdb_cmd cmd, u32 *rsp)
{
	u32 val;
	int ret;
	struct mt7530_dummy_poll p;

	/* Set the command operating upon the MAC address entries */
	val = ATC_BUSY | ATC_MAT(0) | cmd;
	mt7530_write(priv, MT7530_ATC, val);

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_ATC);
	ret = readx_poll_timeout(_mt7530_read, &p, val,
				 !(val & ATC_BUSY), 20, 20000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	/* Additional sanity for read command if the specified
	 * entry is invalid
	 */
	val = mt7530_read(priv, MT7530_ATC);
	if ((cmd == MT7530_FDB_READ) && (val & ATC_INVALID))
		return -EINVAL;

	if (rsp)
		*rsp = val;

	return 0;
}

static void
mt7530_fdb_read(struct mt7530_priv *priv, struct mt7530_fdb *fdb)
{
	u32 reg[3];
	int i;

	/* Read from ARL table into an array */
	for (i = 0; i < 3; i++) {
		reg[i] = mt7530_read(priv, MT7530_TSRA1 + (i * 4));

		dev_dbg(priv->dev, "%s(%d) reg[%d]=0x%x\n",
			__func__, __LINE__, i, reg[i]);
	}

	fdb->vid = (reg[1] >> CVID) & CVID_MASK;
	fdb->aging = (reg[2] >> AGE_TIMER) & AGE_TIMER_MASK;
	fdb->port_mask = (reg[2] >> PORT_MAP) & PORT_MAP_MASK;
	fdb->mac[0] = (reg[0] >> MAC_BYTE_0) & MAC_BYTE_MASK;
	fdb->mac[1] = (reg[0] >> MAC_BYTE_1) & MAC_BYTE_MASK;
	fdb->mac[2] = (reg[0] >> MAC_BYTE_2) & MAC_BYTE_MASK;
	fdb->mac[3] = (reg[0] >> MAC_BYTE_3) & MAC_BYTE_MASK;
	fdb->mac[4] = (reg[1] >> MAC_BYTE_4) & MAC_BYTE_MASK;
	fdb->mac[5] = (reg[1] >> MAC_BYTE_5) & MAC_BYTE_MASK;
	fdb->noarp = ((reg[2] >> ENT_STATUS) & ENT_STATUS_MASK) == STATIC_ENT;
}

static void
mt7530_fdb_write(struct mt7530_priv *priv, u16 vid,
		 u8 port_mask, const u8 *mac,
		 u8 aging, u8 type)
{
	u32 reg[3] = { 0 };
	int i;

	reg[1] |= vid & CVID_MASK;
	reg[1] |= ATA2_IVL;
	reg[1] |= ATA2_FID(FID_BRIDGED);
	reg[2] |= (aging & AGE_TIMER_MASK) << AGE_TIMER;
	reg[2] |= (port_mask & PORT_MAP_MASK) << PORT_MAP;
	/* STATIC_ENT indicate that entry is static wouldn't
	 * be aged out and STATIC_EMP specified as erasing an
	 * entry
	 */
	reg[2] |= (type & ENT_STATUS_MASK) << ENT_STATUS;
	reg[1] |= mac[5] << MAC_BYTE_5;
	reg[1] |= mac[4] << MAC_BYTE_4;
	reg[0] |= mac[3] << MAC_BYTE_3;
	reg[0] |= mac[2] << MAC_BYTE_2;
	reg[0] |= mac[1] << MAC_BYTE_1;
	reg[0] |= mac[0] << MAC_BYTE_0;

	/* Write array into the ARL table */
	for (i = 0; i < 3; i++)
		mt7530_write(priv, MT7530_ATA1 + (i * 4), reg[i]);
}

/* Set up switch core clock for MT7530 */
static void mt7530_pll_setup(struct mt7530_priv *priv)
{
	/* Disable core clock */
	core_clear(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);

	/* Disable PLL */
	core_write(priv, CORE_GSWPLL_GRP1, 0);

	/* Set core clock into 500Mhz */
	core_write(priv, CORE_GSWPLL_GRP2,
		   RG_GSWPLL_POSDIV_500M(1) |
		   RG_GSWPLL_FBKDIV_500M(25));

	/* Enable PLL */
	core_write(priv, CORE_GSWPLL_GRP1,
		   RG_GSWPLL_EN_PRE |
		   RG_GSWPLL_POSDIV_200M(2) |
		   RG_GSWPLL_FBKDIV_200M(32));

	udelay(20);

	/* Enable core clock */
	core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);
}

/* If port 6 is available as a CPU port, always prefer that as the default,
 * otherwise don't care.
 */
static struct dsa_port *
mt753x_preferred_default_local_cpu_port(struct dsa_switch *ds)
{
	struct dsa_port *cpu_dp = dsa_to_port(ds, 6);

	if (dsa_port_is_cpu(cpu_dp))
		return cpu_dp;

	return NULL;
}

/* Setup port 6 interface mode and TRGMII TX circuit */
static void
mt7530_setup_port6(struct dsa_switch *ds, phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;
	u32 ncpo1, ssc_delta, xtal;

	/* Disable the MT7530 TRGMII clocks */
	core_clear(priv, CORE_TRGMII_GSW_CLK_CG, REG_TRGMIICK_EN);

	if (interface == PHY_INTERFACE_MODE_RGMII) {
		mt7530_rmw(priv, MT7530_P6ECR, P6_INTF_MODE_MASK,
			   P6_INTF_MODE(0));
		return;
	}

	mt7530_rmw(priv, MT7530_P6ECR, P6_INTF_MODE_MASK, P6_INTF_MODE(1));

	xtal = mt7530_read(priv, MT753X_MTRAP) & MT7530_XTAL_MASK;

	if (xtal == MT7530_XTAL_25MHZ)
		ssc_delta = 0x57;
	else
		ssc_delta = 0x87;

	if (priv->id == ID_MT7621) {
		/* PLL frequency: 125MHz: 1.0GBit */
		if (xtal == MT7530_XTAL_40MHZ)
			ncpo1 = 0x0640;
		if (xtal == MT7530_XTAL_25MHZ)
			ncpo1 = 0x0a00;
	} else { /* PLL frequency: 250MHz: 2.0Gbit */
		if (xtal == MT7530_XTAL_40MHZ)
			ncpo1 = 0x0c80;
		if (xtal == MT7530_XTAL_25MHZ)
			ncpo1 = 0x1400;
	}

	/* Setup the MT7530 TRGMII Tx Clock */
	core_write(priv, CORE_PLL_GROUP5, RG_LCDDS_PCW_NCPO1(ncpo1));
	core_write(priv, CORE_PLL_GROUP6, RG_LCDDS_PCW_NCPO0(0));
	core_write(priv, CORE_PLL_GROUP10, RG_LCDDS_SSC_DELTA(ssc_delta));
	core_write(priv, CORE_PLL_GROUP11, RG_LCDDS_SSC_DELTA1(ssc_delta));
	core_write(priv, CORE_PLL_GROUP4, RG_SYSPLL_DDSFBK_EN |
		   RG_SYSPLL_BIAS_EN | RG_SYSPLL_BIAS_LPF_EN);
	core_write(priv, CORE_PLL_GROUP2, RG_SYSPLL_EN_NORMAL |
		   RG_SYSPLL_VODEN | RG_SYSPLL_POSDIV(1));
	core_write(priv, CORE_PLL_GROUP7, RG_LCDDS_PCW_NCPO_CHG |
		   RG_LCCDS_C(3) | RG_LCDDS_PWDB | RG_LCDDS_ISO_EN);

	/* Enable the MT7530 TRGMII clocks */
	core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_TRGMIICK_EN);
}

static void
mt7531_pll_setup(struct mt7530_priv *priv)
{
	enum mt7531_xtal_fsel xtal;
	u32 top_sig;
	u32 hwstrap;
	u32 val;

	val = mt7530_read(priv, MT7531_CREV);
	top_sig = mt7530_read(priv, MT7531_TOP_SIG_SR);
	hwstrap = mt7530_read(priv, MT753X_TRAP);
	if ((val & CHIP_REV_M) > 0)
		xtal = (top_sig & PAD_MCM_SMI_EN) ? MT7531_XTAL_FSEL_40MHZ :
						    MT7531_XTAL_FSEL_25MHZ;
	else
		xtal = (hwstrap & MT7531_XTAL25) ? MT7531_XTAL_FSEL_25MHZ :
						   MT7531_XTAL_FSEL_40MHZ;

	/* Step 1 : Disable MT7531 COREPLL */
	val = mt7530_read(priv, MT7531_PLLGP_EN);
	val &= ~EN_COREPLL;
	mt7530_write(priv, MT7531_PLLGP_EN, val);

	/* Step 2: switch to XTAL output */
	val = mt7530_read(priv, MT7531_PLLGP_EN);
	val |= SW_CLKSW;
	mt7530_write(priv, MT7531_PLLGP_EN, val);

	val = mt7530_read(priv, MT7531_PLLGP_CR0);
	val &= ~RG_COREPLL_EN;
	mt7530_write(priv, MT7531_PLLGP_CR0, val);

	/* Step 3: disable PLLGP and enable program PLLGP */
	val = mt7530_read(priv, MT7531_PLLGP_EN);
	val |= SW_PLLGP;
	mt7530_write(priv, MT7531_PLLGP_EN, val);

	/* Step 4: program COREPLL output frequency to 500MHz */
	val = mt7530_read(priv, MT7531_PLLGP_CR0);
	val &= ~RG_COREPLL_POSDIV_M;
	val |= 2 << RG_COREPLL_POSDIV_S;
	mt7530_write(priv, MT7531_PLLGP_CR0, val);
	usleep_range(25, 35);

	switch (xtal) {
	case MT7531_XTAL_FSEL_25MHZ:
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_M;
		val |= 0x140000 << RG_COREPLL_SDM_PCW_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		break;
	case MT7531_XTAL_FSEL_40MHZ:
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_M;
		val |= 0x190000 << RG_COREPLL_SDM_PCW_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		break;
	}

	/* Set feedback divide ratio update signal to high */
	val = mt7530_read(priv, MT7531_PLLGP_CR0);
	val |= RG_COREPLL_SDM_PCW_CHG;
	mt7530_write(priv, MT7531_PLLGP_CR0, val);
	/* Wait for at least 16 XTAL clocks */
	usleep_range(10, 20);

	/* Step 5: set feedback divide ratio update signal to low */
	val = mt7530_read(priv, MT7531_PLLGP_CR0);
	val &= ~RG_COREPLL_SDM_PCW_CHG;
	mt7530_write(priv, MT7531_PLLGP_CR0, val);

	/* Enable 325M clock for SGMII */
	mt7530_write(priv, MT7531_ANA_PLLGP_CR5, 0xad0000);

	/* Enable 250SSC clock for RGMII */
	mt7530_write(priv, MT7531_ANA_PLLGP_CR2, 0x4f40000);

	/* Step 6: Enable MT7531 PLL */
	val = mt7530_read(priv, MT7531_PLLGP_CR0);
	val |= RG_COREPLL_EN;
	mt7530_write(priv, MT7531_PLLGP_CR0, val);

	val = mt7530_read(priv, MT7531_PLLGP_EN);
	val |= EN_COREPLL;
	mt7530_write(priv, MT7531_PLLGP_EN, val);
	usleep_range(25, 35);
}

static void
mt7530_mib_reset(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_FLUSH);
	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_ACTIVATE);
}

static int mt7530_phy_read_c22(struct mt7530_priv *priv, int port, int regnum)
{
	return mdiobus_read_nested(priv->bus, port, regnum);
}

static int mt7530_phy_write_c22(struct mt7530_priv *priv, int port, int regnum,
				u16 val)
{
	return mdiobus_write_nested(priv->bus, port, regnum, val);
}

static int mt7530_phy_read_c45(struct mt7530_priv *priv, int port,
			       int devad, int regnum)
{
	return mdiobus_c45_read_nested(priv->bus, port, devad, regnum);
}

static int mt7530_phy_write_c45(struct mt7530_priv *priv, int port, int devad,
				int regnum, u16 val)
{
	return mdiobus_c45_write_nested(priv->bus, port, devad, regnum, val);
}

static int
mt7531_ind_c45_phy_read(struct mt7530_priv *priv, int port, int devad,
			int regnum)
{
	struct mt7530_dummy_poll p;
	u32 reg, val;
	int ret;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mt7530_mutex_lock(priv);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MT7531_MDIO_CL45_ADDR | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_DEV_ADDR(devad) | regnum;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MT7531_MDIO_CL45_READ | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_DEV_ADDR(devad);
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	ret = val & MT7531_MDIO_RW_DATA_MASK;
out:
	mt7530_mutex_unlock(priv);

	return ret;
}

static int
mt7531_ind_c45_phy_write(struct mt7530_priv *priv, int port, int devad,
			 int regnum, u16 data)
{
	struct mt7530_dummy_poll p;
	u32 val, reg;
	int ret;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mt7530_mutex_lock(priv);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MT7531_MDIO_CL45_ADDR | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_DEV_ADDR(devad) | regnum;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MT7531_MDIO_CL45_WRITE | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_DEV_ADDR(devad) | data;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

out:
	mt7530_mutex_unlock(priv);

	return ret;
}

static int
mt7531_ind_c22_phy_read(struct mt7530_priv *priv, int port, int regnum)
{
	struct mt7530_dummy_poll p;
	int ret;
	u32 val;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mt7530_mutex_lock(priv);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	val = MT7531_MDIO_CL22_READ | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_REG_ADDR(regnum);

	mt7530_mii_write(priv, MT7531_PHY_IAC, val | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	ret = val & MT7531_MDIO_RW_DATA_MASK;
out:
	mt7530_mutex_unlock(priv);

	return ret;
}

static int
mt7531_ind_c22_phy_write(struct mt7530_priv *priv, int port, int regnum,
			 u16 data)
{
	struct mt7530_dummy_poll p;
	int ret;
	u32 reg;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mt7530_mutex_lock(priv);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, reg,
				 !(reg & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MT7531_MDIO_CL22_WRITE | MT7531_MDIO_PHY_ADDR(port) |
	      MT7531_MDIO_REG_ADDR(regnum) | data;

	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | MT7531_PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, reg,
				 !(reg & MT7531_PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

out:
	mt7530_mutex_unlock(priv);

	return ret;
}

static int
mt753x_phy_read_c22(struct mii_bus *bus, int port, int regnum)
{
	struct mt7530_priv *priv = bus->priv;

	return priv->info->phy_read_c22(priv, port, regnum);
}

static int
mt753x_phy_read_c45(struct mii_bus *bus, int port, int devad, int regnum)
{
	struct mt7530_priv *priv = bus->priv;

	return priv->info->phy_read_c45(priv, port, devad, regnum);
}

static int
mt753x_phy_write_c22(struct mii_bus *bus, int port, int regnum, u16 val)
{
	struct mt7530_priv *priv = bus->priv;

	return priv->info->phy_write_c22(priv, port, regnum, val);
}

static int
mt753x_phy_write_c45(struct mii_bus *bus, int port, int devad, int regnum,
		     u16 val)
{
	struct mt7530_priv *priv = bus->priv;

	return priv->info->phy_write_c45(priv, port, devad, regnum, val);
}

static void
mt7530_get_strings(struct dsa_switch *ds, int port, u32 stringset,
		   uint8_t *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++)
		ethtool_puts(&data, mt7530_mib[i].name);
}

static void
mt7530_read_port_stats(struct mt7530_priv *priv, int port,
		       u32 offset, u8 size, uint64_t *data)
{
	u32 val, reg = MT7530_PORT_MIB_COUNTER(port) + offset;

	val = mt7530_read(priv, reg);
	*data = val;

	if (size == 2) {
		val = mt7530_read(priv, reg + 4);
		*data |= (u64)val << 32;
	}
}

static void
mt7530_get_ethtool_stats(struct dsa_switch *ds, int port,
			 uint64_t *data)
{
	struct mt7530_priv *priv = ds->priv;
	const struct mt7530_mib_desc *mib;
	int i;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++) {
		mib = &mt7530_mib[i];

		mt7530_read_port_stats(priv, port, mib->offset, mib->size,
				       data + i);
	}
}

static int
mt7530_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(mt7530_mib);
}

static void mt7530_get_eth_mac_stats(struct dsa_switch *ds, int port,
				     struct ethtool_eth_mac_stats *mac_stats)
{
	struct mt7530_priv *priv = ds->priv;

	/* MIB counter doesn't provide a FramesTransmittedOK but instead
	 * provide stats for Unicast, Broadcast and Multicast frames separately.
	 * To simulate a global frame counter, read Unicast and addition Multicast
	 * and Broadcast later
	 */
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_UNICAST, 1,
			       &mac_stats->FramesTransmittedOK);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_SINGLE_COLLISION, 1,
			       &mac_stats->SingleCollisionFrames);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_MULTIPLE_COLLISION, 1,
			       &mac_stats->MultipleCollisionFrames);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_UNICAST, 1,
			       &mac_stats->FramesReceivedOK);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_BYTES, 2,
			       &mac_stats->OctetsTransmittedOK);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_ALIGN_ERR, 1,
			       &mac_stats->AlignmentErrors);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_DEFERRED, 1,
			       &mac_stats->FramesWithDeferredXmissions);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_LATE_COLLISION, 1,
			       &mac_stats->LateCollisions);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_EXCESSIVE_COLLISION, 1,
			       &mac_stats->FramesAbortedDueToXSColls);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_BYTES, 2,
			       &mac_stats->OctetsReceivedOK);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_MULTICAST, 1,
			       &mac_stats->MulticastFramesXmittedOK);
	mac_stats->FramesTransmittedOK += mac_stats->MulticastFramesXmittedOK;
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_BROADCAST, 1,
			       &mac_stats->BroadcastFramesXmittedOK);
	mac_stats->FramesTransmittedOK += mac_stats->BroadcastFramesXmittedOK;

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_MULTICAST, 1,
			       &mac_stats->MulticastFramesReceivedOK);
	mac_stats->FramesReceivedOK += mac_stats->MulticastFramesReceivedOK;
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_BROADCAST, 1,
			       &mac_stats->BroadcastFramesReceivedOK);
	mac_stats->FramesReceivedOK += mac_stats->BroadcastFramesReceivedOK;
}

static const struct ethtool_rmon_hist_range mt7530_rmon_ranges[] = {
	{ 0, 64 },
	{ 65, 127 },
	{ 128, 255 },
	{ 256, 511 },
	{ 512, 1023 },
	{ 1024, MT7530_MAX_MTU },
	{}
};

static void mt7530_get_rmon_stats(struct dsa_switch *ds, int port,
				  struct ethtool_rmon_stats *rmon_stats,
				  const struct ethtool_rmon_hist_range **ranges)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_UNDER_SIZE_ERR, 1,
			       &rmon_stats->undersize_pkts);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_OVER_SZ_ERR, 1,
			       &rmon_stats->oversize_pkts);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_FRAG_ERR, 1,
			       &rmon_stats->fragments);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_JABBER_ERR, 1,
			       &rmon_stats->jabbers);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_64, 1,
			       &rmon_stats->hist[0]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_65_TO_127, 1,
			       &rmon_stats->hist[1]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_128_TO_255, 1,
			       &rmon_stats->hist[2]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_256_TO_511, 1,
			       &rmon_stats->hist[3]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_512_TO_1023, 1,
			       &rmon_stats->hist[4]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PKT_SZ_1024_TO_MAX, 1,
			       &rmon_stats->hist[5]);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_64, 1,
			       &rmon_stats->hist_tx[0]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_65_TO_127, 1,
			       &rmon_stats->hist_tx[1]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_128_TO_255, 1,
			       &rmon_stats->hist_tx[2]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_256_TO_511, 1,
			       &rmon_stats->hist_tx[3]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_512_TO_1023, 1,
			       &rmon_stats->hist_tx[4]);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PKT_SZ_1024_TO_MAX, 1,
			       &rmon_stats->hist_tx[5]);

	*ranges = mt7530_rmon_ranges;
}

static void mt7530_get_stats64(struct dsa_switch *ds, int port,
			       struct rtnl_link_stats64 *storage)
{
	struct mt7530_priv *priv = ds->priv;
	uint64_t data;

	/* MIB counter doesn't provide a FramesTransmittedOK but instead
	 * provide stats for Unicast, Broadcast and Multicast frames separately.
	 * To simulate a global frame counter, read Unicast and addition Multicast
	 * and Broadcast later
	 */
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_UNICAST, 1,
			       &storage->rx_packets);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_MULTICAST, 1,
			       &storage->multicast);
	storage->rx_packets += storage->multicast;
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_BROADCAST, 1,
			       &data);
	storage->rx_packets += data;

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_UNICAST, 1,
			       &storage->tx_packets);
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_MULTICAST, 1,
			       &data);
	storage->tx_packets += data;
	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_BROADCAST, 1,
			       &data);
	storage->tx_packets += data;

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_BYTES, 2,
			       &storage->rx_bytes);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_BYTES, 2,
			       &storage->tx_bytes);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_DROP, 1,
			       &storage->rx_dropped);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_DROP, 1,
			       &storage->tx_dropped);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_CRC_ERR, 1,
			       &storage->rx_crc_errors);
}

static void mt7530_get_eth_ctrl_stats(struct dsa_switch *ds, int port,
				      struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_TX_PAUSE, 1,
			       &ctrl_stats->MACControlFramesTransmitted);

	mt7530_read_port_stats(priv, port, MT7530_PORT_MIB_RX_PAUSE, 1,
			       &ctrl_stats->MACControlFramesReceived);
}

static int
mt7530_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct mt7530_priv *priv = ds->priv;
	unsigned int secs = msecs / 1000;
	unsigned int tmp_age_count;
	unsigned int error = -1;
	unsigned int age_count;
	unsigned int age_unit;

	/* Applied timer is (AGE_CNT + 1) * (AGE_UNIT + 1) seconds */
	if (secs < 1 || secs > (AGE_CNT_MAX + 1) * (AGE_UNIT_MAX + 1))
		return -ERANGE;

	/* iterate through all possible age_count to find the closest pair */
	for (tmp_age_count = 0; tmp_age_count <= AGE_CNT_MAX; ++tmp_age_count) {
		unsigned int tmp_age_unit = secs / (tmp_age_count + 1) - 1;

		if (tmp_age_unit <= AGE_UNIT_MAX) {
			unsigned int tmp_error = secs -
				(tmp_age_count + 1) * (tmp_age_unit + 1);

			/* found a closer pair */
			if (error > tmp_error) {
				error = tmp_error;
				age_count = tmp_age_count;
				age_unit = tmp_age_unit;
			}

			/* found the exact match, so break the loop */
			if (!error)
				break;
		}
	}

	mt7530_write(priv, MT7530_AAC, AGE_CNT(age_count) | AGE_UNIT(age_unit));

	return 0;
}

static const char *mt7530_p5_mode_str(unsigned int mode)
{
	switch (mode) {
	case MUX_PHY_P0:
		return "MUX PHY P0";
	case MUX_PHY_P4:
		return "MUX PHY P4";
	default:
		return "GMAC5";
	}
}

static void mt7530_setup_port5(struct dsa_switch *ds, phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;
	u8 tx_delay = 0;
	int val;

	mutex_lock(&priv->reg_mutex);

	val = mt7530_read(priv, MT753X_MTRAP);

	val &= ~MT7530_P5_PHY0_SEL & ~MT7530_P5_MAC_SEL & ~MT7530_P5_RGMII_MODE;

	switch (priv->p5_mode) {
	/* MUX_PHY_P0: P0 -> P5 -> SoC MAC */
	case MUX_PHY_P0:
		val |= MT7530_P5_PHY0_SEL;
		fallthrough;

	/* MUX_PHY_P4: P4 -> P5 -> SoC MAC */
	case MUX_PHY_P4:
		/* Setup the MAC by default for the cpu port */
		mt7530_write(priv, MT753X_PMCR_P(5), 0x56300);
		break;

	/* GMAC5: P5 -> SoC MAC or external PHY */
	default:
		val |= MT7530_P5_MAC_SEL;
		break;
	}

	/* Setup RGMII settings */
	if (phy_interface_mode_is_rgmii(interface)) {
		val |= MT7530_P5_RGMII_MODE;

		/* P5 RGMII RX Clock Control: delay setting for 1000M */
		mt7530_write(priv, MT7530_P5RGMIIRXCR, CSR_RGMII_EDGE_ALIGN);

		/* Don't set delay in DSA mode */
		if (!dsa_is_dsa_port(priv->ds, 5) &&
		    (interface == PHY_INTERFACE_MODE_RGMII_TXID ||
		     interface == PHY_INTERFACE_MODE_RGMII_ID))
			tx_delay = 4; /* n * 0.5 ns */

		/* P5 RGMII TX Clock Control: delay x */
		mt7530_write(priv, MT7530_P5RGMIITXCR,
			     CSR_RGMII_TXC_CFG(0x10 + tx_delay));

		/* reduce P5 RGMII Tx driving, 8mA */
		mt7530_write(priv, MT7530_IO_DRV_CR,
			     P5_IO_CLK_DRV(1) | P5_IO_DATA_DRV(1));
	}

	mt7530_write(priv, MT753X_MTRAP, val);

	dev_dbg(ds->dev, "Setup P5, HWTRAP=0x%x, mode=%s, phy-mode=%s\n", val,
		mt7530_p5_mode_str(priv->p5_mode), phy_modes(interface));

	mutex_unlock(&priv->reg_mutex);
}

/* In Clause 5 of IEEE Std 802-2014, two sublayers of the data link layer (DLL)
 * of the Open Systems Interconnection basic reference model (OSI/RM) are
 * described; the medium access control (MAC) and logical link control (LLC)
 * sublayers. The MAC sublayer is the one facing the physical layer.
 *
 * In 8.2 of IEEE Std 802.1Q-2022, the Bridge architecture is described. A
 * Bridge component comprises a MAC Relay Entity for interconnecting the Ports
 * of the Bridge, at least two Ports, and higher layer entities with at least a
 * Spanning Tree Protocol Entity included.
 *
 * Each Bridge Port also functions as an end station and shall provide the MAC
 * Service to an LLC Entity. Each instance of the MAC Service is provided to a
 * distinct LLC Entity that supports protocol identification, multiplexing, and
 * demultiplexing, for protocol data unit (PDU) transmission and reception by
 * one or more higher layer entities.
 *
 * It is described in 8.13.9 of IEEE Std 802.1Q-2022 that in a Bridge, the LLC
 * Entity associated with each Bridge Port is modeled as being directly
 * connected to the attached Local Area Network (LAN).
 *
 * On the switch with CPU port architecture, CPU port functions as Management
 * Port, and the Management Port functionality is provided by software which
 * functions as an end station. Software is connected to an IEEE 802 LAN that is
 * wholly contained within the system that incorporates the Bridge. Software
 * provides access to the LLC Entity associated with each Bridge Port by the
 * value of the source port field on the special tag on the frame received by
 * software.
 *
 * We call frames that carry control information to determine the active
 * topology and current extent of each Virtual Local Area Network (VLAN), i.e.,
 * spanning tree or Shortest Path Bridging (SPB) and Multiple VLAN Registration
 * Protocol Data Units (MVRPDUs), and frames from other link constrained
 * protocols, such as Extensible Authentication Protocol over LAN (EAPOL) and
 * Link Layer Discovery Protocol (LLDP), link-local frames. They are not
 * forwarded by a Bridge. Permanently configured entries in the filtering
 * database (FDB) ensure that such frames are discarded by the Forwarding
 * Process. In 8.6.3 of IEEE Std 802.1Q-2022, this is described in detail:
 *
 * Each of the reserved MAC addresses specified in Table 8-1
 * (01-80-C2-00-00-[00,01,02,03,04,05,06,07,08,09,0A,0B,0C,0D,0E,0F]) shall be
 * permanently configured in the FDB in C-VLAN components and ERs.
 *
 * Each of the reserved MAC addresses specified in Table 8-2
 * (01-80-C2-00-00-[01,02,03,04,05,06,07,08,09,0A,0E]) shall be permanently
 * configured in the FDB in S-VLAN components.
 *
 * Each of the reserved MAC addresses specified in Table 8-3
 * (01-80-C2-00-00-[01,02,04,0E]) shall be permanently configured in the FDB in
 * TPMR components.
 *
 * The FDB entries for reserved MAC addresses shall specify filtering for all
 * Bridge Ports and all VIDs. Management shall not provide the capability to
 * modify or remove entries for reserved MAC addresses.
 *
 * The addresses in Table 8-1, Table 8-2, and Table 8-3 determine the scope of
 * propagation of PDUs within a Bridged Network, as follows:
 *
 *   The Nearest Bridge group address (01-80-C2-00-00-0E) is an address that no
 *   conformant Two-Port MAC Relay (TPMR) component, Service VLAN (S-VLAN)
 *   component, Customer VLAN (C-VLAN) component, or MAC Bridge can forward.
 *   PDUs transmitted using this destination address, or any other addresses
 *   that appear in Table 8-1, Table 8-2, and Table 8-3
 *   (01-80-C2-00-00-[00,01,02,03,04,05,06,07,08,09,0A,0B,0C,0D,0E,0F]), can
 *   therefore travel no further than those stations that can be reached via a
 *   single individual LAN from the originating station.
 *
 *   The Nearest non-TPMR Bridge group address (01-80-C2-00-00-03), is an
 *   address that no conformant S-VLAN component, C-VLAN component, or MAC
 *   Bridge can forward; however, this address is relayed by a TPMR component.
 *   PDUs using this destination address, or any of the other addresses that
 *   appear in both Table 8-1 and Table 8-2 but not in Table 8-3
 *   (01-80-C2-00-00-[00,03,05,06,07,08,09,0A,0B,0C,0D,0F]), will be relayed by
 *   any TPMRs but will propagate no further than the nearest S-VLAN component,
 *   C-VLAN component, or MAC Bridge.
 *
 *   The Nearest Customer Bridge group address (01-80-C2-00-00-00) is an address
 *   that no conformant C-VLAN component, MAC Bridge can forward; however, it is
 *   relayed by TPMR components and S-VLAN components. PDUs using this
 *   destination address, or any of the other addresses that appear in Table 8-1
 *   but not in either Table 8-2 or Table 8-3 (01-80-C2-00-00-[00,0B,0C,0D,0F]),
 *   will be relayed by TPMR components and S-VLAN components but will propagate
 *   no further than the nearest C-VLAN component or MAC Bridge.
 *
 * Because the LLC Entity associated with each Bridge Port is provided via CPU
 * port, we must not filter these frames but forward them to CPU port.
 *
 * In a Bridge, the transmission Port is majorly decided by ingress and egress
 * rules, FDB, and spanning tree Port State functions of the Forwarding Process.
 * For link-local frames, only CPU port should be designated as destination port
 * in the FDB, and the other functions of the Forwarding Process must not
 * interfere with the decision of the transmission Port. We call this process
 * trapping frames to CPU port.
 *
 * Therefore, on the switch with CPU port architecture, link-local frames must
 * be trapped to CPU port, and certain link-local frames received by a Port of a
 * Bridge comprising a TPMR component or an S-VLAN component must be excluded
 * from it.
 *
 * A Bridge of the switch with CPU port architecture cannot comprise a Two-Port
 * MAC Relay (TPMR) component as a TPMR component supports only a subset of the
 * functionality of a MAC Bridge. A Bridge comprising two Ports (Management Port
 * doesn't count) of this architecture will either function as a standard MAC
 * Bridge or a standard VLAN Bridge.
 *
 * Therefore, a Bridge of this architecture can only comprise S-VLAN components,
 * C-VLAN components, or MAC Bridge components. Since there's no TPMR component,
 * we don't need to relay PDUs using the destination addresses specified on the
 * Nearest non-TPMR section, and the proportion of the Nearest Customer Bridge
 * section where they must be relayed by TPMR components.
 *
 * One option to trap link-local frames to CPU port is to add static FDB entries
 * with CPU port designated as destination port. However, because that
 * Independent VLAN Learning (IVL) is being used on every VID, each entry only
 * applies to a single VLAN Identifier (VID). For a Bridge comprising a MAC
 * Bridge component or a C-VLAN component, there would have to be 16 times 4096
 * entries. This switch intellectual property can only hold a maximum of 2048
 * entries. Using this option, there also isn't a mechanism to prevent
 * link-local frames from being discarded when the spanning tree Port State of
 * the reception Port is discarding.
 *
 * The remaining option is to utilise the BPC, RGAC1, RGAC2, RGAC3, and RGAC4
 * registers. Whilst this applies to every VID, it doesn't contain all of the
 * reserved MAC addresses without affecting the remaining Standard Group MAC
 * Addresses. The REV_UN frame tag utilised using the RGAC4 register covers the
 * remaining 01-80-C2-00-00-[04,05,06,07,08,09,0A,0B,0C,0D,0F] destination
 * addresses. It also includes the 01-80-C2-00-00-22 to 01-80-C2-00-00-FF
 * destination addresses which may be relayed by MAC Bridges or VLAN Bridges.
 * The latter option provides better but not complete conformance.
 *
 * This switch intellectual property also does not provide a mechanism to trap
 * link-local frames with specific destination addresses to CPU port by Bridge,
 * to conform to the filtering rules for the distinct Bridge components.
 *
 * Therefore, regardless of the type of the Bridge component, link-local frames
 * with these destination addresses will be trapped to CPU port:
 *
 * 01-80-C2-00-00-[00,01,02,03,0E]
 *
 * In a Bridge comprising a MAC Bridge component or a C-VLAN component:
 *
 *   Link-local frames with these destination addresses won't be trapped to CPU
 *   port which won't conform to IEEE Std 802.1Q-2022:
 *
 *   01-80-C2-00-00-[04,05,06,07,08,09,0A,0B,0C,0D,0F]
 *
 * In a Bridge comprising an S-VLAN component:
 *
 *   Link-local frames with these destination addresses will be trapped to CPU
 *   port which won't conform to IEEE Std 802.1Q-2022:
 *
 *   01-80-C2-00-00-00
 *
 *   Link-local frames with these destination addresses won't be trapped to CPU
 *   port which won't conform to IEEE Std 802.1Q-2022:
 *
 *   01-80-C2-00-00-[04,05,06,07,08,09,0A]
 *
 * To trap link-local frames to CPU port as conformant as this switch
 * intellectual property can allow, link-local frames are made to be regarded as
 * Bridge Protocol Data Units (BPDUs). This is because this switch intellectual
 * property only lets the frames regarded as BPDUs bypass the spanning tree Port
 * State function of the Forwarding Process.
 *
 * The only remaining interference is the ingress rules. When the reception Port
 * has no PVID assigned on software, VLAN-untagged frames won't be allowed in.
 * There doesn't seem to be a mechanism on the switch intellectual property to
 * have link-local frames bypass this function of the Forwarding Process.
 */
static void
mt753x_trap_frames(struct mt7530_priv *priv)
{
	/* Trap 802.1X PAE frames and BPDUs to the CPU port(s) and egress them
	 * VLAN-untagged.
	 */
	mt7530_rmw(priv, MT753X_BPC,
		   PAE_BPDU_FR | PAE_EG_TAG_MASK | PAE_PORT_FW_MASK |
			   BPDU_EG_TAG_MASK | BPDU_PORT_FW_MASK,
		   PAE_BPDU_FR | PAE_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   PAE_PORT_FW(TO_CPU_FW_CPU_ONLY) |
			   BPDU_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   TO_CPU_FW_CPU_ONLY);

	/* Trap frames with :01 and :02 MAC DAs to the CPU port(s) and egress
	 * them VLAN-untagged.
	 */
	mt7530_rmw(priv, MT753X_RGAC1,
		   R02_BPDU_FR | R02_EG_TAG_MASK | R02_PORT_FW_MASK |
			   R01_BPDU_FR | R01_EG_TAG_MASK | R01_PORT_FW_MASK,
		   R02_BPDU_FR | R02_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   R02_PORT_FW(TO_CPU_FW_CPU_ONLY) | R01_BPDU_FR |
			   R01_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   TO_CPU_FW_CPU_ONLY);

	/* Trap frames with :03 and :0E MAC DAs to the CPU port(s) and egress
	 * them VLAN-untagged.
	 */
	mt7530_rmw(priv, MT753X_RGAC2,
		   R0E_BPDU_FR | R0E_EG_TAG_MASK | R0E_PORT_FW_MASK |
			   R03_BPDU_FR | R03_EG_TAG_MASK | R03_PORT_FW_MASK,
		   R0E_BPDU_FR | R0E_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   R0E_PORT_FW(TO_CPU_FW_CPU_ONLY) | R03_BPDU_FR |
			   R03_EG_TAG(MT7530_VLAN_EG_UNTAGGED) |
			   TO_CPU_FW_CPU_ONLY);
}

static void
mt753x_cpu_port_enable(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	/* Enable Mediatek header mode on the cpu port */
	mt7530_write(priv, MT7530_PVC_P(port),
		     PORT_SPEC_TAG);

	/* Enable flooding on the CPU port */
	mt7530_set(priv, MT753X_MFC, BC_FFP(BIT(port)) | UNM_FFP(BIT(port)) |
		   UNU_FFP(BIT(port)));

	/* Add the CPU port to the CPU port bitmap for MT7531 and the switch on
	 * the MT7988 SoC. Trapped frames will be forwarded to the CPU port that
	 * is affine to the inbound user port.
	 */
	if (priv->id == ID_MT7531 || priv->id == ID_MT7988 ||
	    priv->id == ID_EN7581 || priv->id == ID_AN7583)
		mt7530_set(priv, MT7531_CFC, MT7531_CPU_PMAP(BIT(port)));

	/* CPU port gets connected to all user ports of
	 * the switch.
	 */
	mt7530_write(priv, MT7530_PCR_P(port),
		     PCR_MATRIX(dsa_user_ports(priv->ds)));

	/* Set to fallback mode for independent VLAN learning */
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
		   MT7530_PORT_FALLBACK_MODE);
}

static int
mt7530_port_enable(struct dsa_switch *ds, int port,
		   struct phy_device *phy)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	/* Allow the user port gets connected to the cpu port and also
	 * restore the port matrix if the port is the member of a certain
	 * bridge.
	 */
	if (dsa_port_is_user(dp)) {
		struct dsa_port *cpu_dp = dp->cpu_dp;

		priv->ports[port].pm |= PCR_MATRIX(BIT(cpu_dp->index));
	}
	priv->ports[port].enable = true;
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK,
		   priv->ports[port].pm);

	mutex_unlock(&priv->reg_mutex);

	if (priv->id != ID_MT7530 && priv->id != ID_MT7621)
		return 0;

	if (port == 5)
		mt7530_clear(priv, MT753X_MTRAP, MT7530_P5_DIS);
	else if (port == 6)
		mt7530_clear(priv, MT753X_MTRAP, MT7530_P6_DIS);

	return 0;
}

static void
mt7530_port_disable(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	/* Clear up all port matrix which could be restored in the next
	 * enablement for the port.
	 */
	priv->ports[port].enable = false;
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK,
		   PCR_MATRIX_CLR);

	mutex_unlock(&priv->reg_mutex);

	if (priv->id != ID_MT7530 && priv->id != ID_MT7621)
		return;

	/* Do not set MT7530_P5_DIS when port 5 is being used for PHY muxing. */
	if (port == 5 && priv->p5_mode == GMAC5)
		mt7530_set(priv, MT753X_MTRAP, MT7530_P5_DIS);
	else if (port == 6)
		mt7530_set(priv, MT753X_MTRAP, MT7530_P6_DIS);
}

static int
mt7530_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct mt7530_priv *priv = ds->priv;
	int length;
	u32 val;

	/* When a new MTU is set, DSA always set the CPU port's MTU to the
	 * largest MTU of the user ports. Because the switch only has a global
	 * RX length register, only allowing CPU port here is enough.
	 */
	if (!dsa_is_cpu_port(ds, port))
		return 0;

	mt7530_mutex_lock(priv);

	val = mt7530_mii_read(priv, MT7530_GMACCR);
	val &= ~MAX_RX_PKT_LEN_MASK;

	/* RX length also includes Ethernet header, MTK tag, and FCS length */
	length = new_mtu + ETH_HLEN + MTK_HDR_LEN + ETH_FCS_LEN;
	if (length <= 1522) {
		val |= MAX_RX_PKT_LEN_1522;
	} else if (length <= 1536) {
		val |= MAX_RX_PKT_LEN_1536;
	} else if (length <= 1552) {
		val |= MAX_RX_PKT_LEN_1552;
	} else {
		val &= ~MAX_RX_JUMBO_MASK;
		val |= MAX_RX_JUMBO(DIV_ROUND_UP(length, 1024));
		val |= MAX_RX_PKT_LEN_JUMBO;
	}

	mt7530_mii_write(priv, MT7530_GMACCR, val);

	mt7530_mutex_unlock(priv);

	return 0;
}

static int
mt7530_port_max_mtu(struct dsa_switch *ds, int port)
{
	return MT7530_MAX_MTU;
}

static void
mt7530_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = MT7530_STP_DISABLED;
		break;
	case BR_STATE_BLOCKING:
		stp_state = MT7530_STP_BLOCKING;
		break;
	case BR_STATE_LISTENING:
		stp_state = MT7530_STP_LISTENING;
		break;
	case BR_STATE_LEARNING:
		stp_state = MT7530_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = MT7530_STP_FORWARDING;
		break;
	}

	mt7530_rmw(priv, MT7530_SSP_P(port), FID_PST_MASK(FID_BRIDGED),
		   FID_PST(FID_BRIDGED, stp_state));
}

static void mt7530_update_port_member(struct mt7530_priv *priv, int port,
				      const struct net_device *bridge_dev,
				      bool join) __must_hold(&priv->reg_mutex)
{
	struct dsa_port *dp = dsa_to_port(priv->ds, port), *other_dp;
	struct mt7530_port *p = &priv->ports[port], *other_p;
	struct dsa_port *cpu_dp = dp->cpu_dp;
	u32 port_bitmap = BIT(cpu_dp->index);
	int other_port;
	bool isolated;

	dsa_switch_for_each_user_port(other_dp, priv->ds) {
		other_port = other_dp->index;
		other_p = &priv->ports[other_port];

		if (dp == other_dp)
			continue;

		/* Add/remove this port to/from the port matrix of the other
		 * ports in the same bridge. If the port is disabled, port
		 * matrix is kept and not being setup until the port becomes
		 * enabled.
		 */
		if (!dsa_port_offloads_bridge_dev(other_dp, bridge_dev))
			continue;

		isolated = p->isolated && other_p->isolated;

		if (join && !isolated) {
			other_p->pm |= PCR_MATRIX(BIT(port));
			port_bitmap |= BIT(other_port);
		} else {
			other_p->pm &= ~PCR_MATRIX(BIT(port));
		}

		if (other_p->enable)
			mt7530_rmw(priv, MT7530_PCR_P(other_port),
				   PCR_MATRIX_MASK, other_p->pm);
	}

	/* Add/remove the all other ports to this port matrix. For !join
	 * (leaving the bridge), only the CPU port will remain in the port matrix
	 * of this port.
	 */
	p->pm = PCR_MATRIX(port_bitmap);
	if (priv->ports[port].enable)
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK, p->pm);
}

static int
mt7530_port_pre_bridge_flags(struct dsa_switch *ds, int port,
			     struct switchdev_brport_flags flags,
			     struct netlink_ext_ack *extack)
{
	if (flags.mask & ~(BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD |
			   BR_BCAST_FLOOD | BR_ISOLATED))
		return -EINVAL;

	return 0;
}

static int
mt7530_port_bridge_flags(struct dsa_switch *ds, int port,
			 struct switchdev_brport_flags flags,
			 struct netlink_ext_ack *extack)
{
	struct mt7530_priv *priv = ds->priv;

	if (flags.mask & BR_LEARNING)
		mt7530_rmw(priv, MT7530_PSC_P(port), SA_DIS,
			   flags.val & BR_LEARNING ? 0 : SA_DIS);

	if (flags.mask & BR_FLOOD)
		mt7530_rmw(priv, MT753X_MFC, UNU_FFP(BIT(port)),
			   flags.val & BR_FLOOD ? UNU_FFP(BIT(port)) : 0);

	if (flags.mask & BR_MCAST_FLOOD)
		mt7530_rmw(priv, MT753X_MFC, UNM_FFP(BIT(port)),
			   flags.val & BR_MCAST_FLOOD ? UNM_FFP(BIT(port)) : 0);

	if (flags.mask & BR_BCAST_FLOOD)
		mt7530_rmw(priv, MT753X_MFC, BC_FFP(BIT(port)),
			   flags.val & BR_BCAST_FLOOD ? BC_FFP(BIT(port)) : 0);

	if (flags.mask & BR_ISOLATED) {
		struct dsa_port *dp = dsa_to_port(ds, port);
		struct net_device *bridge_dev = dsa_port_bridge_dev_get(dp);

		priv->ports[port].isolated = !!(flags.val & BR_ISOLATED);

		mutex_lock(&priv->reg_mutex);
		mt7530_update_port_member(priv, port, bridge_dev, true);
		mutex_unlock(&priv->reg_mutex);
	}

	return 0;
}

static int
mt7530_port_bridge_join(struct dsa_switch *ds, int port,
			struct dsa_bridge bridge, bool *tx_fwd_offload,
			struct netlink_ext_ack *extack)
{
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	mt7530_update_port_member(priv, port, bridge.dev, true);

	/* Set to fallback mode for independent VLAN learning */
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
		   MT7530_PORT_FALLBACK_MODE);

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static void
mt7530_port_set_vlan_unaware(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;
	bool all_user_ports_removed = true;
	int i;

	/* This is called after .port_bridge_leave when leaving a VLAN-aware
	 * bridge. Don't set standalone ports to fallback mode.
	 */
	if (dsa_port_bridge_dev_get(dsa_to_port(ds, port)))
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
			   MT7530_PORT_FALLBACK_MODE);

	mt7530_rmw(priv, MT7530_PVC_P(port),
		   VLAN_ATTR_MASK | PVC_EG_TAG_MASK | ACC_FRM_MASK,
		   VLAN_ATTR(MT7530_VLAN_TRANSPARENT) |
		   PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT) |
		   MT7530_VLAN_ACC_ALL);

	/* Set PVID to 0 */
	mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK,
		   G0_PORT_VID_DEF);

	for (i = 0; i < priv->ds->num_ports; i++) {
		if (dsa_is_user_port(ds, i) &&
		    dsa_port_is_vlan_filtering(dsa_to_port(ds, i))) {
			all_user_ports_removed = false;
			break;
		}
	}

	/* CPU port also does the same thing until all user ports belonging to
	 * the CPU port get out of VLAN filtering mode.
	 */
	if (all_user_ports_removed) {
		struct dsa_port *dp = dsa_to_port(ds, port);
		struct dsa_port *cpu_dp = dp->cpu_dp;

		mt7530_write(priv, MT7530_PCR_P(cpu_dp->index),
			     PCR_MATRIX(dsa_user_ports(priv->ds)));
		mt7530_write(priv, MT7530_PVC_P(cpu_dp->index), PORT_SPEC_TAG
			     | PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));
	}
}

static void
mt7530_port_set_vlan_aware(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	/* Trapped into security mode allows packet forwarding through VLAN
	 * table lookup.
	 */
	if (dsa_is_user_port(ds, port)) {
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
			   MT7530_PORT_SECURITY_MODE);
		mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK,
			   G0_PORT_VID(priv->ports[port].pvid));

		/* Only accept tagged frames if PVID is not set */
		if (!priv->ports[port].pvid)
			mt7530_rmw(priv, MT7530_PVC_P(port), ACC_FRM_MASK,
				   MT7530_VLAN_ACC_TAGGED);

		/* Set the port as a user port which is to be able to recognize
		 * VID from incoming packets before fetching entry within the
		 * VLAN table.
		 */
		mt7530_rmw(priv, MT7530_PVC_P(port),
			   VLAN_ATTR_MASK | PVC_EG_TAG_MASK,
			   VLAN_ATTR(MT7530_VLAN_USER) |
			   PVC_EG_TAG(MT7530_VLAN_EG_DISABLED));
	} else {
		/* Also set CPU ports to the "user" VLAN port attribute, to
		 * allow VLAN classification, but keep the EG_TAG attribute as
		 * "consistent" (i.o.w. don't change its value) for packets
		 * received by the switch from the CPU, so that tagged packets
		 * are forwarded to user ports as tagged, and untagged as
		 * untagged.
		 */
		mt7530_rmw(priv, MT7530_PVC_P(port), VLAN_ATTR_MASK,
			   VLAN_ATTR(MT7530_VLAN_USER));
	}
}

static void
mt7530_port_bridge_leave(struct dsa_switch *ds, int port,
			 struct dsa_bridge bridge)
{
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	mt7530_update_port_member(priv, port, bridge.dev, false);

	/* When a port is removed from the bridge, the port would be set up
	 * back to the default as is at initial boot which is a VLAN-unaware
	 * port.
	 */
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
		   MT7530_PORT_MATRIX_MODE);

	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_port_fdb_add(struct dsa_switch *ds, int port,
		    const unsigned char *addr, u16 vid,
		    struct dsa_db db)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1, STATIC_ENT);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_fdb_del(struct dsa_switch *ds, int port,
		    const unsigned char *addr, u16 vid,
		    struct dsa_db db)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1, STATIC_EMP);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_fdb_dump(struct dsa_switch *ds, int port,
		     dsa_fdb_dump_cb_t *cb, void *data)
{
	struct mt7530_priv *priv = ds->priv;
	struct mt7530_fdb _fdb = { 0 };
	int cnt = MT7530_NUM_FDB_RECORDS;
	int ret = 0;
	u32 rsp = 0;

	mutex_lock(&priv->reg_mutex);

	ret = mt7530_fdb_cmd(priv, MT7530_FDB_START, &rsp);
	if (ret < 0)
		goto err;

	do {
		if (rsp & ATC_SRCH_HIT) {
			mt7530_fdb_read(priv, &_fdb);
			if (_fdb.port_mask & BIT(port)) {
				ret = cb(_fdb.mac, _fdb.vid, _fdb.noarp,
					 data);
				if (ret < 0)
					break;
			}
		}
	} while (--cnt &&
		 !(rsp & ATC_SRCH_END) &&
		 !mt7530_fdb_cmd(priv, MT7530_FDB_NEXT, &rsp));
err:
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
mt7530_port_mdb_add(struct dsa_switch *ds, int port,
		    const struct switchdev_obj_port_mdb *mdb,
		    struct dsa_db db)
{
	struct mt7530_priv *priv = ds->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;
	u8 port_mask = 0;
	int ret;

	mutex_lock(&priv->reg_mutex);

	mt7530_fdb_write(priv, vid, 0, addr, 0, STATIC_EMP);
	if (!mt7530_fdb_cmd(priv, MT7530_FDB_READ, NULL))
		port_mask = (mt7530_read(priv, MT7530_ATRD) >> PORT_MAP)
			    & PORT_MAP_MASK;

	port_mask |= BIT(port);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1, STATIC_ENT);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);

	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_mdb_del(struct dsa_switch *ds, int port,
		    const struct switchdev_obj_port_mdb *mdb,
		    struct dsa_db db)
{
	struct mt7530_priv *priv = ds->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;
	u8 port_mask = 0;
	int ret;

	mutex_lock(&priv->reg_mutex);

	mt7530_fdb_write(priv, vid, 0, addr, 0, STATIC_EMP);
	if (!mt7530_fdb_cmd(priv, MT7530_FDB_READ, NULL))
		port_mask = (mt7530_read(priv, MT7530_ATRD) >> PORT_MAP)
			    & PORT_MAP_MASK;

	port_mask &= ~BIT(port);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1,
			 port_mask ? STATIC_ENT : STATIC_EMP);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);

	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_vlan_cmd(struct mt7530_priv *priv, enum mt7530_vlan_cmd cmd, u16 vid)
{
	struct mt7530_dummy_poll p;
	u32 val;
	int ret;

	val = VTCR_BUSY | VTCR_FUNC(cmd) | vid;
	mt7530_write(priv, MT7530_VTCR, val);

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_VTCR);
	ret = readx_poll_timeout(_mt7530_read, &p, val,
				 !(val & VTCR_BUSY), 20, 20000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		return ret;
	}

	val = mt7530_read(priv, MT7530_VTCR);
	if (val & VTCR_INVALID) {
		dev_err(priv->dev, "read VTCR invalid\n");
		return -EINVAL;
	}

	return 0;
}

static int
mt7530_port_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering,
			   struct netlink_ext_ack *extack)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct dsa_port *cpu_dp = dp->cpu_dp;

	if (vlan_filtering) {
		/* The port is being kept as VLAN-unaware port when bridge is
		 * set up with vlan_filtering not being set, Otherwise, the
		 * port and the corresponding CPU port is required the setup
		 * for becoming a VLAN-aware port.
		 */
		mt7530_port_set_vlan_aware(ds, port);
		mt7530_port_set_vlan_aware(ds, cpu_dp->index);
	} else {
		mt7530_port_set_vlan_unaware(ds, port);
	}

	return 0;
}

static void
mt7530_hw_vlan_add(struct mt7530_priv *priv,
		   struct mt7530_hw_vlan_entry *entry)
{
	struct dsa_port *dp = dsa_to_port(priv->ds, entry->port);
	u8 new_members;
	u32 val;

	new_members = entry->old_members | BIT(entry->port);

	/* Validate the entry with independent learning, create egress tag per
	 * VLAN and joining the port as one of the port members.
	 */
	val = IVL_MAC | VTAG_EN | PORT_MEM(new_members) | FID(FID_BRIDGED) |
	      VLAN_VALID;
	mt7530_write(priv, MT7530_VAWD1, val);

	/* Decide whether adding tag or not for those outgoing packets from the
	 * port inside the VLAN.
	 * CPU port is always taken as a tagged port for serving more than one
	 * VLANs across and also being applied with egress type stack mode for
	 * that VLAN tags would be appended after hardware special tag used as
	 * DSA tag.
	 */
	if (dsa_port_is_cpu(dp))
		val = MT7530_VLAN_EGRESS_STACK;
	else if (entry->untagged)
		val = MT7530_VLAN_EGRESS_UNTAG;
	else
		val = MT7530_VLAN_EGRESS_TAG;
	mt7530_rmw(priv, MT7530_VAWD2,
		   ETAG_CTRL_P_MASK(entry->port),
		   ETAG_CTRL_P(entry->port, val));
}

static void
mt7530_hw_vlan_del(struct mt7530_priv *priv,
		   struct mt7530_hw_vlan_entry *entry)
{
	u8 new_members;
	u32 val;

	new_members = entry->old_members & ~BIT(entry->port);

	val = mt7530_read(priv, MT7530_VAWD1);
	if (!(val & VLAN_VALID)) {
		dev_err(priv->dev,
			"Cannot be deleted due to invalid entry\n");
		return;
	}

	if (new_members) {
		val = IVL_MAC | VTAG_EN | PORT_MEM(new_members) |
		      VLAN_VALID;
		mt7530_write(priv, MT7530_VAWD1, val);
	} else {
		mt7530_write(priv, MT7530_VAWD1, 0);
		mt7530_write(priv, MT7530_VAWD2, 0);
	}
}

static void
mt7530_hw_vlan_update(struct mt7530_priv *priv, u16 vid,
		      struct mt7530_hw_vlan_entry *entry,
		      mt7530_vlan_op vlan_op)
{
	u32 val;

	/* Fetch entry */
	mt7530_vlan_cmd(priv, MT7530_VTCR_RD_VID, vid);

	val = mt7530_read(priv, MT7530_VAWD1);

	entry->old_members = (val >> PORT_MEM_SHFT) & PORT_MEM_MASK;

	/* Manipulate entry */
	vlan_op(priv, entry);

	/* Flush result to hardware */
	mt7530_vlan_cmd(priv, MT7530_VTCR_WR_VID, vid);
}

static int
mt7530_setup_vlan0(struct mt7530_priv *priv)
{
	u32 val;

	/* Validate the entry with independent learning, keep the original
	 * ingress tag attribute.
	 */
	val = IVL_MAC | EG_CON | PORT_MEM(MT7530_ALL_MEMBERS) | FID(FID_BRIDGED) |
	      VLAN_VALID;
	mt7530_write(priv, MT7530_VAWD1, val);

	return mt7530_vlan_cmd(priv, MT7530_VTCR_WR_VID, 0);
}

static int
mt7530_port_vlan_add(struct dsa_switch *ds, int port,
		     const struct switchdev_obj_port_vlan *vlan,
		     struct netlink_ext_ack *extack)
{
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	struct mt7530_hw_vlan_entry new_entry;
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	mt7530_hw_vlan_entry_init(&new_entry, port, untagged);
	mt7530_hw_vlan_update(priv, vlan->vid, &new_entry, mt7530_hw_vlan_add);

	if (pvid) {
		priv->ports[port].pvid = vlan->vid;

		/* Accept all frames if PVID is set */
		mt7530_rmw(priv, MT7530_PVC_P(port), ACC_FRM_MASK,
			   MT7530_VLAN_ACC_ALL);

		/* Only configure PVID if VLAN filtering is enabled */
		if (dsa_port_is_vlan_filtering(dsa_to_port(ds, port)))
			mt7530_rmw(priv, MT7530_PPBV1_P(port),
				   G0_PORT_VID_MASK,
				   G0_PORT_VID(vlan->vid));
	} else if (vlan->vid && priv->ports[port].pvid == vlan->vid) {
		/* This VLAN is overwritten without PVID, so unset it */
		priv->ports[port].pvid = G0_PORT_VID_DEF;

		/* Only accept tagged frames if the port is VLAN-aware */
		if (dsa_port_is_vlan_filtering(dsa_to_port(ds, port)))
			mt7530_rmw(priv, MT7530_PVC_P(port), ACC_FRM_MASK,
				   MT7530_VLAN_ACC_TAGGED);

		mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK,
			   G0_PORT_VID_DEF);
	}

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
mt7530_port_vlan_del(struct dsa_switch *ds, int port,
		     const struct switchdev_obj_port_vlan *vlan)
{
	struct mt7530_hw_vlan_entry target_entry;
	struct mt7530_priv *priv = ds->priv;

	mutex_lock(&priv->reg_mutex);

	mt7530_hw_vlan_entry_init(&target_entry, port, 0);
	mt7530_hw_vlan_update(priv, vlan->vid, &target_entry,
			      mt7530_hw_vlan_del);

	/* PVID is being restored to the default whenever the PVID port
	 * is being removed from the VLAN.
	 */
	if (priv->ports[port].pvid == vlan->vid) {
		priv->ports[port].pvid = G0_PORT_VID_DEF;

		/* Only accept tagged frames if the port is VLAN-aware */
		if (dsa_port_is_vlan_filtering(dsa_to_port(ds, port)))
			mt7530_rmw(priv, MT7530_PVC_P(port), ACC_FRM_MASK,
				   MT7530_VLAN_ACC_TAGGED);

		mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK,
			   G0_PORT_VID_DEF);
	}


	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int mt753x_port_mirror_add(struct dsa_switch *ds, int port,
				  struct dsa_mall_mirror_tc_entry *mirror,
				  bool ingress, struct netlink_ext_ack *extack)
{
	struct mt7530_priv *priv = ds->priv;
	int monitor_port;
	u32 val;

	/* Check for existent entry */
	if ((ingress ? priv->mirror_rx : priv->mirror_tx) & BIT(port))
		return -EEXIST;

	val = mt7530_read(priv, MT753X_MIRROR_REG(priv->id));

	/* MT7530 only supports one monitor port */
	monitor_port = MT753X_MIRROR_PORT_GET(priv->id, val);
	if (val & MT753X_MIRROR_EN(priv->id) &&
	    monitor_port != mirror->to_local_port)
		return -EEXIST;

	val |= MT753X_MIRROR_EN(priv->id);
	val &= ~MT753X_MIRROR_PORT_MASK(priv->id);
	val |= MT753X_MIRROR_PORT_SET(priv->id, mirror->to_local_port);
	mt7530_write(priv, MT753X_MIRROR_REG(priv->id), val);

	val = mt7530_read(priv, MT7530_PCR_P(port));
	if (ingress) {
		val |= PORT_RX_MIR;
		priv->mirror_rx |= BIT(port);
	} else {
		val |= PORT_TX_MIR;
		priv->mirror_tx |= BIT(port);
	}
	mt7530_write(priv, MT7530_PCR_P(port), val);

	return 0;
}

static void mt753x_port_mirror_del(struct dsa_switch *ds, int port,
				   struct dsa_mall_mirror_tc_entry *mirror)
{
	struct mt7530_priv *priv = ds->priv;
	u32 val;

	val = mt7530_read(priv, MT7530_PCR_P(port));
	if (mirror->ingress) {
		val &= ~PORT_RX_MIR;
		priv->mirror_rx &= ~BIT(port);
	} else {
		val &= ~PORT_TX_MIR;
		priv->mirror_tx &= ~BIT(port);
	}
	mt7530_write(priv, MT7530_PCR_P(port), val);

	if (!priv->mirror_rx && !priv->mirror_tx) {
		val = mt7530_read(priv, MT753X_MIRROR_REG(priv->id));
		val &= ~MT753X_MIRROR_EN(priv->id);
		mt7530_write(priv, MT753X_MIRROR_REG(priv->id), val);
	}
}

static enum dsa_tag_protocol
mtk_get_tag_protocol(struct dsa_switch *ds, int port,
		     enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_MTK;
}

#ifdef CONFIG_GPIOLIB
static inline u32
mt7530_gpio_to_bit(unsigned int offset)
{
	/* Map GPIO offset to register bit
	 * [ 2: 0]  port 0 LED 0..2 as GPIO 0..2
	 * [ 6: 4]  port 1 LED 0..2 as GPIO 3..5
	 * [10: 8]  port 2 LED 0..2 as GPIO 6..8
	 * [14:12]  port 3 LED 0..2 as GPIO 9..11
	 * [18:16]  port 4 LED 0..2 as GPIO 12..14
	 */
	return BIT(offset + offset / 3);
}

static int
mt7530_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct mt7530_priv *priv = gpiochip_get_data(gc);
	u32 bit = mt7530_gpio_to_bit(offset);

	return !!(mt7530_read(priv, MT7530_LED_GPIO_DATA) & bit);
}

static int
mt7530_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct mt7530_priv *priv = gpiochip_get_data(gc);
	u32 bit = mt7530_gpio_to_bit(offset);

	if (value)
		mt7530_set(priv, MT7530_LED_GPIO_DATA, bit);
	else
		mt7530_clear(priv, MT7530_LED_GPIO_DATA, bit);

	return 0;
}

static int
mt7530_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct mt7530_priv *priv = gpiochip_get_data(gc);
	u32 bit = mt7530_gpio_to_bit(offset);

	return (mt7530_read(priv, MT7530_LED_GPIO_DIR) & bit) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int
mt7530_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct mt7530_priv *priv = gpiochip_get_data(gc);
	u32 bit = mt7530_gpio_to_bit(offset);

	mt7530_clear(priv, MT7530_LED_GPIO_OE, bit);
	mt7530_clear(priv, MT7530_LED_GPIO_DIR, bit);

	return 0;
}

static int
mt7530_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct mt7530_priv *priv = gpiochip_get_data(gc);
	u32 bit = mt7530_gpio_to_bit(offset);

	mt7530_set(priv, MT7530_LED_GPIO_DIR, bit);

	if (value)
		mt7530_set(priv, MT7530_LED_GPIO_DATA, bit);
	else
		mt7530_clear(priv, MT7530_LED_GPIO_DATA, bit);

	mt7530_set(priv, MT7530_LED_GPIO_OE, bit);

	return 0;
}

static int
mt7530_setup_gpio(struct mt7530_priv *priv)
{
	struct device *dev = priv->dev;
	struct gpio_chip *gc;

	gc = devm_kzalloc(dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	mt7530_write(priv, MT7530_LED_GPIO_OE, 0);
	mt7530_write(priv, MT7530_LED_GPIO_DIR, 0);
	mt7530_write(priv, MT7530_LED_IO_MODE, 0);

	gc->label = "mt7530";
	gc->parent = dev;
	gc->owner = THIS_MODULE;
	gc->get_direction = mt7530_gpio_get_direction;
	gc->direction_input = mt7530_gpio_direction_input;
	gc->direction_output = mt7530_gpio_direction_output;
	gc->get = mt7530_gpio_get;
	gc->set_rv = mt7530_gpio_set;
	gc->base = -1;
	gc->ngpio = 15;
	gc->can_sleep = true;

	return devm_gpiochip_add_data(dev, gc, priv);
}
#endif /* CONFIG_GPIOLIB */

static void
mt7530_setup_mdio_irq(struct mt7530_priv *priv)
{
	struct dsa_switch *ds = priv->ds;
	int p;

	for (p = 0; p < MT7530_NUM_PHYS; p++) {
		if (BIT(p) & ds->phys_mii_mask) {
			unsigned int irq;

			irq = irq_create_mapping(priv->irq_domain, p);
			ds->user_mii_bus->irq[p] = irq;
		}
	}
}

static const struct regmap_irq mt7530_irqs[] = {
	REGMAP_IRQ_REG_LINE(0, 32),  /* PHY0_LC */
	REGMAP_IRQ_REG_LINE(1, 32),  /* PHY1_LC */
	REGMAP_IRQ_REG_LINE(2, 32),  /* PHY2_LC */
	REGMAP_IRQ_REG_LINE(3, 32),  /* PHY3_LC */
	REGMAP_IRQ_REG_LINE(4, 32),  /* PHY4_LC */
	REGMAP_IRQ_REG_LINE(5, 32),  /* PHY5_LC */
	REGMAP_IRQ_REG_LINE(6, 32),  /* PHY6_LC */
	REGMAP_IRQ_REG_LINE(16, 32), /* MAC_PC */
	REGMAP_IRQ_REG_LINE(17, 32), /* BMU */
	REGMAP_IRQ_REG_LINE(18, 32), /* MIB */
	REGMAP_IRQ_REG_LINE(22, 32), /* ARL_COL_FULL_COL */
	REGMAP_IRQ_REG_LINE(23, 32), /* ARL_COL_FULL */
	REGMAP_IRQ_REG_LINE(24, 32), /* ARL_TBL_ERR */
	REGMAP_IRQ_REG_LINE(25, 32), /* ARL_PKT_QERR */
	REGMAP_IRQ_REG_LINE(26, 32), /* ARL_EQ_ERR */
	REGMAP_IRQ_REG_LINE(27, 32), /* ARL_PKT_BC */
	REGMAP_IRQ_REG_LINE(28, 32), /* ARL_SEC_IG1X */
	REGMAP_IRQ_REG_LINE(29, 32), /* ARL_SEC_VLAN */
	REGMAP_IRQ_REG_LINE(30, 32), /* ARL_SEC_TAG */
	REGMAP_IRQ_REG_LINE(31, 32), /* ACL */
};

static const struct regmap_irq_chip mt7530_regmap_irq_chip = {
	.name = KBUILD_MODNAME,
	.status_base = MT7530_SYS_INT_STS,
	.unmask_base = MT7530_SYS_INT_EN,
	.ack_base = MT7530_SYS_INT_STS,
	.init_ack_masked = true,
	.irqs = mt7530_irqs,
	.num_irqs = ARRAY_SIZE(mt7530_irqs),
	.num_regs = 1,
};

static int
mt7530_setup_irq(struct mt7530_priv *priv)
{
	struct regmap_irq_chip_data *irq_data;
	struct device *dev = priv->dev;
	struct device_node *np = dev->of_node;
	int irq, ret;

	if (!of_property_read_bool(np, "interrupt-controller")) {
		dev_info(dev, "no interrupt support\n");
		return 0;
	}

	irq = of_irq_get(np, 0);
	if (irq <= 0) {
		dev_err(dev, "failed to get parent IRQ: %d\n", irq);
		return irq ? : -EINVAL;
	}

	/* This register must be set for MT7530 to properly fire interrupts */
	if (priv->id == ID_MT7530 || priv->id == ID_MT7621)
		mt7530_set(priv, MT7530_TOP_SIG_CTRL, TOP_SIG_CTRL_NORMAL);

	ret = devm_regmap_add_irq_chip_fwnode(dev, dev_fwnode(dev),
					      priv->regmap, irq,
					      IRQF_ONESHOT,
					      0, &mt7530_regmap_irq_chip,
					      &irq_data);
	if (ret)
		return ret;

	priv->irq_domain = regmap_irq_get_domain(irq_data);

	return 0;
}

static void
mt7530_free_mdio_irq(struct mt7530_priv *priv)
{
	int p;

	for (p = 0; p < MT7530_NUM_PHYS; p++) {
		if (BIT(p) & priv->ds->phys_mii_mask) {
			unsigned int irq;

			irq = irq_find_mapping(priv->irq_domain, p);
			irq_dispose_mapping(irq);
		}
	}
}

static int
mt7530_setup_mdio(struct mt7530_priv *priv)
{
	struct device_node *mnp, *np = priv->dev->of_node;
	struct dsa_switch *ds = priv->ds;
	struct device *dev = priv->dev;
	struct mii_bus *bus;
	static int idx;
	int ret = 0;

	mnp = of_get_child_by_name(np, "mdio");

	if (mnp && !of_device_is_available(mnp))
		goto out;

	bus = devm_mdiobus_alloc(dev);
	if (!bus) {
		ret = -ENOMEM;
		goto out;
	}

	if (!mnp)
		ds->user_mii_bus = bus;

	bus->priv = priv;
	bus->name = KBUILD_MODNAME "-mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, KBUILD_MODNAME "-%d", idx++);
	bus->read = mt753x_phy_read_c22;
	bus->write = mt753x_phy_write_c22;
	bus->read_c45 = mt753x_phy_read_c45;
	bus->write_c45 = mt753x_phy_write_c45;
	bus->parent = dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	if (priv->irq_domain && !mnp)
		mt7530_setup_mdio_irq(priv);

	ret = devm_of_mdiobus_register(dev, bus, mnp);
	if (ret) {
		dev_err(dev, "failed to register MDIO bus: %d\n", ret);
		if (priv->irq_domain && !mnp)
			mt7530_free_mdio_irq(priv);
	}

out:
	of_node_put(mnp);
	return ret;
}

static int
mt7530_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	struct device_node *dn = NULL;
	struct device_node *phy_node;
	struct device_node *mac_np;
	struct mt7530_dummy_poll p;
	phy_interface_t interface;
	struct dsa_port *cpu_dp;
	u32 id, val;
	int ret, i;

	/* The parent node of conduit netdev which holds the common system
	 * controller also is the container for two GMACs nodes representing
	 * as two netdev instances.
	 */
	dsa_switch_for_each_cpu_port(cpu_dp, ds) {
		dn = cpu_dp->conduit->dev.of_node->parent;
		/* It doesn't matter which CPU port is found first,
		 * their conduits should share the same parent OF node
		 */
		break;
	}

	if (!dn) {
		dev_err(ds->dev, "parent OF node of DSA conduit not found");
		return -EINVAL;
	}

	ds->assisted_learning_on_cpu_port = true;
	ds->mtu_enforcement_ingress = true;

	if (priv->id == ID_MT7530) {
		regulator_set_voltage(priv->core_pwr, 1000000, 1000000);
		ret = regulator_enable(priv->core_pwr);
		if (ret < 0) {
			dev_err(priv->dev,
				"Failed to enable core power: %d\n", ret);
			return ret;
		}

		regulator_set_voltage(priv->io_pwr, 3300000, 3300000);
		ret = regulator_enable(priv->io_pwr);
		if (ret < 0) {
			dev_err(priv->dev, "Failed to enable io pwr: %d\n",
				ret);
			return ret;
		}
	}

	/* Reset whole chip through gpio pin or memory-mapped registers for
	 * different type of hardware
	 */
	if (priv->mcm) {
		reset_control_assert(priv->rstc);
		usleep_range(5000, 5100);
		reset_control_deassert(priv->rstc);
	} else {
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(5000, 5100);
		gpiod_set_value_cansleep(priv->reset, 1);
	}

	/* Waiting for MT7530 got to stable */
	INIT_MT7530_DUMMY_POLL(&p, priv, MT753X_TRAP);
	ret = readx_poll_timeout(_mt7530_read, &p, val, val != 0,
				 20, 1000000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	id = mt7530_read(priv, MT7530_CREV);
	id >>= CHIP_NAME_SHIFT;
	if (id != MT7530_ID) {
		dev_err(priv->dev, "chip %x can't be supported\n", id);
		return -ENODEV;
	}

	if ((val & MT7530_XTAL_MASK) == MT7530_XTAL_20MHZ) {
		dev_err(priv->dev,
			"MT7530 with a 20MHz XTAL is not supported!\n");
		return -EINVAL;
	}

	/* Reset the switch through internal reset */
	mt7530_write(priv, MT7530_SYS_CTRL,
		     SYS_CTRL_PHY_RST | SYS_CTRL_SW_RST |
		     SYS_CTRL_REG_RST);

	/* Lower Tx driving for TRGMII path */
	for (i = 0; i < NUM_TRGMII_CTRL; i++)
		mt7530_write(priv, MT7530_TRGMII_TD_ODT(i),
			     TD_DM_DRVP(8) | TD_DM_DRVN(8));

	for (i = 0; i < NUM_TRGMII_CTRL; i++)
		mt7530_rmw(priv, MT7530_TRGMII_RD(i),
			   RD_TAP_MASK, RD_TAP(16));

	/* Allow modifying the trap and directly access PHY registers via the
	 * MDIO bus the switch is on.
	 */
	mt7530_rmw(priv, MT753X_MTRAP, MT7530_CHG_TRAP |
		   MT7530_PHY_INDIRECT_ACCESS, MT7530_CHG_TRAP);

	if ((val & MT7530_XTAL_MASK) == MT7530_XTAL_40MHZ)
		mt7530_pll_setup(priv);

	mt753x_trap_frames(priv);

	/* Enable and reset MIB counters */
	mt7530_mib_reset(ds);

	for (i = 0; i < priv->ds->num_ports; i++) {
		/* Clear link settings and enable force mode to force link down
		 * on all ports until they're enabled later.
		 */
		mt7530_rmw(priv, MT753X_PMCR_P(i),
			   PMCR_LINK_SETTINGS_MASK |
			   MT753X_FORCE_MODE(priv->id),
			   MT753X_FORCE_MODE(priv->id));

		/* Disable forwarding by default on all ports */
		mt7530_rmw(priv, MT7530_PCR_P(i), PCR_MATRIX_MASK,
			   PCR_MATRIX_CLR);

		/* Disable learning by default on all ports */
		mt7530_set(priv, MT7530_PSC_P(i), SA_DIS);

		if (dsa_is_cpu_port(ds, i)) {
			mt753x_cpu_port_enable(ds, i);
		} else {
			mt7530_port_disable(ds, i);

			/* Set default PVID to 0 on all user ports */
			mt7530_rmw(priv, MT7530_PPBV1_P(i), G0_PORT_VID_MASK,
				   G0_PORT_VID_DEF);
		}
		/* Enable consistent egress tag */
		mt7530_rmw(priv, MT7530_PVC_P(i), PVC_EG_TAG_MASK,
			   PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));
	}

	/* Allow mirroring frames received on the local port (monitor port). */
	mt7530_set(priv, MT753X_AGC, LOCAL_EN);

	/* Setup VLAN ID 0 for VLAN-unaware bridges */
	ret = mt7530_setup_vlan0(priv);
	if (ret)
		return ret;

	/* Check for PHY muxing on port 5 */
	if (dsa_is_unused_port(ds, 5)) {
		/* Scan the ethernet nodes. Look for GMAC1, lookup the used PHY.
		 * Set priv->p5_mode to the appropriate value if PHY muxing is
		 * detected.
		 */
		for_each_child_of_node(dn, mac_np) {
			if (!of_device_is_compatible(mac_np,
						     "mediatek,eth-mac"))
				continue;

			ret = of_property_read_u32(mac_np, "reg", &id);
			if (ret < 0 || id != 1)
				continue;

			phy_node = of_parse_phandle(mac_np, "phy-handle", 0);
			if (!phy_node)
				continue;

			if (phy_node->parent == priv->dev->of_node->parent ||
			    phy_node->parent->parent == priv->dev->of_node) {
				ret = of_get_phy_mode(mac_np, &interface);
				if (ret && ret != -ENODEV) {
					of_node_put(mac_np);
					of_node_put(phy_node);
					return ret;
				}
				id = of_mdio_parse_addr(ds->dev, phy_node);
				if (id == 0)
					priv->p5_mode = MUX_PHY_P0;
				if (id == 4)
					priv->p5_mode = MUX_PHY_P4;
			}
			of_node_put(mac_np);
			of_node_put(phy_node);
			break;
		}

		if (priv->p5_mode == MUX_PHY_P0 ||
		    priv->p5_mode == MUX_PHY_P4) {
			mt7530_clear(priv, MT753X_MTRAP, MT7530_P5_DIS);
			mt7530_setup_port5(ds, interface);
		}
	}

#ifdef CONFIG_GPIOLIB
	if (of_property_read_bool(priv->dev->of_node, "gpio-controller")) {
		ret = mt7530_setup_gpio(priv);
		if (ret)
			return ret;
	}
#endif /* CONFIG_GPIOLIB */

	/* Flush the FDB table */
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_FLUSH, NULL);
	if (ret < 0)
		return ret;

	return 0;
}

static int
mt7531_setup_common(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	int ret, i;

	ds->assisted_learning_on_cpu_port = true;
	ds->mtu_enforcement_ingress = true;

	mt753x_trap_frames(priv);

	/* Enable and reset MIB counters */
	mt7530_mib_reset(ds);

	/* Disable flooding on all ports */
	mt7530_clear(priv, MT753X_MFC, BC_FFP_MASK | UNM_FFP_MASK |
		     UNU_FFP_MASK);

	for (i = 0; i < priv->ds->num_ports; i++) {
		/* Clear link settings and enable force mode to force link down
		 * on all ports until they're enabled later.
		 */
		mt7530_rmw(priv, MT753X_PMCR_P(i),
			   PMCR_LINK_SETTINGS_MASK |
			   MT753X_FORCE_MODE(priv->id),
			   MT753X_FORCE_MODE(priv->id));

		/* Disable forwarding by default on all ports */
		mt7530_rmw(priv, MT7530_PCR_P(i), PCR_MATRIX_MASK,
			   PCR_MATRIX_CLR);

		/* Disable learning by default on all ports */
		mt7530_set(priv, MT7530_PSC_P(i), SA_DIS);

		mt7530_set(priv, MT7531_DBG_CNT(i), MT7531_DIS_CLR);

		if (dsa_is_cpu_port(ds, i)) {
			mt753x_cpu_port_enable(ds, i);
		} else {
			mt7530_port_disable(ds, i);

			/* Set default PVID to 0 on all user ports */
			mt7530_rmw(priv, MT7530_PPBV1_P(i), G0_PORT_VID_MASK,
				   G0_PORT_VID_DEF);
		}

		/* Enable consistent egress tag */
		mt7530_rmw(priv, MT7530_PVC_P(i), PVC_EG_TAG_MASK,
			   PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));
	}

	/* Allow mirroring frames received on the local port (monitor port). */
	mt7530_set(priv, MT753X_AGC, LOCAL_EN);

	/* Enable Special Tag for rx frames */
	if (priv->id == ID_EN7581 || priv->id == ID_AN7583)
		mt7530_write(priv, MT753X_CPORT_SPTAG_CFG,
			     CPORT_SW2FE_STAG_EN | CPORT_FE2SW_STAG_EN);

	/* Flush the FDB table */
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_FLUSH, NULL);
	if (ret < 0)
		return ret;

	/* Setup VLAN ID 0 for VLAN-unaware bridges */
	return mt7530_setup_vlan0(priv);
}

static int
mt7531_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	struct mt7530_dummy_poll p;
	u32 val, id;
	int ret, i;

	/* Reset whole chip through gpio pin or memory-mapped registers for
	 * different type of hardware
	 */
	if (priv->mcm) {
		reset_control_assert(priv->rstc);
		usleep_range(5000, 5100);
		reset_control_deassert(priv->rstc);
	} else {
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(5000, 5100);
		gpiod_set_value_cansleep(priv->reset, 1);
	}

	/* Waiting for MT7530 got to stable */
	INIT_MT7530_DUMMY_POLL(&p, priv, MT753X_TRAP);
	ret = readx_poll_timeout(_mt7530_read, &p, val, val != 0,
				 20, 1000000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	id = mt7530_read(priv, MT7531_CREV);
	id >>= CHIP_NAME_SHIFT;

	if (id != MT7531_ID) {
		dev_err(priv->dev, "chip %x can't be supported\n", id);
		return -ENODEV;
	}

	/* MT7531AE has got two SGMII units. One for port 5, one for port 6.
	 * MT7531BE has got only one SGMII unit which is for port 6.
	 */
	val = mt7530_read(priv, MT7531_TOP_SIG_SR);
	priv->p5_sgmii = !!(val & PAD_DUAL_SGMII_EN);

	/* Force link down on all ports before internal reset */
	for (i = 0; i < priv->ds->num_ports; i++)
		mt7530_write(priv, MT753X_PMCR_P(i), MT7531_FORCE_MODE_LNK);

	/* Reset the switch through internal reset */
	mt7530_write(priv, MT7530_SYS_CTRL, SYS_CTRL_SW_RST | SYS_CTRL_REG_RST);

	if (!priv->p5_sgmii) {
		mt7531_pll_setup(priv);
	} else {
		/* Unlike MT7531BE, the GPIO 6-12 pins are not used for RGMII on
		 * MT7531AE. Set the GPIO 11-12 pins to function as MDC and MDIO
		 * to expose the MDIO bus of the switch.
		 */
		mt7530_rmw(priv, MT7531_GPIO_MODE1, MT7531_GPIO11_RG_RXD2_MASK,
			   MT7531_EXT_P_MDC_11);
		mt7530_rmw(priv, MT7531_GPIO_MODE1, MT7531_GPIO12_RG_RXD3_MASK,
			   MT7531_EXT_P_MDIO_12);
	}

	mt7530_rmw(priv, MT7531_GPIO_MODE0, MT7531_GPIO0_MASK,
		   MT7531_GPIO0_INTERRUPT);

	/* Enable Energy-Efficient Ethernet (EEE) and PHY core PLL, since
	 * phy_device has not yet been created provided for
	 * phy_[read,write]_mmd_indirect is called, we provide our own
	 * mt7531_ind_mmd_phy_[read,write] to complete this function.
	 */
	val = mt7531_ind_c45_phy_read(priv,
				      MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
				      MDIO_MMD_VEND2, CORE_PLL_GROUP4);
	val |= MT7531_RG_SYSPLL_DMY2 | MT7531_PHY_PLL_BYPASS_MODE;
	val &= ~MT7531_PHY_PLL_OFF;
	mt7531_ind_c45_phy_write(priv,
				 MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr),
				 MDIO_MMD_VEND2, CORE_PLL_GROUP4, val);

	/* Disable EEE advertisement on the switch PHYs. */
	for (i = MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr);
	     i < MT753X_CTRL_PHY_ADDR(priv->mdiodev->addr) + MT7530_NUM_PHYS;
	     i++) {
		mt7531_ind_c45_phy_write(priv, i, MDIO_MMD_AN, MDIO_AN_EEE_ADV,
					 0);
	}

	ret = mt7531_setup_common(ds);
	if (ret)
		return ret;

	return 0;
}

static void mt7530_mac_port_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	config->mac_capabilities |= MAC_10 | MAC_100 | MAC_1000FD;

	switch (port) {
	/* Ports which are connected to switch PHYs. There is no MII pinout. */
	case 0 ... 4:
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  config->supported_interfaces);
		break;

	/* Port 5 supports rgmii with delays, mii, and gmii. */
	case 5:
		phy_interface_set_rgmii(config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_MII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  config->supported_interfaces);
		break;

	/* Port 6 supports rgmii and trgmii. */
	case 6:
		__set_bit(PHY_INTERFACE_MODE_RGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_TRGMII,
			  config->supported_interfaces);
		break;
	}
}

static void mt7531_mac_port_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	struct mt7530_priv *priv = ds->priv;

	config->mac_capabilities |= MAC_10 | MAC_100 | MAC_1000FD;

	switch (port) {
	/* Ports which are connected to switch PHYs. There is no MII pinout. */
	case 0 ... 4:
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  config->supported_interfaces);
		break;

	/* Port 5 supports rgmii with delays on MT7531BE, sgmii/802.3z on
	 * MT7531AE.
	 */
	case 5:
		if (!priv->p5_sgmii) {
			phy_interface_set_rgmii(config->supported_interfaces);
			break;
		}
		fallthrough;

	/* Port 6 supports sgmii/802.3z. */
	case 6:
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);

		config->mac_capabilities |= MAC_2500FD;
		break;
	}
}

static void mt7988_mac_port_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	switch (port) {
	/* Ports which are connected to switch PHYs. There is no MII pinout. */
	case 0 ... 3:
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);

		config->mac_capabilities |= MAC_10 | MAC_100 | MAC_1000FD;
		break;

	/* Port 6 is connected to SoC's XGMII MAC. There is no MII pinout. */
	case 6:
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);

		config->mac_capabilities |= MAC_10000FD;
		break;
	}
}

static void en7581_mac_port_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	switch (port) {
	/* Ports which are connected to switch PHYs. There is no MII pinout. */
	case 0 ... 4:
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);

		config->mac_capabilities |= MAC_10 | MAC_100 | MAC_1000FD;
		break;

	/* Port 6 is connected to SoC's XGMII MAC. There is no MII pinout. */
	case 6:
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);

		config->mac_capabilities |= MAC_10000FD;
		break;
	}
}

static void
mt7530_mac_config(struct dsa_switch *ds, int port, unsigned int mode,
		  phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;

	if (port == 5)
		mt7530_setup_port5(priv->ds, interface);
	else if (port == 6)
		mt7530_setup_port6(priv->ds, interface);
}

static void mt7531_rgmii_setup(struct mt7530_priv *priv,
			       phy_interface_t interface,
			       struct phy_device *phydev)
{
	u32 val;

	val = mt7530_read(priv, MT7531_CLKGEN_CTRL);
	val |= GP_CLK_EN;
	val &= ~GP_MODE_MASK;
	val |= GP_MODE(MT7531_GP_MODE_RGMII);
	val &= ~CLK_SKEW_IN_MASK;
	val |= CLK_SKEW_IN(MT7531_CLK_SKEW_NO_CHG);
	val &= ~CLK_SKEW_OUT_MASK;
	val |= CLK_SKEW_OUT(MT7531_CLK_SKEW_NO_CHG);
	val |= TXCLK_NO_REVERSE | RXCLK_NO_DELAY;

	/* Do not adjust rgmii delay when vendor phy driver presents. */
	if (!phydev || phy_driver_is_genphy(phydev)) {
		val &= ~(TXCLK_NO_REVERSE | RXCLK_NO_DELAY);
		switch (interface) {
		case PHY_INTERFACE_MODE_RGMII:
			val |= TXCLK_NO_REVERSE;
			val |= RXCLK_NO_DELAY;
			break;
		case PHY_INTERFACE_MODE_RGMII_RXID:
			val |= TXCLK_NO_REVERSE;
			break;
		case PHY_INTERFACE_MODE_RGMII_TXID:
			val |= RXCLK_NO_DELAY;
			break;
		case PHY_INTERFACE_MODE_RGMII_ID:
			break;
		default:
			break;
		}
	}

	mt7530_write(priv, MT7531_CLKGEN_CTRL, val);
}

static void
mt7531_mac_config(struct dsa_switch *ds, int port, unsigned int mode,
		  phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;
	struct phy_device *phydev;
	struct dsa_port *dp;

	if (phy_interface_mode_is_rgmii(interface)) {
		dp = dsa_to_port(ds, port);
		phydev = dp->user->phydev;
		mt7531_rgmii_setup(priv, interface, phydev);
	}
}

static struct phylink_pcs *
mt753x_phylink_mac_select_pcs(struct phylink_config *config,
			      phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mt7530_priv *priv = dp->ds->priv;

	switch (interface) {
	case PHY_INTERFACE_MODE_TRGMII:
		return &priv->pcs[dp->index].pcs;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		return priv->ports[dp->index].sgmii_pcs;
	default:
		return NULL;
	}
}

static void
mt753x_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			  const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct dsa_switch *ds = dp->ds;
	struct mt7530_priv *priv;
	int port = dp->index;

	priv = ds->priv;

	if ((port == 5 || port == 6) && priv->info->mac_port_config)
		priv->info->mac_port_config(ds, port, mode, state->interface);

	/* Are we connected to external phy */
	if (port == 5 && dsa_is_user_port(ds, 5))
		mt7530_set(priv, MT753X_PMCR_P(port), PMCR_EXT_PHY);
}

static void mt753x_phylink_mac_link_down(struct phylink_config *config,
					 unsigned int mode,
					 phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mt7530_priv *priv = dp->ds->priv;

	mt7530_clear(priv, MT753X_PMCR_P(dp->index), PMCR_LINK_SETTINGS_MASK);
}

static void mt753x_phylink_mac_link_up(struct phylink_config *config,
				       struct phy_device *phydev,
				       unsigned int mode,
				       phy_interface_t interface,
				       int speed, int duplex,
				       bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mt7530_priv *priv = dp->ds->priv;
	u32 mcr;

	mcr = PMCR_MAC_RX_EN | PMCR_MAC_TX_EN | PMCR_FORCE_LNK;

	switch (speed) {
	case SPEED_1000:
	case SPEED_2500:
	case SPEED_10000:
		mcr |= PMCR_FORCE_SPEED_1000;
		break;
	case SPEED_100:
		mcr |= PMCR_FORCE_SPEED_100;
		break;
	}
	if (duplex == DUPLEX_FULL) {
		mcr |= PMCR_FORCE_FDX;
		if (tx_pause)
			mcr |= PMCR_FORCE_TX_FC_EN;
		if (rx_pause)
			mcr |= PMCR_FORCE_RX_FC_EN;
	}

	mt7530_set(priv, MT753X_PMCR_P(dp->index), mcr);
}

static void mt753x_phylink_mac_disable_tx_lpi(struct phylink_config *config)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mt7530_priv *priv = dp->ds->priv;

	mt7530_clear(priv, MT753X_PMCR_P(dp->index),
		     PMCR_FORCE_EEE1G | PMCR_FORCE_EEE100);
}

static int mt753x_phylink_mac_enable_tx_lpi(struct phylink_config *config,
					    u32 timer, bool tx_clock_stop)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mt7530_priv *priv = dp->ds->priv;
	u32 val;

	/* If the timer is zero, then set LPI_MODE_EN, which allows the
	 * system to enter LPI mode immediately rather than waiting for
	 * the LPI threshold.
	 */
	if (!timer)
		val = LPI_MODE_EN;
	else if (FIELD_FIT(LPI_THRESH_MASK, timer))
		val = FIELD_PREP(LPI_THRESH_MASK, timer);
	else
		val = LPI_THRESH_MASK;

	mt7530_rmw(priv, MT753X_PMEEECR_P(dp->index),
		   LPI_THRESH_MASK | LPI_MODE_EN, val);

	mt7530_set(priv, MT753X_PMCR_P(dp->index),
		   PMCR_FORCE_EEE1G | PMCR_FORCE_EEE100);

	return 0;
}

static void mt753x_phylink_get_caps(struct dsa_switch *ds, int port,
				    struct phylink_config *config)
{
	struct mt7530_priv *priv = ds->priv;
	u32 eeecr;

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE;

	config->lpi_capabilities = MAC_100FD | MAC_1000FD | MAC_2500FD;

	eeecr = mt7530_read(priv, MT753X_PMEEECR_P(port));
	/* tx_lpi_timer should be in microseconds. The time units for
	 * LPI threshold are unspecified.
	 */
	config->lpi_timer_default = FIELD_GET(LPI_THRESH_MASK, eeecr);

	priv->info->mac_port_get_caps(ds, port, config);
}

static int mt753x_pcs_validate(struct phylink_pcs *pcs,
			       unsigned long *supported,
			       const struct phylink_link_state *state)
{
	/* Autonegotiation is not supported in TRGMII nor 802.3z modes */
	if (state->interface == PHY_INTERFACE_MODE_TRGMII ||
	    phy_interface_mode_is_8023z(state->interface))
		phylink_clear(supported, Autoneg);

	return 0;
}

static void mt7530_pcs_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
				 struct phylink_link_state *state)
{
	struct mt7530_priv *priv = pcs_to_mt753x_pcs(pcs)->priv;
	int port = pcs_to_mt753x_pcs(pcs)->port;
	u32 pmsr;

	pmsr = mt7530_read(priv, MT7530_PMSR_P(port));

	state->link = (pmsr & PMSR_LINK);
	state->an_complete = state->link;
	state->duplex = !!(pmsr & PMSR_DPX);

	switch (pmsr & PMSR_SPEED_MASK) {
	case PMSR_SPEED_10:
		state->speed = SPEED_10;
		break;
	case PMSR_SPEED_100:
		state->speed = SPEED_100;
		break;
	case PMSR_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	default:
		state->speed = SPEED_UNKNOWN;
		break;
	}

	state->pause &= ~(MLO_PAUSE_RX | MLO_PAUSE_TX);
	if (pmsr & PMSR_RX_FC)
		state->pause |= MLO_PAUSE_RX;
	if (pmsr & PMSR_TX_FC)
		state->pause |= MLO_PAUSE_TX;
}

static int mt753x_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	return 0;
}

static void mt7530_pcs_an_restart(struct phylink_pcs *pcs)
{
}

static const struct phylink_pcs_ops mt7530_pcs_ops = {
	.pcs_validate = mt753x_pcs_validate,
	.pcs_get_state = mt7530_pcs_get_state,
	.pcs_config = mt753x_pcs_config,
	.pcs_an_restart = mt7530_pcs_an_restart,
};

static int
mt753x_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	int ret = priv->info->sw_setup(ds);
	int i;

	if (ret)
		return ret;

	ret = mt7530_setup_irq(priv);
	if (ret)
		return ret;

	ret = mt7530_setup_mdio(priv);
	if (ret)
		return ret;

	/* Initialise the PCS devices */
	for (i = 0; i < priv->ds->num_ports; i++) {
		priv->pcs[i].pcs.ops = priv->info->pcs_ops;
		priv->pcs[i].priv = priv;
		priv->pcs[i].port = i;
	}

	if (priv->create_sgmii)
		ret = priv->create_sgmii(priv);

	if (ret && priv->irq_domain)
		mt7530_free_mdio_irq(priv);

	return ret;
}

static int mt753x_set_mac_eee(struct dsa_switch *ds, int port,
			      struct ethtool_keee *e)
{
	if (e->tx_lpi_timer > 0xFFF)
		return -EINVAL;

	return 0;
}

static void
mt753x_conduit_state_change(struct dsa_switch *ds,
			    const struct net_device *conduit,
			    bool operational)
{
	struct dsa_port *cpu_dp = conduit->dsa_ptr;
	struct mt7530_priv *priv = ds->priv;
	int val = 0;
	u8 mask;

	/* Set the CPU port to trap frames to for MT7530. Trapped frames will be
	 * forwarded to the numerically smallest CPU port whose conduit
	 * interface is up.
	 */
	if (priv->id != ID_MT7530 && priv->id != ID_MT7621)
		return;

	mask = BIT(cpu_dp->index);

	if (operational)
		priv->active_cpu_ports |= mask;
	else
		priv->active_cpu_ports &= ~mask;

	if (priv->active_cpu_ports) {
		val = MT7530_CPU_EN |
		      MT7530_CPU_PORT(__ffs(priv->active_cpu_ports));
	}

	mt7530_rmw(priv, MT753X_MFC, MT7530_CPU_EN | MT7530_CPU_PORT_MASK, val);
}

static int mt753x_tc_setup_qdisc_tbf(struct dsa_switch *ds, int port,
				     struct tc_tbf_qopt_offload *qopt)
{
	struct tc_tbf_qopt_offload_replace_params *p = &qopt->replace_params;
	struct mt7530_priv *priv = ds->priv;
	u32 rate = 0;

	switch (qopt->command) {
	case TC_TBF_REPLACE:
		rate = div_u64(p->rate.rate_bytes_ps, 1000) << 3; /* kbps */
		fallthrough;
	case TC_TBF_DESTROY: {
		u32 val, tick;

		mt7530_rmw(priv, MT753X_GERLCR, EGR_BC_MASK,
			   EGR_BC_CRC_IPG_PREAMBLE);

		/* if rate is greater than 10Mbps tick is 1/32 ms,
		 * 1ms otherwise
		 */
		tick = rate > 10000 ? 2 : 7;
		val = FIELD_PREP(ERLCR_CIR_MASK, (rate >> 5)) |
		      FIELD_PREP(ERLCR_EN_MASK, !!rate) |
		      FIELD_PREP(ERLCR_EXP_MASK, tick) |
		      ERLCR_TBF_MODE_MASK |
		      FIELD_PREP(ERLCR_MANT_MASK, 0xf);
		mt7530_write(priv, MT753X_ERLCR_P(port), val);
		break;
	}
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mt753x_setup_tc(struct dsa_switch *ds, int port,
			   enum tc_setup_type type, void *type_data)
{
	switch (type) {
	case TC_SETUP_QDISC_TBF:
		return mt753x_tc_setup_qdisc_tbf(ds, port, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

static int mt7988_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;

	/* Reset the switch */
	reset_control_assert(priv->rstc);
	usleep_range(20, 50);
	reset_control_deassert(priv->rstc);
	usleep_range(20, 50);

	/* AN7583 require additional tweak to CONN_CFG */
	if (priv->id == ID_AN7583)
		mt7530_rmw(priv, AN7583_GEPHY_CONN_CFG,
			   AN7583_CSR_DPHY_CKIN_SEL |
			   AN7583_CSR_PHY_CORE_REG_CLK_SEL |
			   AN7583_CSR_ETHER_AFE_PWD,
			   AN7583_CSR_DPHY_CKIN_SEL |
			   AN7583_CSR_PHY_CORE_REG_CLK_SEL |
			   FIELD_PREP(AN7583_CSR_ETHER_AFE_PWD, 0));

	/* Reset the switch PHYs */
	mt7530_write(priv, MT7530_SYS_CTRL, SYS_CTRL_PHY_RST);

	return mt7531_setup_common(ds);
}

const struct dsa_switch_ops mt7530_switch_ops = {
	.get_tag_protocol	= mtk_get_tag_protocol,
	.setup			= mt753x_setup,
	.preferred_default_local_cpu_port = mt753x_preferred_default_local_cpu_port,
	.get_strings		= mt7530_get_strings,
	.get_ethtool_stats	= mt7530_get_ethtool_stats,
	.get_sset_count		= mt7530_get_sset_count,
	.get_eth_mac_stats	= mt7530_get_eth_mac_stats,
	.get_rmon_stats		= mt7530_get_rmon_stats,
	.get_eth_ctrl_stats	= mt7530_get_eth_ctrl_stats,
	.get_stats64		= mt7530_get_stats64,
	.set_ageing_time	= mt7530_set_ageing_time,
	.port_enable		= mt7530_port_enable,
	.port_disable		= mt7530_port_disable,
	.port_change_mtu	= mt7530_port_change_mtu,
	.port_max_mtu		= mt7530_port_max_mtu,
	.port_stp_state_set	= mt7530_stp_state_set,
	.port_pre_bridge_flags	= mt7530_port_pre_bridge_flags,
	.port_bridge_flags	= mt7530_port_bridge_flags,
	.port_bridge_join	= mt7530_port_bridge_join,
	.port_bridge_leave	= mt7530_port_bridge_leave,
	.port_fdb_add		= mt7530_port_fdb_add,
	.port_fdb_del		= mt7530_port_fdb_del,
	.port_fdb_dump		= mt7530_port_fdb_dump,
	.port_mdb_add		= mt7530_port_mdb_add,
	.port_mdb_del		= mt7530_port_mdb_del,
	.port_vlan_filtering	= mt7530_port_vlan_filtering,
	.port_vlan_add		= mt7530_port_vlan_add,
	.port_vlan_del		= mt7530_port_vlan_del,
	.port_mirror_add	= mt753x_port_mirror_add,
	.port_mirror_del	= mt753x_port_mirror_del,
	.phylink_get_caps	= mt753x_phylink_get_caps,
	.support_eee		= dsa_supports_eee,
	.set_mac_eee		= mt753x_set_mac_eee,
	.conduit_state_change	= mt753x_conduit_state_change,
	.port_setup_tc		= mt753x_setup_tc,
};
EXPORT_SYMBOL_GPL(mt7530_switch_ops);

static const struct phylink_mac_ops mt753x_phylink_mac_ops = {
	.mac_select_pcs	= mt753x_phylink_mac_select_pcs,
	.mac_config	= mt753x_phylink_mac_config,
	.mac_link_down	= mt753x_phylink_mac_link_down,
	.mac_link_up	= mt753x_phylink_mac_link_up,
	.mac_disable_tx_lpi = mt753x_phylink_mac_disable_tx_lpi,
	.mac_enable_tx_lpi = mt753x_phylink_mac_enable_tx_lpi,
};

const struct mt753x_info mt753x_table[] = {
	[ID_MT7621] = {
		.id = ID_MT7621,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7530_setup,
		.phy_read_c22 = mt7530_phy_read_c22,
		.phy_write_c22 = mt7530_phy_write_c22,
		.phy_read_c45 = mt7530_phy_read_c45,
		.phy_write_c45 = mt7530_phy_write_c45,
		.mac_port_get_caps = mt7530_mac_port_get_caps,
		.mac_port_config = mt7530_mac_config,
	},
	[ID_MT7530] = {
		.id = ID_MT7530,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7530_setup,
		.phy_read_c22 = mt7530_phy_read_c22,
		.phy_write_c22 = mt7530_phy_write_c22,
		.phy_read_c45 = mt7530_phy_read_c45,
		.phy_write_c45 = mt7530_phy_write_c45,
		.mac_port_get_caps = mt7530_mac_port_get_caps,
		.mac_port_config = mt7530_mac_config,
	},
	[ID_MT7531] = {
		.id = ID_MT7531,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7531_setup,
		.phy_read_c22 = mt7531_ind_c22_phy_read,
		.phy_write_c22 = mt7531_ind_c22_phy_write,
		.phy_read_c45 = mt7531_ind_c45_phy_read,
		.phy_write_c45 = mt7531_ind_c45_phy_write,
		.mac_port_get_caps = mt7531_mac_port_get_caps,
		.mac_port_config = mt7531_mac_config,
	},
	[ID_MT7988] = {
		.id = ID_MT7988,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7988_setup,
		.phy_read_c22 = mt7531_ind_c22_phy_read,
		.phy_write_c22 = mt7531_ind_c22_phy_write,
		.phy_read_c45 = mt7531_ind_c45_phy_read,
		.phy_write_c45 = mt7531_ind_c45_phy_write,
		.mac_port_get_caps = mt7988_mac_port_get_caps,
	},
	[ID_EN7581] = {
		.id = ID_EN7581,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7988_setup,
		.phy_read_c22 = mt7531_ind_c22_phy_read,
		.phy_write_c22 = mt7531_ind_c22_phy_write,
		.phy_read_c45 = mt7531_ind_c45_phy_read,
		.phy_write_c45 = mt7531_ind_c45_phy_write,
		.mac_port_get_caps = en7581_mac_port_get_caps,
	},
	[ID_AN7583] = {
		.id = ID_AN7583,
		.pcs_ops = &mt7530_pcs_ops,
		.sw_setup = mt7988_setup,
		.phy_read_c22 = mt7531_ind_c22_phy_read,
		.phy_write_c22 = mt7531_ind_c22_phy_write,
		.phy_read_c45 = mt7531_ind_c45_phy_read,
		.phy_write_c45 = mt7531_ind_c45_phy_write,
		.mac_port_get_caps = en7581_mac_port_get_caps,
	},
};
EXPORT_SYMBOL_GPL(mt753x_table);

int
mt7530_probe_common(struct mt7530_priv *priv)
{
	struct device *dev = priv->dev;

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = dev;
	priv->ds->num_ports = MT7530_NUM_PORTS;

	/* Get the hardware identifier from the devicetree node.
	 * We will need it for some of the clock and regulator setup.
	 */
	priv->info = of_device_get_match_data(dev);
	if (!priv->info)
		return -EINVAL;

	priv->id = priv->info->id;
	priv->dev = dev;
	priv->ds->priv = priv;
	priv->ds->ops = &mt7530_switch_ops;
	priv->ds->phylink_mac_ops = &mt753x_phylink_mac_ops;
	mutex_init(&priv->reg_mutex);
	dev_set_drvdata(dev, priv);

	return 0;
}
EXPORT_SYMBOL_GPL(mt7530_probe_common);

void
mt7530_remove_common(struct mt7530_priv *priv)
{
	if (priv->irq_domain)
		mt7530_free_mdio_irq(priv);

	dsa_unregister_switch(priv->ds);

	mutex_destroy(&priv->reg_mutex);
}
EXPORT_SYMBOL_GPL(mt7530_remove_common);

MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_DESCRIPTION("Driver for Mediatek MT7530 Switch");
MODULE_LICENSE("GPL");
