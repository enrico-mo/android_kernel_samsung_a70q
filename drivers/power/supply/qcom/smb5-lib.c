/* Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/irq.h>
#include <linux/iio/consumer.h>
#include <linux/pmic-voter.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_batterydata.h>
#include "smb5-lib.h"
#include "smb5-reg.h"
#include "battery.h"
#include "schgm-flash.h"
#include "step-chg-jeita.h"
#include "storm-watch.h"
#include "schgm-flash.h"
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#include "../../../battery_qc/include/sec_battery_qc.h"
#endif
#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
#include <linux/usb/typec/pm6150/pm6150_typec.h>
#endif
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT)
#include "pm6150-sbu-vbus-short.h"
#endif
#if defined(CONFIG_AFC)
#include <linux/afc/pm6150-afc.h>
#endif
#if defined(CONFIG_VBUS_NOTIFIER)
#include <linux/vbus_notifier.h>
#endif /* CONFIG_VBUS_NOTIFIER */

#if defined(CONFIG_SEC_FACTORY)
#define LPD_RECHECK_INTERVAL	1000
#else
#define LPD_RECHECK_INTERVAL	10000
#endif
#define RSBU_K_300K_UV			3000000

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define smblib_dbg(chg, reason, fmt, ...)			\
	do {							\
		if (*chg->debug_mask & (reason))		\
			pr_info("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
		else						\
			pr_debug("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
	} while (0)


#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#define typec_rp_med_high(chg, typec_mode)			\
	(typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM	\
	|| typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH)
#else
#define typec_rp_med_high(chg, typec_mode)			\
	((typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM	\
	|| typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH)	\
	&& !chg->typec_legacy)
#endif

#define SUPPORT_RUSTPROOF (0)

#if defined(CONFIG_SEC_A90Q_PROJECT)
bool pd_charging = false;
EXPORT_SYMBOL(pd_charging);
#endif

#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
extern void pm6150_ccic_event_work(int dest,
		int id, int attach, int event, int sub);
extern void pm6150_set_pd_state(int state);
extern void pm6150_set_cable(int cable);
#endif

static void update_sw_icl_max(struct smb_charger *chg, int pst);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
static int smblib_update_jeita(struct smb_charger *chg, u32 *thresholds, int type);
extern int get_rid_type(void);
static int settled_icl = 0;
#endif


int smblib_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int value;
	int rc = 0;

	rc = regmap_read(chg->regmap, addr, &value);
	if (rc >= 0)
		*val = (u8)value;

	return rc;
}

int smblib_batch_read(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_read(chg->regmap, addr, val, count);
}

int smblib_write(struct smb_charger *chg, u16 addr, u8 val)
{
	return regmap_write(chg->regmap, addr, val);
}

int smblib_batch_write(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_write(chg->regmap, addr, val, count);
}

int smblib_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	return regmap_update_bits(chg->regmap, addr, mask, val);
}

int smblib_get_iio_channel(struct smb_charger *chg, const char *propname,
					struct iio_channel **chan)
{
	int rc = 0;

	rc = of_property_match_string(chg->dev->of_node,
					"io-channel-names", propname);
	if (rc < 0)
		return 0;

	*chan = iio_channel_get(chg->dev, propname);
	if (IS_ERR(*chan)) {
		rc = PTR_ERR(*chan);
		if (rc != -EPROBE_DEFER)
			smblib_err(chg, "%s channel unavailable, %d\n",
							propname, rc);
		*chan = NULL;
	}

	return rc;
}

#define DIV_FACTOR_MICRO_V_I	1
#define DIV_FACTOR_MILI_V_I	1000
#define DIV_FACTOR_DECIDEGC	100

#if defined(CONFIG_VBUS_NOTIFIER)
static void smblib_handle_vbus(bool vbus)
{
	vbus_status_t status = vbus ? STATUS_VBUS_HIGH : STATUS_VBUS_LOW;

	pr_info("%s: <%d>\n", __func__, status);

	vbus_notifier_handle(status);
}
#endif

int smblib_read_iio_channel(struct smb_charger *chg, struct iio_channel *chan,
							int div, int *data)
{
	int rc = 0;
	*data = -ENODATA;

	if (chan) {
		rc = iio_read_channel_processed(chan, data);
		if (rc < 0) {
			smblib_err(chg, "Error in reading IIO channel data, rc=%d\n",
					rc);
			return rc;
		}

		if (div != 0)
			*data /= div;
	}

	return rc;
}

int smblib_get_jeita_cc_delta(struct smb_charger *chg, int *cc_delta_ua)
{
	int rc, cc_minus_ua;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_7 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_7 = 0x%02x\n",
		   stat);

	if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT) {
		rc = smblib_get_charge_param(chg, &chg->param.jeita_cc_comp_hot,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT) {
		rc = smblib_get_charge_param(chg,
					&chg->param.jeita_cc_comp_cold,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else {
		cc_minus_ua = 0;
	}

	*cc_delta_ua = -cc_minus_ua;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: cc_delta_ua = %d\n", __func__, *cc_delta_ua);
#endif

	return 0;
}

int smblib_icl_override(struct smb_charger *chg, enum icl_override_mode  mode)
{
	int rc;
	u8 usb51_mode, icl_override, apsd_override;

	switch (mode) {
	case SW_OVERRIDE_USB51_MODE:
		usb51_mode = 0;
		icl_override = ICL_OVERRIDE_BIT;
		apsd_override = 0;
		break;
	case SW_OVERRIDE_HC_MODE:
		usb51_mode = USBIN_MODE_CHG_BIT;
		icl_override = 0;
		apsd_override = ICL_OVERRIDE_AFTER_APSD_BIT;
		break;
	case HW_AUTO_MODE:
	default:
		usb51_mode = USBIN_MODE_CHG_BIT;
		icl_override = 0;
		apsd_override = 0;
		break;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_SEC_FACTORY)
	if(!factory_mode)
#endif
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
			USBIN_MODE_CHG_BIT, usb51_mode);

	if (rc < 0) {
		smblib_err(chg, "Couldn't set USBIN_ICL_OPTIONS rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, CMD_ICL_OVERRIDE_REG,
				ICL_OVERRIDE_BIT, icl_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't override ICL rc=%d\n", rc);
		return rc;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_SEC_FACTORY)
	if(!factory_mode)
#endif
	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT, apsd_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't override ICL_AFTER_APSD rc=%d\n", rc);
		return rc;
	}

	return rc;
}

/*
 * This function does smb_en pin access, which is lock protected.
 * It should be called with smb_lock held.
 */
static int smblib_select_sec_charger_locked(struct smb_charger *chg,
					int sec_chg)
{
	int rc = 0;

	switch (sec_chg) {
	case POWER_SUPPLY_CHARGER_SEC_CP:
		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, true, 0);

		/* select Charge Pump instead of slave charger */
		rc = smblib_masked_write(chg, MISC_SMB_CFG_REG,
					SMB_EN_SEL_BIT, SMB_EN_SEL_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't select SMB charger rc=%d\n",
				rc);
			return rc;
		}
		/* Enable Charge Pump, under HW control */
		rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
					EN_CP_CMD_BIT, EN_CP_CMD_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable SMB charger rc=%d\n",
						rc);
			return rc;
		}
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, false, 0);
		break;
	case POWER_SUPPLY_CHARGER_SEC_PL:
		/* select slave charger instead of Charge Pump */
		rc = smblib_masked_write(chg, MISC_SMB_CFG_REG,
					SMB_EN_SEL_BIT, 0);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't select SMB charger rc=%d\n",
				rc);
			return rc;
		}
		/* Enable slave charger, under HW control */
		rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
					EN_STAT_CMD_BIT, EN_STAT_CMD_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable SMB charger rc=%d\n",
						rc);
			return rc;
		}
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, false, 0);

		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, false, 0);

		break;
	case POWER_SUPPLY_CHARGER_SEC_NONE:
	default:
		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, true, 0);

		/* SW override, disabling secondary charger(s) */
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, true, 0);
		break;
	}

	return rc;
}

static int smblib_select_sec_charger(struct smb_charger *chg, int sec_chg,
					int reason, bool toggle)
{
	int rc;

	mutex_lock(&chg->smb_lock);

	if (toggle && sec_chg == POWER_SUPPLY_CHARGER_SEC_CP) {
		rc = smblib_select_sec_charger_locked(chg,
					POWER_SUPPLY_CHARGER_SEC_NONE);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't disable secondary charger rc=%d\n",
				rc);
			goto unlock_out;
		}

		/*
		 * A minimum of 20us delay is expected before switching on STAT
		 * pin.
		 */
		usleep_range(20, 30);
	}

	rc = smblib_select_sec_charger_locked(chg, sec_chg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't switch secondary charger rc=%d\n",
			rc);
		goto unlock_out;
	}

	chg->sec_chg_selected = sec_chg;
	chg->cp_reason = reason;

unlock_out:
	mutex_unlock(&chg->smb_lock);

	return rc;
}

static void smblib_notify_extcon_props(struct smb_charger *chg, int id)
{
	union extcon_property_value val;
	union power_supply_propval prop_val;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC) {
		smblib_get_prop_typec_cc_orientation(chg, &prop_val);
		val.intval = ((prop_val.intval == 2) ? 1 : 0);
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_TYPEC_POLARITY, val);
		val.intval = true;
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
	} else if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		val.intval = false;
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
	}
}

static void smblib_notify_device_mode(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB);

	extcon_set_state_sync(chg->extcon, EXTCON_USB, enable);
#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
	/*
	 * add to turn on/off USB code for
	 * some samples set to micro USB type by HW in USB type C model
	 * using Samsung USB notifier instead of QC extcon.
	 * 0 : none, 1 : DFP, 2 : UFP
	 */
	pr_info("%s : %d\n" , __func__, enable);
	if (enable)
		pm6150_ccic_event_work(CCIC_NOTIFY_DEV_USB, CCIC_NOTIFY_ID_USB,
			CCIC_NOTIFY_ATTACH/*attach*/, USB_STATUS_NOTIFY_ATTACH_UFP/*drp*/, 0);
	else
		pm6150_ccic_event_work(CCIC_NOTIFY_DEV_USB, CCIC_NOTIFY_ID_USB,
			CCIC_NOTIFY_DETACH/*attach*/, USB_STATUS_NOTIFY_DETACH/*drp*/, 0);
#endif
}

static void smblib_notify_usb_host(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	if (enable) {
		smblib_dbg(chg, PR_OTG, "enabling VBUS in OTG mode\n");
		rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG,
					OTG_EN_BIT, OTG_EN_BIT);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't enable VBUS in OTG mode rc=%d\n", rc);
			return;
		}

		smblib_notify_extcon_props(chg, EXTCON_USB_HOST);
	} else {
		smblib_dbg(chg, PR_OTG, "disabling VBUS in OTG mode\n");
		rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG,
					OTG_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't disable VBUS in OTG mode rc=%d\n",
				rc);
			return;
		}
	}

	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, enable);
}

/********************
 * REGISTER GETTERS *
 ********************/

int smblib_get_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int *val_u)
{
	int rc = 0;
	u8 val_raw;

	rc = smblib_read(chg, param->reg, &val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't read from 0x%04x rc=%d\n",
			param->name, param->reg, rc);
		return rc;
	}

	if (param->get_proc)
		*val_u = param->get_proc(param, val_raw);
	else
		*val_u = val_raw * param->step_u + param->min_u;

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, *val_u, val_raw);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: %s = %d (0x%02x)\n", __func__,
				param->name, *val_u, val_raw);
#endif

	return rc;
}

int smblib_get_usb_suspend(struct smb_charger *chg, int *suspend)
{
	int rc = 0;
	u8 temp;

	rc = smblib_read(chg, USBIN_CMD_IL_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n", rc);
		return rc;
	}
	*suspend = temp & USBIN_SUSPEND_BIT;

	return rc;
}


static const s16 therm_lookup_table[] = {
	/* Index -30C~85C, ADC raw code */
	0x6C92, 0x6C43, 0x6BF0, 0x6B98, 0x6B3A, 0x6AD8, 0x6A70, 0x6A03,
	0x6990, 0x6916, 0x6897, 0x6811, 0x6785, 0x66F2, 0x6658, 0x65B7,
	0x650F, 0x6460, 0x63AA, 0x62EC, 0x6226, 0x6159, 0x6084, 0x5FA8,
	0x5EC3, 0x5DD8, 0x5CE4, 0x5BE9, 0x5AE7, 0x59DD, 0x58CD, 0x57B5,
	0x5696, 0x5571, 0x5446, 0x5314, 0x51DD, 0x50A0, 0x4F5E, 0x4E17,
	0x4CCC, 0x4B7D, 0x4A2A, 0x48D4, 0x477C, 0x4621, 0x44C4, 0x4365,
	0x4206, 0x40A6, 0x3F45, 0x3DE6, 0x3C86, 0x3B28, 0x39CC, 0x3872,
	0x3719, 0x35C4, 0x3471, 0x3322, 0x31D7, 0x308F, 0x2F4C, 0x2E0D,
	0x2CD3, 0x2B9E, 0x2A6E, 0x2943, 0x281D, 0x26FE, 0x25E3, 0x24CF,
	0x23C0, 0x22B8, 0x21B5, 0x20B8, 0x1FC2, 0x1ED1, 0x1DE6, 0x1D01,
	0x1C22, 0x1B49, 0x1A75, 0x19A8, 0x18E0, 0x181D, 0x1761, 0x16A9,
	0x15F7, 0x154A, 0x14A2, 0x13FF, 0x1361, 0x12C8, 0x1234, 0x11A4,
	0x1119, 0x1091, 0x100F, 0x0F90, 0x0F15, 0x0E9E, 0x0E2B, 0x0DBC,
	0x0D50, 0x0CE8, 0x0C83, 0x0C21, 0x0BC3, 0x0B67, 0x0B0F, 0x0AB9,
	0x0A66, 0x0A16, 0x09C9, 0x097E,
};

int smblib_get_thermal_threshold(struct smb_charger *chg, u16 addr, int *val)
{
	u8 buff[2];
	s16 temp;
	int rc = 0;
	int i, lower, upper;

	rc = smblib_batch_read(chg, addr, buff, 2);
	if (rc < 0) {
		pr_err("failed to write to 0x%04X, rc=%d\n", addr, rc);
		return rc;
	}

	temp = buff[1] | buff[0] << 8;

	lower = 0;
	upper = ARRAY_SIZE(therm_lookup_table) - 1;
	while (lower <= upper) {
		i = (upper + lower) / 2;
		if (therm_lookup_table[i] < temp)
			upper = i - 1;
		else if (therm_lookup_table[i] > temp)
			lower = i + 1;
		else
			break;
	}

	/* index 0 corresonds to -30C */
	*val = (i - 30) * 10;

	return rc;
}

struct apsd_result {
	const char * const name;
	const u8 bit;
	const enum power_supply_type pst;
};

enum {
	UNKNOWN,
	SDP,
	CDP,
	DCP,
	OCP,
	FLOAT,
	HVDCP2,
	HVDCP3,
	MAX_TYPES
};

static const struct apsd_result smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "SDP",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "CDP",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "DCP",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "OCP",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "FLOAT",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_FLOAT
	},
	[HVDCP2] = {
		.name	= "HVDCP2",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "HVDCP3",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
};

static const struct apsd_result *smblib_get_apsd_result(struct smb_charger *chg)
{
	int rc, i;
	u8 apsd_stat, stat;
	const struct apsd_result *result = &smblib_apsd_results[UNKNOWN];

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return result;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", apsd_stat);

	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT))
		return result;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_RESULT_STATUS rc=%d\n",
			rc);
		return result;
	}
	stat &= APSD_RESULT_STATUS_MASK;

	for (i = 0; i < ARRAY_SIZE(smblib_apsd_results); i++) {
		if (smblib_apsd_results[i].bit == stat)
			result = &smblib_apsd_results[i];
	}

	if (apsd_stat & QC_CHARGER_BIT) {
		/* since its a qc_charger, either return HVDCP3 or HVDCP2 */
		if (result != &smblib_apsd_results[HVDCP3])
			result = &smblib_apsd_results[HVDCP2];
	}

	return result;
}

#define INPUT_NOT_PRESENT	0
#define INPUT_PRESENT_USB	BIT(1)
#define INPUT_PRESENT_DC	BIT(2)
static int smblib_is_input_present(struct smb_charger *chg,
				   int *present)
{
	int rc;
	union power_supply_propval pval = {0, };

	*present = INPUT_NOT_PRESENT;

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0) {
		pr_err("Couldn't get usb presence status rc=%d\n", rc);
		return rc;
	}
	*present |= pval.intval ? INPUT_PRESENT_USB : INPUT_NOT_PRESENT;

	rc = smblib_get_prop_dc_present(chg, &pval);
	if (rc < 0) {
		pr_err("Couldn't get dc presence status rc=%d\n", rc);
		return rc;
	}
	*present |= pval.intval ? INPUT_PRESENT_DC : INPUT_NOT_PRESENT;

	return 0;
}

#define AICL_RANGE2_MIN_MV		5600
#define AICL_RANGE2_STEP_DELTA_MV	200
#define AICL_RANGE2_OFFSET		16
int smblib_get_aicl_cont_threshold(struct smb_chg_param *param, u8 val_raw)
{
	int base = param->min_u;
	u8 reg = val_raw;
	int step = param->step_u;


	if (val_raw >= AICL_RANGE2_OFFSET) {
		reg = val_raw - AICL_RANGE2_OFFSET;
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
	}

	return base + (reg * step);
}

/********************
 * REGISTER SETTERS *
 ********************/
static const struct buck_boost_freq chg_freq_list[] = {
	[0] = {
		.freq_khz	= 2400,
		.val		= 7,
	},
	[1] = {
		.freq_khz	= 2100,
		.val		= 8,
	},
	[2] = {
		.freq_khz	= 1600,
		.val		= 11,
	},
	[3] = {
		.freq_khz	= 1200,
		.val		= 15,
	},
};

int smblib_set_chg_freq(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	u8 i;

	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	/* Charger FSW is the configured freqency / 2 */
	val_u *= 2;
	for (i = 0; i < ARRAY_SIZE(chg_freq_list); i++) {
		if (chg_freq_list[i].freq_khz == val_u)
			break;
	}
	if (i == ARRAY_SIZE(chg_freq_list)) {
		pr_err("Invalid frequency %d Hz\n", val_u / 2);
		return -EINVAL;
	}

	*val_raw = chg_freq_list[i].val;

	return 0;
}

int smblib_set_opt_switcher_freq(struct smb_charger *chg, int fsw_khz)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher, fsw_khz);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_buck rc=%d\n", rc);

	if (chg->mode == PARALLEL_MASTER && chg->pl.psy) {
		pval.intval = fsw_khz;
		/*
		 * Some parallel charging implementations may not have
		 * PROP_BUCK_FREQ property - they could be running
		 * with a fixed frequency
		 */
		power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_BUCK_FREQ, &pval);
	}

	return rc;
}

int smblib_set_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int val_u)
{
	int rc = 0;
	u8 val_raw;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_SEC_FACTORY)
	if (factory_mode)
		if (param->reg == USBIN_CURRENT_LIMIT_CFG_REG)
			return rc;
#endif

	if (param->set_proc) {
		rc = param->set_proc(param, val_u, &val_raw);
		if (rc < 0)
			return -EINVAL;
	} else {
		if (val_u > param->max_u || val_u < param->min_u)
			smblib_dbg(chg, PR_MISC,
				"%s: %d is out of range [%d, %d]\n",
				param->name, val_u, param->min_u, param->max_u);

		if (val_u > param->max_u)
			val_u = param->max_u;
		if (val_u < param->min_u)
			val_u = param->min_u;

		val_raw = (val_u - param->min_u) / param->step_u;
	}

	rc = smblib_write(chg, param->reg, val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't write 0x%02x to 0x%04x rc=%d\n",
			param->name, val_raw, param->reg, rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, val_u, val_raw);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: %s = %d (0x%02x)\n", __func__,
				param->name, val_u, val_raw);
#endif

	return rc;
}

int smblib_set_usb_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	int irq = chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq;

	pr_info("%s: suspend: %d \n", __func__, suspend);

	if (suspend && irq) {
		if (chg->usb_icl_change_irq_enabled) {
			disable_irq_nosync(irq);
			chg->usb_icl_change_irq_enabled = false;
		}
	}
	if (suspend)
		vote(chg->icl_irq_disable_votable, USB_SUSPEND_VOTER,
				true, 0);

	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
				 suspend ? USBIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	if (!suspend)
		vote(chg->icl_irq_disable_votable, USB_SUSPEND_VOTER,
				false, 0);

	return rc;
}

int smblib_set_dc_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_SUSPEND_BIT,
				 suspend ? DCIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to DCIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	return rc;
}

static int smblib_set_adapter_allowance(struct smb_charger *chg,
					u8 allowed_voltage)
{
	int rc = 0;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	return rc;
#endif

	/* PMI632 only support max. 9V */
	if (chg->smb_version == PMI632_SUBTYPE) {
		switch (allowed_voltage) {
		case USBIN_ADAPTER_ALLOW_12V:
		case USBIN_ADAPTER_ALLOW_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_OR_12V:
		case USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_OR_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
			break;
		}
	}

	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
			allowed_voltage, rc);
		return rc;
	}

	return rc;
}

#if !defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#define MICRO_5V	5000000
#define MICRO_9V	9000000
#define MICRO_12V	12000000
#endif

static int smblib_set_usb_pd_fsw(struct smb_charger *chg, int voltage)
{
	int rc = 0;

	if (voltage == MICRO_5V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_5V);
	else if (voltage > MICRO_5V && voltage < MICRO_9V)
		rc = smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
	else if (voltage >= MICRO_9V && voltage < MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_9V);
	else if (voltage == MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_12V);
	else {
		smblib_err(chg, "Couldn't set Fsw: invalid voltage %d\n",
				voltage);
		return -EINVAL;
	}

	return rc;
}

static int smblib_set_usb_pd_allowed_voltage(struct smb_charger *chg,
					int min_allowed_uv, int max_allowed_uv)
{
	int rc;
	u8 allowed_voltage;

	if (min_allowed_uv == MICRO_5V && max_allowed_uv == MICRO_5V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V;
	} else if (min_allowed_uv == MICRO_9V && max_allowed_uv == MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
	} else if (min_allowed_uv == MICRO_12V && max_allowed_uv == MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_12V;
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_12V;
	} else if (min_allowed_uv < MICRO_12V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V_TO_12V;
	} else {
		smblib_err(chg, "invalid allowed voltage [%d, %d]\n",
			min_allowed_uv, max_allowed_uv);
		return -EINVAL;
	}

	rc = smblib_set_adapter_allowance(chg, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't configure adapter allowance rc=%d\n",
				rc);
		return rc;
	}

	return rc;
}

int smblib_set_aicl_cont_threshold(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	int base = param->min_u;
	int offset = 0;
	int step = param->step_u;

	if (val_u > param->max_u)
		val_u = param->max_u;
	if (val_u < param->min_u)
		val_u = param->min_u;

	if (val_u >= AICL_RANGE2_MIN_MV) {
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
		offset = AICL_RANGE2_OFFSET;
	};

	*val_raw = ((val_u - base) / step) + offset;

	return 0;
}

/********************
 * HELPER FUNCTIONS *
 ********************/

int smblib_get_prop_from_bms(struct smb_charger *chg,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy, psp, val);

	return rc;
}

void smblib_apsd_enable(struct smb_charger *chg, bool enable)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				BC1P2_SRC_DETECT_BIT,
				enable ? BC1P2_SRC_DETECT_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);
}

void smblib_hvdcp_detect_enable(struct smb_charger *chg, bool enable)
{
	int rc;
	u8 mask;

	if (chg->hvdcp_disable || chg->pd_not_supported)
		return;

	smblib_err(chg, "smblib_hvdcp_detect_enable: %d\n",enable);
	mask = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT;
	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, mask,
						enable ? mask : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);

	return;
}

void smblib_hvdcp_exit_config(struct smb_charger *chg)
{
	u8 stat;
	int rc;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0)
		return;

	if (stat & (QC_3P0_BIT | QC_2P0_BIT)) {
		/* force HVDCP to 5V */
		smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT, 0);
		smblib_write(chg, CMD_HVDCP_2_REG, FORCE_5V_BIT);

		/* rerun APSD */
		smblib_masked_write(chg, CMD_APSD_REG, APSD_RERUN_BIT,
				APSD_RERUN_BIT);
	}
}

static int smblib_request_dpdm(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	if (chg->pr_swap_in_progress)
		return 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg && of_get_property(chg->dev->of_node,
				"dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			smblib_err(chg, "Couldn't get dpdm regulator rc=%d\n",
					rc);
			chg->dpdm_reg = NULL;
			return rc;
		}
	}

	if (enable) {
		if (chg->dpdm_reg && !regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "enabling DPDM regulator\n");
			rc = regulator_enable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't enable dpdm regulator rc=%d\n",
					rc);
		}
	} else {
		if (chg->dpdm_reg && regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "disabling DPDM regulator\n");
			rc = regulator_disable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't disable dpdm regulator rc=%d\n",
					rc);
		}
	}

	return rc;
}

static void smblib_rerun_apsd(struct smb_charger *chg)
{
	int rc;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_SEC_FACTORY)
	if(factory_mode)
		return;
#endif

	smblib_dbg(chg, PR_MISC, "re-running APSD\n");

	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't re-run APSD rc=%d\n", rc);
}

static const struct apsd_result *smblib_update_usb_type(struct smb_charger *chg)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

#if defined(CONFIG_AFC)
	if ((chg->real_charger_type == POWER_SUPPLY_TYPE_AFC) && (apsd_result->pst == POWER_SUPPLY_TYPE_USB_DCP)) {
		pr_info("%s: Ignore DCP after AFC\n", __func__);
		return apsd_result;
	}
#endif

	/* if PD is active, APSD is disabled so won't have a valid result */
	if (chg->pd_active) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
	} else {
		/*
		 * Update real charger type only if its not FLOAT
		 * detected as as SDP
		 */
		if (!(apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
			chg->real_charger_type == POWER_SUPPLY_TYPE_USB))
			chg->real_charger_type = apsd_result->pst;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (chg->real_charger_type > POWER_SUPPLY_TYPE_UNKNOWN) {
		sec_bat_set_cable_type_current(chg->real_charger_type, true);
	}
#endif

	smblib_dbg(chg, PR_MISC, "APSD=%s PD=%d\n",
					apsd_result->name, chg->pd_active);
	return apsd_result;
}

static int smblib_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct smb_charger *chg = container_of(nb, struct smb_charger, nb);

	if (!strcmp(psy->desc->name, "bms")) {
		if (!chg->bms_psy)
			chg->bms_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED)
			schedule_work(&chg->bms_update_work);
	}

	if (chg->jeita_configured == JEITA_CFG_NONE)
		schedule_work(&chg->jeita_update_work);

	if (chg->sec_pl_present && !chg->pl.psy
		&& !strcmp(psy->desc->name, "parallel")) {
		chg->pl.psy = psy;
		schedule_work(&chg->pl_update_work);
	}

	return NOTIFY_OK;
}

static int smblib_register_notifier(struct smb_charger *chg)
{
	int rc;

	chg->nb.notifier_call = smblib_notifier_call;
	rc = power_supply_reg_notifier(&chg->nb);
	if (rc < 0) {
		smblib_err(chg, "Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_mapping_soc_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	*val_raw = val_u << 1;

	return 0;
}

int smblib_mapping_cc_delta_to_field_value(struct smb_chg_param *param,
					   u8 val_raw)
{
	int val_u  = val_raw * param->step_u + param->min_u;

	if (val_u > param->max_u)
		val_u -= param->max_u * 2;

	return val_u;
}

int smblib_mapping_cc_delta_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u - param->max_u)
		return -EINVAL;

	val_u += param->max_u * 2 - param->min_u;
	val_u %= param->max_u * 2;
	*val_raw = val_u / param->step_u;

	return 0;
}

static void smblib_uusb_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	int sec_charger;

	sec_charger = chg->sec_pl_present ? POWER_SUPPLY_CHARGER_SEC_PL :
				POWER_SUPPLY_CHARGER_SEC_NONE;
	smblib_select_sec_charger(chg, sec_charger, POWER_SUPPLY_CP_NONE,
					false);

	cancel_delayed_work_sync(&chg->pl_enable_work);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	cancel_delayed_work_sync(&chg->compliant_check_work);
#endif

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset both usbin current and voltage votes */
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			is_flash_active(chg) ? SDP_CURRENT_UA : SDP_100_MA);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
	vote(chg->usb_icl_votable, THERMAL_THROTTLE_VOTER, false, 0);
	vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
			true, 0);
	vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);

	/* Remove SW thermal regulation WA votes */
	vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER, false, 0);
	if (chg->cp_disable_votable)
		vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
								false, 0);

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usbin_forced_max_uv = 0;
	chg->usb_icl_delta_ua = 0;
	chg->pulse_cnt = 0;
	chg->uusb_apsd_rerun_done = false;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* clear USB ICL vote for USB_PSY_VOTER */
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't un-vote for USB ICL rc=%d\n", rc);

	/* clear USB ICL vote for DCP_VOTER */
	rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg,
			"Couldn't un-vote DCP from USB ICL rc=%d\n", rc);

	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->qc2_unsupported_voltage) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->qc2_unsupported_voltage = QC2_COMPLIANT;
	}
}

void smblib_suspend_on_debug_battery(struct smb_charger *chg)
{
	int rc;
	union power_supply_propval val;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_DEBUG_BATTERY, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get debug battery prop rc=%d\n", rc);
		return;
	}
	if (chg->suspend_input_on_debug_batt) {
		vote(chg->usb_icl_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		vote(chg->dc_suspend_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		if (val.intval)
			pr_info("Input suspended: Fake battery\n");
	} else {
		vote(chg->chg_disable_votable, DEBUG_BOARD_VOTER,
					val.intval, 0);
	}
}

int smblib_rerun_apsd_if_required(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;

	rc = smblib_request_dpdm(chg, true);
	if (rc < 0)
		smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

	chg->uusb_apsd_rerun_done = true;
	smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_get_pulse_cnt(struct smb_charger *chg, int *count)
{
	*count = chg->pulse_cnt;
	return 0;
}

#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_150MA	150000
#define USBIN_500MA	500000
#define USBIN_900MA	900000
static int set_sdp_current(struct smb_charger *chg, int icl_ua)
{
	int rc;
	u8 icl_options;
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* power source is SDP */
	switch (icl_ua) {
	case USBIN_100MA:
		/* USB 2.0 100mA */
		icl_options = 0;
		break;
	case USBIN_150MA:
		/* USB 3.0 150mA */
		icl_options = CFG_USB3P0_SEL_BIT;
		break;
	case USBIN_500MA:
		/* USB 2.0 500mA */
		icl_options = USB51_MODE_BIT;
		break;
	case USBIN_900MA:
		/* USB 3.0 900mA */
		icl_options = CFG_USB3P0_SEL_BIT | USB51_MODE_BIT;
		break;
	default:
		return -EINVAL;
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		/*
		 * change the float charger configuration to SDP, if this
		 * is the case of SDP being detected as FLOAT
		 */
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		pr_info("%s: set FORCE_FLOAT_SDP_CFG_BIT\n", __func__);
#endif
		rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
			FORCE_FLOAT_SDP_CFG_BIT, FORCE_FLOAT_SDP_CFG_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set float ICL options rc=%d\n",
						rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
			CFG_USB3P0_SEL_BIT | USB51_MODE_BIT, icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
		return rc;
	}

	rc = smblib_icl_override(chg, SW_OVERRIDE_USB51_MODE);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		return rc;
	}

	return rc;
}

int smblib_set_icl_current(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	enum icl_override_mode icl_override = HW_AUTO_MODE;
	/* suspend if 25mA or less is requested */
	bool suspend = (icl_ua <= USBIN_25MA);

	pr_info("%s:icl-> %d \n", __func__, icl_ua);

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC) {
		rc = smblib_masked_write(chg, USB_CMD_PULLDOWN_REG,
				EN_PULLDOWN_USB_IN_BIT,
				suspend ? 0 : EN_PULLDOWN_USB_IN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't write %s to EN_PULLDOWN_USB_IN_BIT rc=%d\n",
				suspend ? "disable" : "enable", rc);
			goto out;
		}
	}

	if (suspend)
		return smblib_set_usb_suspend(chg, true);

	if (icl_ua == INT_MAX)
		goto set_mode;

	/* configure current */
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB
		&&
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		&& (get_rid_type() != 6 /*RID_ADC_619K*/))
#else
		(chg->typec_legacy
		|| chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB))
#endif
		{
		rc = set_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set SDP ICL rc=%d\n", rc);
			goto out;
		}
	} else {
		/*
		 * Try USB 2.0/3,0 option first on USB path when maximum input
		 * current limit is 500mA or below for better accuracy; in case
		 * of error, proceed to use USB high-current mode.
		 */
		if (icl_ua <= USBIN_500MA) {
			rc = set_sdp_current(chg, icl_ua);
			if (rc >= 0)
				goto unsuspend;
		}

		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto out;
		}
		icl_override = SW_OVERRIDE_HC_MODE;
	}

set_mode:
	rc = smblib_icl_override(chg, icl_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto out;
	}

unsuspend:
	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto out;
	}

	/* Re-run AICL */
	if (icl_override != SW_OVERRIDE_HC_MODE)
		rc = smblib_run_aicl(chg, RERUN_AICL);
out:
	return rc;
}

int smblib_get_icl_current(struct smb_charger *chg, int *icl_ua)
{
	int rc;

	rc = smblib_get_charge_param(chg, &chg->param.icl_max_stat, icl_ua);
	if (rc < 0)
		smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);

	return rc;
}

int smblib_toggle_smb_en(struct smb_charger *chg, int toggle)
{
	int rc = 0;

	if (!toggle)
		return rc;

	rc = smblib_select_sec_charger(chg, chg->sec_chg_selected,
				chg->cp_reason, true);

	return rc;
}

int smblib_get_irq_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 reg;

	mutex_lock(&chg->irq_status_lock);
	/* Report and clear cached status */
	val->intval = chg->irq_status;
	chg->irq_status = 0;

	/* get real time status of pulse skip irq */
	rc = smblib_read(chg, MISC_PBS_RT_STS_REG, &reg);
	if (rc < 0)
		smblib_err(chg, "Couldn't read MISC_PBS_RT_STS_REG rc=%d\n",
				rc);
	else
		val->intval |= (reg & PULSE_SKIP_IRQ_BIT);
	mutex_unlock(&chg->irq_status_lock);

	return rc;
}

/****************************
 * uUSB Moisture Protection *
 ****************************/
#define MICRO_USB_DETECTION_ON_TIME_20_MS 0x08
#define MICRO_USB_DETECTION_PERIOD_X_100 0x03
#define U_USB_STATUS_WATER_PRESENT 0x00
static int smblib_set_moisture_protection(struct smb_charger *chg,
				bool enable)
{
	int rc = 0;

	if (chg->moisture_present == enable) {
		smblib_dbg(chg, PR_MISC, "No change in moisture protection status\n");
		return rc;
	}

	if (enable) {
		chg->moisture_present = true;

		/* Disable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}

		/* Disable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable moisture detection interrupt rc=%d\n",
			rc);
			return rc;
		}

		/* Set 1% duty cycle on ID detection */
		rc = smblib_masked_write(chg,
				((chg->smb_version == PMI632_SUBTYPE) ?
				PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
				TYPEC_U_USB_WATER_PROTECTION_CFG_REG),
				EN_MICRO_USB_WATER_PROTECTION_BIT |
				MICRO_USB_DETECTION_ON_TIME_CFG_MASK |
				MICRO_USB_DETECTION_PERIOD_CFG_MASK,
				EN_MICRO_USB_WATER_PROTECTION_BIT |
				MICRO_USB_DETECTION_ON_TIME_20_MS |
				MICRO_USB_DETECTION_PERIOD_X_100);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		vote(chg->usb_icl_votable, MOISTURE_VOTER, true, 0);
	} else {
		chg->moisture_present = false;
		vote(chg->usb_icl_votable, MOISTURE_VOTER, false, 0);

		/* Enable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable moisture detection and uUSB state change interrupt rc=%d\n",
				rc);
			return rc;
		}

		/* Disable periodic monitoring of CC_ID pin */
		rc = smblib_write(chg, ((chg->smb_version == PMI632_SUBTYPE) ?
				PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
				TYPEC_U_USB_WATER_PROTECTION_CFG_REG), 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		/* Enable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}
	}

	smblib_dbg(chg, PR_MISC, "Moisture protection %s\n",
			chg->moisture_present ? "enabled" : "disabled");
	return rc;
}

/*********************
 * VOTABLE CALLBACKS *
 *********************/
static int smblib_smb_disable_override_vote_callback(struct votable *votable,
			void *data, int disable_smb, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;

	/* Enable/disable SMB_EN pin */
	rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
			SMB_EN_OVERRIDE_BIT | SMB_EN_OVERRIDE_VALUE_BIT,
			disable_smb ? SMB_EN_OVERRIDE_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't configure SMB_EN, rc=%d\n", rc);

	return rc;
}

static int smblib_dc_suspend_vote_callback(struct votable *votable, void *data,
			int suspend, const char *client)
{
	struct smb_charger *chg = data;

	if (chg->smb_version == PMI632_SUBTYPE)
		return 0;

	/* resume input if suspend is invalid */
	if (suspend < 0)
		suspend = 0;

	return smblib_set_dc_suspend(chg, (bool)suspend);
}

static int smblib_awake_vote_callback(struct votable *votable, void *data,
			int awake, const char *client)
{
	struct smb_charger *chg = data;

	if (awake)
		pm_stay_awake(chg->dev);
	else
		pm_relax(chg->dev);

	return 0;
}

static int smblib_chg_disable_vote_callback(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_SEC_FACTORY)
	if(factory_mode)
		return rc;
#endif

	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 chg_disable ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s charging rc=%d\n",
			chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int smblib_hdc_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		return 0;

	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	}

	chg->irq_info[HIGH_DUTY_CYCLE_IRQ].enabled = !disable;

	return 0;
}

static int smblib_limited_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq)
		return 0;

	if (chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(
				chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
	}

	chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].enabled = !disable;

	return 0;
}

static int smblib_icl_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq)
		return 0;

	if (chg->irq_info[USBIN_ICL_CHANGE_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	}

	chg->irq_info[USBIN_ICL_CHANGE_IRQ].enabled = !disable;

	return 0;
}

/*******************
 * VCONN REGULATOR *
 * *****************/

int smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 stat, orientation;

	smblib_dbg(chg, PR_OTG, "enabling VCONN\n");

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}

	/* VCONN orientation is opposite to that of CC */
	orientation =
		stat & TYPEC_CCOUT_VALUE_BIT ? 0 : VCONN_EN_ORIENTATION_BIT;
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT,
				VCONN_EN_VALUE_BIT | orientation);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
			rc);
		return rc;
	}

	return 0;
}

int smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	smblib_dbg(chg, PR_OTG, "disabling VCONN\n");
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable vconn regulator rc=%d\n", rc);

	return 0;
}

int smblib_vconn_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;
	u8 cmd;

	rc = smblib_read(chg, TYPE_C_VCONN_CONTROL_REG, &cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			rc);
		return rc;
	}

	return (cmd & VCONN_EN_VALUE_BIT) ? 1 : 0;
}

/*****************
 * OTG REGULATOR *
 *****************/

int smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "enabling OTG\n");

	rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG, OTG_EN_BIT, OTG_EN_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "disabling OTG\n");

	rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG, OTG_EN_BIT, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable OTG regulator rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_vbus_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 cmd;

	rc = smblib_read(chg, DCDC_CMD_OTG_REG, &cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CMD_OTG rc=%d", rc);
		return rc;
	}

	return (cmd & OTG_EN_BIT) ? 1 : 0;
}

/********************
 * BATT PSY GETTERS *
 ********************/

int smblib_get_prop_input_suspend(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	val->intval
		= (get_client_vote(chg->usb_icl_votable, USER_VOTER) == 0)
		 && get_client_vote(chg->dc_suspend_votable, USER_VOTER);
	return 0;
}

int smblib_get_prop_batt_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));

	return rc;
}

int smblib_get_prop_batt_capacity(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc = -EINVAL;

	if (chg->fake_capacity >= 0) {
		val->intval = chg->fake_capacity;
		return 0;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CAPACITY, val);

	return rc;
}

static bool is_charging_paused(struct smb_charger *chg)
{
	int rc;
	u8 val;

	rc = smblib_read(chg, CHARGING_PAUSE_CMD_REG, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHARGING_PAUSE_CMD rc=%d\n", rc);
		return false;
	}

	return val & CHARGING_PAUSE_CMD_BIT;
}

int smblib_get_prop_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online;
	u8 stat;
	int rc, suspend = 0;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	int soc;
	bool ignore_not_charging_flag = false;
	u8 stat1, pp_stat;
#endif

	if (chg->dbc_usbov) {
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb present prop rc=%d\n", rc);
			return rc;
		}

		rc = smblib_get_usb_suspend(chg, &suspend);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb suspend rc=%d\n", rc);
			return rc;
		}

		/*
		 * Report charging as long as USBOV is not debounced and
		 * charging path is un-suspended.
		 */
		if (pval.intval && !suspend) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &pp_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	//pr_info("%s: power_path_status = 0x%02x\n", pp_stat);
#endif

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_1 = 0x%02x\n",
		   stat);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	stat1 = stat;
#endif

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	smblib_get_prop_batt_capacity(chg, &pval);
	soc = pval.intval;
	if (chg->ta_alert_mode) {
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		pr_info("%s: ta_alert : %d\n", __func__, chg->ta_alert_mode);
		return 0;
	}
#endif
	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		pr_info("%s: power_path_status = 0x%02x, status_1 = 0x%02x, batt_status = %d\n",
				__func__, pp_stat, stat, val->intval);
#endif
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (soc < 100) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		} else
#endif
			val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		rc = power_supply_get_property(chg->batt_psy,
				POWER_SUPPLY_PROP_HEALTH, &pval);
		if (rc < 0) {
			pr_info("%s: Fail to get health prop. rc=%d\n", __func__, rc);
			pval.intval = POWER_SUPPLY_HEALTH_GOOD;
		}

		smblib_get_usb_suspend(chg, &suspend);

		if (chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		} else if (!get_effective_result(chg->chg_disable_votable) && !suspend && (pval.intval == POWER_SUPPLY_HEALTH_GOOD)) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			ignore_not_charging_flag = true;
		} else {
			if (get_effective_result(chg->chg_disable_votable) &&
					!strcmp(get_effective_client(chg->chg_disable_votable), FORCE_RECHARGE_VOTER) &&
					(pval.intval == POWER_SUPPLY_HEALTH_GOOD)) {
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
				ignore_not_charging_flag = true;
			}
			else
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		}
		break;
#endif
	case PAUSE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (is_charging_paused(chg)) {
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if ((val->intval == POWER_SUPPLY_STATUS_CHARGING) && (soc == 100)) {
		pr_info("[%s] soc 100 => FULL\n", __func__);
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}
#endif
	/*
	 * If charge termination WA is active and has suspended charging, then
	 * continue reporting charging status as FULL.
	 */
	if (is_client_vote_enabled(chg->usb_icl_votable,
						CHG_TERMINATION_VOTER)) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	if (!usb_online && dc_online
		&& chg->fake_batt_status == POWER_SUPPLY_STATUS_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_5 rc=%d\n",
				rc);
			return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_5 = 0x%02x\n",
		   stat);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: power_path_status = 0x%02x, status_1 = 0x%02x, status_5 = 0x%02x\n",
			__func__, pp_stat, stat1, stat);
#endif

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (ignore_not_charging_flag)
		return 0;
#endif

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
						ENABLE_FULLON_MODE_BIT;

	if (!stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

int smblib_get_prop_batt_charge_type(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case FULLON_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (settled_icl < SLOW_CHARGING_CURRENT_STANDARD  && val->intval != POWER_SUPPLY_CHARGE_TYPE_NONE
			&& chg->real_charger_type != POWER_SUPPLY_TYPE_USB_PD && settled_icl != 0) {
		pr_info("%s: slow charging on \n",__func__);
		val->intval = POWER_SUPPLY_CHARGE_TYPE_SLOW;
	}
#endif

	return rc;
}

int smblib_get_prop_batt_health(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc;
	int effective_fv_uv;
	u8 stat;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	u32 jeita_thresholds[2] = {0,};
	static int pre_health = POWER_SUPPLY_HEALTH_GOOD;
	union power_supply_propval value = {0, };
	u8 stat2;
#endif

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_2 = 0x%02x\n",
		   stat);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	stat2 = stat;
#endif

	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (!rc) {
			/*
			 * If Vbatt is within 40mV above Vfloat, then don't
			 * treat it as overvoltage.
			 */
			effective_fv_uv = get_effective_result(chg->fv_votable);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
			power_supply_get_property(chg->batt_psy,
					(enum power_supply_property)POWER_SUPPLY_EXT_PROP_CHG_SWELLING_STATE, &value);
			if (value.intval == swelling_index[swelling_type][JEITA_WARM]) {
				power_supply_get_property(chg->batt_psy,
						POWER_SUPPLY_PROP_VOLTAGE_MAX, &value);
				if (pval.intval >= value.intval + 40000) {
					val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
					smblib_err(chg, "battery over-voltage in warm swelling mode vbat_fg = %duV, fv = %duV\n",
							pval.intval, value.intval);
					goto done;
				}
			} else
#endif
			if (pval.intval >= effective_fv_uv + 40000) {
				val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
				smblib_err(chg, "battery over-voltage vbat_fg = %duV, fv = %duV\n",
						pval.intval, effective_fv_uv);
				goto done;
			}
		}
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_7 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_7 = 0x%02x\n",
		   stat);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: status_2 = 0x%02x, status_7 = 0x%02x\n", __func__, stat2, stat);
#endif

	if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (pre_health != val->intval) {
		pr_err("Battery health change! (%d --> %d)\n", pre_health, val->intval);
		if (val->intval == POWER_SUPPLY_HEALTH_OVERHEAT) {
			jeita_thresholds[0] = chg->jeita_cold_trigger;
			jeita_thresholds[1] = chg->jeita_hot_release;
		} else if (val->intval == POWER_SUPPLY_HEALTH_COLD) {
			jeita_thresholds[0] = chg->jeita_cold_release;
			jeita_thresholds[1] = chg->jeita_hot_trigger;
		} else {
			jeita_thresholds[0] = chg->jeita_cold_trigger;
			jeita_thresholds[1] = chg->jeita_hot_trigger;
		}
		rc = smblib_update_jeita(chg, jeita_thresholds, 1); /* 1 means JEITA_HARD */
		if (rc < 0)
			pr_err("Couldn't configure Hard Jeita rc=%d\n", rc);
		pre_health = val->intval;
	}
#endif
done:
	return rc;
}

int smblib_get_prop_system_temp_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->system_temp_level;
	return 0;
}

int smblib_get_prop_system_temp_level_max(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->thermal_levels;
	return 0;
}

int smblib_get_prop_input_current_limited(struct smb_charger *chg,
				union power_supply_propval *val)
{
	u8 stat;
	int rc;

	if (chg->fake_input_current_limited >= 0) {
		val->intval = chg->fake_input_current_limited;
		return 0;
	}

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return rc;
	}
	val->intval = (stat & SOFT_ILIMIT_BIT) || chg->is_hdc;
	return 0;
}

int smblib_get_prop_batt_iterm(struct smb_charger *chg,
		union power_supply_propval *val)
{
	int rc, temp;
	u8 stat, buf[2];

	/*
	 * Currently, only ADC comparator-based termination is supported,
	 * hence read only the threshold corresponding to ADC source.
	 * Proceed only if CHGR_ITERM_USE_ANALOG_BIT is 0.
	 */
	rc = smblib_read(chg, CHGR_ENG_CHARGING_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHGR_ENG_CHARGING_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	if (stat & CHGR_ITERM_USE_ANALOG_BIT) {
		val->intval = -EINVAL;
		return 0;
	}

	rc = smblib_batch_read(chg, CHGR_ADC_ITERM_UP_THD_MSB_REG, buf, 2);

	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHGR_ADC_ITERM_UP_THD_MSB_REG rc=%d\n",
				rc);
		return rc;
	}

	temp = buf[1] | (buf[0] << 8);
	temp = sign_extend32(temp, 15);

	if (chg->smb_version == PMI632_SUBTYPE)
		temp = DIV_ROUND_CLOSEST(temp * ITERM_LIMITS_PMI632_MA,
					ADC_CHG_ITERM_MASK);
	else
		temp = DIV_ROUND_CLOSEST(temp * ITERM_LIMITS_PM8150B_MA,
					ADC_CHG_ITERM_MASK);

	val->intval = temp;

	return rc;
}

int smblib_get_prop_batt_charge_done(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	int soc;
	union power_supply_propval pval = {0, };
#endif

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	val->intval = (stat == TERMINATE_CHARGE);

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	smblib_get_prop_batt_capacity(chg, &pval);
	soc = pval.intval;

	if ((val->intval == 1) && (soc == 100)) {	// reach to 2nd termination current
		if(lpcharge && (!chg->vbus_chg_by_full)) {
			chg->input_voltage_limit = MICRO_5V;
			pr_info("[%s]: Voltage max limit set 5V by vbus_chg_by_full\n", __func__);
#if defined(CONFIG_AFC)
			if(chg->afc_sts == AFC_9V)
				afc_set_voltage(SET_5V);
#endif
			chg->vbus_chg_by_full = true;
		}
	}
#endif

	return 0;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#define SKIP_MODE_BIT	BIT(4)
int smblib_get_prop_skip_mode_status(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, MISC_PBS_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	if(stat & SKIP_MODE_BIT)
		val->intval = 1;
	else
		val->intval = 0;
	return 0;
}
#endif

/***********************
 * BATTERY PSY SETTERS *
 ***********************/

int smblib_set_prop_input_suspend(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc;

	/* vote 0mA when suspended */
	rc = vote(chg->usb_icl_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s USB rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	power_supply_changed(chg->batt_psy);
	return rc;
}

int smblib_set_prop_batt_capacity(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_capacity = val->intval;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_status(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	/* Faking battery full */
	if (val->intval == POWER_SUPPLY_STATUS_FULL)
		chg->fake_batt_status = val->intval;
	else
		chg->fake_batt_status = -EINVAL;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_system_temp_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;

	if (chg->system_temp_level == chg->thermal_levels)
		return vote(chg->chg_disable_votable,
			THERMAL_DAEMON_VOTER, true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	if (chg->system_temp_level == 0)
		return vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);

	vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
			chg->thermal_mitigation[chg->system_temp_level]);
	return 0;
}

int smblib_set_prop_input_current_limited(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_input_current_limited = val->intval;
	return 0;
}

int smblib_set_prop_rechg_soc_thresh(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u8 new_thr = DIV_ROUND_CLOSEST(val->intval * 255, 100);

	rc = smblib_write(chg, CHARGE_RCHG_SOC_THRESHOLD_CFG_REG,
			new_thr);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write to RCHG_SOC_THRESHOLD_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	chg->auto_recharge_soc = val->intval;

	return rc;
}

int smblib_run_aicl(struct smb_charger *chg, int type)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
								rc);
		return rc;
	}

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return rc;

	smblib_dbg(chg, PR_MISC, "re-running AICL\n");

	stat = (type == RERUN_AICL) ? RERUN_AICL_BIT : RESTART_AICL_BIT;
	rc = smblib_masked_write(chg, AICL_CMD_REG, stat, stat);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to AICL_CMD_REG rc=%d\n",
				rc);
	return 0;
}

static int smblib_dp_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 increment */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_INCREMENT_BIT,
			SINGLE_INCREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_dm_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 decrement */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_DECREMENT_BIT,
			SINGLE_DECREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

int smblib_force_vbus_voltage(struct smb_charger *chg, u8 val)
{
	int rc;

	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, val, val);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static void smblib_hvdcp_set_fsw(struct smb_charger *chg, int bit)
{
	switch (bit) {
	case QC_5V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
		break;
	case QC_9V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
		break;
	case QC_12V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);
		break;
	default:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_removal);
		break;
	}
}

#define QC3_PULSES_FOR_6V	5
#define QC3_PULSES_FOR_9V	20
#define QC3_PULSES_FOR_12V	35
static int smblib_hvdcp3_set_fsw(struct smb_charger *chg)
{
	int pulse_count, rc;

	rc = smblib_get_pulse_cnt(chg, &pulse_count);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
		return rc;
	}

	if (pulse_count < QC3_PULSES_FOR_6V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
	else if (pulse_count < QC3_PULSES_FOR_9V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
	else if (pulse_count < QC3_PULSES_FOR_12V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
	else
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);

	return 0;
}

static void smblib_hvdcp_adaptive_voltage_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			return;
		}

		smblib_hvdcp_set_fsw(chg, stat & QC_2P0_STATUS_MASK);
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		rc = smblib_hvdcp3_set_fsw(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't set QC3.0 Fsw rc=%d\n", rc);
	}

	power_supply_changed(chg->usb_main_psy);
}

int smblib_dp_dm(struct smb_charger *chg, int val)
{
	int target_icl_ua, rc = 0;
	union power_supply_propval pval;
	u8 stat;

	switch (val) {
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		/*
		 * Pre-emptively increment pulse count to enable the setting
		 * of FSW prior to increasing voltage.
		 */
		chg->pulse_cnt++;

		rc = smblib_hvdcp3_set_fsw(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't set QC3.0 Fsw rc=%d\n", rc);

		rc = smblib_dp_pulse(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't increase pulse count rc=%d\n",
				rc);
			/*
			 * Increment pulse count failed;
			 * reset to former value.
			 */
			chg->pulse_cnt--;
		}

		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DP_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		rc = smblib_dm_pulse(chg);
		if (!rc && chg->pulse_cnt)
			chg->pulse_cnt--;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DM_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
		target_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (target_icl_ua < 0) {
			/* no client vote, get the ICL from charger */
			rc = power_supply_get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_HW_CURRENT_MAX,
					&pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get max curr rc=%d\n",
					rc);
				return rc;
			}
			target_icl_ua = pval.intval;
		}

		/*
		 * Check if any other voter voted on USB_ICL in case of
		 * voter other than SW_QC3_VOTER reset and restart reduction
		 * again.
		 */
		if (target_icl_ua != get_client_vote(chg->usb_icl_votable,
							SW_QC3_VOTER))
			chg->usb_icl_delta_ua = 0;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if((target_icl_ua - 100000) >= 500000/*500mA*/) {
#endif
			chg->usb_icl_delta_ua += 100000;
			vote(chg->usb_icl_votable, SW_QC3_VOTER, true,
						target_icl_ua - 100000);
			smblib_dbg(chg, PR_PARALLEL, "ICL DOWN ICL=%d reduction=%d\n",
					target_icl_ua, chg->usb_icl_delta_ua);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		}
		else {
			smblib_dbg(chg, PR_PARALLEL, "Skip ICL DOWN ICL if (%d - 100000) is lower than 500mA", target_icl_ua);
		}
#endif
		break;
	case POWER_SUPPLY_DP_DM_FORCE_5V:
#if defined(CONFIG_AFC)
		if (chg->afc_sts >= AFC_5V) {
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, 500000);  /* 500mA for AFC communication */
			afc_set_voltage(SET_5V);
		} else
#endif
		{
			rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
			if (rc < 0)
				pr_err("Failed to force 5V\n");
		}
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->hv_disable) {
			pr_err("%s: DP_DM_FORCE_9V but HV is DISABLE\n", __func__);
			return -EINVAL;
		}
#endif
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			smblib_err(chg, "Couldn't set 9V: unsupported\n");
			return -EINVAL;
		}

		/* If we are increasing voltage to get to 9V, set FSW first */
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
			break;
		}

		if (stat & QC_5V_BIT) {
			/* Force 1A ICL before requesting higher voltage */
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER,
					true, 1000000);
			smblib_hvdcp_set_fsw(chg, QC_9V_BIT);
		}

#if defined(CONFIG_AFC)
		if(chg->afc_sts >= AFC_5V) {
			rc = smblib_set_adapter_allowance(chg,
					USBIN_ADAPTER_ALLOW_5V_TO_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_TO_9V rc=%d\n", rc);
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, 500000);    /*  500mA for AFC communication  */
			afc_set_voltage(SET_9V);
		} else
#endif
		{
			rc = smblib_force_vbus_voltage(chg, FORCE_9V_BIT);
			if (rc < 0)
				pr_err("Failed to force 9V\n");

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
				schedule_delayed_work(&chg->compliant_check_work,
					msecs_to_jiffies(2000));
#endif
		}
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_12V) {
			smblib_err(chg, "Couldn't set 12V: unsupported\n");
			return -EINVAL;
		}

		/* If we are increasing voltage to get to 12V, set FSW first */
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
			break;
		}

		if ((stat & QC_9V_BIT) || (stat & QC_5V_BIT)) {
			/* Force 1A ICL before requesting higher voltage */
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER,
					true, 1000000);
			smblib_hvdcp_set_fsw(chg, QC_12V_BIT);
		}

		rc = smblib_force_vbus_voltage(chg, FORCE_12V_BIT);
		if (rc < 0)
			pr_err("Failed to force 12V\n");
		break;
	case POWER_SUPPLY_DP_DM_ICL_UP:
	default:
		break;
	}

	return rc;
}

int smblib_disable_hw_jeita(struct smb_charger *chg, bool disable)
{
	int rc;
	u8 mask;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG,	JEITA_EN_HARDLIMIT_BIT,
			chg->hard_jeita_enabled ? JEITA_EN_HARDLIMIT_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set hard jeita, rc=%d\n", rc);
#endif
	/*
	 * Disable h/w base JEITA compensation if s/w JEITA is enabled
	 */
	mask = JEITA_EN_COLD_SL_FCV_BIT
		| JEITA_EN_HOT_SL_FCV_BIT
		| JEITA_EN_HOT_SL_CCC_BIT
		| JEITA_EN_COLD_SL_CCC_BIT,
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG, mask,
			disable ? 0 : mask);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure s/w jeita rc=%d\n",
				rc);
		return rc;
	}

	return 0;
}

static int smblib_set_sw_thermal_regulation(struct smb_charger *chg,
						bool enable)
{
	int rc = 0;

	if (!(chg->wa_flags & SW_THERM_REGULATION_WA))
		return rc;

	if (enable) {
		/*
		 * Configure min time to quickly address thermal
		 * condition.
		 */
		rc = smblib_masked_write(chg, SNARL_BARK_BITE_WD_CFG_REG,
			SNARL_WDOG_TIMEOUT_MASK, SNARL_WDOG_TMOUT_62P5MS);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure snarl wdog tmout, rc=%d\n",
					rc);
			return rc;
		}

		/*
		 * Schedule SW_THERM_REGULATION_WORK directly if USB input
		 * is suspended due to SW thermal regulation WA since WDOG
		 * IRQ won't trigger with input suspended.
		 */
		if (is_client_vote_enabled(chg->usb_icl_votable,
						SW_THERM_REGULATION_VOTER)) {
			vote(chg->awake_votable, SW_THERM_REGULATION_VOTER,
								true, 0);
			schedule_delayed_work(&chg->thermal_regulation_work, 0);
		}
	} else {
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, false, 0);
	}

	smblib_dbg(chg, PR_MISC, "WDOG SNARL INT %s\n",
				enable ? "Enabled" : "Disabled");

	return rc;
}

static int smblib_update_thermal_readings(struct smb_charger *chg)
{
	union power_supply_propval pval = {0, };
	int rc = 0;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	int temp_adc = 0, temp_data = 0;
#endif

	if (!chg->pl.psy)
		chg->pl.psy = power_supply_get_by_name("parallel");

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (sec_bat_use_chg_temp_adc_table()) {
		rc = iio_read_channel_raw(chg->iio.die_temp_chan, &temp_adc);
		if (rc < 0) {
			pr_err("Failed reading CHG_TEMP(DIE_TEMP) ADC RAW. rc=%d\n", rc);
			return rc;
		}
		pr_debug("%s: chg_temp_adc_raw = %d\n", __func__, temp_adc);

		temp_data = sec_bat_convert_adc_to_temp(SEC_BAT_ADC_CHANNEL_CHG_TEMP, temp_adc);
		chg->die_temp = temp_data;
	}
	else {
#endif

	rc = smblib_read_iio_channel(chg, chg->iio.die_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->die_temp);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	}
#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	if (chg->batt_test_chg_temp > (-500))
		chg->die_temp = chg->batt_test_chg_temp;
#endif
#endif
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE TEMP channel, rc=%d\n", rc);
		return rc;
	}

	rc = smblib_read_iio_channel(chg, chg->iio.connector_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->connector_temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CONN TEMP channel, rc=%d\n", rc);
		return rc;
	}

	rc = smblib_read_iio_channel(chg, chg->iio.skin_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->skin_temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SKIN TEMP channel, rc=%d\n", rc);
		return rc;
	}

	if (chg->sec_chg_selected == POWER_SUPPLY_CHARGER_SEC_CP) {
		if (!chg->cp_psy)
			chg->cp_psy =
				power_supply_get_by_name("charge_pump_master");
		if (chg->cp_psy) {
			rc = power_supply_get_property(chg->cp_psy,
				POWER_SUPPLY_PROP_CP_DIE_TEMP, &pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get smb1390 charger temp, rc=%d\n",
					rc);
				return rc;
			}
			chg->smb_temp = pval.intval;
		} else {
			smblib_dbg(chg, PR_MISC, "Coudln't find cp_psy\n");
			chg->smb_temp = -ENODATA;
		}
	} else if (chg->pl.psy && chg->sec_chg_selected ==
					POWER_SUPPLY_CHARGER_SEC_PL) {
		rc = power_supply_get_property(chg->pl.psy,
				POWER_SUPPLY_PROP_CHARGER_TEMP, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get smb1355 charger temp, rc=%d\n",
					rc);
			return rc;
		}
		chg->smb_temp = pval.intval;
	} else {
		chg->smb_temp = -ENODATA;
	}

	return rc;
}

/* SW thermal regulation thresholds in deciDegC */
#define DIE_TEMP_RST_THRESH		1000
#define DIE_TEMP_REG_H_THRESH		800
#define DIE_TEMP_REG_L_THRESH		700

#define CONNECTOR_TEMP_SHDN_THRESH	980
#define CONNECTOR_TEMP_RST_THRESH	970
#define CONNECTOR_TEMP_REG_H_THRESH	960
#define CONNECTOR_TEMP_REG_L_THRESH	950

#define SMB_TEMP_SHDN_THRESH		1400
#define SMB_TEMP_RST_THRESH		900
#define SMB_TEMP_REG_H_THRESH		800
#define SMB_TEMP_REG_L_THRESH		700

#define SKIN_TEMP_SHDN_THRESH		700
#define SKIN_TEMP_RST_THRESH		600
#define SKIN_TEMP_REG_H_THRESH		550
#define SKIN_TEMP_REG_L_THRESH		500

#define THERM_REG_RECHECK_DELAY_1S	1000	/* 1 sec */
#define THERM_REG_RECHECK_DELAY_8S	8000	/* 8 sec */
static int smblib_process_thermal_readings(struct smb_charger *chg)
{
	int rc = 0, wdog_timeout = SNARL_WDOG_TMOUT_8S;
	u32 thermal_status = TEMP_BELOW_RANGE;
	bool suspend_input = false, disable_smb = false;

	/*
	 * Following is the SW thermal regulation flow:
	 *
	 * TEMP_SHUT_DOWN_LEVEL: If either connector temp or skin temp
	 * exceeds their respective SHDN threshold. Need to suspend input
	 * and secondary charger.
	 *
	 * TEMP_SHUT_DOWN_SMB_LEVEL: If smb temp exceed its SHDN threshold
	 * but connector and skin temp are below it. Need to suspend SMB.
	 *
	 * TEMP_ALERT_LEVEL: If die, connector, smb or skin temp exceeds it's
	 * respective RST threshold. Stay put and monitor temperature closely.
	 *
	 * TEMP_ABOVE_RANGE or TEMP_WITHIN_RANGE or TEMP_BELOW_RANGE: If die,
	 * connector, smb or skin temp exceeds it's respective REG_H or REG_L
	 * threshold. Unsuspend input and SMB.
	 */

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (chg->charging_test_mode) {
		smblib_err(chg, "chg->charging_test_mode: %d\n", chg->charging_test_mode);
		return 0;
	}
#endif

	if (chg->connector_temp > (*chg->connector_temp_shdn_thresh) ||
		chg->skin_temp > (*chg->skin_temp_shdn_thresh)) {
		thermal_status = TEMP_SHUT_DOWN;
		switch(*chg->snarl_delay_alert) {
		case 125:
			wdog_timeout = 1 << 4;
			break;
		case 250:
			wdog_timeout = 2 << 4;
			break;
		case 500:
			wdog_timeout = 3 << 4;
			break;
		case 1:
			wdog_timeout = 4 << 4;
			break;
		case 2:
			wdog_timeout = 5 << 4;
			break;
		case 4:
			wdog_timeout = 6 << 4;
			break;
		case 8:
			wdog_timeout = 7 << 4;
			break;
		default:
			wdog_timeout = 7 << 4;
			break;
		};
		suspend_input = true;
		disable_smb = true;
		goto out;
	}

	if (chg->smb_temp > (*chg->smb_temp_shdn_thresh)) {
		thermal_status = TEMP_SHUT_DOWN_SMB;
		switch(*chg->snarl_delay_alert) {
		case 125:
			wdog_timeout = 1 << 4;
			break;
		case 250:
			wdog_timeout = 2 << 4;
			break;
		case 500:
			wdog_timeout = 3 << 4;
			break;
		case 1:
			wdog_timeout = 4 << 4;
			break;
		case 2:
			wdog_timeout = 5 << 4;
			break;
		case 4:
			wdog_timeout = 6 << 4;
			break;
		case 8:
			wdog_timeout = 7 << 4;
			break;
		default:
			wdog_timeout = 7 << 4;
			break;
		};
		disable_smb = true;
		goto out;
	}

	if (chg->connector_temp > (*chg->connector_temp_rst_thresh) ||
			chg->skin_temp > (*chg->skin_temp_rst_thresh) ||
			chg->smb_temp > (*chg->smb_temp_rst_thresh) ||
			chg->die_temp > (*chg->die_temp_rst_thresh)) {
		thermal_status = TEMP_ALERT_LEVEL;
		switch(*chg->snarl_delay_alert) {
		case 125:
			wdog_timeout = 1 << 4;
			break;
		case 250:
			wdog_timeout = 2 << 4;
			break;
		case 500:
			wdog_timeout = 3 << 4;
			break;
		case 1:
			wdog_timeout = 4 << 4;
			break;
		case 2:
			wdog_timeout = 5 << 4;
			break;
		case 4:
			wdog_timeout = 6 << 4;
			break;
		case 8:
			wdog_timeout = 7 << 4;
			break;
		default:
			wdog_timeout = 7 << 4;
			break;
		};
		goto out;
	}

	if (chg->connector_temp > (*chg->connector_temp_reg_h_thresh) ||
			chg->skin_temp > (*chg->skin_temp_reg_h_thresh) ||
			chg->smb_temp > (*chg->smb_temp_reg_h_thresh) ||
			chg->die_temp > (*chg->die_temp_reg_h_thresh)) {
		thermal_status = TEMP_ABOVE_RANGE;
		switch(*chg->snarl_delay_above_range) {
		case 125:
			wdog_timeout = 1 << 4;
			break;
		case 250:
			wdog_timeout = 2 << 4;
			break;
		case 500:
			wdog_timeout = 3 << 4;
			break;
		case 1:
			wdog_timeout = 4 << 4;
			break;
		case 2:
			wdog_timeout = 5 << 4;
			break;
		case 4:
			wdog_timeout = 6 << 4;
			break;
		case 8:
			wdog_timeout = 7 << 4;
			break;
		default:
			wdog_timeout = 7 << 4;
			break;
		};
		goto out;
	}

	if (chg->connector_temp > (*chg->connector_temp_reg_l_thresh) ||
			chg->skin_temp > (*chg->skin_temp_reg_l_thresh) ||
			chg->smb_temp > (*chg->smb_temp_reg_l_thresh) ||
			chg->die_temp > (*chg->die_temp_reg_l_thresh)) {
		thermal_status = TEMP_WITHIN_RANGE;
		switch(*chg->snarl_delay_within_range) {
		case 125:
			wdog_timeout = 1 << 4;
			break;
		case 250:
			wdog_timeout = 2 << 4;
			break;
		case 500:
			wdog_timeout = 3 << 4;
			break;
		case 1:
			wdog_timeout = 4 << 4;
			break;
		case 2:
			wdog_timeout = 5 << 4;
			break;
		case 4:
			wdog_timeout = 6 << 4;
			break;
		case 8:
			wdog_timeout = 7 << 4;
			break;
		default:
			wdog_timeout = 7 << 4;
			break;
		};
	}
out:
	smblib_dbg(chg, PR_MISC, "Current temperatures: \tDIE_TEMP: %d,\tCONN_TEMP: %d,\tSMB_TEMP: %d,\tSKIN_TEMP: %d\nTHERMAL_STATUS: %d\n",
			chg->die_temp, chg->connector_temp, chg->smb_temp,
			chg->skin_temp, thermal_status);

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	sec_bat_run_monitor_work();
#endif

	if (thermal_status != chg->thermal_status) {
		chg->thermal_status = thermal_status;
		/*
		 * If thermal level changes to TEMP ALERT LEVEL, don't
		 * enable/disable main/parallel charging.
		 */
		if (chg->thermal_status == TEMP_ALERT_LEVEL)
			goto exit;

		vote(chg->smb_override_votable, SW_THERM_REGULATION_VOTER,
				disable_smb, 0);

		/*
		 * Enable/disable secondary charger through votables to ensure
		 * that if SMB_EN pin get's toggled somehow, secondary charger
		 * remains enabled/disabled according to SW thermal regulation.
		 */
		if (!chg->cp_disable_votable)
			chg->cp_disable_votable = find_votable("CP_DISABLE");
		if (chg->cp_disable_votable)
			vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
							disable_smb, 0);

		vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER,
							disable_smb, 0);
		smblib_dbg(chg, PR_MISC, "Parallel %s as per SW thermal regulation\n",
				disable_smb ? "disabled" : "enabled");

		/*
		 * If thermal level changes to TEMP_SHUT_DOWN_SMB, don't
		 * enable/disable main charger.
		 */
		if (chg->thermal_status == TEMP_SHUT_DOWN_SMB)
			goto exit;

		/* Suspend input if SHDN threshold reached */
		vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER,
							suspend_input, 0);
		vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER,
							suspend_input, 0);
		smblib_dbg(chg, PR_MISC, "USB/DC %s as per SW thermal regulation\n",
				suspend_input ? "suspended" : "unsuspended");
	}
exit:
	/*
	 * On USB suspend, WDOG IRQ stops triggering. To continue thermal
	 * monitoring and regulation until USB is plugged out, reschedule
	 * the SW thermal regulation work without releasing the wake lock.
	 */
	if (is_client_vote_enabled(chg->usb_icl_votable,
					SW_THERM_REGULATION_VOTER)) {
		schedule_delayed_work(&chg->thermal_regulation_work,
				msecs_to_jiffies(THERM_REG_RECHECK_DELAY_1S));
		return 0;
	}

	rc = smblib_masked_write(chg, SNARL_BARK_BITE_WD_CFG_REG,
			SNARL_WDOG_TIMEOUT_MASK, wdog_timeout);
	if (rc < 0)
		smblib_err(chg, "Couldn't set WD SNARL timer, rc=%d\n", rc);

	vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, false, 0);
	return rc;
}

/*******************
 * DC PSY GETTERS *
 *******************/

int smblib_get_prop_voltage_wls_output(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_get_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't get POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);

	return rc;
}

int smblib_get_prop_dc_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	if (chg->smb_version == PMI632_SUBTYPE) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_dc_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (chg->smb_version == PMI632_SUBTYPE) {
		val->intval = 0;
		return 0;
	}

	if (get_client_vote(chg->dc_suspend_votable, USER_VOTER)) {
		val->intval = false;
		return rc;
	}

	if (is_client_vote_enabled(chg->dc_suspend_votable,
						CHG_TERMINATION_VOTER)) {
		rc = smblib_get_prop_dc_present(chg, val);
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_DCIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);

	return rc;
}

int smblib_get_prop_dc_current_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.dc_icl, &val->intval);
}

int smblib_get_prop_dc_voltage_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	val->intval = MICRO_12V;
	return 0;
}

int smblib_get_prop_dc_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_get_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't get POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);
	return rc;
}

/*******************
 * DC PSY SETTERS *
 *******************/

int smblib_set_prop_dc_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	return smblib_set_charge_param(chg, &chg->param.dc_icl, val->intval);
}

int smblib_set_prop_voltage_wls_output(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_set_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);

	smblib_dbg(chg, PR_WLS, "Set WLS output voltage %d\n", val->intval);

	return rc;
}

int smblib_set_prop_dc_reset(struct smb_charger *chg)
{
	int rc;

	rc = vote(chg->dc_suspend_votable, VOUT_VOTER, true, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't suspend DC rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_EN_MASK,
				DCIN_EN_OVERRIDE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DCIN_EN_OVERRIDE_BIT rc=%d\n",
			rc);
		return rc;
	}

	rc = smblib_write(chg, DCIN_CMD_PON_REG, DCIN_PON_BIT | MID_CHG_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %d to DCIN_CMD_PON_REG rc=%d\n",
			DCIN_PON_BIT | MID_CHG_BIT, rc);
		return rc;
	}

	/* Wait for 10ms to allow the charge to get drained */
	usleep_range(10000, 10010);

	rc = smblib_write(chg, DCIN_CMD_PON_REG, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't clear DCIN_CMD_PON_REG rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_EN_MASK, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't clear DCIN_EN_OVERRIDE_BIT rc=%d\n",
			rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, VOUT_VOTER, false, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't unsuspend  DC rc=%d\n", rc);
		return rc;
	}

	smblib_dbg(chg, PR_MISC, "Wireless charger removal detection successful\n");
	return rc;
}

/*******************
 * USB PSY GETTERS *
 *******************/

int smblib_get_prop_usb_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_usb_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote_locked(chg->usb_icl_votable, USER_VOTER) == 0) {
		val->intval = false;
		return rc;
	}

	if (is_client_vote_enabled(chg->usb_icl_votable,
					CHG_TERMINATION_VOTER)) {
		rc = smblib_get_prop_usb_present(chg, val);
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_USBIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smblib_get_prop_usb_voltage_max_design(struct smb_charger *chg,
				    union power_supply_propval *val)
{
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT) || defined(CONFIG_PM6150_CC_VBUS_SHORT)
	if (chg->is_short != SBUx_VBUS_OPEN){
        val->intval = MICRO_5V;
		return 0;
    }
#endif
	switch (chg->real_charger_type) {
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC) && defined(CONFIG_AFC)
	case POWER_SUPPLY_TYPE_AFC:
		val->intval = MICRO_9V;
		break;
#endif
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			val->intval = MICRO_5V;
			break;
		} else if (chg->qc2_unsupported_voltage ==
				QC2_NON_COMPLIANT_12V) {
			val->intval = MICRO_9V;
			break;
		}
		/* else, fallthrough */
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		val->intval = MICRO_9V;
		break;
#endif
	case POWER_SUPPLY_TYPE_USB_PD:
		if (chg->smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_max(struct smb_charger *chg,
					union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			val->intval = MICRO_5V;
			break;
		} else if (chg->qc2_unsupported_voltage ==
				QC2_NON_COMPLIANT_12V) {
			val->intval = MICRO_9V;
			break;
		}
		/* else, fallthrough */
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		if (chg->smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_max_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

#define HVDCP3_STEP_UV	200000
static int smblib_estimate_adaptor_voltage(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		val->intval = MICRO_5V + (HVDCP3_STEP_UV * chg->pulse_cnt);
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		/* Take the average of min and max values */
		val->intval = chg->voltage_min_uv +
			((chg->voltage_max_uv - chg->voltage_min_uv) / 2);
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

static int smblib_read_mid_voltage_chan(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.mid_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.mid_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read MID channel rc=%d\n", rc);
		return rc;
	}

	/*
	 * If MID voltage < 1V, it is unreliable.
	 * Figure out voltage from registers and calculations.
	 */
	if (val->intval < 1000000)
		return smblib_estimate_adaptor_voltage(chg, val);

	return 0;
}

static int smblib_read_usbin_voltage_chan(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.usbin_v_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.usbin_v_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN channel rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	int rc;

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb presence status rc=%d\n", rc);
		return -ENODATA;
	}

	/*
	 * For PM8150B, use MID_CHG ADC channel because overvoltage is observed
	 * to occur randomly in the USBIN channel, particularly at high
	 * voltages.
	 */
	if (chg->smb_version == PM8150B_SUBTYPE && pval.intval)
		return smblib_read_mid_voltage_chan(chg, val);
	else
		return smblib_read_usbin_voltage_chan(chg, val);
}

int smblib_get_prop_vph_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.vph_v_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.vph_v_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read vph channel rc=%d\n", rc);
		return rc;
	}

	return 0;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
int smblib_get_prop_otg_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	/*
	 * For PM8150B, use MID_CHG ADC channel because overvoltage is observed
	 * to occur randomly in the USBIN channel, particularly at high
	 * voltages.
	 */
	if (chg->smb_version == PM8150B_SUBTYPE)
		return smblib_read_mid_voltage_chan(chg, val);
	else
		return smblib_read_usbin_voltage_chan(chg, val);
}
#endif

bool smblib_rsbux_low(struct smb_charger *chg, int r_thr)
{
	int r_sbu1 = -1, r_sbu2 = -1;
	bool ret = false;
	int rc;

	if (!chg->iio.sbux_chan)
		return false;

	/* disable crude sensors */
	rc = smblib_masked_write(chg, TYPE_C_CRUDE_SENSOR_CFG_REG,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT,
			0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable crude sensor rc=%d\n", rc);
		return false;
	}

	/* select SBU1 as current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, SEL_SBU1_ISRC_VAL);
	if (rc < 0) {
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	rc = iio_read_channel_processed(chg->iio.sbux_chan, &r_sbu1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	if (r_sbu1 < r_thr) {
		ret = true;
		goto cleanup;
	}

	/* select SBU2 as current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, SEL_SBU2_ISRC_VAL);
	if (rc < 0) {
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	rc = iio_read_channel_processed(chg->iio.sbux_chan, &r_sbu2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	if (r_sbu2 < r_thr)
		ret = true;
cleanup:
	pr_info("[HICCUP] lpd - %s, SBU1: %x SBU2: %x THD: %x, lpd flag: %d \n", __func__, r_sbu1, r_sbu2, r_thr, ret);

	/* enable crude sensors */
	rc = smblib_masked_write(chg, TYPE_C_CRUDE_SENSOR_CFG_REG,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable crude sensor rc=%d\n", rc);

	/* disable current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);

	return ret;
}

int smblib_get_prop_charger_temp(struct smb_charger *chg,
				 union power_supply_propval *val)
{
	int temp, rc;
	int input_present;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return rc;

	if (input_present == INPUT_NOT_PRESENT)
		return -ENODATA;

	if (chg->iio.temp_chan) {
		rc = iio_read_channel_processed(chg->iio.temp_chan,
				&temp);
		if (rc < 0) {
			pr_err("Error in reading temp channel, rc=%d", rc);
			return rc;
		}
		val->intval = temp / 100;
	} else {
		return -ENODATA;
	}

	return rc;
}

int smblib_get_prop_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_4 = 0x%02x\n", stat);

	if (stat & CC_ATTACHED_BIT)
		val->intval = (bool)(stat & CC_ORIENTATION_BIT) + 1;
	else
		val->intval = 0;

	return rc;
}

static const char * const smblib_typec_mode_name[] = {
	[POWER_SUPPLY_TYPEC_NONE]		  = "NONE",
	[POWER_SUPPLY_TYPEC_SOURCE_DEFAULT]	  = "SOURCE_DEFAULT",
	[POWER_SUPPLY_TYPEC_SOURCE_MEDIUM]	  = "SOURCE_MEDIUM",
	[POWER_SUPPLY_TYPEC_SOURCE_HIGH]	  = "SOURCE_HIGH",
	[POWER_SUPPLY_TYPEC_NON_COMPLIANT]	  = "NON_COMPLIANT",
	[POWER_SUPPLY_TYPEC_SINK]		  = "SINK",
	[POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE]   = "SINK_POWERED_CABLE",
	[POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY] = "SINK_DEBUG_ACCESSORY",
	[POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER]   = "SINK_AUDIO_ADAPTER",
	[POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY]   = "POWERED_CABLE_ONLY",
};

#if !defined(CONFIG_PM6150_SBU_VBUS_SHORT)
static
#endif
int smblib_get_prop_ufp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_SNK_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_1 rc=%d\n", rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_1 = 0x%02x\n", stat);

	switch (stat & DETECTED_SRC_TYPE_MASK) {
	case SNK_RP_STD_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	case SNK_RP_1P5_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case SNK_RP_3P0_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	case SNK_RP_SHORT_BIT:
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT) || defined(CONFIG_PM6150_CC_VBUS_SHORT)
		chg->is_short = CCx_VBUS_SHORT;
		smblib_dbg(chg, PR_MISC, "CC-Vbus short detected \n");
#endif
		return POWER_SUPPLY_TYPEC_NON_COMPLIANT;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_dfp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->lpd_stage == LPD_STAGE_COMMIT)
		return POWER_SUPPLY_TYPEC_NONE;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_SRC_STATUS_REG = 0x%02x\n", stat);

	switch (stat & DETECTED_SNK_TYPE_MASK) {
	case AUDIO_ACCESS_RA_RA_BIT:
		return POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	case SRC_DEBUG_ACCESS_BIT:
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	case SRC_RD_RA_VCONN_BIT:
		return POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
	case SRC_RD_OPEN_BIT:
		return POWER_SUPPLY_TYPEC_SINK;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_typec_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
		return 0;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MISC_STATUS_REG = 0x%02x\n", stat);

	if (stat & SNK_SRC_MODE_BIT)
		return smblib_get_prop_dfp_mode(chg);
	else
		return smblib_get_prop_ufp_mode(chg);
}

int smblib_get_prop_typec_power_role(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc = 0;
	u8 ctrl;

	rc = smblib_read(chg, TYPE_C_MODE_CFG_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MODE_CFG_REG rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MODE_CFG_REG = 0x%02x\n",
		   ctrl);

	if (ctrl & TYPEC_DISABLE_CMD_BIT) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return rc;
	}

	switch (ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT)) {
	case 0:
		val->intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		break;
	case EN_SRC_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case EN_SNK_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	default:
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_err(chg, "unsupported power role 0x%02lx\n",
			ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT));
		return -EINVAL;
	}

	return rc;
}

static inline bool typec_in_src_mode(struct smb_charger *chg)
{
	return (chg->typec_mode > POWER_SUPPLY_TYPEC_NONE &&
		chg->typec_mode < POWER_SUPPLY_TYPEC_SOURCE_DEFAULT);
}

int smblib_get_prop_typec_select_rp(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc, rp;
	u8 stat;

	if (!typec_in_src_mode(chg))
		return -ENODATA;

	rc = smblib_read(chg, TYPE_C_CURRSRC_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CURRSRC_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	switch (stat & TYPEC_SRC_RP_SEL_MASK) {
	case TYPEC_SRC_RP_STD:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_STD;
		break;
	case TYPEC_SRC_RP_1P5A:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_1P5A;
		break;
	case TYPEC_SRC_RP_3A:
	case TYPEC_SRC_RP_3A_DUPLICATE:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_3A;
		break;
	default:
		return -EINVAL;
	}

	val->intval = rp;

	return 0;
}

int smblib_get_prop_usb_current_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	int rc = 0, buck_scale = 1, boost_scale = 1;

	if (chg->iio.usbin_i_chan) {
		rc = iio_read_channel_processed(chg->iio.usbin_i_chan,
				&val->intval);
		if (rc < 0) {
			pr_err("Error in reading USBIN_I channel, rc=%d", rc);
			return rc;
		}

		/*
		 * For PM8150B, scaling factor = reciprocal of
		 * 0.2V/A in Buck mode, 0.4V/A in Boost mode.
		 * For PMI632, scaling factor = reciprocal of
		 * 0.4V/A in Buck mode, 0.8V/A in Boost mode.
		 */
		switch (chg->smb_version) {
		case PMI632_SUBTYPE:
			buck_scale = 40;
			boost_scale = 80;
			break;
		default:
			buck_scale = 20;
			boost_scale = 40;
			break;
		}

		if (chg->otg_present || smblib_get_prop_dfp_mode(chg) !=
				POWER_SUPPLY_TYPEC_NONE) {
			val->intval = DIV_ROUND_CLOSEST(val->intval * 100,
								boost_scale);
			return rc;
		}

		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get usb present status,rc=%d\n",
				rc);
			return -ENODATA;
		}

		/* If USB is not present, return 0 */
		if (!pval.intval)
			val->intval = 0;
		else
			val->intval = DIV_ROUND_CLOSEST(val->intval * 100,
								buck_scale);
	} else {
		val->intval = 0;
		rc = -ENODATA;
	}

	return rc;
}

int smblib_get_prop_low_power(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return rc;
	}

	val->intval = !(stat & SRC_HIGH_BATT_BIT);

	return 0;
}

int smblib_get_prop_input_current_settled(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
int smblib_get_prop_input_current_register(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.usb_icl, &val->intval);
}
#endif
int smblib_get_prop_input_voltage_settled(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc, pulses;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return 0;
		}
		val->intval = MICRO_5V + HVDCP3_STEP_UV * pulses;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_min_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_pd_in_hard_reset(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->pd_hard_reset;
	return 0;
}

int smblib_get_pe_start(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->ok_to_pd;
	return 0;
}

int smblib_get_prop_die_health(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int input_present;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return rc;

	if (input_present == INPUT_NOT_PRESENT)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		if (chg->die_temp == -ENODATA)
			return POWER_SUPPLY_HEALTH_UNKNOWN;

		if (chg->die_temp > DIE_TEMP_RST_THRESH)
			return POWER_SUPPLY_HEALTH_OVERHEAT;

		if (chg->die_temp > DIE_TEMP_REG_H_THRESH)
			return POWER_SUPPLY_HEALTH_HOT;

		if (chg->die_temp > DIE_TEMP_REG_L_THRESH)
			return POWER_SUPPLY_HEALTH_WARM;

		return POWER_SUPPLY_HEALTH_COOL;
	}

	rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE_TEMP_STATUS_REG, rc=%d\n",
				rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (stat & DIE_TEMP_RST_BIT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (stat & DIE_TEMP_UB_BIT)
		return POWER_SUPPLY_HEALTH_HOT;

	if (stat & DIE_TEMP_LB_BIT)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

int smblib_get_prop_connector_health(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		if (chg->connector_temp == -ENODATA)
			return POWER_SUPPLY_HEALTH_UNKNOWN;

		if (chg->connector_temp > CONNECTOR_TEMP_RST_THRESH)
			return POWER_SUPPLY_HEALTH_OVERHEAT;

		if (chg->connector_temp > CONNECTOR_TEMP_REG_H_THRESH)
			return POWER_SUPPLY_HEALTH_HOT;

		if (chg->connector_temp > CONNECTOR_TEMP_REG_L_THRESH)
			return POWER_SUPPLY_HEALTH_WARM;

		return POWER_SUPPLY_HEALTH_COOL;
	}

	rc = smblib_read(chg, CONNECTOR_TEMP_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CONNECTOR_TEMP_STATUS_REG, rc=%d\n",
				rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (stat & CONNECTOR_TEMP_RST_BIT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (stat & CONNECTOR_TEMP_UB_BIT)
		return POWER_SUPPLY_HEALTH_HOT;

	if (stat & CONNECTOR_TEMP_LB_BIT)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

static int get_rp_based_dcp_current(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		rp_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		rp_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
#endif
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	/* fall through */
	default:
		rp_ua = DCP_CURRENT_UA;
	}

	return rp_ua;
}

/*******************
 * USB PSY SETTERS *
 * *****************/

int smblib_set_prop_pd_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, icl;

	if (chg->pd_active) {
		icl = get_client_vote(chg->usb_icl_votable, PD_VOTER);
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, val->intval);
		if (val->intval != icl)
			power_supply_changed(chg->usb_psy);
	} else {
		rc = -EPERM;
	}

	return rc;
}

static int smblib_handle_usb_current(struct smb_charger *chg,
					int usb_current)
{
	int rc = 0, rp_ua, typec_mode;
	union power_supply_propval val = {0, };

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (get_rid_type() == 6 /*RID_ADC_619K*/)
		return rc;
#if !defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
		if (usb_current == 100000)
			usb_current = 500000;
#endif
#endif

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (usb_current == -ETIMEDOUT) {
			if ((chg->float_cfg & FLOAT_OPTIONS_MASK)
						== FORCE_FLOAT_SDP_CFG_BIT) {
				/*
				 * Confiugure USB500 mode if Float charger is
				 * configured for SDP mode.
				 */
				rc = set_sdp_current(chg, USBIN_500MA);
				if (rc < 0)
					smblib_err(chg,
						"Couldn't set SDP ICL rc=%d\n",
						rc);

				return rc;
			}

			if (chg->connector_type ==
					POWER_SUPPLY_CONNECTOR_TYPEC) {
				/*
				 * Valid FLOAT charger, report the current
				 * based of Rp.
				 */
				typec_mode = smblib_get_prop_typec_mode(chg);
				rp_ua = get_rp_based_dcp_current(chg,
								typec_mode);
				rc = vote(chg->usb_icl_votable,
						SW_ICL_MAX_VOTER, true, rp_ua);
				if (rc < 0)
					return rc;
			} else {
				rc = vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, DCP_CURRENT_UA);
				if (rc < 0)
					return rc;
			}
		} else {
			/*
			 * FLOAT charger detected as SDP by USB driver,
			 * charge with the requested current and update the
			 * real_charger_type
			 */
			chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
						true, usb_current);
			if (rc < 0)
				return rc;
			rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER,
							false, 0);
			if (rc < 0)
				return rc;
		}
	} else {
		rc = smblib_get_prop_usb_present(chg, &val);
		if (!rc && !val.intval)
			return 0;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->connector_type ==
				POWER_SUPPLY_CONNECTOR_TYPEC) {
			typec_mode = smblib_get_prop_typec_mode(chg);
			if (typec_rp_med_high(chg, typec_mode))
				return 0;
		}
#endif

		/* if flash is active force 500mA */
		if ((usb_current < SDP_CURRENT_UA) && is_flash_active(chg))
			usb_current = SDP_CURRENT_UA;

		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, true,
							usb_current);
		if (rc < 0) {
			pr_err("Couldn't vote ICL USB_PSY_VOTER rc=%d\n", rc);
			return rc;
		}

		rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		if (rc < 0) {
			pr_err("Couldn't remove SW_ICL_MAX vote rc=%d\n", rc);
			return rc;
		}

	}

	return 0;
}

int smblib_set_prop_sdp_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc = 0;

	if (!chg->pd_active) {
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get usb present rc = %d\n",
						rc);
			return rc;
		}

		/* handle the request only when USB is present */
		if (pval.intval)
			rc = smblib_handle_usb_current(chg, val->intval);
	} else if (chg->system_suspend_supported) {
		if (val->intval <= USBIN_25MA)
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, true, val->intval);
		else
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, false, 0);
	}
	return rc;
}

int smblib_set_prop_boost_current(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				val->intval <= chg->boost_threshold_ua ?
				chg->chg_freq.freq_below_otg_threshold :
				chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0) {
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);
		return rc;
	}

	chg->boost_current_ua = val->intval;
	return rc;
}

int smblib_set_prop_usb_voltage_max_limit(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };

	/* Exit if same value is re-configured */
	if (val->intval == chg->usbin_forced_max_uv)
		return 0;

	smblib_get_prop_usb_voltage_max_design(chg, &pval);

	if (val->intval >= MICRO_5V && val->intval <= pval.intval) {
		chg->usbin_forced_max_uv = val->intval;
		smblib_dbg(chg, PR_MISC, "Max VBUS limit changed to: %d\n",
				val->intval);
	} else if (chg->usbin_forced_max_uv) {
		chg->usbin_forced_max_uv = 0;
	} else {
		return 0;
	}

	power_supply_changed(chg->usb_psy);

	return 0;
}

int smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	int rc = 0;
	u8 power_role;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return 0;

	switch (val->intval) {
	case POWER_SUPPLY_TYPEC_PR_NONE:
		power_role = TYPEC_DISABLE_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_DUAL:
		power_role = chg->typec_try_mode;
		break;
	case POWER_SUPPLY_TYPEC_PR_SINK:
		power_role = EN_SNK_ONLY_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_SOURCE:
		power_role = EN_SRC_ONLY_BIT;
		break;
	default:
		smblib_err(chg, "power role %d not supported\n", val->intval);
		return -EINVAL;
	}

	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				TYPEC_POWER_ROLE_CMD_MASK | TYPEC_TRY_MODE_MASK,
				power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	return rc;
}

int smblib_set_prop_typec_select_rp(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (!typec_in_src_mode(chg)) {
		smblib_err(chg, "Couldn't set curr src: not in SRC mode\n");
		return -EINVAL;
	}

	if (val->intval < TYPEC_SRC_RP_MAX_ELEMENTS) {
		rc = smblib_masked_write(chg, TYPE_C_CURRSRC_CFG_REG,
				TYPEC_SRC_RP_SEL_MASK,
				val->intval);
		if (rc < 0)
			smblib_err(chg, "Couldn't write to TYPE_C_CURRSRC_CFG rc=%d\n",
					rc);
		return rc;
	}

	return -EINVAL;
}

int smblib_set_prop_pd_voltage_min(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, min_uv;

	min_uv = min(val->intval, chg->voltage_max_uv);
	if (chg->voltage_min_uv == min_uv)
		return 0;

	rc = smblib_set_usb_pd_allowed_voltage(chg, min_uv,
					       chg->voltage_max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid min voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_min_uv = min_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_voltage_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, max_uv;

	max_uv = max(val->intval, chg->voltage_min_uv);
	if (chg->voltage_max_uv == max_uv)
		return 0;

	rc = smblib_set_usb_pd_fsw(chg, max_uv);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set FSW for voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	rc = smblib_set_usb_pd_allowed_voltage(chg, chg->voltage_min_uv,
					       max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid max voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_max_uv = max_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_active(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	int rc = 0;
	int sec_charger;

	chg->pd_active = val->intval;

#if defined(CONFIG_SEC_A90Q_PROJECT)
	if (chg->pd_active == POWER_SUPPLY_PD_PPS_ACTIVE)
		pd_charging = true;
	else
		pd_charging = false;
#endif

	smblib_apsd_enable(chg, !chg->pd_active);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: APSD=%s\n", __func__, apsd->name);
#endif
	update_sw_icl_max(chg, apsd->pst);

	if (chg->pd_active) {
		vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
				false, 0);
		vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER,
				false, 0);

		/*
		 * Enforce 100mA for PD until the real vote comes in later.
		 * It is guaranteed that pd_active is set prior to
		 * pd_current_max
		 */
		vote(chg->usb_icl_votable, PD_VOTER, true, USBIN_100MA);
		vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);

		/*
		 * For PPS, Charge Pump is preferred over parallel charger if
		 * present.
		 */
		if (chg->pd_active == POWER_SUPPLY_PD_PPS_ACTIVE
						&& chg->sec_cp_present) {
			rc = smblib_select_sec_charger(chg,
						POWER_SUPPLY_CHARGER_SEC_CP,
						POWER_SUPPLY_CP_PPS, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't enable secondary charger rc=%d\n",
					rc);
		}
	} else {
		vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
				true, 0);
		vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER,
				true, 0);

		sec_charger = chg->sec_pl_present ?
						POWER_SUPPLY_CHARGER_SEC_PL :
						POWER_SUPPLY_CHARGER_SEC_NONE;
		rc = smblib_select_sec_charger(chg, sec_charger,
						POWER_SUPPLY_CP_NONE, false);
		if (rc < 0)
			dev_err(chg->dev,
				"Couldn't enable secondary charger rc=%d\n",
					rc);

		/* PD hard resets failed, proceed to detect QC2/3 */
		if (chg->ok_to_pd) {
			chg->ok_to_pd = false;
#if !defined(CONFIG_AFC)
			smblib_hvdcp_detect_enable(chg, true);
#endif
		}
	}

	smblib_update_usb_type(chg);
	power_supply_changed(chg->usb_psy);
	return rc;
}

int smblib_set_prop_ship_mode(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "Set ship mode: %d!!\n", !!val->intval);

	rc = smblib_masked_write(chg, SHIP_MODE_REG, SHIP_MODE_EN_BIT,
			!!val->intval ? SHIP_MODE_EN_BIT : 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't %s ship mode, rc=%d\n",
				!!val->intval ? "enable" : "disable", rc);

	return rc;
}

int smblib_set_prop_pd_in_hard_reset(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	if (chg->pd_hard_reset == val->intval)
		return rc;

	chg->pd_hard_reset = val->intval;
	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			EXIT_SNK_BASED_ON_CC_BIT,
			(chg->pd_hard_reset) ? EXIT_SNK_BASED_ON_CC_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set EXIT_SNK_BASED_ON_CC rc=%d\n",
				rc);

	return rc;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
int smblib_set_prop_rechg_vbat_thresh(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u32 temp = VBAT_TO_VRAW_ADC(val->intval);

	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg,
		CHGR_ADC_RECHARGE_THRESHOLD_MSB_REG, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write to ADC_RECHARGE_THRESHOLD REG rc=%d\n",
				rc);
		return rc;
	}

	chg->auto_recharge_vbat_mv = val->intval;
	return rc;
}

#endif
#define JEITA_SOFT			0
#define JEITA_HARD			1
static int smblib_update_jeita(struct smb_charger *chg, u32 *thresholds,
								int type)
{
	int rc;
	u16 temp, base;

	base = CHGR_JEITA_THRESHOLD_BASE_REG(type);

	temp = thresholds[1] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s hot threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	temp = thresholds[0] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base + 2, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s cold threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	smblib_dbg(chg, PR_MISC, "%s Jeita threshold configured\n",
				(type == JEITA_SOFT) ? "Soft" : "Hard");

	return 0;
}

static int smblib_charge_inhibit_en(struct smb_charger *chg, bool enable)
{
	int rc;

	rc = smblib_masked_write(chg, CHGR_CFG2_REG,
					CHARGER_INHIBIT_BIT,
					enable ? CHARGER_INHIBIT_BIT : 0);
	return rc;
}

static int smblib_soft_jeita_arb_wa(struct smb_charger *chg)
{
	union power_supply_propval pval;
	int rc = 0;
	bool soft_jeita;

	rc = smblib_get_prop_batt_health(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get battery health rc=%d\n", rc);
		return rc;
	}

	/* Do nothing on entering hard JEITA condition */
	if (pval.intval == POWER_SUPPLY_HEALTH_COLD ||
		pval.intval == POWER_SUPPLY_HEALTH_HOT)
		return 0;

	if (chg->jeita_soft_fcc[0] < 0 || chg->jeita_soft_fcc[1] < 0 ||
		chg->jeita_soft_fv[0] < 0 || chg->jeita_soft_fv[1] < 0)
		return 0;

	soft_jeita = (pval.intval == POWER_SUPPLY_HEALTH_COOL) ||
			(pval.intval == POWER_SUPPLY_HEALTH_WARM);

	/* Do nothing on entering soft JEITA from hard JEITA */
	if (chg->jeita_arb_flag && soft_jeita)
		return 0;

	/* Do nothing, initial to health condition */
	if (!chg->jeita_arb_flag && !soft_jeita)
		return 0;

	/* Entering soft JEITA from normal state */
	if (!chg->jeita_arb_flag && soft_jeita) {
		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, true, 0);

		rc = smblib_charge_inhibit_en(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable charge inhibit rc=%d\n",
					rc);

		rc = smblib_update_jeita(chg, chg->jeita_soft_hys_thlds,
					JEITA_SOFT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't configure Jeita soft threshold rc=%d\n",
				rc);

		if (pval.intval == POWER_SUPPLY_HEALTH_COOL) {
			vote(chg->fcc_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fcc[0]);
			vote(chg->fv_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fv[0]);
		} else {
			vote(chg->fcc_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fcc[1]);
			vote(chg->fv_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fv[1]);
		}

		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, false, 0);
		chg->jeita_arb_flag = true;
	} else if (chg->jeita_arb_flag && !soft_jeita) {
		/* Exit to health state from soft JEITA */

		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, true, 0);

		rc = smblib_charge_inhibit_en(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable charge inhibit rc=%d\n",
					rc);

		rc = smblib_update_jeita(chg, chg->jeita_soft_thlds,
							JEITA_SOFT);
		if (rc < 0)
			smblib_err(chg, "Couldn't configure Jeita soft threshold rc=%d\n",
				rc);

		vote(chg->fcc_votable, JEITA_ARB_VOTER, false, 0);
		vote(chg->fv_votable, JEITA_ARB_VOTER, false, 0);
		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, false, 0);
		chg->jeita_arb_flag = false;
	}

	smblib_dbg(chg, PR_MISC, "JEITA ARB status %d, soft JEITA status %d\n",
			chg->jeita_arb_flag, soft_jeita);
	return rc;
}

/************************
 * USB MAIN PSY GETTERS *
 ************************/
int smblib_get_prop_fcc_delta(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc, jeita_cc_delta_ua = 0;

	if (chg->sw_jeita_enabled) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_get_jeita_cc_delta(chg, &jeita_cc_delta_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc delta rc=%d\n", rc);
		jeita_cc_delta_ua = 0;
	}

	val->intval = jeita_cc_delta_ua;
	return 0;
}

/************************
 * USB MAIN PSY SETTERS *
 ************************/
int smblib_get_charge_current(struct smb_charger *chg,
				int *total_current_ua)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
	union power_supply_propval val = {0, };
	int rc = 0, typec_source_rd, current_ua;
	bool non_compliant;
	u8 stat;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: apsd_result: APSD=%s, bit = 0x%02x\n", __func__, apsd_result->name, apsd_result->bit);
#endif

	if (chg->pd_active) {
		*total_current_ua =
			get_client_vote_locked(chg->usb_icl_votable, PD_VOTER);
		return rc;
	}

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = stat & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT;

	/* get settled ICL */
	rc = smblib_get_prop_input_current_settled(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	typec_source_rd = smblib_get_prop_ufp_mode(chg);

	/* QC 2.0/3.0 adapter */
	if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
		*total_current_ua = HVDCP_CURRENT_UA;
		return 0;
	}

	if (non_compliant) {
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = DCP_CURRENT_UA;
			break;
		default:
			current_ua = 0;
			break;
		}

		*total_current_ua = max(current_ua, val.intval);
		return 0;
	}

	switch (typec_source_rd) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = chg->default_icl_ua;
			break;
		default:
			current_ua = 0;
			break;
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		current_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		current_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	case POWER_SUPPLY_TYPEC_NONE:
	default:
		current_ua = 0;
		break;
	}

	*total_current_ua = max(current_ua, val.intval);
	return 0;
}

/**********************
 * INTERRUPT HANDLERS *
 **********************/

irqreturn_t default_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc = 0;
	int max_icl = 0;
#if !defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	int settled_icl = 0;
#endif

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (strcmp(irq_data->name, "aicl-done") == 0) {
		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		if (stat & AICL_DONE_BIT) {
			rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_icl);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
				return IRQ_HANDLED;
			}

			rc = smblib_get_charge_param(chg, &chg->param.icl_max_stat, &max_icl);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
				return IRQ_HANDLED;
			}

			if(max_icl > settled_icl)
				chg->now_icl = settled_icl;

			smblib_dbg(chg, PR_INTERRUPT, "AICL STATUS:%x,  max_icl:%d, settled_icl:%d now_icl:%d\n", stat,  max_icl, settled_icl, chg->now_icl);
		}
	}
#endif

	return IRQ_HANDLED;
}

irqreturn_t sdam_sts_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	mutex_lock(&chg->irq_status_lock);
	chg->irq_status |= PULSE_SKIP_IRQ_BIT;
	mutex_unlock(&chg->irq_status_lock);

	power_supply_changed(chg->usb_main_psy);
	return IRQ_HANDLED;
}

#define CHG_TERM_WA_ENTRY_DELAY_MS		300000		/* 5 min */
#define CHG_TERM_WA_EXIT_DELAY_MS		60000		/* 1 min */
static void smblib_eval_chg_termination(struct smb_charger *chg, u8 batt_status)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SOC value, rc=%d\n", rc);
		return;
	}

	/*
	 * Post charge termination, switch to BSM mode triggers the risk of
	 * over charging as BATFET opening may take some time post the necessity
	 * of staying in supplemental mode, leading to unintended charging of
	 * battery. Trigger the charge termination WA once charging is completed
	 * to prevent overcharing.
	 */
	if ((batt_status == TERMINATE_CHARGE) && (pval.intval == 100)) {
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
		alarm_start_relative(&chg->chg_termination_alarm,
				ms_to_ktime(CHG_TERM_WA_ENTRY_DELAY_MS));
	} else if (pval.intval < 100) {
		/*
		 * Reset CC_SOC reference value for charge termination WA once
		 * we exit the TERMINATE_CHARGE state and soc drops below 100%
		 */
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
	}
}

irqreturn_t chg_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (chg->wa_flags & CHG_TERMINATION_WA)
		smblib_eval_chg_termination(chg, stat);

	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t batt_temp_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->jeita_configured != JEITA_CFG_COMPLETE)
		return IRQ_HANDLED;

	rc = smblib_soft_jeita_arb_wa(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't fix soft jeita arb rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	return IRQ_HANDLED;
}

irqreturn_t batt_psy_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

#define AICL_STEP_MV		200
#define MAX_AICL_THRESHOLD_MV	4800
irqreturn_t usbin_uv_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);
	int rc;
	u8 stat = 0, max_pulses = 0;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if ((chg->wa_flags & WEAK_ADAPTER_WA)
			&& is_storming(&irq_data->storm_data)) {

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
					"USBIN_UV storm at max AICL threshold\n");
			return IRQ_HANDLED;
		}

		smblib_dbg(chg, PR_MISC, "USBIN_UV storm at threshold %d\n",
				chg->aicl_5v_threshold_mv);

		/* suspend USBIN before updating AICL threshold */
		vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER, true, 0);

		/* delay for VASHDN deglitch */
		msleep(20);

		if (chg->aicl_5v_threshold_mv > MAX_AICL_THRESHOLD_MV) {
			/* reached max AICL threshold */
			chg->aicl_max_reached = true;
			goto unsuspend_input;
		}

		/* Increase AICL threshold by 200mV */
		rc = smblib_set_charge_param(chg, &chg->param.aicl_5v_threshold,
				chg->aicl_5v_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_5v_threshold_mv += AICL_STEP_MV;

		rc = smblib_set_charge_param(chg,
				&chg->param.aicl_cont_threshold,
				chg->aicl_cont_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_cont_threshold_mv += AICL_STEP_MV;

unsuspend_input:
		/* Force torch in boost mode to ensure it works with low ICL */
		if (chg->smb_version == PMI632_SUBTYPE)
			schgm_flash_torch_priority(chg, TORCH_BOOST_MODE);

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
				"Reached max AICL threshold resctricting ICL to 100mA\n");
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					true, USBIN_100MA);
			smblib_run_aicl(chg, RESTART_AICL);
		} else {
			smblib_run_aicl(chg, RESTART_AICL);
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		wdata = &chg->irq_info[USBIN_UV_IRQ].irq_data->storm_data;
		reset_storm_count(wdata);
	}

	if (!chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data)
		return IRQ_HANDLED;

	wdata = &chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data->storm_data;
	reset_storm_count(wdata);

	/* Workaround for non-QC2.0-compliant chargers follows */
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: APSD=%s\n", __func__, apsd->name);
#endif
	if (!chg->qc2_unsupported_voltage &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return IRQ_HANDLED;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_12V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_9V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		smblib_rerun_apsd(chg);
	}

	return IRQ_HANDLED;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
void non_compliant_chg_WA(struct smb_charger *chg){
	int rc;
	u8 stat = 0, max_pulses = 0;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	pr_info("%s: APSD=%s\n", __func__, apsd->name);

	if (!chg->qc2_unsupported_voltage &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_12V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_9V;
			pr_info("%s: qc2_unsupported_voltage(%d)\n", __func__, chg->qc2_unsupported_voltage);
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		pr_info("%s: qc2_unsupported_voltage : %d \n", __func__, chg->qc2_unsupported_voltage );
		smblib_rerun_apsd(chg);
	}
}
#endif

#define USB_WEAK_INPUT_UA	1400000
#define ICL_CHANGE_DELAY_MS	1000
irqreturn_t icl_change_irq_handler(int irq, void *data)
{
	u8 stat;
	int rc, settled_ua, delay = ICL_CHANGE_DELAY_MS;
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->mode == PARALLEL_MASTER) {
		/*
		 * Ignore if change in ICL is due to DIE temp mitigation.
		 * This is to prevent any further ICL split.
		 */
		if (chg->hw_die_temp_mitigation) {
			rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &stat);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't read DIE_TEMP rc=%d\n", rc);
				return IRQ_HANDLED;
			}
			if (stat & (DIE_TEMP_UB_BIT | DIE_TEMP_LB_BIT)) {
				smblib_dbg(chg, PR_PARALLEL,
					"skip ICL change DIE_TEMP %x\n", stat);
				return IRQ_HANDLED;
			}
		}

		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n",
					rc);
			return IRQ_HANDLED;
		}

		rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
					&settled_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		/* If AICL settled then schedule work now */
		if (settled_ua == get_effective_result(chg->usb_icl_votable))
			delay = 0;

		cancel_delayed_work_sync(&chg->icl_change_work);
		schedule_delayed_work(&chg->icl_change_work,
						msecs_to_jiffies(delay));
	}

	return IRQ_HANDLED;
}

static void smblib_micro_usb_plugin(struct smb_charger *chg, bool vbus_rising)
{
	if (!vbus_rising) {
		smblib_update_usb_type(chg);
		smblib_notify_device_mode(chg, false);
		smblib_uusb_removal(chg);
	}
}

void smblib_usb_plugin_hard_reset_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (vbus_rising) {
		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->ta_alert_mode) {
			cancel_delayed_work_sync(&chg->ta_alert_wa_work);
		}
#endif
	} else {
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on USB removal if fcc stepper is enabled */
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->fcc_stepper_enable) {
			if (chg->ta_alert_mode) {
				cancel_delayed_work_sync(&chg->ta_alert_wa_work);
				schedule_delayed_work(&chg->ta_alert_wa_work,
						msecs_to_jiffies(3000));
			}
#if defined(CONFIG_SEC_FACTORY)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
					true, 2100000);
#else
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
					true, 100000);
#endif
		}
#else
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
					true, 1500000);
#endif
	}
#if defined(CONFIG_VBUS_NOTIFIER)
	smblib_handle_vbus(vbus_rising);
#endif

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

#define PL_DELAY_MS	30000
void smblib_usb_plugin_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	smblib_set_opt_switcher_freq(chg, vbus_rising ? chg->chg_freq.freq_5V :
						chg->chg_freq.freq_removal);

	if (vbus_rising) {
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->ta_alert_mode) {
			cancel_delayed_work_sync(&chg->ta_alert_wa_work);
		}
		/* Do fast FCC stepping for initial plugin for fast VBUS jump */
		chg->initial_ramp = true;
#endif

#if defined(CONFIG_PM6150_WATER_DETECT)
		if(chg->water_det_en) {
			chg->hiccup_mode |= HICCUP_VBUS;
#if SUPPORT_RUSTPROOF
			/* If Water + VBUS, enable Hiccup */
			if(chg->hiccup_mode == HICCUP_MODE)
				gpio_direction_output(chg->hiccup_gpio, 1);
#endif

			pr_info("[HICCUP] hiccup_mode = %x", chg->hiccup_mode);
		}
#endif
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		vote(chg->awake_votable, DETACH_DETECT_VOTER, false, 0);
		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

		/* Enable SW Thermal regulation */
		rc = smblib_set_sw_thermal_regulation(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't start SW thermal regulation WA, rc=%d\n",
				rc);

		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);

		/* Schedule work to enable parallel charger */
		vote(chg->awake_votable, PL_DELAY_VOTER, true, 0);
		schedule_delayed_work(&chg->pl_enable_work,
					msecs_to_jiffies(PL_DELAY_MS));
	} else {
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (chg->ta_alert_mode) {
			cancel_delayed_work_sync(&chg->ta_alert_wa_work);
			schedule_delayed_work(&chg->ta_alert_wa_work,
					msecs_to_jiffies(3000));
		}
		/* disable fast FCC step rate */
		chg->initial_ramp = false;
#endif

		/* Disable SW Thermal Regulation */
		rc = smblib_set_sw_thermal_regulation(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't stop SW thermal regulation WA, rc=%d\n",
				rc);

		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#if defined(CONFIG_SEC_FACTORY)
							true, 2100000);
#else
							true, 100000);
#endif
#else
							true, 1500000);
#endif

		if (chg->wa_flags & WEAK_ADAPTER_WA) {
			chg->aicl_5v_threshold_mv =
					chg->default_aicl_5v_threshold_mv;
			chg->aicl_cont_threshold_mv =
					chg->default_aicl_cont_threshold_mv;

			smblib_set_charge_param(chg,
					&chg->param.aicl_5v_threshold,
					chg->aicl_5v_threshold_mv);
			smblib_set_charge_param(chg,
					&chg->param.aicl_cont_threshold,
					chg->aicl_cont_threshold_mv);
			chg->aicl_max_reached = false;

			if (chg->smb_version == PMI632_SUBTYPE)
				schgm_flash_torch_priority(chg,
						TORCH_BUCK_MODE);

			data = chg->irq_info[USBIN_UV_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				reset_storm_count(wdata);
			}
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

		smblib_update_usb_type(chg);
#if defined(CONFIG_PM6150_WATER_DETECT)
//		if(chg->water_det_en) {
//			pr_info("[HICCUP] hiccup_mode = %x", chg->hiccup_mode);
//			if(chg->hiccup_mode == HICCUP_MODE)
//				gpio_direction_output(chg->hiccup_gpio, 0);
//			chg->hiccup_mode &= ~HICCUP_VBUS;
//		}
#endif
	}
#if defined(CONFIG_VBUS_NOTIFIER)
 	smblib_handle_vbus(vbus_rising);
#endif

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		smblib_micro_usb_plugin(chg, vbus_rising);

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

irqreturn_t usb_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->pd_hard_reset)
		smblib_usb_plugin_hard_reset_locked(chg);
	else
		smblib_usb_plugin_locked(chg);

	return IRQ_HANDLED;
}

static void smblib_handle_slow_plugin_timeout(struct smb_charger *chg,
					      bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: slow-plugin-timeout %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_handle_sdp_enumeration_done(struct smb_charger *chg,
					       bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: sdp-enumeration-done %s\n",
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP 3.0 authentication has finished */
static void smblib_handle_hvdcp_3p0_auth_done(struct smb_charger *chg,
					      bool rising)
{
	const struct apsd_result *apsd_result;
	int rc;

	if (!rising)
		return;

	if (chg->mode == PARALLEL_MASTER)
		vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, true, 0);

	/* the APSD done handler will set the USB supply type */
	apsd_result = smblib_get_apsd_result(chg);

	/* for QC3, switch to CP if present */
	if ((apsd_result->bit & QC_3P0_BIT) && chg->sec_cp_present) {
		rc = smblib_select_sec_charger(chg, POWER_SUPPLY_CHARGER_SEC_CP,
					POWER_SUPPLY_CP_HVDCP3, false);
		if (rc < 0)
			dev_err(chg->dev,
			"Couldn't enable secondary chargers  rc=%d\n", rc);
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-3p0-auth-done rising; %s detected\n",
		   apsd_result->name);
}

static void smblib_handle_hvdcp_check_timeout(struct smb_charger *chg,
					      bool rising, bool qc_charger)
{
	if (rising) {
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		if (qc_charger && !chg->qc2_unsupported_voltage) {
#else
		if (qc_charger) {
#endif
			/* enable HDC and ICL irq for QC2/3 charger */
			vote(chg->limited_irq_disable_votable,
					CHARGER_TYPE_VOTER, false, 0);
			vote(chg->hdc_irq_disable_votable,
					CHARGER_TYPE_VOTER, false, 0);
			vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
				HVDCP_CURRENT_UA);
		} else {
			/* A plain DCP, enforce DCP ICL if specified */
			vote(chg->usb_icl_votable, DCP_VOTER,
				chg->dcp_icl_ua != -EINVAL, chg->dcp_icl_ua);
		}
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s %s\n", __func__,
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP is detected */
static void smblib_handle_hvdcp_detect_done(struct smb_charger *chg,
					    bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-detect-done %s\n",
		   rising ? "rising" : "falling");
}

static void update_sw_icl_max(struct smb_charger *chg, int pst)
{
	int typec_mode;
	int rp_ua;

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active)
		return;

	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER) {
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 500000);
		return;
	}

	/*
	 * HVDCP 2/3, handled separately
	 */
	if (pst == POWER_SUPPLY_TYPE_USB_HVDCP
			|| pst == POWER_SUPPLY_TYPE_USB_HVDCP_3
#if defined(CONFIG_AFC)
			|| (chg->real_charger_type == POWER_SUPPLY_TYPE_AFC)
#endif
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
			|| (get_rid_type() == 6 /*RID_ADC_619K*/)
#endif
	)
		return;

	/* TypeC rp med or high, use rp value */
	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_rp_med_high(chg, typec_mode)) {
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		chg->now_icl = rp_ua;
		smblib_dbg(chg, PR_MISC,"now_icl: %d\n", chg->now_icl);
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB) {
			vote(chg->fcc_votable, SEC_BATTERY_CABLE_TYPE_VOTER, true, rp_ua);
		}
#endif
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, rp_ua);
		return;
	}

	/* rp-std or legacy, USB BC 1.2 */
	switch (pst) {
	case POWER_SUPPLY_TYPE_USB:
		/*
		 * USB_PSY will vote to increase the current to 500/900mA once
		 * enumeration is done.
		 */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						USB_PSY_VOTER)) {
			/* if flash is active force 500mA */
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true,
					is_flash_active(chg) ?
					SDP_CURRENT_UA : SDP_100_MA);
		}
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					CDP_CURRENT_UA);
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					DCP_CURRENT_UA);
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		/*
		 * limit ICL to 100mA, the USB driver will enumerate to check
		 * if this is a SDP and appropriately set the current
		 */
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					SDP_100_MA);
		break;
	case POWER_SUPPLY_TYPE_UNKNOWN:
	default:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					SDP_100_MA);
		break;
	}
}

static void smblib_handle_apsd_done(struct smb_charger *chg, bool rising)
{
	const struct apsd_result *apsd_result;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#if !defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
	int typec_mode;
#endif
#endif

	if (!rising)
		return;

	apsd_result = smblib_update_usb_type(chg);

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if ((apsd_result->bit == FLOAT_CHARGER_BIT) && !chg->float_type_recheck) {
		smblib_rerun_apsd(chg);
		chg->float_type_recheck = true;
	}
#endif

	update_sw_icl_max(chg, apsd_result->pst);

	switch (apsd_result->bit) {
	case SDP_CHARGER_BIT:
	case CDP_CHARGER_BIT:
	case FLOAT_CHARGER_BIT:
		if (chg->use_extcon)
			smblib_notify_device_mode(chg, true);
		break;
	case OCP_CHARGER_BIT:
	case DCP_CHARGER_BIT:
		break;
	default:
		break;
	}
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT) || defined(CONFIG_PM6150_CC_VBUS_SHORT)
	if(chg->is_short == SBUx_VBUS_OPEN) {
#endif
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
#if !defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
	typec_mode = smblib_get_prop_typec_mode(chg);
	if((apsd_result->bit == SDP_CHARGER_BIT) && (get_rid_type() != 6 /*RID_ADC_619K)*/) && !typec_rp_med_high(chg, typec_mode)) {
		vote(chg->usb_icl_votable, USB_PSY_VOTER, true, 500000);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
	}
#endif
		//cable type DCP , then check for short detection
	if(apsd_result->bit == DCP_CHARGER_BIT && chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
#if defined(CONFIG_AFC)
		if ((chg->afc_sts == AFC_INIT) && (!chg->hv_disable)) {
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, 500000);   /* 500mA for AFC communication  */
			/* AFC function call */
			smblib_dbg(chg, PR_MISC, "Start AFC!!!\n");
			afc_set_voltage(SET_9V); //is_afc();
		} else {
			pr_err("%s: Do not start AFC, afc_sts(%d), hv_disable(%d)\n",
					__func__, chg->afc_sts, chg->hv_disable);
			/* afc_sts enum
			 * AFC_INIT = 0, NOT_AFC = 1, AFC_FAIL = 2, AFC_DISABLE = 3 */
		}
#endif
	}
#endif
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT) || defined(CONFIG_PM6150_CC_VBUS_SHORT)
	} else {
		smblib_dbg(chg, PR_MISC, "Short Detected, restricting to 5V\n");
	}
#endif
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: apsd-done rising; %s detected\n",
		   apsd_result->name);
}

irqreturn_t usb_source_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
	u8 stat;

	/* PD session is ongoing, ignore BC1.2 and QC detection */
	if (chg->pd_active)
		return IRQ_HANDLED;

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_INTERRUPT, "APSD_STATUS = 0x%02x\n", stat);

	if ((chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		&& (stat & APSD_DTC_STATUS_DONE_BIT)
		&& !chg->uusb_apsd_rerun_done) {
		/*
		 * Force re-run APSD to handle slow insertion related
		 * charger-mis-detection.
		 */
		chg->uusb_apsd_rerun_done = true;
		smblib_rerun_apsd_if_required(chg);
		return IRQ_HANDLED;
	}

	smblib_handle_apsd_done(chg,
		(bool)(stat & APSD_DTC_STATUS_DONE_BIT));

	smblib_handle_hvdcp_detect_done(chg,
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_check_timeout(chg,
		(bool)(stat & HVDCP_CHECK_TIMEOUT_BIT),
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_3p0_auth_done(chg,
		(bool)(stat & QC_AUTH_DONE_STATUS_BIT));

	smblib_handle_sdp_enumeration_done(chg,
		(bool)(stat & ENUMERATION_DONE_BIT));

	smblib_handle_slow_plugin_timeout(chg,
		(bool)(stat & SLOW_PLUGIN_TIMEOUT_BIT));

	smblib_hvdcp_adaptive_voltage_change(chg);

	power_supply_changed(chg->usb_psy);

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_INTERRUPT, "APSD_STATUS = 0x%02x\n", stat);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		sec_bat_run_siop_work();
#endif

	return IRQ_HANDLED;
}

#if defined(CONFIG_PM6150_WATER_DETECT)
#define LPD_DRY_CHECK_INTERVAL	2000

void smblib_lpd_notify_dry(struct smb_charger *chg)
{
	if(!chg->water_det_en)
		return;

	if(chg->lpd_notify == LPD_NOTIFY_MOISTURE) {
		pr_info("[HICCUP] Water dried - notified.hiccup_mode=%x\n", \
				chg->hiccup_mode);
		/* If Water + VBUS, enable Hiccup */
		if(chg->hiccup_mode == HICCUP_MODE)
			gpio_direction_output(chg->hiccup_gpio, 0);
		chg->hiccup_mode &= ~HICCUP_WATER;

#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
#if SUPPORT_RUSTPROOF
		/* Notify UI via CCIC notifier */
		pm6150_ccic_event_work(CCIC_NOTIFY_DEV_BATTERY, \
			CCIC_NOTIFY_ID_WATER, 0/*attach*/, 0, 0);
#endif
#endif //CONFIG_USB_CCIC_NOTIFIER_USING_QC

		chg->lpd_notify = LPD_NOTIFY_DRY;
	}
	else
		pr_info("[HICCUP] Water dried - same state\n");
}

void smblib_lpd_notify_moisture(struct smb_charger *chg)
{
	if(!chg->water_det_en)
		return;

	if(chg->lpd_notify == LPD_NOTIFY_DRY) {
		chg->hiccup_mode |= HICCUP_WATER;
#if SUPPORT_RUSTPROOF
		/* If Water + VBUS, enable Hiccup */
		if(chg->hiccup_mode == HICCUP_MODE)
			gpio_direction_output(chg->hiccup_gpio, 1);
#endif

		pr_info("[HICCUP] Water/moisture - notified, hiccup_mode=%x\n", \
				chg->hiccup_mode);

#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
#if SUPPORT_RUSTPROOF
		/* Notify UI via CCIC notifier */
		pm6150_ccic_event_work(CCIC_NOTIFY_DEV_BATTERY, \
			CCIC_NOTIFY_ID_WATER, 1/*attach*/, 0, 0);
#endif
#endif	// CONFIG_USB_CCIC_NOTIFIER_USING_QC

		chg->lpd_notify = LPD_NOTIFY_MOISTURE;
	}
	else
		pr_info("[HICCUP] Water/moisture - same state\n");
}

enum alarmtimer_restart smblib_lpd_dry_check_timer(struct alarm *alarm,
						ktime_t time)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
							lpd_dry_check_timer);

	/* if there is not interrupt for water for a while */
	if (chg->lpd_reason == LPD_NONE) {
		smblib_lpd_notify_dry(chg);
	}

	return ALARMTIMER_NORESTART;
}
#endif // CONFIG_PM6150_WATER_DETECT


enum alarmtimer_restart smblib_lpd_recheck_timer(struct alarm *alarm,
						ktime_t time)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
							lpd_recheck_timer);
	int rc;

	if (chg->lpd_reason == LPD_MOISTURE_DETECTED) {
#if defined(CONFIG_PM6150_WATER_DETECT)
		if(chg->water_det_en)
			schedule_delayed_work(&chg->lpd_recheck_work,
						msecs_to_jiffies(100));
		else
#endif	// CONFIG_PM6150_WATER_DETECT
		{
			union power_supply_propval pval;

			pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			rc = smblib_set_prop_typec_power_role(chg, &pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
					pval.intval, rc);
					return ALARMTIMER_NORESTART;
			}
			chg->moisture_present = false;
			power_supply_changed(chg->usb_psy);
		}
	} else {
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT,
					TYPEC_WATER_DETECTION_INT_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set TYPE_C_INTERRUPT_EN_CFG_2_REG rc=%d\n",
					rc);
			return ALARMTIMER_NORESTART;
		}
	}

	chg->lpd_stage = LPD_STAGE_NONE;
	chg->lpd_reason = LPD_NONE;
#if defined(CONFIG_PM6150_WATER_DETECT)
	if(chg->water_det_en) {
		pr_info("[HICCUP] lpd - %s\n", __func__);
		//smblib_lpd_notify_dry(chg);

		/* Start a timer to check whether really dried */
		alarm_start_relative(&chg->lpd_dry_check_timer,
							ms_to_ktime(LPD_DRY_CHECK_INTERVAL));
	}
#endif

	return ALARMTIMER_NORESTART;
}


static bool smblib_src_lpd(struct smb_charger *chg)
{
	union power_supply_propval pval;
	bool lpd_flag = false;
	u8 stat;
	int rc;

	if (chg->lpd_disabled)
		return false;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return false;
	}

	switch (stat & DETECTED_SNK_TYPE_MASK) {
	case SRC_DEBUG_ACCESS_BIT:
		if (smblib_rsbux_low(chg, RSBU_K_300K_UV))
			lpd_flag = true;
		break;
	case SRC_RD_RA_VCONN_BIT:
	case SRC_RD_OPEN_BIT:
	case AUDIO_ACCESS_RA_RA_BIT:
	default:
		break;
	}

	if (lpd_flag) {
		chg->lpd_stage = LPD_STAGE_COMMIT;
		pval.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
				pval.intval, rc);
		chg->lpd_reason = LPD_MOISTURE_DETECTED;
		chg->moisture_present =  true;
#if defined(CONFIG_PM6150_WATER_DETECT)
		pr_info("[HICCUP] lpd - %s\n", __func__);
		smblib_lpd_notify_moisture(chg);
#endif
		alarm_start_relative(&chg->lpd_recheck_timer,
						ms_to_ktime(LPD_RECHECK_INTERVAL));
		power_supply_changed(chg->usb_psy);
	} else {
		chg->lpd_reason = LPD_NONE;
		chg->typec_mode = smblib_get_prop_typec_mode(chg);
#if defined(CONFIG_PM6150_WATER_DETECT)
		pr_info("[HICCUP] lpd - %s\n", __func__);
		smblib_lpd_notify_dry(chg);
#endif
	}

	return lpd_flag;
}

static void typec_src_fault_condition_cfg(struct smb_charger *chg, bool src)
{
	int rc;
	u8 mask = USBIN_MID_COMP_FAULT_EN_BIT | USBIN_COLLAPSE_FAULT_EN_BIT;

	rc = smblib_masked_write(chg, OTG_FAULT_CONDITION_CFG_REG, mask,
					src ? 0 : mask);
	if (rc < 0)
		smblib_err(chg, "Couldn't write OTG_FAULT_CONDITION_CFG_REG rc=%d\n",
			rc);
}

static void typec_sink_insertion(struct smb_charger *chg)
{
	int rc;

	typec_src_fault_condition_cfg(chg, true);
	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
					chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);

	if (chg->use_extcon) {
		smblib_notify_usb_host(chg, true);
		chg->otg_present = true;
	}

	if (!chg->pr_swap_in_progress)
		chg->ok_to_pd = (!(*chg->pd_disabled) || chg->early_usb_attach)
					&& !chg->pd_not_supported;
}

static void typec_src_insertion(struct smb_charger *chg)
{
	int rc = 0;
	u8 stat;

	if (chg->pr_swap_in_progress)
		return;

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return;
	}

	chg->typec_legacy = stat & TYPEC_LEGACY_CABLE_STATUS_BIT;

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	smblib_dbg(chg, PR_MISC,
				"Legacy Cable detected: %d\n",chg->typec_legacy);
#endif

	chg->ok_to_pd = (!(chg->typec_legacy || *chg->pd_disabled)
			|| chg->early_usb_attach) && !chg->pd_not_supported;

	#if !defined(CONFIG_AFC) && !defined(CONFIG_PM6150_SBU_VBUS_SHORT) && !defined(CONFIG_PM6150_CC_VBUS_SHORT)
	/* allow apsd proceed to detect QC2/3 */
	if (!chg->ok_to_pd)
		smblib_hvdcp_detect_enable(chg, true);
	#endif
}

static void typec_ra_ra_insertion(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 500000);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	chg->ok_to_pd = false;
	smblib_hvdcp_detect_enable(chg, true);
}

static void typec_sink_removal(struct smb_charger *chg)
{
	int rc;

	typec_src_fault_condition_cfg(chg, false);
	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
					chg->chg_freq.freq_removal);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_removal rc=%d\n", rc);

	if (chg->use_extcon) {
		if (chg->otg_present)
			smblib_notify_usb_host(chg, false);
		chg->otg_present = false;
	}
}

static void typec_src_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	int sec_charger;

	sec_charger = chg->sec_pl_present ? POWER_SUPPLY_CHARGER_SEC_PL :
				POWER_SUPPLY_CHARGER_SEC_NONE;

	rc = smblib_select_sec_charger(chg, sec_charger, POWER_SUPPLY_CP_NONE,
					false);
	if (rc < 0)
		dev_err(chg->dev,
			"Couldn't disable secondary charger rc=%d\n", rc);

	typec_src_fault_condition_cfg(chg, false);
	smblib_hvdcp_detect_enable(chg, false);
	smblib_update_usb_type(chg);
	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}

	cancel_delayed_work_sync(&chg->pl_enable_work);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	cancel_delayed_work_sync(&chg->compliant_check_work);
#endif

	/* reset input current limit voters */
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			is_flash_active(chg) ? SDP_CURRENT_UA : SDP_100_MA);
	vote(chg->usb_icl_votable, PD_VOTER, false, 0);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, CTM_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
	vote(chg->usb_icl_votable, THERMAL_THROTTLE_VOTER, false, 0);

	/* reset usb irq voters */
	vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
			true, 0);
	vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);

	/* reset parallel voters */
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->pl_disable_votable, PL_FCC_LOW_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* Remove SW thermal regulation WA votes */
	vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER, false, 0);
	if (chg->cp_disable_votable)
		vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
								false, 0);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->pulse_cnt = 0;
	chg->usb_icl_delta_ua = 0;
	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT) || defined(CONFIG_PM6150_CC_VBUS_SHORT)
	chg->is_short = SBUx_VBUS_OPEN;
#endif
	chg->now_icl = 0;
	chg->usbin_forced_max_uv = 0;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->qc2_unsupported_voltage) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->qc2_unsupported_voltage = QC2_COMPLIANT;
	}

	if (chg->use_extcon)
		smblib_notify_device_mode(chg, false);

	chg->typec_legacy = false;
}

static void typec_mode_unattached(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, USBIN_100MA);
}

static void smblib_handle_rp_change(struct smb_charger *chg, int typec_mode)
{
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	/*
	 * We want the ICL vote @ 100mA for a FLOAT charger
	 * until the detection by the USB stack is complete.
	 * Ignore the Rp changes unless there is a
	 * pre-existing valid vote or FLOAT is configured for
	 * SDP current.
	 */
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	pr_info("%s: APSD=%s\n", __func__, apsd->name);
#endif
	if (apsd->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (get_client_vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER)
					<= USBIN_100MA
			|| (chg->float_cfg & FLOAT_OPTIONS_MASK)
					== FORCE_FLOAT_SDP_CFG_BIT)
			return;
	}

	update_sw_icl_max(chg, apsd->pst);

	smblib_dbg(chg, PR_MISC, "CC change old_mode=%d new_mode=%d\n",
						chg->typec_mode, typec_mode);
}

static void smblib_lpd_launch_ra_open_work(struct smb_charger *chg)
{
	u8 stat;
	int rc;

	if (chg->lpd_disabled)
		return;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
			rc);
		return;
	}

	if (!(stat & TYPEC_TCCDEBOUNCE_DONE_STATUS_BIT)
			&& chg->lpd_stage == LPD_STAGE_NONE) {
		chg->lpd_stage = LPD_STAGE_FLOAT;
		cancel_delayed_work_sync(&chg->lpd_ra_open_work);
		vote(chg->awake_votable, LPD_VOTER, true, 0);
		schedule_delayed_work(&chg->lpd_ra_open_work,
						msecs_to_jiffies(300));
	}
}

irqreturn_t typec_or_rid_detection_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
#if !defined(CONFIG_SEC_FACTORY)
		if (chg->uusb_moisture_protection_enabled) {
			/*
			 * Adding pm_stay_awake as because pm_relax is called
			 * on exit path from the work routine.
			 */
			pm_stay_awake(chg->dev);
			schedule_work(&chg->moisture_protection_work);
		}

		cancel_delayed_work_sync(&chg->uusb_otg_work);
		/*
		 * Skip OTG enablement if RID interrupt triggers with moisture
		 * protection still enabled.
		 */
		if (!chg->moisture_present) {
			vote(chg->awake_votable, OTG_DELAY_VOTER, true, 0);
			smblib_dbg(chg, PR_INTERRUPT, "Scheduling OTG work\n");
			schedule_delayed_work(&chg->uusb_otg_work,
				msecs_to_jiffies(chg->otg_delay_ms));
		}
#endif

		goto out;
	}

	if (chg->pr_swap_in_progress || chg->pd_hard_reset)
		goto out;

	smblib_lpd_launch_ra_open_work(chg);

	if (chg->usb_psy)
		power_supply_changed(chg->usb_psy);

out:
	return IRQ_HANDLED;
}

irqreturn_t typec_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int typec_mode;
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT)
	int is_short = 0;
#endif

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		smblib_dbg(chg, PR_INTERRUPT,
				"Ignoring for micro USB\n");
		return IRQ_HANDLED;
	}
#if defined(CONFIG_PM6150_SBU_VBUS_SHORT)
	is_short = pm6150_detect_sbu_short();
	if(is_short != SBUx_VBUS_OPEN)
	{
		chg->is_short = is_short;
		smblib_dbg(chg, PR_INTERRUPT,
				"Sbu Short detected: %d\n",is_short);
	}
#endif
	typec_mode = smblib_get_prop_typec_mode(chg);
	if (chg->sink_src_mode != UNATTACHED_MODE
			&& (typec_mode != chg->typec_mode))
		smblib_handle_rp_change(chg, typec_mode);
	chg->typec_mode = typec_mode;

#if defined(CONFIG_USB_CCIC_NOTIFIER_USING_QC)
	switch(chg->typec_mode) {
	case POWER_SUPPLY_TYPEC_NONE:
		pm6150_set_pd_state(pm6150_State_PE_Initial_detach);
		break;
	case POWER_SUPPLY_TYPEC_SINK:
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		pm6150_set_pd_state(pm6150_State_PE_SRC_Send_Capabilities);
		break;
	case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		pm6150_set_pd_state(pm6150_State_PE_SNK_Wait_for_Capabilities);
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	default:
		break;
	}
#endif

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: cc-state-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

static void smblib_lpd_clear_ra_open_work(struct smb_charger *chg)
{
	if (chg->lpd_disabled)
		return;

	cancel_delayed_work_sync(&chg->lpd_detach_work);
	chg->lpd_stage = LPD_STAGE_FLOAT_CANCEL;
	cancel_delayed_work_sync(&chg->lpd_ra_open_work);
	vote(chg->awake_votable, LPD_VOTER, false, 0);
}

irqreturn_t typec_attach_detach_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, TYPE_C_STATE_MACHINE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return IRQ_HANDLED;
	}

	if (stat & TYPEC_ATTACH_DETACH_STATE_BIT) {

		smblib_lpd_clear_ra_open_work(chg);

		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
			return IRQ_HANDLED;
		}

		if (smblib_get_prop_dfp_mode(chg) ==
				POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER) {
			chg->sink_src_mode = AUDIO_ACCESS_MODE;
			typec_ra_ra_insertion(chg);
		} else if (stat & SNK_SRC_MODE_BIT) {
			if (smblib_src_lpd(chg))
				return IRQ_HANDLED;
			chg->sink_src_mode = SRC_MODE;
			typec_sink_insertion(chg);
		} else {
			chg->sink_src_mode = SINK_MODE;
			typec_src_insertion(chg);
		}

	} else {
		switch (chg->sink_src_mode) {
		case SRC_MODE:
			typec_sink_removal(chg);
			break;
		case SINK_MODE:
		case AUDIO_ACCESS_MODE:
			typec_src_removal(chg);
			break;
		case UNATTACHED_MODE:
		default:
			typec_mode_unattached(chg);
			break;
		}

		if (!chg->pr_swap_in_progress) {
			chg->ok_to_pd = false;
			chg->sink_src_mode = UNATTACHED_MODE;
			chg->early_usb_attach = false;
			smblib_apsd_enable(chg, true);
		}

		if (chg->lpd_stage == LPD_STAGE_FLOAT_CANCEL)
			schedule_delayed_work(&chg->lpd_detach_work,
					msecs_to_jiffies(1000));
	}

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

irqreturn_t dc_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	union power_supply_propval pval;
	int input_present;
	bool dcin_present, vbus_present;
	int rc, wireless_vout = 0;
	int sec_charger;

	rc = smblib_get_prop_vph_voltage_now(chg, &pval);
	if (rc < 0)
		return IRQ_HANDLED;

	/* 2*VPH, with a granularity of 100mV */
	wireless_vout = ((pval.intval * 2) / 100000) * 100000;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return IRQ_HANDLED;

	dcin_present = input_present & INPUT_PRESENT_DC;
	vbus_present = input_present & INPUT_PRESENT_USB;

	if (dcin_present) {
		if (!vbus_present && chg->sec_cp_present) {
			pval.intval = wireless_vout;
			rc = smblib_set_prop_voltage_wls_output(chg, &pval);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't set dc voltage to 2*vph  rc=%d\n",
					rc);

			rc = smblib_select_sec_charger(chg,
					POWER_SUPPLY_CHARGER_SEC_CP,
					POWER_SUPPLY_CP_WIRELESS, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't enable secondary chargers  rc=%d\n",
					rc);
		}
	} else {
		if (chg->cp_reason == POWER_SUPPLY_CP_WIRELESS) {
			sec_charger = chg->sec_pl_present ?
					POWER_SUPPLY_CHARGER_SEC_PL :
					POWER_SUPPLY_CHARGER_SEC_NONE;
			rc = smblib_select_sec_charger(chg, sec_charger,
					POWER_SUPPLY_CP_NONE, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't disable secondary charger rc=%d\n",
					rc);
		}

		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
	}

	power_supply_changed(chg->dc_psy);

	smblib_dbg(chg, PR_WLS, "dcin_present= %d, usbin_present= %d, cp_reason = %d\n",
			dcin_present, vbus_present, chg->cp_reason);

	return IRQ_HANDLED;
}

irqreturn_t high_duty_cycle_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	chg->is_hdc = true;
	/*
	 * Disable usb IRQs after the flag set and re-enable IRQs after
	 * the flag cleared in the delayed work queue, to avoid any IRQ
	 * storming during the delays
	 */
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, true, 0);

	schedule_delayed_work(&chg->clear_hdc_work, msecs_to_jiffies(60));

	return IRQ_HANDLED;
}

static void smblib_bb_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bb_removal_work.work);

	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	vote(chg->awake_votable, BOOST_BACK_VOTER, false, 0);
}

#define BOOST_BACK_UNVOTE_DELAY_MS		750
#define BOOST_BACK_STORM_COUNT			3
#define WEAK_CHG_STORM_COUNT			8
irqreturn_t switcher_power_ok_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata = &irq_data->storm_data;
	int rc, usb_icl;
	u8 stat;

	if (!(chg->wa_flags & BOOST_BACK_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* skip suspending input if its already suspended by some other voter */
	usb_icl = get_effective_result(chg->usb_icl_votable);
	if ((stat & USE_USBIN_BIT) && usb_icl >= 0 && usb_icl <= USBIN_25MA)
		return IRQ_HANDLED;

	if (stat & USE_DCIN_BIT)
		return IRQ_HANDLED;

	if (is_storming(&irq_data->storm_data)) {
		/* This could be a weak charger reduce ICL */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						WEAK_CHARGER_VOTER)) {
			smblib_err(chg,
				"Weak charger detected: voting %dmA ICL\n",
				*chg->weak_chg_icl_ua / 1000);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					true, *chg->weak_chg_icl_ua);
			/*
			 * reset storm data and set the storm threshold
			 * to 3 for reverse boost detection.
			 */
			update_storm_count(wdata, BOOST_BACK_STORM_COUNT);
		} else {
			smblib_err(chg,
				"Reverse boost detected: voting 0mA to suspend input\n");
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
			vote(chg->awake_votable, BOOST_BACK_VOTER, true, 0);
			/*
			 * Remove the boost-back vote after a delay, to avoid
			 * permanently suspending the input if the boost-back
			 * condition is unintentionally hit.
			 */
			schedule_delayed_work(&chg->bb_removal_work,
				msecs_to_jiffies(BOOST_BACK_UNVOTE_DELAY_MS));
		}
	}

	return IRQ_HANDLED;
}

irqreturn_t wdog_snarl_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	u8 barkbite_status;
	rc = smblib_read(chg, 0x160C, &barkbite_status);
	if (rc < 0)
		smblib_err(chg, "MISC_WDOG_STATUS(0x160C) error rc=%d\n", rc);
	else
		smblib_err(chg, "MISC_WDOG_STATUS(0x160C) = %x\n", barkbite_status);
	if (sec_bat_get_wdt_control()) {
		smblib_err(chg, "WDOG pet is disabled for test\n");
		return IRQ_HANDLED;
	}
#endif

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, true, 0);
		schedule_delayed_work(&chg->thermal_regulation_work, 0);
	}

	if (chg->step_chg_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

irqreturn_t wdog_bark_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	u8 barkbite_status;
	rc = smblib_read(chg, 0x160C, &barkbite_status);
	if (rc < 0)
		smblib_err(chg, "MISC_WDOG_STATUS(0x160C) error rc=%d\n", rc);
	else
		smblib_err(chg, "MISC_WDOG_STATUS(0x160C) = %x\n", barkbite_status);
	if (sec_bat_get_wdt_control()) {
		smblib_err(chg, "WDOG pet is disabled for test\n");
		return IRQ_HANDLED;
	}
#endif
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	if (chg->step_chg_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

static void smblib_die_rst_icl_regulate(struct smb_charger *chg)
{
	int rc;
	u8 temp;

	rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE_TEMP_STATUS_REG rc=%d\n",
				rc);
		return;
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	if (chg->charging_test_mode) {
		vote(chg->usb_icl_votable, DIE_TEMP_VOTER, false, 500000);
		smblib_err(chg, "DIE_TEMP_STATUS_REG rc=0x%x\n", temp);
		return;
	}
#endif
	/* Regulate ICL on die temp crossing DIE_RST threshold */
	vote(chg->usb_icl_votable, DIE_TEMP_VOTER,
				temp & DIE_TEMP_RST_BIT, 500000);
}

/*
 * triggered when DIE or SKIN or CONNECTOR temperature across
 * either of the _REG_L, _REG_H, _RST, or _SHDN thresholds
 */
irqreturn_t temp_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_die_rst_icl_regulate(chg);

	return IRQ_HANDLED;
}

static void smblib_usbov_dbc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						usbov_dbc_work.work);

	smblib_dbg(chg, PR_MISC, "Resetting USBOV debounce\n");
	chg->dbc_usbov = false;
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
}
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
static void smblib_ta_alert_wa_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						ta_alert_wa_work.work);

	smblib_err(chg, "%s\n", __func__);
	chg->ta_alert_mode = false;
	power_supply_changed(chg->batt_psy);
}
#endif

#define USB_OV_DBC_PERIOD_MS		1000
irqreturn_t usbin_ov_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (!(chg->wa_flags & USBIN_OV_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/*
	 * On specific PMICs, OV IRQ triggers for very small duration in
	 * interim periods affecting charging status reflection. In order to
	 * differentiate between OV IRQ glitch and real OV_IRQ, add a debounce
	 * period for evaluation.
	 */
	if (stat & USBIN_OV_RT_STS_BIT) {
		chg->dbc_usbov = true;
		vote(chg->awake_votable, USBOV_DBC_VOTER, true, 0);
		schedule_delayed_work(&chg->usbov_dbc_work,
				msecs_to_jiffies(USB_OV_DBC_PERIOD_MS));
	} else {
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		chg->dbc_usbov = false;
		vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	}

	smblib_dbg(chg, PR_MISC, "USBOV debounce status %d\n",
				chg->dbc_usbov);
	return IRQ_HANDLED;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
irqreturn_t skip_mode_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, MISC_PBS_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s, RT_STS=0x%X\n", irq_data->name, stat);

	if (stat & SKIP_MODE_BIT)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}
#endif

/**************
 * Additional USB PSY getters/setters
 * that call interrupt functions
 ***************/

int smblib_get_prop_pr_swap_in_progress(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->pr_swap_in_progress;
	return 0;
}

#define DETACH_DETECT_DELAY_MS 20
int smblib_set_prop_pr_swap_in_progress(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u8 stat, orientation;

	smblib_dbg(chg, PR_MISC, "Requested PR_SWAP %d\n", val->intval);
	chg->pr_swap_in_progress = val->intval;

	/* check for cable removal during pr_swap */
	if (!chg->pr_swap_in_progress) {
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		vote(chg->awake_votable, DETACH_DETECT_VOTER, true, 0);
		schedule_delayed_work(&chg->pr_swap_detach_work,
				msecs_to_jiffies(DETACH_DETECT_DELAY_MS));
	}

	rc = smblib_masked_write(chg, TYPE_C_DEBOUNCE_OPTION_REG,
			REDUCE_TCCDEBOUNCE_TO_2MS_BIT,
			val->intval ? REDUCE_TCCDEBOUNCE_TO_2MS_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set tCC debounce rc=%d\n", rc);

	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
			val->intval ? BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set exit state cfg rc=%d\n", rc);

	if (chg->pr_swap_in_progress) {
		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
				rc);
		}

		orientation =
			stat & CC_ORIENTATION_BIT ? TYPEC_CCOUT_VALUE_BIT : 0;
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| TYPEC_CCOUT_VALUE_BIT,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| orientation);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
		}
	} else {
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
		}

		/* enable DRP */
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable DRP rc=%d\n", rc);
	}

	return 0;
}

/***************
 * Work Queues *
 ***************/
static void smblib_pr_swap_detach_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pr_swap_detach_work.work);
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_STATE_MACHINE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read STATE_MACHINE_STS rc=%d\n", rc);
		goto out;
	}
	smblib_dbg(chg, PR_REGISTER, "STATE_MACHINE_STS %x\n", stat);
	if (!(stat & TYPEC_ATTACH_DETACH_STATE_BIT)) {
		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);
	}
out:
	vote(chg->awake_votable, DETACH_DETECT_VOTER, false, 0);
}

static void smblib_uusb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						uusb_otg_work.work);
	int rc;
	u8 stat;
	bool otg;

	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_3 rc=%d\n", rc);
		goto out;
	}
	otg = !!(stat & U_USB_GROUND_NOVBUS_BIT);
	if (chg->otg_present != otg)
		smblib_notify_usb_host(chg, otg);
	else
		goto out;

	chg->otg_present = otg;
	if (!otg)
		chg->boost_current_ua = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				otg ? chg->chg_freq.freq_below_otg_threshold
					: chg->chg_freq.freq_removal);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);

	smblib_dbg(chg, PR_REGISTER, "TYPE_C_U_USB_STATUS = 0x%02x OTG=%d\n",
			stat, otg);
	power_supply_changed(chg->usb_psy);

out:
	vote(chg->awake_votable, OTG_DELAY_VOTER, false, 0);
}

static void bms_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bms_update_work);

	smblib_suspend_on_debug_battery(chg);

	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);
}

static void pl_update_work(struct work_struct *work)
{
	union power_supply_propval prop_val;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pl_update_work);
	int rc;

	if (chg->smb_temp_max == -EINVAL) {
		rc = smblib_get_thermal_threshold(chg,
					SMB_REG_H_THRESHOLD_MSB_REG,
					&chg->smb_temp_max);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't get charger_temp_max rc=%d\n",
					rc);
			return;
		}
	}

	prop_val.intval = chg->smb_temp_max;
	rc = power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
				&prop_val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_CHARGER_TEMP_MAX rc=%d\n",
				rc);
		return;
	}

	if (chg->sec_chg_selected == POWER_SUPPLY_CHARGER_SEC_CP)
		return;

	smblib_select_sec_charger(chg, POWER_SUPPLY_CHARGER_SEC_PL,
				POWER_SUPPLY_CP_NONE, false);
}

static void clear_hdc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						clear_hdc_work.work);

	chg->is_hdc = 0;
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);
}

static void smblib_icl_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							icl_change_work.work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	power_supply_changed(chg->usb_main_psy);

	smblib_dbg(chg, PR_INTERRUPT, "icl_settled=%d\n", settled_ua);
}

static void smblib_pl_enable_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							pl_enable_work.work);

	smblib_dbg(chg, PR_PARALLEL, "timer expired, enabling parallel\n");
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);
}

static void smblib_thermal_regulation_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						thermal_regulation_work.work);
	int rc;

	rc = smblib_update_thermal_readings(chg);
	if (rc < 0)
		smblib_err(chg, "Couldn't read current thermal values %d\n",
					rc);

	rc = smblib_process_thermal_readings(chg);
	if (rc < 0)
		smblib_err(chg, "Couldn't run sw thermal regulation %d\n",
					rc);
}

#define MOISTURE_PROTECTION_CHECK_DELAY_MS 300000		/* 5 mins */
static void smblib_moisture_protection_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						moisture_protection_work);
	int rc;
	bool usb_plugged_in;
	u8 stat;

	/*
	 * Disable 1% duty cycle on CC_ID pin and enable uUSB factory mode
	 * detection to track any change on RID, as interrupts are disable.
	 */
	rc = smblib_write(chg, ((chg->smb_version == PMI632_SUBTYPE) ?
			PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
			TYPEC_U_USB_WATER_PROTECTION_CFG_REG), 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable periodic monitoring of CC_ID rc=%d\n",
			rc);
		goto out;
	}

	rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable uUSB factory mode detection rc=%d\n",
			rc);
		goto out;
	}

	/*
	 * Add a delay of 100ms to allow change in rid to reflect on
	 * status registers.
	 */
	msleep(100);

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	usb_plugged_in = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	/* Check uUSB status for moisture presence */
	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_U_USB_STATUS_REG rc=%d\n",
				rc);
		goto out;
	}

	/*
	 * Factory mode detection happens in case of USB plugged-in by using
	 * a different current source of 2uA which can hamper moisture
	 * detection. Since factory mode is not supported in kernel, factory
	 * mode detection can be considered as equivalent to presence of
	 * moisture.
	 */
	if (stat == U_USB_STATUS_WATER_PRESENT || stat == U_USB_FMB1_BIT ||
			stat == U_USB_FMB2_BIT || (usb_plugged_in &&
			stat == U_USB_FLOAT1_BIT)) {
		smblib_set_moisture_protection(chg, true);
		alarm_start_relative(&chg->moisture_protection_alarm,
			ms_to_ktime(MOISTURE_PROTECTION_CHECK_DELAY_MS));
	} else {
		smblib_set_moisture_protection(chg, false);
		rc = alarm_cancel(&chg->moisture_protection_alarm);
		if (rc < 0)
			smblib_err(chg, "Couldn't cancel moisture protection alarm\n");
	}

out:
	pm_relax(chg->dev);
}

static enum alarmtimer_restart moisture_protection_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
					moisture_protection_alarm);

	smblib_dbg(chg, PR_MISC, "moisture Protection Alarm Triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->moisture_protection_work);

	return ALARMTIMER_NORESTART;
}

static void smblib_chg_termination_work(struct work_struct *work)
{
	union power_supply_propval pval;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						chg_termination_work);
	int rc, input_present, delay = CHG_TERM_WA_ENTRY_DELAY_MS;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, true, 0);

	rc = smblib_is_input_present(chg, &input_present);
	if ((rc < 0) || !input_present)
		goto out;

	rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if ((rc < 0) || (pval.intval < 100)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
		goto out;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CHARGE_FULL,
					&pval);
	if (rc < 0)
		goto out;

	/*
	 * On change in the value of learned capacity, re-initialize the
	 * reference cc_soc value due to change in cc_soc characteristic value
	 * at full capacity. Also, in case cc_soc_ref value is reset,
	 * re-initialize it.
	 */
	if (pval.intval != chg->charge_full_cc || !chg->cc_soc_ref) {
		chg->charge_full_cc = pval.intval;
		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;

		chg->cc_soc_ref = pval.intval;
	} else {
		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;
	}

	/*
	 * In BSM a sudden jump in CC_SOC is not expected. If seen, its a
	 * good_ocv or updated capacity, reject it.
	 */
	if (chg->last_cc_soc && pval.intval > (chg->last_cc_soc + 100)) {
		/* CC_SOC has increased by 1% from last time */
		chg->cc_soc_ref = pval.intval;
		smblib_dbg(chg, PR_MISC, "cc_soc jumped(%d->%d), reset cc_soc_ref\n",
				chg->last_cc_soc, pval.intval);
	}
	chg->last_cc_soc = pval.intval;

	/*
	 * Suspend/Unsuspend USB input to keep cc_soc within the 0.5% to 0.75%
	 * overshoot range of the cc_soc value at termination, to prevent
	 * overcharging.
	 */
	if (pval.intval < DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10050, 10000)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
		delay = CHG_TERM_WA_ENTRY_DELAY_MS;
#if defined(CONFIG_SEC_A60Q_PROJECT)
	} else if (pval.intval > DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10075,
								10000) && (pval.intval >= 10000)) {
#else
	} else if (pval.intval > DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10075,
								10000)) {
#endif
		if (input_present & INPUT_PRESENT_USB)
			vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER,
					true, 0);
		if (input_present & INPUT_PRESENT_DC)
			vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER,
					true, 0);
		delay = CHG_TERM_WA_EXIT_DELAY_MS;
	}

	smblib_dbg(chg, PR_MISC, "Chg Term WA readings: cc_soc: %d, cc_soc_ref: %d, delay: %d\n",
			pval.intval, chg->cc_soc_ref, delay);
	alarm_start_relative(&chg->chg_termination_alarm, ms_to_ktime(delay));
out:
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, false, 0);
}

static enum alarmtimer_restart chg_termination_alarm_cb(struct alarm *alarm,
								ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
							chg_termination_alarm);

	smblib_dbg(chg, PR_MISC, "Charge termination WA alarm triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->chg_termination_work);

	return ALARMTIMER_NORESTART;
}

static void jeita_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						jeita_update_work);
	struct device_node *node = chg->dev->of_node;
	struct device_node *batt_node, *pnode;
	union power_supply_propval val;
	int rc, tmp[2], max_fcc_ma, max_fv_uv;
	u32 jeita_hard_thresholds[2];

	batt_node = of_find_node_by_name(node, "qcom,battery-data");
	if (!batt_node) {
		smblib_err(chg, "Batterydata not available\n");
		goto out;
	}

	/* if BMS is not ready, defer the work */
	if (!chg->bms_psy)
		return;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_RESISTANCE_ID, &val);
	if (rc < 0) {
		smblib_err(chg, "Failed to get batt-id rc=%d\n", rc);
		goto out;
	}

	/* if BMS hasn't read out the batt_id yet, defer the work */
	if (val.intval <= 0)
		return;

	pnode = of_batterydata_get_best_profile(batt_node,
					val.intval / 1000, NULL);
	if (IS_ERR(pnode)) {
		rc = PTR_ERR(pnode);
		smblib_err(chg, "Failed to detect valid battery profile %d\n",
				rc);
		goto out;
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-hard-thresholds",
				jeita_hard_thresholds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, jeita_hard_thresholds,
					JEITA_HARD);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Hard Jeita rc=%d\n",
					rc);
			goto out;
		}
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-thresholds",
				chg->jeita_soft_thlds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, chg->jeita_soft_thlds,
					JEITA_SOFT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Soft Jeita rc=%d\n",
					rc);
			goto out;
		}

		rc = of_property_read_u32_array(pnode,
					"qcom,jeita-soft-hys-thresholds",
					chg->jeita_soft_hys_thlds, 2);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get Soft Jeita hysteresis thresholds rc=%d\n",
					rc);
			goto out;
		}
	}

	chg->jeita_soft_fcc[0] = chg->jeita_soft_fcc[1] = -EINVAL;
	chg->jeita_soft_fv[0] = chg->jeita_soft_fv[1] = -EINVAL;
	max_fcc_ma = max_fv_uv = -EINVAL;

	of_property_read_u32(pnode, "qcom,fastchg-current-ma", &max_fcc_ma);
	of_property_read_u32(pnode, "qcom,max-voltage-uv", &max_fv_uv);

	if (max_fcc_ma <= 0 || max_fv_uv <= 0) {
		smblib_err(chg, "Incorrect fastchg-current-ma or max-voltage-uv\n");
		goto out;
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-fcc-ua",
					tmp, 2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get fcc values for soft JEITA rc=%d\n",
				rc);
		goto out;
	}

	max_fcc_ma *= 1000;
	if (tmp[0] > max_fcc_ma || tmp[1] > max_fcc_ma) {
		smblib_err(chg, "Incorrect FCC value [%d %d] max: %d\n", tmp[0],
			tmp[1], max_fcc_ma);
		goto out;
	}
	chg->jeita_soft_fcc[0] = tmp[0];
	chg->jeita_soft_fcc[1] = tmp[1];

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-fv-uv", tmp,
					2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get fv values for soft JEITA rc=%d\n",
				rc);
		goto out;
	}

	if (tmp[0] > max_fv_uv || tmp[1] > max_fv_uv) {
		smblib_err(chg, "Incorrect FV value [%d %d] max: %d\n", tmp[0],
			tmp[1], max_fv_uv);
		goto out;
	}
	chg->jeita_soft_fv[0] = tmp[0];
	chg->jeita_soft_fv[1] = tmp[1];

	rc = smblib_soft_jeita_arb_wa(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't fix soft jeita arb rc=%d\n",
				rc);
		goto out;
	}

	chg->jeita_configured = JEITA_CFG_COMPLETE;
	return;

out:
	chg->jeita_configured = JEITA_CFG_FAILURE;
}

#if defined(CONFIG_PM6150_WATER_DETECT)
static void smblib_lpd_recheck_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							lpd_recheck_work.work);

	if(chg->lpd_reason == LPD_MOISTURE_DETECTED){
		if (smblib_rsbux_low(chg, RSBU_K_300K_UV)) {
			alarm_start_relative(&chg->lpd_recheck_timer,
						ms_to_ktime(LPD_RECHECK_INTERVAL));
			return;
		}
		else
		{
			union power_supply_propval pval;
			int rc;

			chg->lpd_stage = LPD_STAGE_NONE;
			chg->lpd_reason = LPD_NONE;
			pr_info("[HICCUP] lpd - %s\n", __func__);
			alarm_start_relative(&chg->lpd_dry_check_timer,
						ms_to_ktime(LPD_DRY_CHECK_INTERVAL));

			pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			rc = smblib_set_prop_typec_power_role(chg, &pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
					pval.intval, rc);
			}
		}
	} else if(chg->lpd_notify == LPD_NOTIFY_MOISTURE){
		pr_info("[HICCUP] lpd - %s : Problem ? Dry missing?\n", __func__);
	}
}
#endif

static void smblib_lpd_ra_open_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							lpd_ra_open_work.work);
	union power_supply_propval pval;
	u8 stat;
	int rc;

	if (chg->pr_swap_in_progress || chg->pd_hard_reset) {
		chg->lpd_stage = LPD_STAGE_NONE;
		goto out;
	}

	if (chg->lpd_stage != LPD_STAGE_FLOAT)
		goto out;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
			rc);
		goto out;
	}

	/* quit if moisture status is gone or in attached state */
	if (!(stat & TYPEC_WATER_DETECTION_STATUS_BIT)
			|| (stat & TYPEC_TCCDEBOUNCE_DONE_STATUS_BIT)) {
		chg->lpd_stage = LPD_STAGE_NONE;
#if defined(CONFIG_PM6150_WATER_DETECT)
		pr_info("[HICCUP] lpd - %s\n", __func__);
		smblib_lpd_notify_dry(chg);
#endif
		goto out;
	}

	chg->lpd_stage = LPD_STAGE_COMMIT;

	/* Enable source only mode */
	pval.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
	rc = smblib_set_prop_typec_power_role(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set typec source only mode rc=%d\n",
					rc);
		goto out;
	}

	/* Wait 1.5ms to get SBUx ready */
	usleep_range(1500, 1510);

	if (smblib_rsbux_low(chg, RSBU_K_300K_UV)) {
		/* Moisture detected, enable sink only mode */
		pval.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set typec sink only rc=%d\n",
				rc);
			goto out;
		}

		chg->lpd_reason = LPD_MOISTURE_DETECTED;
		chg->moisture_present =  true;
#if defined(CONFIG_PM6150_WATER_DETECT)
		pr_info("[HICCUP] lpd - %s\n", __func__);
		smblib_lpd_notify_moisture(chg);
#endif
	} else {
		/* Floating cable, disable water detection irq temporarily */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set TYPE_C_INTERRUPT_EN_CFG_2_REG rc=%d\n",
					rc);
			goto out;
		}

		/* restore DRP mode */
		pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
				pval.intval, rc);
			goto out;
		}

		chg->lpd_reason = LPD_FLOATING_CABLE;
#if defined(CONFIG_PM6150_WATER_DETECT)
		pr_info("[HICCUP] Water/moisture - Floating Stage\n");
#endif
	}

	/* recheck in 60 seconds */
	alarm_start_relative(&chg->lpd_recheck_timer, \
			ms_to_ktime(LPD_RECHECK_INTERVAL));
out:
	vote(chg->awake_votable, LPD_VOTER, false, 0);
}

static void smblib_lpd_detach_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							lpd_detach_work.work);

	if (chg->lpd_stage == LPD_STAGE_FLOAT_CANCEL)
		chg->lpd_stage = LPD_STAGE_NONE;
}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
static void smblib_compliant_check_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							compliant_check_work.work);
	int rc;
	u8 stat;

	rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
		return;
	}

	if (stat & QC_9V_BIT) {
		 rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		 if (rc < 0) {
			 smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
			 return;
		 }

		 if( (stat & USBIN_CH_COLLAPSE) && (stat & ICL_IMIN) && (!chg->qc2_unsupported_voltage))
		{
			non_compliant_chg_WA(chg);
			smblib_run_aicl(chg, RESTART_AICL);
			smblib_hvdcp_set_fsw(chg, QC_5V_BIT);
			rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
			if (rc < 0)
				pr_err("Failed to force 5V\n");
		}
	}
}
#endif

static int smblib_create_votables(struct smb_charger *chg)
{
	int rc = 0;

	chg->fcc_votable = find_votable("FCC");
	if (chg->fcc_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC votable rc=%d\n", rc);
		return rc;
	}

	chg->fv_votable = find_votable("FV");
	if (chg->fv_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FV votable rc=%d\n", rc);
		return rc;
	}

	chg->usb_icl_votable = find_votable("USB_ICL");
	if (chg->usb_icl_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find USB_ICL votable rc=%d\n", rc);
		return rc;
	}

	chg->pl_disable_votable = find_votable("PL_DISABLE");
	if (chg->pl_disable_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find votable PL_DISABLE rc=%d\n", rc);
		return rc;
	}

	chg->pl_enable_votable_indirect = find_votable("PL_ENABLE_INDIRECT");
	if (chg->pl_enable_votable_indirect == NULL) {
		rc = -EINVAL;
		smblib_err(chg,
			"Couldn't find votable PL_ENABLE_INDIRECT rc=%d\n",
			rc);
		return rc;
	}

	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);

	chg->smb_override_votable = create_votable("SMB_EN_OVERRIDE",
				VOTE_SET_ANY,
				smblib_smb_disable_override_vote_callback, chg);
	if (IS_ERR(chg->smb_override_votable)) {
		rc = PTR_ERR(chg->smb_override_votable);
		chg->smb_override_votable = NULL;
		return rc;
	}

	chg->dc_suspend_votable = create_votable("DC_SUSPEND", VOTE_SET_ANY,
					smblib_dc_suspend_vote_callback,
					chg);
	if (IS_ERR(chg->dc_suspend_votable)) {
		rc = PTR_ERR(chg->dc_suspend_votable);
		chg->dc_suspend_votable = NULL;
		return rc;
	}

	chg->awake_votable = create_votable("AWAKE", VOTE_SET_ANY,
					smblib_awake_vote_callback,
					chg);
	if (IS_ERR(chg->awake_votable)) {
		rc = PTR_ERR(chg->awake_votable);
		chg->awake_votable = NULL;
		return rc;
	}

	chg->chg_disable_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					smblib_chg_disable_vote_callback,
					chg);
	if (IS_ERR(chg->chg_disable_votable)) {
		rc = PTR_ERR(chg->chg_disable_votable);
		chg->chg_disable_votable = NULL;
		return rc;
	}

	chg->limited_irq_disable_votable = create_votable(
				"USB_LIMITED_IRQ_DISABLE",
				VOTE_SET_ANY,
				smblib_limited_irq_disable_vote_callback,
				chg);
	if (IS_ERR(chg->limited_irq_disable_votable)) {
		rc = PTR_ERR(chg->limited_irq_disable_votable);
		chg->limited_irq_disable_votable = NULL;
		return rc;
	}

	chg->hdc_irq_disable_votable = create_votable("USB_HDC_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_hdc_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->hdc_irq_disable_votable)) {
		rc = PTR_ERR(chg->hdc_irq_disable_votable);
		chg->hdc_irq_disable_votable = NULL;
		return rc;
	}

	chg->icl_irq_disable_votable = create_votable("USB_ICL_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_icl_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->icl_irq_disable_votable)) {
		rc = PTR_ERR(chg->icl_irq_disable_votable);
		chg->icl_irq_disable_votable = NULL;
		return rc;
	}

	return rc;
}

static void smblib_destroy_votables(struct smb_charger *chg)
{
	if (chg->dc_suspend_votable)
		destroy_votable(chg->dc_suspend_votable);
	if (chg->usb_icl_votable)
		destroy_votable(chg->usb_icl_votable);
	if (chg->awake_votable)
		destroy_votable(chg->awake_votable);
	if (chg->chg_disable_votable)
		destroy_votable(chg->chg_disable_votable);
}

static void smblib_iio_deinit(struct smb_charger *chg)
{
	if (!IS_ERR_OR_NULL(chg->iio.usbin_v_chan))
		iio_channel_release(chg->iio.usbin_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_i_chan))
		iio_channel_release(chg->iio.usbin_i_chan);
	if (!IS_ERR_OR_NULL(chg->iio.temp_chan))
		iio_channel_release(chg->iio.temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.sbux_chan))
		iio_channel_release(chg->iio.sbux_chan);
	if (!IS_ERR_OR_NULL(chg->iio.vph_v_chan))
		iio_channel_release(chg->iio.vph_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.die_temp_chan))
		iio_channel_release(chg->iio.die_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.connector_temp_chan))
		iio_channel_release(chg->iio.connector_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.skin_temp_chan))
		iio_channel_release(chg->iio.skin_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.smb_temp_chan))
		iio_channel_release(chg->iio.smb_temp_chan);
}

int smblib_init(struct smb_charger *chg)
{
	union power_supply_propval prop_val;
	int rc = 0;

	mutex_init(&chg->smb_lock);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	mutex_init(&chg->pdp_limit_w_lock);
#endif
	mutex_init(&chg->irq_status_lock);
	INIT_WORK(&chg->bms_update_work, bms_update_work);
	INIT_WORK(&chg->pl_update_work, pl_update_work);
	INIT_WORK(&chg->jeita_update_work, jeita_update_work);
	INIT_DELAYED_WORK(&chg->clear_hdc_work, clear_hdc_work);
	INIT_DELAYED_WORK(&chg->icl_change_work, smblib_icl_change_work);
	INIT_DELAYED_WORK(&chg->pl_enable_work, smblib_pl_enable_work);
	INIT_DELAYED_WORK(&chg->uusb_otg_work, smblib_uusb_otg_work);
	INIT_DELAYED_WORK(&chg->bb_removal_work, smblib_bb_removal_work);
#if defined(CONFIG_PM6150_WATER_DETECT)
	INIT_DELAYED_WORK(&chg->lpd_recheck_work, smblib_lpd_recheck_work);
#endif
	INIT_DELAYED_WORK(&chg->lpd_ra_open_work, smblib_lpd_ra_open_work);
	INIT_DELAYED_WORK(&chg->lpd_detach_work, smblib_lpd_detach_work);
	INIT_DELAYED_WORK(&chg->thermal_regulation_work,
					smblib_thermal_regulation_work);
	INIT_DELAYED_WORK(&chg->usbov_dbc_work, smblib_usbov_dbc_work);
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	INIT_DELAYED_WORK(&chg->compliant_check_work, smblib_compliant_check_work);
	INIT_DELAYED_WORK(&chg->ta_alert_wa_work, smblib_ta_alert_wa_work);
#endif
	INIT_DELAYED_WORK(&chg->pr_swap_detach_work,
					smblib_pr_swap_detach_work);

	if (chg->wa_flags & CHG_TERMINATION_WA) {
		INIT_WORK(&chg->chg_termination_work,
					smblib_chg_termination_work);

		if (alarmtimer_get_rtcdev()) {
			alarm_init(&chg->chg_termination_alarm, ALARM_BOOTTIME,
						chg_termination_alarm_cb);
		} else {
			smblib_err(chg, "Couldn't get rtc device\n");
			return -ENODEV;
		}
	}

	if (chg->uusb_moisture_protection_enabled) {
		INIT_WORK(&chg->moisture_protection_work,
					smblib_moisture_protection_work);

		if (alarmtimer_get_rtcdev()) {
			alarm_init(&chg->moisture_protection_alarm,
				ALARM_BOOTTIME, moisture_protection_alarm_cb);
		} else {
			smblib_err(chg, "Failed to initialize moisture protection alarm\n");
			return -ENODEV;
		}
	}

	chg->fake_capacity = -EINVAL;
	chg->fake_input_current_limited = -EINVAL;
	chg->fake_batt_status = -EINVAL;
	chg->sink_src_mode = UNATTACHED_MODE;
	chg->jeita_configured = false;
	chg->sec_chg_selected = POWER_SUPPLY_CHARGER_SEC_NONE;
	chg->cp_reason = POWER_SUPPLY_CP_NONE;
	chg->thermal_status = TEMP_BELOW_RANGE;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	chg->forced_5v_qc30 = false;
#endif

	switch (chg->mode) {
	case PARALLEL_MASTER:
		rc = qcom_batt_init(chg->smb_version);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_batt_init rc=%d\n",
				rc);
			return rc;
		}

		rc = qcom_step_chg_init(chg->dev, chg->step_chg_enabled,
						chg->sw_jeita_enabled, false);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_step_chg_init rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_create_votables(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't create votables rc=%d\n",
				rc);
			return rc;
		}

		chg->bms_psy = power_supply_get_by_name("bms");

		if (chg->sec_pl_present) {
			chg->pl.psy = power_supply_get_by_name("parallel");
			if (chg->pl.psy) {
				if (chg->sec_chg_selected
					!= POWER_SUPPLY_CHARGER_SEC_CP) {
					rc = smblib_select_sec_charger(chg,
						POWER_SUPPLY_CHARGER_SEC_PL,
						POWER_SUPPLY_CP_NONE, false);
					if (rc < 0)
						smblib_err(chg, "Couldn't config pl charger rc=%d\n",
							rc);
				}

				if (chg->smb_temp_max == -EINVAL) {
					rc = smblib_get_thermal_threshold(chg,
						SMB_REG_H_THRESHOLD_MSB_REG,
						&chg->smb_temp_max);
					if (rc < 0) {
						dev_err(chg->dev, "Couldn't get charger_temp_max rc=%d\n",
								rc);
						return rc;
					}
				}

				prop_val.intval = chg->smb_temp_max;
				rc = power_supply_set_property(chg->pl.psy,
					POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
					&prop_val);
				if (rc < 0) {
					dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_CHARGER_TEMP_MAX rc=%d\n",
							rc);
					return rc;
				}
			}
		}

		rc = smblib_register_notifier(chg);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't register notifier rc=%d\n", rc);
			return rc;
		}
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return rc;
}

int smblib_deinit(struct smb_charger *chg)
{
	switch (chg->mode) {
	case PARALLEL_MASTER:
		if (chg->uusb_moisture_protection_enabled) {
			alarm_cancel(&chg->moisture_protection_alarm);
			cancel_work_sync(&chg->moisture_protection_work);
		}
		if (chg->wa_flags & CHG_TERMINATION_WA) {
			alarm_cancel(&chg->chg_termination_alarm);
			cancel_work_sync(&chg->chg_termination_work);
		}
		cancel_work_sync(&chg->bms_update_work);
		cancel_work_sync(&chg->jeita_update_work);
		cancel_work_sync(&chg->pl_update_work);
		cancel_delayed_work_sync(&chg->clear_hdc_work);
		cancel_delayed_work_sync(&chg->icl_change_work);
		cancel_delayed_work_sync(&chg->pl_enable_work);
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		cancel_delayed_work_sync(&chg->bb_removal_work);
		cancel_delayed_work_sync(&chg->lpd_ra_open_work);
		cancel_delayed_work_sync(&chg->lpd_detach_work);
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		power_supply_unreg_notifier(&chg->nb);
		smblib_destroy_votables(chg);
		qcom_step_chg_deinit();
		qcom_batt_deinit();
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	smblib_iio_deinit(chg);

	return 0;
}
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
int smblib_force_off_batfet_bodydiode(struct smb_charger *chg, bool off)
{
	int rc;
	u8 mask;

	mask = BAT_2_SYS_FET_DIS_BIT;
	rc = smblib_masked_write(chg, BAT_FET_CONTROL_REG, mask,
		off ? BAT_2_SYS_FET_DIS_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure BAT_FET_CONTROL_REG rc=%d\n", rc);
		return rc;
	}

	mask = BAT_FET_CFG_BIT
		| BATFET_SHUTDOWN_CFG_BIT;
	rc = smblib_masked_write(chg, BAT_FET_CFG_REG, mask,
		off ? BAT_FET_CFG_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure BAT_FET_CFG_REG rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_read_batfet(struct smb_charger *chg)
{
	int rc;
	u8 ret1, ret2;

	rc = smblib_read(chg, BAT_FET_CONTROL_REG, &ret1);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read BAT_FET_CONTROL_REG rc=%d\n", rc);
		return rc;
	}
	rc = smblib_read(chg, BAT_FET_CFG_REG, &ret2);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read BAT_FET_CFG_REG rc=%d\n", rc);
		return rc;
	}
	pr_info("%s: BAT_FET_CONTROL_REG(0x%x), BAT_FET_CFG_REG(0x%x)\n", __func__, ret1, ret2);

	return (ret1 & BAT_2_SYS_FET_DIS_BIT);
}
#endif
#if defined(CONFIG_AFC)
int is_afc_result(struct smb_charger *chg, int result)
{
	if (chg->real_charger_type != POWER_SUPPLY_TYPE_USB_DCP
			&& chg->real_charger_type != POWER_SUPPLY_TYPE_AFC) {
		smblib_err(chg, "cable is not DCP OR AFC %d\n", result);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
		return 0;
	}
	smblib_err(chg, "is_afc_result = %d, before afc_sts(%d)\n", result, chg->afc_sts);
	chg->afc_sts = result;

	if ((result == NOT_AFC) || (result == AFC_FAIL))  {
		if(chg->real_charger_type == POWER_SUPPLY_TYPE_AFC){
			smblib_err(chg, "afc_set_voltage() failed\n");
		}
		else{
			smblib_err(chg, "AFC failed, re-enabling HVDCP\n");
			smblib_hvdcp_detect_enable(chg, true);
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
		}
	} else if (result == AFC_5V) {
		smblib_err(chg, "afc set to 5V\n");
		smblib_hvdcp_set_fsw(chg, QC_5V_BIT);
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, DCP_CURRENT_UA);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
	} else if (result == AFC_9V) {
		smblib_err(chg, "afc set to 9V\n");
		smblib_hvdcp_set_fsw(chg, QC_9V_BIT);
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, HVDCP_CURRENT_UA);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
	} else if (result == AFC_DISABLE) {
		smblib_err(chg, "afc disable\n");
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
	}

	if (result >= AFC_5V) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_AFC;
#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
		sec_bat_set_cable_type_current(chg->real_charger_type, true);
		pm6150_set_cable(POWER_SUPPLY_TYPE_AFC);
#endif
	}

#if defined(CONFIG_BATTERY_SAMSUNG_USING_QC)
	sec_bat_run_siop_work();
#endif
	return 0;
}
#endif
