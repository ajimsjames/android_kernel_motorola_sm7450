// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"BATTERY_CHG: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/extcon-provider.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/rpmsg.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/battery_charger.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include "qti_typec_class.h"
#include <linux/slab.h>

#define MSG_OWNER_BC			32778
#define MSG_TYPE_REQ_RESP		1
#define MSG_TYPE_NOTIFY			2

/* opcode for battery charger */
#define BC_SET_NOTIFY_REQ		0x04
#define BC_DISABLE_NOTIFY_REQ		0x05
#define BC_NOTIFY_IND			0x07
#define BC_BATTERY_STATUS_GET		0x30
#define BC_BATTERY_STATUS_SET		0x31
#define BC_USB_STATUS_GET		0x32
#define BC_USB_STATUS_SET		0x33
#define BC_WLS_STATUS_GET		0x34
#define BC_WLS_STATUS_SET		0x35
#define BC_SHIP_MODE_REQ_SET		0x36
#define BC_WLS_FW_CHECK_UPDATE		0x40
#define BC_WLS_FW_PUSH_BUF_REQ		0x41
#define BC_WLS_FW_UPDATE_STATUS_RESP	0x42
#define BC_WLS_FW_PUSH_BUF_RESP		0x43
#define BC_WLS_FW_GET_VERSION		0x44
#define BC_SHUTDOWN_NOTIFY		0x47
#define BC_WLS_CHECK_CHIP_ID	0x48
#define BC_HBOOST_VMAX_CLAMP_NOTIFY	0x79
#define BC_GENERIC_NOTIFY		0x80

/* Generic definitions */
#define MAX_STR_LEN			128
#define BC_WAIT_TIME_MS			2000
#define WLS_FW_PREPARE_TIME_MS		2000
#define WLS_FW_WAIT_TIME_MS		5000
#define WLS_FW_UPDATE_TIME_MS		1000
#define WLS_FW_BUF_SIZE			128
#define DEFAULT_RESTRICT_FCC_UA		1000000

enum {
	FULL_CHG_OFF = 0x0100,
	FULL_CHG_ON,
	SCHEDULE_CHG_OFF = 0x0200,
	SCHEDULE_CHG_ON,
	CHG_PROTECT_OFF = 0x0300,
	CHG_PROTECT_ON,
};

enum {
	TURN_OFF_LIMIT_CHG = 0,
	TURN_ON_85_LIMIT_CHG,
};

#define BATTERY_DESIGNCAP		4850000
#define CHG_SHOW_MAX_SIZE 50

#define STOP_CHARGE_85_LOW_SOC 85
#define STOP_CHARGE_85_HIGH_SOC 87

#define FCNT_USB_TEMP 7000

#define FCNT_BATTERY_HEALTH_OVERVOLTAGE         4
#define FCNT_BATTERY_HEALTH_COLD                6
#define FCNT_BATTERY_HEALTH_GOOD                1
#define	FCNT_BATTERY_HEALTH_DEVICE_HOT          14
#define FCNT_BATTERY_HEALTH_USB_MOISTURE        15
#define FCNT_BATTERY_HEALTH_CHG_OVERTIME        16
#define	FCNT_BATTERY_HEALTH_CHG_UNDERVOLTAGE    17
#define FCNT_BATTERY_HEALTH_CHG_OVERVOLTAGE     18
#define FCNT_BATTERY_HEALTH_REMOVE_CHG          19

enum {
	HEALTH_NOTIFY_EVENT_TYPE_GOOD = 1,
	HEALTH_NOTIFY_EVENT_TYPE_OVERVOLTAGE = 4,
	HEALTH_NOTIFY_EVENT_TYPE_COLD = 6,
	HEALTH_NOTIFY_EVENT_TYPE_DEVICE_HOT = 14,
	HEALTH_NOTIFY_EVENT_TYPE_USB_MOISTURE,
	HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERTIME,
	HEALTH_NOTIFY_EVENT_TYPE_CHG_UNDERVOLTAGE,
	HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERVOLTAGE,
	HEALTH_NOTIFY_EVENT_TYPE_REMOVE_CHG,
};

enum usb_connector_type {
	USB_CONNECTOR_TYPE_TYPEC,
	USB_CONNECTOR_TYPE_MICRO_USB,
};

static bool wls_fw_udating = false;

enum psy_type {
	PSY_TYPE_BATTERY,
	PSY_TYPE_USB,
	PSY_TYPE_WLS,
	PSY_TYPE_MAX,
};

enum ship_mode_type {
	SHIP_MODE_PMIC,
	SHIP_MODE_PACK_SIDE,
};

/* property ids */
enum battery_property_id {
	BATT_STATUS,
	BATT_HEALTH,
	BATT_PRESENT,
	BATT_CHG_TYPE,
	BATT_CAPACITY,
	BATT_SOH,
	BATT_VOLT_OCV,
	BATT_VOLT_NOW,
	BATT_VOLT_MAX,
	BATT_CURR_NOW,
	BATT_CHG_CTRL_LIM,
	BATT_CHG_CTRL_LIM_MAX,
	BATT_TEMP,
	BATT_TECHNOLOGY,
	BATT_CHG_COUNTER,
	BATT_CYCLE_COUNT,
	BATT_CHG_FULL_DESIGN,
	BATT_CHG_FULL,
	BATT_MODEL_NAME,
	BATT_TTF_AVG,
	BATT_TTE_AVG,
	BATT_RESISTANCE,
	BATT_POWER_NOW,
	BATT_POWER_AVG,
	BATT_QNS_VOLT_MAX,
	BATT_CHG_QNS_CTRL_LIM_MAX,
	BATT_FV_REDUCTION,
	BATT_PROP_MAX,
};

enum usb_property_id {
	USB_ONLINE,
	USB_VOLT_NOW,
	USB_VOLT_MAX,
	USB_CURR_NOW,
	USB_CURR_MAX,
	USB_INPUT_CURR_LIMIT,
	USB_TYPE,
	USB_ADAP_TYPE,
	USB_MOISTURE_DET_EN,
	USB_MOISTURE_DET_STS,
	USB_TEMP,
	USB_REAL_TYPE,
	USB_TYPEC_COMPLIANT,
	USB_SCOPE,
	USB_CONNECTOR_TYPE,
	F_ACTIVE,
	USB_CC_ORIENTATION,
	USB_CC_OPEN,
	USB_PRESENT_USB,
	USB_FULL_CHARGE_SET,
	USB_OTG_ENABLE,
	USB_PROP_MAX,
};

enum wireless_property_id {
	WLS_ONLINE,
	WLS_VOLT_NOW,
	WLS_VOLT_MAX,
	WLS_CURR_NOW,
	WLS_CURR_MAX,
	WLS_TYPE,
	WLS_BOOST_EN,
	WLS_PROP_MAX,
};

enum wireless_vendor {
	WLS_IDT,
	WLS_CPS,
	WLS_CPS4019,
	WLS_CPS4038 = 0x4038,
	WLS_SC9624 = 0x9624,
};

enum {
	QTI_POWER_SUPPLY_USB_TYPE_HVDCP = 0x80,
	QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3,
	QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3P5,
};

struct battery_charger_set_notify_msg {
	struct pmic_glink_hdr	hdr;
	u32			battery_id;
	u32			power_state;
	u32			low_capacity;
	u32			high_capacity;
};

struct battery_charger_notify_msg {
	struct pmic_glink_hdr	hdr;
	u32			notification;
};

struct battery_charger_req_msg {
	struct pmic_glink_hdr	hdr;
	u32			battery_id;
	u32			property_id;
	u32			value;
};

struct battery_charger_resp_msg {
	struct pmic_glink_hdr	hdr;
	u32			property_id;
	u32			value;
	u32			ret_code;
};

struct battery_model_resp_msg {
	struct pmic_glink_hdr	hdr;
	u32			property_id;
	char			model[MAX_STR_LEN];
};

struct wireless_check_chip_id {
	struct pmic_glink_hdr	hdr;
	u32			ret_code;
};

struct wireless_fw_check_req {
	struct pmic_glink_hdr	hdr;
	u32			fw_version;
	u32			fw_size;
	u32			fw_crc;
};

struct wireless_fw_check_resp {
	struct pmic_glink_hdr	hdr;
	u32			ret_code;
};

struct wireless_fw_push_buf_req {
	struct pmic_glink_hdr	hdr;
	u8			buf[WLS_FW_BUF_SIZE];
	u32			fw_chunk_id;
};

struct wireless_fw_push_buf_resp {
	struct pmic_glink_hdr	hdr;
	u32			fw_update_status;
};

struct wireless_fw_update_status {
	struct pmic_glink_hdr	hdr;
	u32			fw_update_done;
};

struct wireless_fw_get_version_req {
	struct pmic_glink_hdr	hdr;
};

struct wireless_fw_get_version_resp {
	struct pmic_glink_hdr	hdr;
	u32			fw_version;
};

struct battery_charger_ship_mode_req_msg {
	struct pmic_glink_hdr	hdr;
	u32			ship_mode_type;
};

struct psy_state {
	struct power_supply	*psy;
	char			*model;
	const int		*map;
	u32			*prop;
	u32			prop_count;
	u32			opcode_get;
	u32			opcode_set;
};

struct battery_chg_dev {
	struct device			*dev;
	struct class			battery_class;
	struct pmic_glink_client	*client;
	struct typec_role_class		*typec_class;
	struct mutex			rw_lock;
	struct rw_semaphore		state_sem;
	struct completion		ack;
	struct completion		fw_buf_ack;
	struct completion		fw_update_ack;
	struct psy_state		psy_list[PSY_TYPE_MAX];
	struct dentry			*debugfs_dir;
	void				*notifier_cookie;
	/* extcon for VBUS/ID notification for USB for micro USB */
	struct extcon_dev		*extcon;
	u32				*thermal_levels;
	const char			*wls_fw_name;
	const char			*sc_wls_fw_name;
	int				curr_thermal_level;
	int				num_thermal_levels;
	int				shutdown_volt_mv;
	atomic_t			state;
	struct work_struct		subsys_up_work;
	struct work_struct		usb_type_work;
	struct work_struct		battery_check_work;
	int				fake_soc;
	bool				block_tx;
	bool				ship_mode_en;
	bool				debug_battery_detected;
	bool				wls_not_supported;
	bool				wls_fw_update_reqd;
	u32             wls_chip_id;
	u32				wls_fw_version;
	u32				wls_fw_vendor;
	u16				wls_fw_crc;
	u32				wls_fw_update_time_ms;
	int				wls_fw_size_min;
	struct notifier_block		reboot_notifier;
	u32				thermal_fcc_ua;
	u32				restrict_fcc_ua;
	u32				last_fcc_ua;
	u32				usb_icl_ua;
	u32				thermal_fcc_step;
	u32				connector_type;
	u32				usb_prev_mode;
	u32				tbatt_filter_interval_ms;
	u32				tbatt_filter_offset_degree;
	int				tbatt_filter_save_degree;
	bool				tbatt_filter_en;
	bool				restrict_chg_en;
	/* To track the driver initialization status */
	bool				initialized;
	bool				notify_en;
	bool				error_prop;
	struct power_supply		*combo_batt_psy;
	struct delayed_work		battery_monitor_work;
	struct delayed_work   		full_charge_setting_work;
	struct class			oem_battery_class;
	char				*batt_uenvp[2];
	bool				full_charge_setting;
	bool				high_soc;
	bool				schedule_chg;
	bool				temp_err;
	bool				vbat_over;
	bool				vchg_err;
	bool				chg_stop;
	bool				chging_over_time;
	bool				total_time;
	bool				cc_open;
	bool				otg_enable;
	bool				cycle_clear;
	int				cycle_count;
	int				oem_cycle_count;
	int				charge_full;
	int				charge_full_design;
	int				age;
	int				batt_present;
	int				batt_status;
	int				batt_temp;
	int				batt_soc;
	int				batt_current;
	int				curr_fcc;
	int				batt_voltage;
	int				batt_ocv;
	int				batt_volmax;
	int				batt_health;
	int				dbg_health;
	int				usb_online;
	int				usb_present;
	int				usb_current;
	int				usb_voltage;
	int				usb_vol_max;
	int				usb_temp;
	int				real_chg_type;
	int				charger_type;
	int				lpd_status;
	bool				fcnt_charging_feature_supported;
};

static const int battery_prop_map[BATT_PROP_MAX] = {
	[BATT_STATUS]		= POWER_SUPPLY_PROP_STATUS,
	[BATT_HEALTH]		= POWER_SUPPLY_PROP_HEALTH,
	[BATT_PRESENT]		= POWER_SUPPLY_PROP_PRESENT,
	[BATT_CHG_TYPE]		= POWER_SUPPLY_PROP_CHARGE_TYPE,
	[BATT_CAPACITY]		= POWER_SUPPLY_PROP_CAPACITY,
	[BATT_VOLT_OCV]		= POWER_SUPPLY_PROP_VOLTAGE_OCV,
	[BATT_VOLT_NOW]		= POWER_SUPPLY_PROP_VOLTAGE_NOW,
	[BATT_VOLT_MAX]		= POWER_SUPPLY_PROP_VOLTAGE_MAX,
	[BATT_CURR_NOW]		= POWER_SUPPLY_PROP_CURRENT_NOW,
	[BATT_CHG_CTRL_LIM]	= POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	[BATT_CHG_CTRL_LIM_MAX]	= POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	[BATT_TEMP]		= POWER_SUPPLY_PROP_TEMP,
	[BATT_TECHNOLOGY]	= POWER_SUPPLY_PROP_TECHNOLOGY,
	[BATT_CHG_COUNTER]	= POWER_SUPPLY_PROP_CHARGE_COUNTER,
	[BATT_CYCLE_COUNT]	= POWER_SUPPLY_PROP_CYCLE_COUNT,
	[BATT_CHG_FULL_DESIGN]	= POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	[BATT_CHG_FULL]		= POWER_SUPPLY_PROP_CHARGE_FULL,
	[BATT_MODEL_NAME]	= POWER_SUPPLY_PROP_MODEL_NAME,
	[BATT_TTF_AVG]		= POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	[BATT_TTE_AVG]		= POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	[BATT_POWER_NOW]	= POWER_SUPPLY_PROP_POWER_NOW,
	[BATT_POWER_AVG]	= POWER_SUPPLY_PROP_POWER_AVG,
};

static const int usb_prop_map[USB_PROP_MAX] = {
	[USB_ONLINE]		= POWER_SUPPLY_PROP_ONLINE,
	[USB_VOLT_NOW]		= POWER_SUPPLY_PROP_VOLTAGE_NOW,
	[USB_VOLT_MAX]		= POWER_SUPPLY_PROP_VOLTAGE_MAX,
	[USB_CURR_NOW]		= POWER_SUPPLY_PROP_CURRENT_NOW,
	[USB_CURR_MAX]		= POWER_SUPPLY_PROP_CURRENT_MAX,
	[USB_INPUT_CURR_LIMIT]	= POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	[USB_ADAP_TYPE]		= POWER_SUPPLY_PROP_USB_TYPE,
	[USB_TEMP]		= POWER_SUPPLY_PROP_TEMP,
	[USB_SCOPE]		= POWER_SUPPLY_PROP_SCOPE,
};

static const int wls_prop_map[WLS_PROP_MAX] = {
	[WLS_ONLINE]		= POWER_SUPPLY_PROP_ONLINE,
	[WLS_VOLT_NOW]		= POWER_SUPPLY_PROP_VOLTAGE_NOW,
	[WLS_VOLT_MAX]		= POWER_SUPPLY_PROP_VOLTAGE_MAX,
	[WLS_CURR_NOW]		= POWER_SUPPLY_PROP_CURRENT_NOW,
	[WLS_CURR_MAX]		= POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const unsigned int bcdev_usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

/* Standard usb_type definitions similar to power_supply_sysfs.c */
static const char * const power_supply_usb_type_text[] = {
	"Unknown", "SDP", "DCP", "CDP", "ACA", "C",
	"PD", "PD_DRP", "PD_PPS", "BrickID"
};

/* Custom usb_type definitions */
static const char * const qc_power_supply_usb_type_text[] = {
	"HVDCP", "HVDCP_3", "HVDCP_3P5"
};

static RAW_NOTIFIER_HEAD(hboost_notifier);

int register_hboost_event_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_register(&hboost_notifier, nb);
}
EXPORT_SYMBOL(register_hboost_event_notifier);

int unregister_hboost_event_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_unregister(&hboost_notifier, nb);
}
EXPORT_SYMBOL(unregister_hboost_event_notifier);

static bool jiffies_timer_expire(struct battery_chg_dev *bcdev, bool clear_jiffies) {
	unsigned long cur_jiffies = jiffies;
	static unsigned long last_jiffies = 0;
	unsigned long intr_timeout = msecs_to_jiffies(bcdev->tbatt_filter_interval_ms);
	bool ret = false;

	if (!last_jiffies)
		last_jiffies = cur_jiffies;

	if (time_after(cur_jiffies, intr_timeout + last_jiffies))
		ret = true;
	else
		ret = false;

	if (ret) {
              pr_err("expire:%dms, curr_jiffies:%dms, last jiffies:%d",
                              bcdev->tbatt_filter_interval_ms, jiffies_to_msecs(cur_jiffies), jiffies_to_msecs(last_jiffies));
		last_jiffies = cur_jiffies;

	}

	if (clear_jiffies) {
		last_jiffies = cur_jiffies;
	}

	return ret;
}

static int battery_chg_fw_write(struct battery_chg_dev *bcdev, void *data,
				int len)
{
	int rc;

	down_read(&bcdev->state_sem);
	if (atomic_read(&bcdev->state) == PMIC_GLINK_STATE_DOWN) {
		pr_err("glink state is down\n");
		up_read(&bcdev->state_sem);
		return -ENOTCONN;
	}

	reinit_completion(&bcdev->fw_buf_ack);
	rc = pmic_glink_write(bcdev->client, data, len);
	if (!rc) {
		rc = wait_for_completion_timeout(&bcdev->fw_buf_ack,
					msecs_to_jiffies(WLS_FW_WAIT_TIME_MS));
		if (!rc) {
			pr_err("Error, timed out sending message, tims_ms %d\n", WLS_FW_WAIT_TIME_MS);
			up_read(&bcdev->state_sem);
			return -ETIMEDOUT;
		}

		rc = 0;
	}

	up_read(&bcdev->state_sem);
	return rc;
}

static int battery_chg_write(struct battery_chg_dev *bcdev, void *data,
				int len)
{
	int rc;

	/*
	 * When the subsystem goes down, it's better to return the last
	 * known values until it comes back up. Hence, return 0 so that
	 * pmic_glink_write() is not attempted until pmic glink is up.
	 */
	down_read(&bcdev->state_sem);
	if (atomic_read(&bcdev->state) == PMIC_GLINK_STATE_DOWN) {
		pr_debug("glink state is down\n");
		up_read(&bcdev->state_sem);
		return 0;
	}

	if (bcdev->debug_battery_detected && bcdev->block_tx) {
		up_read(&bcdev->state_sem);
		return 0;
	}

	mutex_lock(&bcdev->rw_lock);
	reinit_completion(&bcdev->ack);
	bcdev->error_prop = false;
	rc = pmic_glink_write(bcdev->client, data, len);
	if (!rc) {
		rc = wait_for_completion_timeout(&bcdev->ack,
					msecs_to_jiffies(BC_WAIT_TIME_MS));
		if (!rc) {
			pr_err("Error, timed out sending message\n");
			up_read(&bcdev->state_sem);
			mutex_unlock(&bcdev->rw_lock);
			return -ETIMEDOUT;
		}
		rc = 0;

		/*
		 * In case the opcode used is not supported, the remote
		 * processor might ack it immediately with a return code indicating
		 * an error. This additional check is to check if such an error has
		 * happened and return immediately with error in that case. This
		 * avoids wasting time waiting in the above timeout condition for this
		 * type of error.
		 */
		if (bcdev->error_prop) {
			bcdev->error_prop = false;
			rc = -ENODATA;
		}
	}
	mutex_unlock(&bcdev->rw_lock);
	up_read(&bcdev->state_sem);

	return rc;
}

static int write_property_id(struct battery_chg_dev *bcdev,
			struct psy_state *pst, u32 prop_id, u32 val)
{
	struct battery_charger_req_msg req_msg = { { 0 } };

	if (wls_fw_udating) {
		pr_debug("wireless doing fw update, refuse to  write_property_id");
		return -ETIMEDOUT;
	}

	req_msg.property_id = prop_id;
	req_msg.battery_id = 0;
	req_msg.value = val;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_set;

	if (pst->psy)
		pr_debug("psy: %s prop_id: %u val: %u\n", pst->psy->desc->name,
			req_msg.property_id, val);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int read_property_id(struct battery_chg_dev *bcdev,
			struct psy_state *pst, u32 prop_id)
{
	struct battery_charger_req_msg req_msg = { { 0 } };

	if (wls_fw_udating && prop_id != USB_ADAP_TYPE) {
		pr_debug("wireless doing fw update, refuse to  read_property_id");
		return -ETIMEDOUT;
	}

	req_msg.property_id = prop_id;
	req_msg.battery_id = 0;
	req_msg.value = 0;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_get;

	if (pst->psy)
		pr_debug("psy: %s prop_id: %u\n", pst->psy->desc->name,
			req_msg.property_id);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int get_property_id(struct psy_state *pst,
			enum power_supply_property prop)
{
	u32 i;

	for (i = 0; i < pst->prop_count; i++)
		if (pst->map[i] == prop)
			return i;

	if (pst->psy)
		pr_err("No property id for property %d in psy %s\n", prop,
			pst->psy->desc->name);

	return -ENOENT;
}

static void battery_chg_notify_disable(struct battery_chg_dev *bcdev)
{
	struct battery_charger_set_notify_msg req_msg = { { 0 } };
	int rc;

	if (bcdev->notify_en) {
		/* Send request to disable notification */
		req_msg.hdr.owner = MSG_OWNER_BC;
		req_msg.hdr.type = MSG_TYPE_NOTIFY;
		req_msg.hdr.opcode = BC_DISABLE_NOTIFY_REQ;

		rc = battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
		if (rc < 0)
			pr_err("Failed to disable notification rc=%d\n", rc);
		else
			bcdev->notify_en = false;
	}
}

static void battery_chg_notify_enable(struct battery_chg_dev *bcdev)
{
	struct battery_charger_set_notify_msg req_msg = { { 0 } };
	int rc;

	if (!bcdev->notify_en) {
		/* Send request to enable notification */
		req_msg.hdr.owner = MSG_OWNER_BC;
		req_msg.hdr.type = MSG_TYPE_NOTIFY;
		req_msg.hdr.opcode = BC_SET_NOTIFY_REQ;

		rc = battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
		if (rc < 0)
			pr_err("Failed to enable notification rc=%d\n", rc);
		else
			bcdev->notify_en = true;
	}
}

static void battery_chg_state_cb(void *priv, enum pmic_glink_state state)
{
	struct battery_chg_dev *bcdev = priv;

	pr_debug("state: %d\n", state);

	down_write(&bcdev->state_sem);
	if (!bcdev->initialized) {
		pr_warn("Driver not initialized, pmic_glink state %d\n", state);
		up_write(&bcdev->state_sem);
		return;
	}
	atomic_set(&bcdev->state, state);
	up_write(&bcdev->state_sem);

	if (state == PMIC_GLINK_STATE_UP)
		schedule_work(&bcdev->subsys_up_work);
	else if (state == PMIC_GLINK_STATE_DOWN)
		bcdev->notify_en = false;
}

/**
 * qti_battery_charger_get_prop() - Gets the property being requested
 *
 * @name: Power supply name
 * @prop_id: Property id to be read
 * @val: Pointer to value that needs to be updated
 *
 * Return: 0 if success, negative on error.
 */
int qti_battery_charger_get_prop(const char *name,
				enum battery_charger_prop prop_id, int *val)
{
	struct power_supply *psy;
	struct battery_chg_dev *bcdev;
	struct psy_state *pst;
	int rc = 0;

	if (prop_id >= BATTERY_CHARGER_PROP_MAX)
		return -EINVAL;

	if (strcmp(name, "battery") && strcmp(name, "usb") &&
	    strcmp(name, "wireless"))
		return -EINVAL;

	psy = power_supply_get_by_name(name);
	if (!psy)
		return -ENODEV;

	bcdev = power_supply_get_drvdata(psy);
	power_supply_put(psy);
	if (!bcdev)
		return -ENODEV;

	switch (prop_id) {
	case BATTERY_RESISTANCE:
		pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
		rc = read_property_id(bcdev, pst, BATT_RESISTANCE);
		if (!rc)
			*val = pst->prop[BATT_RESISTANCE];
		break;
	default:
		break;
	}

	return rc;
}
EXPORT_SYMBOL(qti_battery_charger_get_prop);

int qti_battery_charger_set_prop(const char *name,
				enum battery_charger_prop prop_id, int val)
{
	struct power_supply *psy;
	struct battery_chg_dev *bcdev;
	struct psy_state *pst;
	int rc = 0;

	if (prop_id >= BATTERY_CHARGER_PROP_MAX)
		return -EINVAL;

	if (strcmp(name, "battery") && strcmp(name, "usb") &&
	    strcmp(name, "wireless"))
		return -EINVAL;

	psy = power_supply_get_by_name(name);
	if (!psy)
		return -ENODEV;

	bcdev = power_supply_get_drvdata(psy);
	power_supply_put(psy);
	if (!bcdev)
		return -ENODEV;

	switch (prop_id) {
	case FLASH_ACTIVE:
		pst = &bcdev->psy_list[PSY_TYPE_USB];
		rc = write_property_id(bcdev, pst, F_ACTIVE, val);
		break;
	default:
		break;
	}

	return rc;
}
EXPORT_SYMBOL(qti_battery_charger_set_prop);

static bool validate_message(struct battery_chg_dev *bcdev,
			struct battery_charger_resp_msg *resp_msg, size_t len)
{
	if (len != sizeof(*resp_msg)) {
		pr_err("Incorrect response length %zu for opcode %#x\n", len,
			resp_msg->hdr.opcode);
		return false;
	}

	if (resp_msg->ret_code) {
		pr_err_ratelimited("Error in response for opcode %#x prop_id %u, rc=%d\n",
			resp_msg->hdr.opcode, resp_msg->property_id,
			(int)resp_msg->ret_code);
		bcdev->error_prop = true;
		return false;
	}

	return true;
}

#define MODEL_DEBUG_BOARD	"Debug_Board"
static void handle_message(struct battery_chg_dev *bcdev, void *data,
				size_t len)
{
	struct battery_charger_resp_msg *resp_msg = data;
	struct battery_model_resp_msg *model_resp_msg = data;
	struct wireless_fw_check_resp *fw_check_msg;
	struct wireless_fw_push_buf_resp *fw_resp_msg;
	struct wireless_fw_update_status *fw_update_msg;
	struct wireless_fw_get_version_resp *fw_ver_msg;
	struct psy_state *pst;
	bool ack_set = false;

	switch (resp_msg->hdr.opcode) {
	case BC_BATTERY_STATUS_GET:
		pst = &bcdev->psy_list[PSY_TYPE_BATTERY];

		/* Handle model response uniquely as it's a string */
		if (pst->model && len == sizeof(*model_resp_msg)) {
			memcpy(pst->model, model_resp_msg->model, MAX_STR_LEN);
			ack_set = true;
			bcdev->debug_battery_detected = !strcmp(pst->model,
					MODEL_DEBUG_BOARD);
			break;
		}

		/* Other response should be of same type as they've u32 value */
		if (validate_message(bcdev, resp_msg, len) &&
		    resp_msg->property_id < pst->prop_count) {
			pst->prop[resp_msg->property_id] = resp_msg->value;
			ack_set = true;
		}

		break;
	case BC_USB_STATUS_GET:
		pst = &bcdev->psy_list[PSY_TYPE_USB];
		if (validate_message(bcdev, resp_msg, len) &&
		    resp_msg->property_id < pst->prop_count) {
			pst->prop[resp_msg->property_id] = resp_msg->value;
			ack_set = true;
		}

		break;
	case BC_WLS_STATUS_GET:
		pst = &bcdev->psy_list[PSY_TYPE_WLS];
		if (validate_message(bcdev, resp_msg, len) &&
		    resp_msg->property_id < pst->prop_count) {
			pst->prop[resp_msg->property_id] = resp_msg->value;
			ack_set = true;
		}

		break;
	case BC_BATTERY_STATUS_SET:
	case BC_USB_STATUS_SET:
	case BC_WLS_STATUS_SET:
		if (validate_message(bcdev, data, len))
			ack_set = true;

		break;
	case BC_SET_NOTIFY_REQ:
	case BC_DISABLE_NOTIFY_REQ:
	case BC_SHUTDOWN_NOTIFY:
	case BC_SHIP_MODE_REQ_SET:
		/* Always ACK response for notify or ship_mode request */
		ack_set = true;
		break;
	case BC_WLS_FW_CHECK_UPDATE:
		if (len == sizeof(*fw_check_msg)) {
			fw_check_msg = data;
			if (fw_check_msg->ret_code == 1)
				bcdev->wls_fw_update_reqd = true;
			ack_set = true;
		} else {
			pr_err("Incorrect response length %zu for wls_fw_check_update\n",
				len);
		}
		break;
	case BC_WLS_FW_PUSH_BUF_RESP:
		if (len == sizeof(*fw_resp_msg)) {
			fw_resp_msg = data;
			if (fw_resp_msg->fw_update_status == 1)
				complete(&bcdev->fw_buf_ack);
		} else {
			pr_err("Incorrect response length %zu for wls_fw_push_buf_resp\n",
				len);
		}
		break;
	case BC_WLS_FW_UPDATE_STATUS_RESP:
		if (len == sizeof(*fw_update_msg)) {
			fw_update_msg = data;
			if (fw_update_msg->fw_update_done == 1)
				complete(&bcdev->fw_update_ack);
			else
				pr_err("Wireless FW update not done %d\n",
					(int)fw_update_msg->fw_update_done);
		} else {
			pr_err("Incorrect response length %zu for wls_fw_update_status_resp\n",
				len);
		}
		break;
	case BC_WLS_FW_GET_VERSION:
		if (len == sizeof(*fw_ver_msg)) {
			fw_ver_msg = data;
			bcdev->wls_fw_version = fw_ver_msg->fw_version;
			ack_set = true;
		} else {
			pr_err("Incorrect response length %zu for wls_fw_get_version\n",
				len);
		}
		break;
	case BC_WLS_CHECK_CHIP_ID:
		if (len == sizeof(*fw_check_msg)) {
			fw_check_msg = data;
			bcdev->wls_chip_id = fw_check_msg->ret_code;
			ack_set = true;
		} else {
			pr_err("Incorrect response length %zu for wls_fw_check_update\n",
				len);
		}
		break;
	default:
		pr_err("Unknown opcode: %u\n", resp_msg->hdr.opcode);
		break;
	}

	if (ack_set || bcdev->error_prop)
		complete(&bcdev->ack);
}

static void battery_chg_update_uusb_type(struct battery_chg_dev *bcdev,
					 u32 adap_type)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	/* Handle the extcon notification for uUSB case only */
	if (bcdev->connector_type != USB_CONNECTOR_TYPE_MICRO_USB)
		return;

	rc = read_property_id(bcdev, pst, USB_SCOPE);
	if (rc < 0) {
		pr_err("Failed to read USB_SCOPE rc=%d\n", rc);
		return;
	}

	switch (pst->prop[USB_SCOPE]) {
	case POWER_SUPPLY_SCOPE_DEVICE:
		if (adap_type == POWER_SUPPLY_USB_TYPE_SDP ||
		    adap_type == POWER_SUPPLY_USB_TYPE_CDP) {
			/* Device mode connect notification */
			extcon_set_state_sync(bcdev->extcon, EXTCON_USB, 1);
			bcdev->usb_prev_mode = EXTCON_USB;
			rc = qti_typec_partner_register(bcdev->typec_class,
							TYPEC_DEVICE);
			if (rc < 0)
				pr_err("Failed to register typec partner rc=%d\n",
					rc);
		}
		break;
	case POWER_SUPPLY_SCOPE_SYSTEM:
		/* Host mode connect notification */
		extcon_set_state_sync(bcdev->extcon, EXTCON_USB_HOST, 1);
		bcdev->usb_prev_mode = EXTCON_USB_HOST;
		rc = qti_typec_partner_register(bcdev->typec_class, TYPEC_HOST);
		if (rc < 0)
			pr_err("Failed to register typec partner rc=%d\n",
				rc);
		break;
	default:
		if (bcdev->usb_prev_mode == EXTCON_USB ||
		    bcdev->usb_prev_mode == EXTCON_USB_HOST) {
			/* Disconnect notification */
			extcon_set_state_sync(bcdev->extcon,
					      bcdev->usb_prev_mode, 0);
			bcdev->usb_prev_mode = EXTCON_NONE;
			qti_typec_partner_unregister(bcdev->typec_class);
		}
		break;
	}
}

static struct power_supply_desc usb_psy_desc;

static void battery_chg_update_usb_type_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work,
					struct battery_chg_dev, usb_type_work);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_ADAP_TYPE);
	if (rc < 0) {
		pr_err("Failed to read USB_ADAP_TYPE rc=%d\n", rc);
		return;
	}

	/* Reset usb_icl_ua whenever USB adapter type changes */
	if (pst->prop[USB_ADAP_TYPE] != POWER_SUPPLY_USB_TYPE_SDP &&
	    pst->prop[USB_ADAP_TYPE] != POWER_SUPPLY_USB_TYPE_PD)
		bcdev->usb_icl_ua = 0;

	pr_debug("usb_adap_type: %u\n", pst->prop[USB_ADAP_TYPE]);

	switch (pst->prop[USB_ADAP_TYPE]) {
	case POWER_SUPPLY_USB_TYPE_SDP:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB;
		break;
	case POWER_SUPPLY_USB_TYPE_DCP:
	case POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID:
	case QTI_POWER_SUPPLY_USB_TYPE_HVDCP:
	case QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3:
	case QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3P5:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case POWER_SUPPLY_USB_TYPE_CDP:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_CDP;
		break;
	case POWER_SUPPLY_USB_TYPE_ACA:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_ACA;
		break;
	case POWER_SUPPLY_USB_TYPE_C:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_TYPE_C;
		break;
	case POWER_SUPPLY_USB_TYPE_PD:
	case POWER_SUPPLY_USB_TYPE_PD_DRP:
	case POWER_SUPPLY_USB_TYPE_PD_PPS:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_PD;
		break;
	default:
		usb_psy_desc.type = POWER_SUPPLY_TYPE_USB;
		break;
	}

	battery_chg_update_uusb_type(bcdev, pst->prop[USB_ADAP_TYPE]);
}

static void battery_chg_check_status_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work,
					struct battery_chg_dev,
					battery_check_work);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_STATUS);
	if (rc < 0) {
		pr_err("Failed to read BATT_STATUS, rc=%d\n", rc);
		return;
	}

	if (pst->prop[BATT_STATUS] == POWER_SUPPLY_STATUS_CHARGING) {
		pr_debug("Battery is charging\n");
		return;
	}

	rc = read_property_id(bcdev, pst, BATT_CAPACITY);
	if (rc < 0) {
		pr_err("Failed to read BATT_CAPACITY, rc=%d\n", rc);
		return;
	}

	if (DIV_ROUND_CLOSEST(pst->prop[BATT_CAPACITY], 100) > 0) {
		pr_debug("Battery SOC is > 0\n");
		return;
	}

	/*
	 * If we are here, then battery is not charging and SOC is 0.
	 * Check the battery voltage and if it's lower than shutdown voltage,
	 * then initiate an emergency shutdown.
	 */

	rc = read_property_id(bcdev, pst, BATT_VOLT_NOW);
	if (rc < 0) {
		pr_err("Failed to read BATT_VOLT_NOW, rc=%d\n", rc);
		return;
	}

	if (pst->prop[BATT_VOLT_NOW] / 1000 > bcdev->shutdown_volt_mv) {
		pr_debug("Battery voltage is > %d mV\n",
			bcdev->shutdown_volt_mv);
		return;
	}

	pr_emerg("Initiating a shutdown in 100 ms\n");
	msleep(100);
	pr_emerg("Attempting kernel_power_off: Battery voltage low\n");
	kernel_power_off();
}

static void handle_notification(struct battery_chg_dev *bcdev, void *data,
				size_t len)
{
	struct battery_charger_notify_msg *notify_msg = data;
	struct psy_state *pst = NULL;
	u32 hboost_vmax_mv, notification;

	if (len != sizeof(*notify_msg)) {
		pr_err("Incorrect response length %zu\n", len);
		return;
	}

	notification = notify_msg->notification;
	pr_debug("notification: %#x\n", notification);
	if ((notification & 0xffff) == BC_HBOOST_VMAX_CLAMP_NOTIFY) {
		hboost_vmax_mv = (notification >> 16) & 0xffff;
		raw_notifier_call_chain(&hboost_notifier, VMAX_CLAMP, &hboost_vmax_mv);
		pr_debug("hBoost is clamped at %u mV\n", hboost_vmax_mv);
		return;
	}

	switch (notification) {
	case BC_BATTERY_STATUS_GET:
	case BC_GENERIC_NOTIFY:
		pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
		if (bcdev->shutdown_volt_mv > 0)
			schedule_work(&bcdev->battery_check_work);
		break;
	case BC_USB_STATUS_GET:
		pst = &bcdev->psy_list[PSY_TYPE_USB];
		schedule_work(&bcdev->usb_type_work);
		break;
	case BC_WLS_STATUS_GET:
		pst = &bcdev->psy_list[PSY_TYPE_WLS];
		break;
	default:
		break;
	}

	if (pst && pst->psy) {
		/*
		 * For charger mode, keep the device awake at least for 50 ms
		 * so that device won't enter suspend when a non-SDP charger
		 * is removed. This would allow the userspace process like
		 * "charger" to be able to read power supply uevents to take
		 * appropriate actions (e.g. shutting down when the charger is
		 * unplugged).
		 */
		power_supply_changed(pst->psy);
		pm_wakeup_dev_event(bcdev->dev, 50, true);
	}
}

static int battery_chg_callback(void *priv, void *data, size_t len)
{
	struct pmic_glink_hdr *hdr = data;
	struct battery_chg_dev *bcdev = priv;

	pr_debug("owner: %u type: %u opcode: %#x len: %zu\n", hdr->owner,
		hdr->type, hdr->opcode, len);

	down_read(&bcdev->state_sem);

	if (!bcdev->initialized) {
		pr_debug("Driver initialization failed: Dropping glink callback message: state %d\n",
			 bcdev->state);
		up_read(&bcdev->state_sem);
		return 0;
	}

	if (hdr->opcode == BC_NOTIFY_IND)
		handle_notification(bcdev, data, len);
	else
		handle_message(bcdev, data, len);

	up_read(&bcdev->state_sem);

	return 0;
}

static int wls_psy_get_prop(struct power_supply *psy,
		enum power_supply_property prop,
		union power_supply_propval *pval)
{
	struct battery_chg_dev *bcdev = power_supply_get_drvdata(psy);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_WLS];
	int prop_id, rc;

	pval->intval = -ENODATA;

	prop_id = get_property_id(pst, prop);
	if (prop_id < 0)
		return prop_id;

	rc = read_property_id(bcdev, pst, prop_id);
#ifdef QCOM_BASE
	if (rc < 0)
		return rc;
#else
	if (rc == -ETIMEDOUT) {
		pr_debug("read prop:%d timeout, use old prop value\n", prop_id);
		rc = 0;
	} else if (rc < 0) {
		pr_err("read prop:%d error, rc = %d", prop_id, rc);
		return rc;
	}
#endif

	pval->intval = pst->prop[prop_id];

	return 0;
}

static int wls_psy_set_prop(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *pval)
{
	return 0;
}

static int wls_psy_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property prop)
{
	return 0;
}

static enum power_supply_property wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc wls_psy_desc = {
	.name			= "wireless",
	.type			= POWER_SUPPLY_TYPE_WIRELESS,
	.properties		= wls_props,
	.num_properties		= ARRAY_SIZE(wls_props),
	.get_property		= wls_psy_get_prop,
	.set_property		= wls_psy_set_prop,
	.property_is_writeable	= wls_psy_prop_is_writeable,
};

static const char *get_usb_type_name(u32 usb_type)
{
	u32 i;

	if (usb_type >= QTI_POWER_SUPPLY_USB_TYPE_HVDCP &&
	    usb_type <= QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3P5) {
		for (i = 0; i < ARRAY_SIZE(qc_power_supply_usb_type_text);
		     i++) {
			if (i == (usb_type - QTI_POWER_SUPPLY_USB_TYPE_HVDCP))
				return qc_power_supply_usb_type_text[i];
		}
		return "Unknown";
	}

	for (i = 0; i < ARRAY_SIZE(power_supply_usb_type_text); i++) {
		if (i == usb_type)
			return power_supply_usb_type_text[i];
	}

	return "Unknown";
}

static int usb_psy_set_icl(struct battery_chg_dev *bcdev, u32 prop_id, int val)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	u32 temp;
	int rc;

	rc = read_property_id(bcdev, pst, USB_ADAP_TYPE);
	if (rc < 0) {
		pr_err("Failed to read prop USB_ADAP_TYPE, rc=%d\n", rc);
		return rc;
	}

	/* Allow this only for SDP, CDP or USB_PD and not for other charger types */
	switch (pst->prop[USB_ADAP_TYPE]) {
	case POWER_SUPPLY_USB_TYPE_SDP:
	case POWER_SUPPLY_USB_TYPE_PD:
	case POWER_SUPPLY_USB_TYPE_CDP:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Input current limit (ICL) can be set by different clients. E.g. USB
	 * driver can request for a current of 500/900 mA depending on the
	 * port type. Also, clients like EUD driver can pass 0 or -22 to
	 * suspend or unsuspend the input for its use case.
	 */

	temp = val;
	if (val < 0)
		temp = UINT_MAX;

	rc = write_property_id(bcdev, pst, prop_id, temp);
	if (rc < 0) {
		pr_err("Failed to set ICL (%u uA) rc=%d\n", temp, rc);
	} else {
		pr_debug("Set ICL to %u\n", temp);
		bcdev->usb_icl_ua = temp;
	}

	return rc;
}

static int usb_psy_get_prop(struct power_supply *psy,
		enum power_supply_property prop,
		union power_supply_propval *pval)
{
	struct battery_chg_dev *bcdev = power_supply_get_drvdata(psy);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int prop_id, rc;

	pval->intval = -ENODATA;

	prop_id = get_property_id(pst, prop);
	if (prop_id < 0)
		return prop_id;

	rc = read_property_id(bcdev, pst, prop_id);
#ifdef QCOM_BASE
	if (rc < 0)
		return rc;
#else
	if (rc == -ETIMEDOUT) {
		pr_debug("read prop:%d timeout, use old prop value\n", prop_id);
		rc = 0;
	} else if (rc < 0) {
		pr_err("read prop:%d error, rc = %d", prop_id, rc);
		return rc;
	}
#endif

	pval->intval = pst->prop[prop_id];
	if (prop == POWER_SUPPLY_PROP_TEMP)
		pval->intval = DIV_ROUND_CLOSEST((int)pval->intval, 10);

	return 0;
}

static int usb_psy_set_prop(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *pval)
{
	struct battery_chg_dev *bcdev = power_supply_get_drvdata(psy);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int prop_id, rc = 0;

	prop_id = get_property_id(pst, prop);
	if (prop_id < 0)
		return prop_id;

	switch (prop) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		rc = usb_psy_set_icl(bcdev, prop_id, pval->intval);
		break;
	default:
		break;
	}

	return rc;
}

static int usb_psy_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return 1;
	default:
		break;
	}

	return 0;
}

static enum power_supply_property usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_SCOPE,
};

static enum power_supply_usb_type usb_psy_supported_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_ACA,
	POWER_SUPPLY_USB_TYPE_C,
	POWER_SUPPLY_USB_TYPE_PD,
	POWER_SUPPLY_USB_TYPE_PD_DRP,
	POWER_SUPPLY_USB_TYPE_PD_PPS,
	POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
};

static struct power_supply_desc usb_psy_desc = {
	.name			= "usb",
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= usb_props,
	.num_properties		= ARRAY_SIZE(usb_props),
	.get_property		= usb_psy_get_prop,
	.set_property		= usb_psy_set_prop,
	.usb_types		= usb_psy_supported_types,
	.num_usb_types		= ARRAY_SIZE(usb_psy_supported_types),
	.property_is_writeable	= usb_psy_prop_is_writeable,
};

static int __battery_psy_set_charge_current(struct battery_chg_dev *bcdev,
					u32 fcc_ua)
{
	int rc;

	if (bcdev->restrict_chg_en) {
		fcc_ua = min_t(u32, fcc_ua, bcdev->restrict_fcc_ua);
		fcc_ua = min_t(u32, fcc_ua, bcdev->thermal_fcc_ua);
	}

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_BATTERY],
				BATT_CHG_CTRL_LIM, fcc_ua);
	if (rc < 0) {
		pr_err("Failed to set FCC %u, rc=%d\n", fcc_ua, rc);
	} else {
		pr_debug("Set FCC to %u uA\n", fcc_ua);
		bcdev->last_fcc_ua = fcc_ua;
	}

	return rc;
}

static int battery_psy_set_charge_current(struct battery_chg_dev *bcdev,
					int val)
{
	int rc = 0;
	u32 fcc_ua, prev_fcc_ua;

	if (!bcdev->num_thermal_levels)
		return 0;

	if (bcdev->num_thermal_levels < 0) {
		pr_err("Incorrect num_thermal_levels\n");
		return -EINVAL;
	}

	if (val < 0 || val > bcdev->num_thermal_levels)
		return -EINVAL;

	if (bcdev->thermal_fcc_step == 0)
		fcc_ua = bcdev->thermal_levels[val];
	else
		fcc_ua = bcdev->psy_list[PSY_TYPE_BATTERY].prop[BATT_CHG_CTRL_LIM_MAX]
				- (bcdev->thermal_fcc_step * val);

	prev_fcc_ua = bcdev->thermal_fcc_ua;
	bcdev->thermal_fcc_ua = fcc_ua;

	if (prev_fcc_ua != bcdev->thermal_fcc_ua) {
		rc = __battery_psy_set_charge_current(bcdev, fcc_ua);
		if (!rc)
			bcdev->curr_thermal_level = val;
		else
			bcdev->thermal_fcc_ua = prev_fcc_ua;
	}

	return rc;
}

static int battery_psy_set_cycle_count(struct battery_chg_dev *bcdev,
					int CycleCount)
{
	int rc;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_BATTERY],
				BATT_CYCLE_COUNT, CycleCount);
	if (rc < 0) {
		pr_err("Failed to set cycle count %d, rc=%d\n", CycleCount, rc);
	} else {
		pr_err("Set CycleCount to %d\n", CycleCount);
	}

	return rc;
}
static int tbatt_filter_handler(struct battery_chg_dev *bcdev, int batt_temp)
{
	int ret = batt_temp;
	if (batt_temp == -400)
		return ret;

	if (!bcdev->tbatt_filter_save_degree) {
		bcdev->tbatt_filter_save_degree = batt_temp;
		jiffies_timer_expire(bcdev, true);
		ret = batt_temp;
	} else {
		if (abs(bcdev->tbatt_filter_save_degree - batt_temp) > bcdev->tbatt_filter_offset_degree &&
			!jiffies_timer_expire(bcdev, false)) {
			ret = bcdev->tbatt_filter_save_degree;
		 	pr_err("fliter out tbatt: %d, report last temp %d\n", batt_temp, bcdev->tbatt_filter_save_degree);
		} else {
			jiffies_timer_expire(bcdev, true);
			ret = batt_temp;
			bcdev->tbatt_filter_save_degree = batt_temp;
		}
	}
	return ret;
}

static int battery_psy_set_battery_fv(struct battery_chg_dev *bcdev, int val)
{
	int rc;
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_BATTERY],
					BATT_QNS_VOLT_MAX, val);
	if (rc < 0)
		pr_err("%s:%d failed to write property", __func__, __LINE__);

	return rc;
}

static int battery_psy_set_battery_fcc(struct battery_chg_dev *bcdev, int val)
{
	int rc;
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_BATTERY],
					BATT_CHG_QNS_CTRL_LIM_MAX, val);
	if (rc < 0)
		pr_err("%s:%d failed to write property", __func__, __LINE__);

	return rc;
}

static int battery_psy_get_prop(struct power_supply *psy,
		enum power_supply_property prop,
		union power_supply_propval *pval)
{
	struct battery_chg_dev *bcdev = power_supply_get_drvdata(psy);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int prop_id, rc;

	pval->intval = -ENODATA;

	/*
	 * The prop id of TIME_TO_FULL_NOW and TIME_TO_FULL_AVG is same.
	 * So, map the prop id of TIME_TO_FULL_AVG for TIME_TO_FULL_NOW.
	 */
	if (prop == POWER_SUPPLY_PROP_TIME_TO_FULL_NOW)
		prop = POWER_SUPPLY_PROP_TIME_TO_FULL_AVG;

	prop_id = get_property_id(pst, prop);
	if (prop_id < 0)
		return prop_id;

	rc = read_property_id(bcdev, pst, prop_id);
#ifdef QCOM_BASE
	if (rc < 0)
		return rc;
#else
	if (rc == -ETIMEDOUT) {
		pr_err("read prop:%d timeout, use old prop value\n", prop_id);
		rc = 0;
	} else if (rc < 0) {
		pr_err("read prop:%d error, rc = %d", prop_id, rc);
		return rc;
	}
#endif

	switch (prop) {
	case POWER_SUPPLY_PROP_MODEL_NAME:
		pval->strval = pst->model;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		pval->intval = DIV_ROUND_CLOSEST(pst->prop[prop_id], 100);
		if (bcdev->combo_batt_psy) {
			power_supply_get_property(bcdev->combo_batt_psy,
						prop, pval);
		}
		if (IS_ENABLED(CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG) &&
		   (bcdev->fake_soc >= 0 && bcdev->fake_soc <= 100))
			pval->intval = bcdev->fake_soc;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (bcdev->tbatt_filter_en) {
			pval->intval = tbatt_filter_handler(bcdev, DIV_ROUND_CLOSEST((int)pst->prop[prop_id], 10));
		} else {
			pval->intval = DIV_ROUND_CLOSEST((int)pst->prop[prop_id], 10);
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		pval->intval = bcdev->curr_thermal_level;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		pval->intval = bcdev->num_thermal_levels;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		if (bcdev->fcnt_charging_feature_supported) {
			bcdev->cycle_count = pst->prop[prop_id];
			pval->intval = bcdev->oem_cycle_count;
			break;
		}
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (bcdev->combo_batt_psy) {
			pval->intval = pst->prop[prop_id];
			power_supply_get_property(bcdev->combo_batt_psy,
						prop, pval);
			break;
		}
	default:
		pval->intval = pst->prop[prop_id];
		break;
	}

	return rc;
}

static int battery_psy_set_prop(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *pval)
{
	struct battery_chg_dev *bcdev = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		return battery_psy_set_charge_current(bcdev, pval->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return battery_psy_set_battery_fv(bcdev, pval->intval);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		return battery_psy_set_battery_fcc(bcdev, pval->intval);
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		return battery_psy_set_cycle_count(bcdev, pval->intval);
	default:
		return -EINVAL;
	}

	return 0;
}

static int battery_psy_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		return 1;
	default:
		break;
	}

	return 0;
}

static enum power_supply_property battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
};

static const struct power_supply_desc batt_psy_desc = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= battery_props,
	.num_properties		= ARRAY_SIZE(battery_props),
	.get_property		= battery_psy_get_prop,
	.set_property		= battery_psy_set_prop,
	.property_is_writeable	= battery_psy_prop_is_writeable,
};

static int battery_chg_init_psy(struct battery_chg_dev *bcdev)
{
	struct power_supply_config psy_cfg = {};
	int rc;

	psy_cfg.drv_data = bcdev;
	psy_cfg.of_node = bcdev->dev->of_node;
	bcdev->psy_list[PSY_TYPE_USB].psy =
		devm_power_supply_register(bcdev->dev, &usb_psy_desc, &psy_cfg);
	if (IS_ERR(bcdev->psy_list[PSY_TYPE_USB].psy)) {
		rc = PTR_ERR(bcdev->psy_list[PSY_TYPE_USB].psy);
		bcdev->psy_list[PSY_TYPE_USB].psy = NULL;
		pr_err("Failed to register USB power supply, rc=%d\n", rc);
		return rc;
	}

	if (bcdev->wls_not_supported) {
		pr_debug("Wireless charging is not supported\n");
	} else {
		bcdev->psy_list[PSY_TYPE_WLS].psy =
			devm_power_supply_register(bcdev->dev, &wls_psy_desc, &psy_cfg);

		if (IS_ERR(bcdev->psy_list[PSY_TYPE_WLS].psy)) {
			rc = PTR_ERR(bcdev->psy_list[PSY_TYPE_WLS].psy);
			bcdev->psy_list[PSY_TYPE_WLS].psy = NULL;
			pr_err("Failed to register wireless power supply, rc=%d\n", rc);
			return rc;
		}
	}

	bcdev->psy_list[PSY_TYPE_BATTERY].psy =
		devm_power_supply_register(bcdev->dev, &batt_psy_desc,
						&psy_cfg);
	if (IS_ERR(bcdev->psy_list[PSY_TYPE_BATTERY].psy)) {
		rc = PTR_ERR(bcdev->psy_list[PSY_TYPE_BATTERY].psy);
		bcdev->psy_list[PSY_TYPE_BATTERY].psy = NULL;
		pr_err("Failed to register battery power supply, rc=%d\n", rc);
		return rc;
	}

	if (of_find_property(bcdev->dev->of_node, "mmi,combo-batt-psy", NULL)) {
		bcdev->combo_batt_psy = devm_power_supply_get_by_phandle(
						bcdev->dev,
						"mmi,combo-batt-psy");
		if (IS_ERR_OR_NULL(bcdev->combo_batt_psy)) {
			pr_err("Couldn't get the combo-batt-psy\n");
			if (!bcdev->combo_batt_psy)
				return -ENODEV;
			else
				return PTR_ERR(bcdev->combo_batt_psy);
		}
	}
	return 0;
}

static void battery_chg_subsys_up_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work,
					struct battery_chg_dev, subsys_up_work);
	int rc;

	battery_chg_notify_enable(bcdev);

	/*
	 * Give some time after enabling notification so that USB adapter type
	 * information can be obtained properly which is essential for setting
	 * USB ICL.
	 */
	msleep(200);

	if (bcdev->last_fcc_ua) {
		rc = __battery_psy_set_charge_current(bcdev,
				bcdev->last_fcc_ua);
		if (rc < 0)
			pr_err("Failed to set FCC (%u uA), rc=%d\n",
				bcdev->last_fcc_ua, rc);
	}

	if (bcdev->usb_icl_ua) {
		rc = usb_psy_set_icl(bcdev, USB_INPUT_CURR_LIMIT,
				bcdev->usb_icl_ua);
		if (rc < 0)
			pr_err("Failed to set ICL(%u uA), rc=%d\n",
				bcdev->usb_icl_ua, rc);
	}
}

static int wireless_fw_send_firmware(struct battery_chg_dev *bcdev,
					const struct firmware *fw)
{
	struct wireless_fw_push_buf_req msg = {};
	const u8 *ptr;
	u32 i, num_chunks, partial_chunk_size;
	int rc;

	num_chunks = fw->size / WLS_FW_BUF_SIZE;
	partial_chunk_size = fw->size % WLS_FW_BUF_SIZE;

	if (!num_chunks)
		return -EINVAL;

	pr_debug("Updating FW...\n");

	ptr = fw->data;
	msg.hdr.owner = MSG_OWNER_BC;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = BC_WLS_FW_PUSH_BUF_REQ;

	for (i = 0; i < num_chunks; i++, ptr += WLS_FW_BUF_SIZE) {
		msg.fw_chunk_id = i + 1;
		memcpy(msg.buf, ptr, WLS_FW_BUF_SIZE);

		pr_info("sending FW chunk %u\n", i + 1);
		rc = battery_chg_fw_write(bcdev, &msg, sizeof(msg));
		if (rc < 0)
			return rc;
	}

	if (partial_chunk_size) {
		msg.fw_chunk_id = i + 1;
		memset(msg.buf, 0, WLS_FW_BUF_SIZE);
		memcpy(msg.buf, ptr, partial_chunk_size);

		pr_info("sending partial FW chunk %u\n", i + 1);
		rc = battery_chg_fw_write(bcdev, &msg, sizeof(msg));
		if (rc < 0)
			return rc;
	}

	return 0;
}

static int wireless_check_chip_id(struct battery_chg_dev *bcdev)
{
	struct wireless_check_chip_id req_msg = {};

	bcdev->wls_chip_id = 0x0000;

	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BC_WLS_CHECK_CHIP_ID;
	req_msg.ret_code = 0;

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int wireless_fw_check_for_update(struct battery_chg_dev *bcdev,
					u32 version, size_t size)
{
	struct wireless_fw_check_req req_msg = {};

	bcdev->wls_fw_update_reqd = false;

	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BC_WLS_FW_CHECK_UPDATE;
	req_msg.fw_version = version;
	req_msg.fw_size = size;
	req_msg.fw_crc = bcdev->wls_fw_crc;

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

#define IDT9415_FW_MAJOR_VER_OFFSET		0x84
#define IDT9415_FW_MINOR_VER_OFFSET		0x86
#define IDT_FW_MAJOR_VER_OFFSET		0x94
#define IDT_FW_MINOR_VER_OFFSET		0x96
#define CPS_FW_MAJOR_VER_OFFSET		0xc4
#define CPS_FW_MINOR_VER_OFFSET		0xc5
#define CPS4019_FW_MAJOR_VER_OFFSET1		0xc4
#define CPS4019_FW_MAJOR_VER_OFFSET2		0xc5
#define CPS4019_FW_MINOR_VER_OFFSET1		0xc6
#define CPS4019_FW_MINOR_VER_OFFSET2		0xc7
#define SC9624_FW_VER_OFFSET		0x104

static int wireless_fw_update(struct battery_chg_dev *bcdev, bool force)
{
	const struct firmware *fw;
	struct psy_state *pst;
	u32 version = 0;
	u16 maj_ver, min_ver, maj_ver1, maj_ver2, min_ver1, min_ver2;
	int rc;

	if (!bcdev->wls_fw_name && !bcdev->sc_wls_fw_name) {
		pr_err("wireless FW name is not specified\n");
		return -EINVAL;
	}

	pm_stay_awake(bcdev->dev);

	/*
	 * Check for USB presence. If nothing is connected, check whether
	 * battery SOC is at least 50% before allowing FW update.
	 */
	pst = &bcdev->psy_list[PSY_TYPE_USB];
	rc = read_property_id(bcdev, pst, USB_ONLINE);
	if (rc < 0)
		goto out;

	if (!pst->prop[USB_ONLINE]) {
		pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
		rc = read_property_id(bcdev, pst, BATT_CAPACITY);
		if (rc < 0)
			goto out;

		if ((pst->prop[BATT_CAPACITY] / 100) < 20) {
			pr_err("Battery SOC should be at least 20%% or connect charger\n");
			rc = -EINVAL;
			goto out;
		}
	}

	if (bcdev->sc_wls_fw_name)
	{
		rc = wireless_check_chip_id(bcdev);
		if (rc) {
			pr_err("Couldn't get chip id rc=%d\n", rc);
			goto out;
		}

		pr_err("wireless chip id = %04x\n", bcdev->wls_chip_id);
		if(bcdev->wls_chip_id == WLS_SC9624) {
			rc = firmware_request_nowarn(&fw, bcdev->sc_wls_fw_name, bcdev->dev);
			if (rc) {
				pr_err("Couldn't get southchip firmware rc=%d\n", rc);
				goto out;
			}
			// version = min_ver << 16 | maj_ver;
			version = *(u32 *)&fw->data[SC9624_FW_VER_OFFSET];
			pr_info("southchip fw version: %08x\n", version);
		} else{
			rc = firmware_request_nowarn(&fw, bcdev->wls_fw_name, bcdev->dev);
			if (rc) {
				pr_err("Couldn't get southchip firmware rc=%d\n", rc);
				goto out;
			}
			if (bcdev->wls_fw_vendor == WLS_CPS4019) {
				maj_ver1 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MAJOR_VER_OFFSET1));
				maj_ver = maj_ver1 >> 8;
				maj_ver2 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MAJOR_VER_OFFSET2));
				maj_ver2 = maj_ver2 & 0xFF00;
				maj_ver = maj_ver + maj_ver2;
				min_ver1 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MINOR_VER_OFFSET1));
				min_ver = min_ver1 >> 8;
				min_ver2 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MINOR_VER_OFFSET2));
				min_ver2 = min_ver2 & 0xFF00;
				min_ver = min_ver + min_ver2;
				pr_info("WLS_CPS 4019 maj_ver %#x, min_ver %#x\n", maj_ver, min_ver);
				version = maj_ver << 16 | min_ver;
			} else if (bcdev->wls_fw_vendor == WLS_CPS) {
				maj_ver = be16_to_cpu(*(__le16 *)(fw->data + CPS_FW_MAJOR_VER_OFFSET));
				maj_ver = maj_ver >> 8;
				min_ver = be16_to_cpu(*(__le16 *)(fw->data + CPS_FW_MINOR_VER_OFFSET));
				min_ver = min_ver >> 8;
				pr_info("WLS_CPS maj_ver %#x, min_ver %#x\n", maj_ver, min_ver);
				version = maj_ver << 16 | min_ver;
			} else {
				maj_ver = le16_to_cpu(*(__le16 *)(fw->data + IDT_FW_MAJOR_VER_OFFSET));
				min_ver = le16_to_cpu(*(__le16 *)(fw->data + IDT_FW_MINOR_VER_OFFSET));
				version = maj_ver << 16 | min_ver;
			}
		}
	} else {
		rc = firmware_request_nowarn(&fw, bcdev->wls_fw_name, bcdev->dev);
		if (rc) {
				pr_err("Couldn't get southchip firmware rc=%d\n", rc);
				goto out;
			}
		if (bcdev->wls_fw_vendor == WLS_CPS4019) {
			maj_ver1 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MAJOR_VER_OFFSET1));
			maj_ver = maj_ver1 >> 8;
			maj_ver2 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MAJOR_VER_OFFSET2));
			maj_ver2 = maj_ver2 & 0xFF00;
			maj_ver = maj_ver + maj_ver2;
			min_ver1 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MINOR_VER_OFFSET1));
			min_ver = min_ver1 >> 8;
			min_ver2 = be16_to_cpu(*(__le16 *)(fw->data + CPS4019_FW_MINOR_VER_OFFSET2));
			min_ver2 = min_ver2 & 0xFF00;
			min_ver = min_ver + min_ver2;
			pr_info("WLS_CPS 4019 maj_ver %#x, min_ver %#x\n", maj_ver, min_ver);
			version = maj_ver << 16 | min_ver;
		} else if (bcdev->wls_fw_vendor == WLS_CPS) {
			maj_ver = be16_to_cpu(*(__le16 *)(fw->data + CPS_FW_MAJOR_VER_OFFSET));
			maj_ver = maj_ver >> 8;
			min_ver = be16_to_cpu(*(__le16 *)(fw->data + CPS_FW_MINOR_VER_OFFSET));
			min_ver = min_ver >> 8;
			pr_info("WLS_CPS maj_ver %#x, min_ver %#x\n", maj_ver, min_ver);
			version = maj_ver << 16 | min_ver;
		} else {
			maj_ver = le16_to_cpu(*(__le16 *)(fw->data + IDT_FW_MAJOR_VER_OFFSET));
			min_ver = le16_to_cpu(*(__le16 *)(fw->data + IDT_FW_MINOR_VER_OFFSET));
			version = maj_ver << 16 | min_ver;
		}
	}

	if (!fw || !fw->data || !fw->size) {
		pr_err("Invalid firmware\n");
		rc = -EINVAL;
		goto release_fw;
	}

	if (fw->size < bcdev->wls_fw_size_min) {
		pr_err("Invalid firmware size (%zu < %zu)\n", fw->size, bcdev->wls_fw_size_min);
		rc = -EINVAL;
		goto release_fw;
	}

	if (force)
		version = UINT_MAX;

	pr_info("FW size: %zu version: %#x\n", fw->size, version);
	rc = wireless_fw_check_for_update(bcdev, version, fw->size);
	if (rc < 0) {
		pr_err("Wireless FW update not needed, rc=%d\n", rc);
		goto release_fw;
	}

	pr_info("southchip %d\n", bcdev->wls_fw_update_reqd);

	if (!bcdev->wls_fw_update_reqd) {
		pr_warn("Wireless FW update not required\n");
		goto release_fw;
	}

	/* Wait for IDT to be setup by charger firmware */
	msleep(WLS_FW_PREPARE_TIME_MS);
        wls_fw_udating = true;
	reinit_completion(&bcdev->fw_update_ack);
	rc = wireless_fw_send_firmware(bcdev, fw);
	if (rc < 0) {
		pr_err("Failed to send FW chunk, rc=%d\n", rc);
		goto release_fw;
	}

	pr_debug("Waiting for fw_update_ack\n");
	rc = wait_for_completion_timeout(&bcdev->fw_update_ack,
				msecs_to_jiffies(bcdev->wls_fw_update_time_ms));
	if (!rc) {
		pr_err("Error, timed out updating firmware\n");
		rc = -ETIMEDOUT;
		goto release_fw;
	} else {
		pr_debug("Waited for %d ms\n",
			bcdev->wls_fw_update_time_ms - jiffies_to_msecs(rc));
		rc = 0;
	}

	pr_info("Wireless FW update done\n");

release_fw:
	bcdev->wls_fw_crc = 0;
	release_firmware(fw);
out:
	pm_relax(bcdev->dev);
	wls_fw_udating = false;
	return rc;
}

static ssize_t wireless_fw_update_time_ms_store(struct class *c,
				struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	if (kstrtou32(buf, 0, &bcdev->wls_fw_update_time_ms))
		return -EINVAL;

	return count;
}

static ssize_t wireless_fw_update_time_ms_show(struct class *c,
				struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%u\n", bcdev->wls_fw_update_time_ms);
}
static CLASS_ATTR_RW(wireless_fw_update_time_ms);

static ssize_t wireless_fw_crc_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	u16 val;

	if (kstrtou16(buf, 0, &val) || !val)
		return -EINVAL;

	bcdev->wls_fw_crc = val;

	return count;
}
static CLASS_ATTR_WO(wireless_fw_crc);

static ssize_t wireless_fw_version_show(struct class *c,
					struct class_attribute *attr,
					char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct wireless_fw_get_version_req req_msg = {};
	int rc;

	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BC_WLS_FW_GET_VERSION;

	rc = battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
	if (rc < 0) {
		pr_err("Failed to get FW version rc=%d\n", rc);
		return rc;
	}

	return scnprintf(buf, PAGE_SIZE, "%#x\n", bcdev->wls_fw_version);
}
static CLASS_ATTR_RO(wireless_fw_version);

static ssize_t wireless_fw_force_update_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	bool val;
	int rc;

	if (kstrtobool(buf, &val) || !val)
		return -EINVAL;

	rc = wireless_fw_update(bcdev, true);
	if (rc < 0)
		return rc;

	return count;
}
static CLASS_ATTR_WO(wireless_fw_force_update);

static ssize_t wireless_fw_update_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	bool val;
	int rc;

	if (kstrtobool(buf, &val) || !val)
		return -EINVAL;

	rc = wireless_fw_update(bcdev, false);
	if (rc < 0)
		return rc;

	return count;
}
static CLASS_ATTR_WO(wireless_fw_update);

static ssize_t usb_typec_compliant_show(struct class *c,
				struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_TYPEC_COMPLIANT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			(int)pst->prop[USB_TYPEC_COMPLIANT]);
}
static CLASS_ATTR_RO(usb_typec_compliant);

static ssize_t usb_real_type_show(struct class *c,
				struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_REAL_TYPE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			get_usb_type_name(pst->prop[USB_REAL_TYPE]));
}
static CLASS_ATTR_RO(usb_real_type);

static ssize_t restrict_cur_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	u32 fcc_ua, prev_fcc_ua;

	if (kstrtou32(buf, 0, &fcc_ua) || fcc_ua > bcdev->thermal_fcc_ua)
		return -EINVAL;

	prev_fcc_ua = bcdev->restrict_fcc_ua;
	bcdev->restrict_fcc_ua = fcc_ua;
	if (bcdev->restrict_chg_en) {
		rc = __battery_psy_set_charge_current(bcdev, fcc_ua);
		if (rc < 0) {
			bcdev->restrict_fcc_ua = prev_fcc_ua;
			return rc;
		}
	}

	return count;
}

static ssize_t restrict_cur_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%u\n", bcdev->restrict_fcc_ua);
}
static CLASS_ATTR_RW(restrict_cur);

static ssize_t restrict_chg_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	bcdev->restrict_chg_en = val;
	rc = __battery_psy_set_charge_current(bcdev, bcdev->restrict_chg_en ?
			bcdev->restrict_fcc_ua : bcdev->thermal_fcc_ua);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t restrict_chg_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->restrict_chg_en);
}
static CLASS_ATTR_RW(restrict_chg);

static ssize_t fake_soc_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	bcdev->fake_soc = val;
	pr_debug("Set fake soc to %d\n", val);

	if (IS_ENABLED(CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG) && pst->psy)
		power_supply_changed(pst->psy);

	return count;
}

static ssize_t fake_soc_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->fake_soc);
}
static CLASS_ATTR_RW(fake_soc);

static ssize_t wireless_boost_en_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_WLS],
				WLS_BOOST_EN, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t wireless_boost_en_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_WLS];
	int rc;

	rc = read_property_id(bcdev, pst, WLS_BOOST_EN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[WLS_BOOST_EN]);
}
static CLASS_ATTR_RW(wireless_boost_en);

static ssize_t moisture_detection_en_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB],
				USB_MOISTURE_DET_EN, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t moisture_detection_en_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_MOISTURE_DET_EN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			pst->prop[USB_MOISTURE_DET_EN]);
}
static CLASS_ATTR_RW(moisture_detection_en);

static ssize_t moisture_detection_status_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_MOISTURE_DET_STS);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			pst->prop[USB_MOISTURE_DET_STS]);
}
static CLASS_ATTR_RO(moisture_detection_status);

static ssize_t resistance_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_RESISTANCE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[BATT_RESISTANCE]);
}
static CLASS_ATTR_RO(resistance);

static ssize_t flash_active_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, F_ACTIVE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[F_ACTIVE]);
}
static CLASS_ATTR_RO(flash_active);

static ssize_t soh_show(struct class *c, struct class_attribute *attr,
			char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_SOH);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[BATT_SOH]);
}
static CLASS_ATTR_RO(soh);

static ssize_t ship_mode_en_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	if (kstrtobool(buf, &bcdev->ship_mode_en))
		return -EINVAL;

	return count;
}

static ssize_t ship_mode_en_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->ship_mode_en);
}
static CLASS_ATTR_RW(ship_mode_en);

static ssize_t dbg_health_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int val;
	char *event_string = NULL;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	bcdev->dbg_health = val;
	pr_debug("Set dbg health to %d\n", val);

	event_string = kmalloc(CHG_SHOW_MAX_SIZE, GFP_KERNEL);
	if (event_string) {
		switch (bcdev->dbg_health) {
		case HEALTH_NOTIFY_EVENT_TYPE_GOOD:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Good");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_OVERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Battery_over_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_COLD:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Cold");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_DEVICE_HOT:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Device_hot");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_USB_MOISTURE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Usb_moisture_det");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERTIME:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Chg_overtime");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_UNDERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Under_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Over_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_REMOVE_CHG:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Usb_connect_err");
			break;
		default:
			pr_info("Invalid notify event type %d\n", bcdev->dbg_health);
			goto out;
		}

		if (bcdev->batt_uenvp[0])
			bcdev->batt_uenvp[0] = NULL;
		bcdev->batt_uenvp[0] = event_string;
		bcdev->batt_uenvp[1] = NULL;

		printk("[charge monitor work entry event_string: %s]\n", event_string);
		kobject_uevent_env(&pst->psy->dev.kobj,
					KOBJ_CHANGE,
					bcdev->batt_uenvp);
	}
out:
	kfree(event_string);
	return count;
}

static ssize_t dbg_health_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->dbg_health);
}
static CLASS_ATTR_RW(dbg_health);

static ssize_t cc_orientation_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	int rc;

	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];

	rc = read_property_id(bcdev, pst, USB_CC_ORIENTATION);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[USB_CC_ORIENTATION]);
}
static CLASS_ATTR_RO(cc_orientation);

static ssize_t cc_open_show(struct class *c,struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_CC_OPEN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
				pst->prop[USB_CC_OPEN]);
}
static CLASS_ATTR_RO(cc_open);

static ssize_t present_usb_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	int rc;

	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];

	rc = read_property_id(bcdev, pst, USB_PRESENT_USB);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[USB_PRESENT_USB]);
}
static CLASS_ATTR_RO(present_usb);

static ssize_t otg_enable_store(struct class *c, struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	bool val;
	int rc;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	printk("change otg to %d",val);
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_OTG_ENABLE, val);
	if (rc < 0) {
		pr_err("%s:%d failed to write property", __func__, __LINE__);
	}

	return count;
}

static ssize_t otg_enable_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	int rc;

	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];

	rc = read_property_id(bcdev, pst, USB_OTG_ENABLE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[USB_OTG_ENABLE]);
}
static CLASS_ATTR_RW(otg_enable);

static ssize_t curr_fcc_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->curr_fcc);
}
static CLASS_ATTR_RO(curr_fcc);

static ssize_t charging_enabled_store(struct class *c,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	if (val) {
		/*
		 * Enable charging, i.e. set the restricted current back to
		 * the thermal limit and unset the restriction boolean flag.
		 */
		rc = __battery_psy_set_charge_current(bcdev,
				bcdev->thermal_fcc_ua);
		if (rc < 0)
			return rc;
		bcdev->restrict_fcc_ua = bcdev->thermal_fcc_ua;
		bcdev->restrict_chg_en = 0;
	} else {
		/*
		 * Disable charging, i.e. set the restricted current to zero
		 * and set the restriction boolean flag.
		 */
		rc = __battery_psy_set_charge_current(bcdev, 0 /* 0 uA */);
		if (rc < 0)
			return rc;
		bcdev->restrict_fcc_ua = 0;
		bcdev->restrict_chg_en = 1;
	}

	return count;
}

static ssize_t charging_enabled_show(struct class *c,
				     struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	bool val = !bcdev->restrict_chg_en && bcdev->restrict_fcc_ua;

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RW(charging_enabled);

static struct attribute *battery_class_attrs[] = {
	&class_attr_soh.attr,
	&class_attr_resistance.attr,
	&class_attr_flash_active.attr,
	&class_attr_moisture_detection_status.attr,
	&class_attr_moisture_detection_en.attr,
	&class_attr_wireless_boost_en.attr,
	&class_attr_fake_soc.attr,
	&class_attr_wireless_fw_update.attr,
	&class_attr_wireless_fw_force_update.attr,
	&class_attr_wireless_fw_version.attr,
	&class_attr_wireless_fw_crc.attr,
	&class_attr_wireless_fw_update_time_ms.attr,
	&class_attr_ship_mode_en.attr,
	&class_attr_restrict_chg.attr,
	&class_attr_restrict_cur.attr,
	&class_attr_usb_real_type.attr,
	&class_attr_usb_typec_compliant.attr,
	&class_attr_dbg_health.attr,
	&class_attr_cc_orientation.attr,
	&class_attr_cc_open.attr,
	&class_attr_present_usb.attr,
	&class_attr_otg_enable.attr,
	&class_attr_curr_fcc.attr,
	&class_attr_charging_enabled.attr,
	NULL,
};
ATTRIBUTE_GROUPS(battery_class);

static struct attribute *battery_class_no_wls_attrs[] = {
	&class_attr_soh.attr,
	&class_attr_resistance.attr,
	&class_attr_flash_active.attr,
	&class_attr_moisture_detection_status.attr,
	&class_attr_moisture_detection_en.attr,
	&class_attr_fake_soc.attr,
	&class_attr_ship_mode_en.attr,
	&class_attr_restrict_chg.attr,
	&class_attr_restrict_cur.attr,
	&class_attr_usb_real_type.attr,
	&class_attr_usb_typec_compliant.attr,
	&class_attr_dbg_health.attr,
	&class_attr_cc_orientation.attr,
	&class_attr_cc_open.attr,
	&class_attr_present_usb.attr,
	&class_attr_otg_enable.attr,
	&class_attr_curr_fcc.attr,
	&class_attr_charging_enabled.attr,
	NULL,
};
ATTRIBUTE_GROUPS(battery_class_no_wls);

#ifdef CONFIG_DEBUG_FS
static void battery_chg_add_debugfs(struct battery_chg_dev *bcdev)
{
	int rc;
	struct dentry *dir;

	dir = debugfs_create_dir("battery_charger", NULL);
	if (IS_ERR(dir)) {
		rc = PTR_ERR(dir);
		pr_err("Failed to create charger debugfs directory, rc=%d\n",
			rc);
		return;
	}

	bcdev->debugfs_dir = dir;
	debugfs_create_bool("block_tx", 0600, dir, &bcdev->block_tx);
}
#else
static void battery_chg_add_debugfs(struct battery_chg_dev *bcdev) { }
#endif

static int battery_chg_parse_dt(struct battery_chg_dev *bcdev)
{
	struct device_node *node = bcdev->dev->of_node;
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int i, rc, len;
	u32 prev, val;

	bcdev->tbatt_filter_en = of_property_read_bool(node, "qcom,tbatt-filter-en");

	rc = of_property_read_u32(node, "qcom,tbatt-filter-interval-ms",
				&bcdev->tbatt_filter_interval_ms);
	if (rc < 0) {
		bcdev->tbatt_filter_interval_ms = 5000;
	}

	rc = of_property_read_u32(node, "qcom,tbatt-filter-offset-degree",
				&bcdev->tbatt_filter_offset_degree);
	if (rc < 0) {
		bcdev->tbatt_filter_offset_degree = 50;
	}

	bcdev->wls_not_supported = of_property_read_bool(node,
			"qcom,wireless-charging-not-supported");

	bcdev->fcnt_charging_feature_supported = of_property_read_bool(node,
			"fcnt,charging-feature-supported");

	of_property_read_string(node, "qcom,wireless-fw-name",
				&bcdev->wls_fw_name);

	of_property_read_string(node, "qcom,wireless-fw-name-sc",
				&bcdev->sc_wls_fw_name);

	of_property_read_u32(node, "qcom,shutdown-voltage",
				&bcdev->shutdown_volt_mv);

	rc = of_property_read_u32(node, "qcom,wireless-fw-size_min",
				&bcdev->wls_fw_size_min);
	if (rc < 0) {
		bcdev->wls_fw_size_min = SZ_16K;
		pr_info("fw size min do not defined, use 16k as default (%d)\n",
			__func__, bcdev->wls_fw_size_min);
	}

	if (bcdev->wls_fw_name && strstr(bcdev->wls_fw_name, "cps4019"))
		bcdev->wls_fw_vendor = WLS_CPS4019;
	else if (bcdev->wls_fw_name && strstr(bcdev->wls_fw_name, "cps"))
		bcdev->wls_fw_vendor = WLS_CPS;

	rc = read_property_id(bcdev, pst, BATT_CHG_CTRL_LIM_MAX);
	if (rc < 0) {
		pr_err("Failed to read prop BATT_CHG_CTRL_LIM_MAX, rc=%d\n",
			rc);
		return rc;
	}

	rc = of_property_count_elems_of_size(node, "qcom,thermal-mitigation",
						sizeof(u32));
	if (rc <= 0) {

		rc = of_property_read_u32(node, "qcom,thermal-mitigation-step",
						&val);

		if (rc < 0)
			return 0;

		if (val < 500000 || val >= pst->prop[BATT_CHG_CTRL_LIM_MAX]) {
			pr_err("thermal_fcc_step %d is invalid\n", val);
			return -EINVAL;
		}

		bcdev->thermal_fcc_step = val;
		len = pst->prop[BATT_CHG_CTRL_LIM_MAX] / bcdev->thermal_fcc_step;

		/*
		 * FCC values must be above 500mA.
		 * Since len is truncated when calculated, check and adjust len so
		 * that the above requirement is met.
		 */
		if (pst->prop[BATT_CHG_CTRL_LIM_MAX] - (bcdev->thermal_fcc_step * len) < 500000)
			len = len - 1;
	} else {
		bcdev->thermal_fcc_step = 0;
		len = rc;
		prev = pst->prop[BATT_CHG_CTRL_LIM_MAX];

		for (i = 0; i < len; i++) {
			rc = of_property_read_u32_index(node, "qcom,thermal-mitigation",
				i, &val);
			if (rc < 0)
				return rc;

			if (val > prev) {
				pr_err("Thermal levels should be in descending order\n");
				bcdev->num_thermal_levels = -EINVAL;
				return 0;
			}

			prev = val;
		}

		bcdev->thermal_levels = devm_kcalloc(bcdev->dev, len + 1,
						sizeof(*bcdev->thermal_levels),
						GFP_KERNEL);
		if (!bcdev->thermal_levels)
			return -ENOMEM;

		/*
		 * Element 0 is for normal charging current. Elements from index 1
		 * onwards is for thermal mitigation charging currents.
		 */

		bcdev->thermal_levels[0] = pst->prop[BATT_CHG_CTRL_LIM_MAX];

		rc = of_property_read_u32_array(node, "qcom,thermal-mitigation",
					&bcdev->thermal_levels[1], len);
		if (rc < 0) {
			pr_err("Error in reading qcom,thermal-mitigation, rc=%d\n", rc);
			return rc;
		}
	}

	bcdev->num_thermal_levels = len;
	bcdev->thermal_fcc_ua = pst->prop[BATT_CHG_CTRL_LIM_MAX];

	return 0;
}

static int battery_chg_ship_mode(struct notifier_block *nb, unsigned long code,
		void *unused)
{
	struct battery_charger_notify_msg msg_notify = { { 0 } };
	struct battery_charger_ship_mode_req_msg msg = { { 0 } };
	struct battery_chg_dev *bcdev = container_of(nb, struct battery_chg_dev,
						     reboot_notifier);
	int rc;

	msg_notify.hdr.owner = MSG_OWNER_BC;
	msg_notify.hdr.type = MSG_TYPE_NOTIFY;
	msg_notify.hdr.opcode = BC_SHUTDOWN_NOTIFY;

	rc = battery_chg_write(bcdev, &msg_notify, sizeof(msg_notify));
	if (rc < 0)
		pr_err("Failed to send shutdown notification rc=%d\n", rc);

	if (!bcdev->ship_mode_en)
		return NOTIFY_DONE;

	msg.hdr.owner = MSG_OWNER_BC;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = BC_SHIP_MODE_REQ_SET;
	msg.ship_mode_type = SHIP_MODE_PMIC;

	if (code == SYS_POWER_OFF) {
		rc = battery_chg_write(bcdev, &msg, sizeof(msg));
		if (rc < 0)
			pr_emerg("Failed to write ship mode: %d\n", rc);
	}

	return NOTIFY_DONE;
}

static void panel_event_notifier_callback(enum panel_event_notifier_tag tag,
			struct panel_event_notification *notification, void *data)
{
	struct battery_chg_dev *bcdev = data;

	if (!notification) {
		pr_debug("Invalid panel notification\n");
		return;
	}

	pr_debug("panel event received, type: %d\n", notification->notif_type);
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		battery_chg_notify_disable(bcdev);
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		battery_chg_notify_enable(bcdev);
		break;
	default:
		pr_debug("Ignore panel event: %d\n", notification->notif_type);
		break;
	}
}

static int battery_chg_register_panel_notifier(struct battery_chg_dev *bcdev)
{
	struct device_node *np = bcdev->dev->of_node;
	struct device_node *pnode;
	struct drm_panel *panel, *active_panel = NULL;
	void *cookie = NULL;
	int i, count, rc;

	count = of_count_phandle_with_args(np, "qcom,display-panels", NULL);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		pnode = of_parse_phandle(np, "qcom,display-panels", i);
		if (!pnode)
			return -ENODEV;

		panel = of_drm_find_panel(pnode);
		of_node_put(pnode);
		if (!IS_ERR(panel)) {
			active_panel = panel;
			break;
		}
	}

	if (!active_panel) {
		rc = PTR_ERR(panel);
		if (rc != -EPROBE_DEFER)
			dev_err(bcdev->dev, "Failed to find active panel, rc=%d\n");
		return rc;
	}

	cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_BATTERY_CHARGER,
			active_panel,
			panel_event_notifier_callback,
			(void *)bcdev);
	if (IS_ERR(cookie)) {
		rc = PTR_ERR(cookie);
		dev_err(bcdev->dev, "Failed to register panel event notifier, rc=%d\n", rc);
		return rc;
	}

	pr_debug("register panel notifier successful\n");
	bcdev->notifier_cookie = cookie;
	return 0;
}

static int register_extcon_conn_type(struct battery_chg_dev *bcdev)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_CONNECTOR_TYPE);
	if (rc < 0) {
		pr_err("Failed to read prop USB_CONNECTOR_TYPE, rc=%d\n",
			rc);
		return rc;
	}

	bcdev->connector_type = pst->prop[USB_CONNECTOR_TYPE];
	bcdev->usb_prev_mode = EXTCON_NONE;

	bcdev->extcon = devm_extcon_dev_allocate(bcdev->dev,
						bcdev_usb_extcon_cable);
	if (IS_ERR(bcdev->extcon)) {
		rc = PTR_ERR(bcdev->extcon);
		pr_err("Failed to allocate extcon device rc=%d\n", rc);
		return rc;
	}

	rc = devm_extcon_dev_register(bcdev->dev, bcdev->extcon);
	if (rc < 0) {
		pr_err("Failed to register extcon device rc=%d\n", rc);
		return rc;
	}
	rc = extcon_set_property_capability(bcdev->extcon, EXTCON_USB,
					    EXTCON_PROP_USB_SS);
	rc |= extcon_set_property_capability(bcdev->extcon,
					     EXTCON_USB_HOST, EXTCON_PROP_USB_SS);
	if (rc < 0)
		pr_err("failed to configure extcon capabilities rc=%d\n", rc);
	else
		pr_debug("Registered extcon, connector_type %s\n",
			 bcdev->connector_type ? "uusb" : "Typec");

	return rc;
}

static void update_battery_data(struct battery_chg_dev *bcdev)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_PRESENT);
	if (rc < 0) {
		pr_err("Failed to read BATT_PRESENT, rc=%d\n", rc);
		return;
	}
	bcdev->batt_present = pst->prop[BATT_PRESENT];

	rc = read_property_id(bcdev, pst, BATT_STATUS);
	if (rc < 0) {
		pr_err("Failed to read BATT_STATUS, rc=%d\n", rc);
		return;
	}
	bcdev->batt_status = pst->prop[BATT_STATUS];

	rc = read_property_id(bcdev, pst, BATT_TEMP);
	if (rc < 0) {
		pr_err("Failed to read BATT_TEMP, rc=%d\n", rc);
		return;
	}
	bcdev->batt_temp = DIV_ROUND_CLOSEST((int)pst->prop[BATT_TEMP], 10);

	rc = read_property_id(bcdev, pst, BATT_CAPACITY);
	if (rc < 0) {
		pr_err("Failed to read BATT_CAPACITY, rc=%d\n", rc);
		return;
	}
	bcdev->batt_soc = DIV_ROUND_CLOSEST(pst->prop[BATT_CAPACITY], 100);

	rc = read_property_id(bcdev, pst, BATT_CURR_NOW);
	if (rc < 0) {
		pr_err("Failed to read BATT_CURR_NOW, rc=%d\n", rc);
		return;
	}
	bcdev->batt_current = pst->prop[BATT_CURR_NOW];

	rc = read_property_id(bcdev, pst, BATT_VOLT_NOW);
	if (rc < 0) {
		pr_err("Failed to read BATT_VOLT_NOW, rc=%d\n", rc);
		return;
	}
	bcdev->batt_voltage = pst->prop[BATT_VOLT_NOW];

	rc = read_property_id(bcdev, pst, BATT_VOLT_OCV);
	if (rc < 0) {
		pr_err("Failed to read BATT_VOLT_OCV, rc=%d\n", rc);
		return;
	}
	bcdev->batt_ocv = pst->prop[BATT_VOLT_OCV];

	rc = read_property_id(bcdev, pst, BATT_VOLT_MAX);
	if (rc < 0) {
		pr_err("Failed to read BATT_VOLT_MAX, rc=%d\n", rc);
		return;
	}
	bcdev->batt_volmax = pst->prop[BATT_VOLT_MAX];

	rc = read_property_id(bcdev, pst, BATT_CHG_CTRL_LIM);
	if (rc < 0) {
		pr_err("Failed to read BATT_CHG_CTRL_LIM, rc=%d\n", rc);
		return;
	}
	bcdev->curr_fcc = pst->prop[BATT_CHG_CTRL_LIM];
}

static void update_chg_data(struct battery_chg_dev *bcdev)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_USB];
	int rc;

	rc = read_property_id(bcdev, pst, USB_ONLINE);
	if (rc < 0) {
		pr_err("Failed to read USB_ONLINE, rc=%d\n", rc);
		return;
	}
	bcdev->usb_online = pst->prop[USB_ONLINE];

	rc = read_property_id(bcdev, pst, USB_CURR_NOW);
	if (rc < 0) {
		pr_err("Failed to read USB_CURR_NOW, rc=%d\n", rc);
		return;
	}
	bcdev->usb_current = pst->prop[USB_CURR_NOW];

	rc = read_property_id(bcdev, pst, USB_VOLT_NOW);
	if (rc < 0) {
		pr_err("Failed to read USB_VOLT_NOW, rc=%d\n", rc);
		return;
	}
	bcdev->usb_voltage = pst->prop[USB_VOLT_NOW];

	rc = read_property_id(bcdev, pst, USB_REAL_TYPE);
	if (rc < 0) {
		pr_err("Failed to read USB_REAL_TYPE, rc=%d\n", rc);
		return;
	}
	bcdev->real_chg_type = pst->prop[USB_REAL_TYPE];//get_usb_type_name(pst->prop[USB_REAL_TYPE]);

	rc = read_property_id(bcdev, pst, USB_VOLT_MAX);
	if (rc < 0) {
		pr_err("Failed to read USB_VOLT_MAX, rc=%d\n", rc);
		return;
	}
	bcdev->usb_vol_max = pst->prop[USB_VOLT_MAX];

	rc = read_property_id(bcdev, pst, USB_TEMP);
	if (rc < 0) {
		pr_err("Failed to read USB_TEMP, rc=%d\n", rc);
		return;
	}
	bcdev->usb_temp = pst->prop[USB_TEMP];

	rc = read_property_id(bcdev, pst, USB_PRESENT_USB);
	if (rc < 0) {
		pr_err("Failed to read USB_PRESENT, rc=%d\n", rc);
		return;
	}
	bcdev->usb_present = pst->prop[USB_PRESENT_USB];

	rc = read_property_id(bcdev, pst, USB_OTG_ENABLE);
	if (rc < 0) {
		pr_err("Failed to read USB_OTG_ENABLE, rc=%d\n", rc);
		return;
	}
	bcdev->otg_enable = pst->prop[USB_OTG_ENABLE];

	rc = read_property_id(bcdev, pst, USB_MOISTURE_DET_STS);
	if (rc < 0) {
		pr_err("Failed to read USB_MOISTURE_DET_STS, rc=%d\n", rc);
		return;
	}
	bcdev->lpd_status = pst->prop[USB_MOISTURE_DET_STS];
}

static void battery_chg_print_log(struct battery_chg_dev *bcdev)
{
	pr_info("[charge_info_usb]usb_present:%d /usb_online:%d / usb_voltage:%d / usb_current:%d / chg_type:%s / usb_vol_max:%d / usb_temp:%d / otg_enable:%d\n",
		bcdev->usb_present,bcdev->usb_online,bcdev->usb_voltage,bcdev->usb_current,get_usb_type_name(bcdev->real_chg_type),bcdev->usb_vol_max,bcdev->usb_temp,bcdev->otg_enable);

	pr_info("[charge_info_battery]batt_present:%d / batt_status:%d / batt_temp:%d / batt_current:%d / batt_voltage:%d / batt_ocv:%d / batt_volmax:%d / batt_soc:%d / fake_soc:%d\n",
		bcdev->batt_present,bcdev->batt_status,bcdev->batt_temp,bcdev->batt_current,bcdev->batt_voltage,bcdev->batt_ocv,bcdev->batt_volmax,bcdev->batt_soc,bcdev->fake_soc,bcdev->batt_health);

	pr_info("[charge_info_other] curr_fcc = %d / water_det = %d / system_temp_level = %d / full_charge_setting:%d /cycle_count:%d /oem_cycle_count:%d / charge_full:%d /age:%d/ batt_health:%d\n",
		bcdev->curr_fcc,bcdev->lpd_status,bcdev->curr_thermal_level,bcdev->charge_full,bcdev->cycle_count,bcdev->oem_cycle_count,bcdev->charge_full,bcdev->age, bcdev->batt_health);
}

static void update_battery_charge_data(struct battery_chg_dev *bcdev)
{
	update_battery_data(bcdev);
	update_chg_data(bcdev);
}

#define CHG_UPDATE_INTERVAL_SEC 		5
#define TEMP_CNT	1
#define HIGH_TEMP	450
#define LOW_TEMP	0
static bool battery_chg_check_tbatt_is_good(struct battery_chg_dev *bcdev)
{
	static bool ret = true;
	static int temp_counts = 0;

	if(bcdev->batt_temp >= HIGH_TEMP || bcdev->batt_temp < LOW_TEMP){
		temp_counts++;
		if (temp_counts >= TEMP_CNT) {
			temp_counts = 0;
			ret = false;
		}
	}else {
		temp_counts = 0;
		ret = true;
	}

	return ret;
}

#define VBAT_CNT	2
#define HIGH_VBAT	120000
static bool battery_chg_check_vbatt_is_good(struct battery_chg_dev *bcdev)
{
	static bool ret = true;
	static int vbat_counts = 0;

	if (bcdev->batt_voltage >= (bcdev->batt_volmax + HIGH_VBAT)) {
		vbat_counts++;
		if (vbat_counts >= VBAT_CNT) {
			vbat_counts = 0;
			ret = false;
		}
	} else {
		vbat_counts = 0;
		ret = true;
	}
	return ret;
}

#define	BOOT_VOLTAGE	3400000
#define	CAP	5000
#define CHG_TIME_LIMIT	(CAP/500 * 60 + 120)
static void battery_chg_check_time_is_good(struct battery_chg_dev *bcdev)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc = 0;

	if(bcdev->usb_online){
		bcdev->total_time += CHG_UPDATE_INTERVAL_SEC;
	}else{
		bcdev->total_time = 0;
	}

	if (bcdev->total_time >= CHG_TIME_LIMIT*60){
		bcdev->chging_over_time = true;
		pr_info("the charging is over time, stop charging and  update the battery voltage\n");
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, CHG_PROTECT_ON);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}
		msleep(500);

		rc = read_property_id(bcdev, pst, BATT_VOLT_NOW);
		if (rc < 0) {
			pr_err("Failed to read BATT_VOLT_NOW, rc=%d\n", rc);
			return;
		}
		bcdev->batt_voltage = pst->prop[BATT_VOLT_NOW];
		if(bcdev->batt_voltage > BOOT_VOLTAGE){
			bcdev->total_time = 0;
			bcdev->chging_over_time = false;
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, CHG_PROTECT_OFF);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
			pr_info("%s: Vbus more than 3.4V recalculate charge time! \n",__func__);
		}
	} else {
		bcdev->chging_over_time = false;
	}
}

#define VCHG_CNT	2
#define HIGH_VBUS_9V	11200000
#define LOW_VBUS_9V		8400000
#define HIGH_VBUS_5V	6400000
#define LOW_VBUS_5V		4000000
#define FORCE_9V		9000000
#define FORCE_5V		5000000
static bool chg_check_vchg_is_good(struct battery_chg_dev *bcdev)
{
	static bool ret = true;
	static int vchg_counts = 0;

	pr_info("%s: SW OVLO:%dV UVLO:%dV VBUS:%d\n",__func__,HIGH_VBUS_9V,LOW_VBUS_5V,bcdev->usb_voltage);
	if (bcdev->usb_voltage >= HIGH_VBUS_9V || bcdev->usb_voltage <= LOW_VBUS_5V) {
		vchg_counts++;
		if (vchg_counts >= VCHG_CNT) {
			vchg_counts = 0;
			ret = false;
		}
	} else {
		vchg_counts = 0;
		ret = true;
	}

	return ret;
}

#define OTG_MAX_VOLTAGE 	5100000
#define OTG_CNT				2
static bool battery_chg_otg_check(struct battery_chg_dev *bcdev)
{
	int rc = 0;
	static int counts = 0;

	if(bcdev->usb_voltage < OTG_MAX_VOLTAGE){
		counts++;
		if(counts >= OTG_CNT) {
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_OTG_ENABLE, false);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
			counts = 0;
			pr_err("%s:%d otg over current!!! ", __func__, __LINE__);
		}
	}else {
		counts = 0;
	}

	return rc;
}

static void battery_chg_protection_check(struct battery_chg_dev *bcdev)
{
	int rc = 0;
	if (false == battery_chg_check_tbatt_is_good(bcdev)) {
		bcdev->temp_err = true;
		//index = BATTERY_TEMPERATURE_ERROR;
		pr_info("%s:battery_chg_check_tbatt_is_good false!\n",__func__);
	} else {
		if (bcdev->temp_err) {
			bcdev->temp_err = false;
			pr_info("%s ,battery_chg_check_tbatt_is_good true! To Normal\n",__func__);
		}
	}

	if (false == battery_chg_check_vbatt_is_good(bcdev)) {
		bcdev->vbat_over = true;
		//index = BATTERY_VOLTAGE_ERROR;
		pr_info("%s: battery_chg_check_vbatt_is_good false!\n",__func__);
	} else {
		if (bcdev->vbat_over) {
			bcdev->vbat_over = false;
			pr_info("%s: battery_chg_check_vbatt_is_good true! To Normal\n",__func__);
		}
	}

	if (false == chg_check_vchg_is_good(bcdev)) {
		bcdev->vchg_err = true;
		//index = USB_IN_VOLTAGE_ERROR;
		pr_info("%s: chg_check_vchg_is_good false!\n",__func__);
	} else {
		if (bcdev->vchg_err) {
			bcdev->vchg_err = false;
			pr_info("%s: chg_check_vchg_is_good true! To Normal\n",__func__);
		}
	}

	battery_chg_check_time_is_good(bcdev);

	if(bcdev->temp_err || bcdev->vbat_over || bcdev->lpd_status || bcdev->vchg_err) {
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, CHG_PROTECT_ON);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}
		bcdev->chg_stop = true;
		//pr_info("%s:stop charge! reason:%s\n",__func__,reason[index]);
		pr_info("%s:stop charge! \n",__func__);
	}else{
		if (bcdev->chg_stop && !bcdev->chging_over_time){
			pr_info("%s:remove stop charge!\n",__func__);
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, CHG_PROTECT_OFF);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
			bcdev->chg_stop = false;
		}
	}
}

static void battery_chg_variables_reset(struct battery_chg_dev *bcdev, bool in)
{
	if(in){
		pr_info("Charger in charger_type = %d\n",bcdev->real_chg_type);
		bcdev->charger_type = bcdev->real_chg_type;
	}else{
		bcdev->charger_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		bcdev->batt_health = POWER_SUPPLY_HEALTH_GOOD;
	}

	bcdev->temp_err = false;
	bcdev->vbat_over = false;
	bcdev->vchg_err = false;
	bcdev->chging_over_time = false;
	bcdev->total_time = 0;
}

void battery_notify_charger_health_event(struct battery_chg_dev *bcdev)
{
	char *event_string = NULL;
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];

	event_string = kmalloc(CHG_SHOW_MAX_SIZE, GFP_KERNEL);
	if (event_string) {
		switch (bcdev->batt_health) {
		case HEALTH_NOTIFY_EVENT_TYPE_GOOD:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Good");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_OVERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Battery_over_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_COLD:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Cold");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_DEVICE_HOT:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Device_hot");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_USB_MOISTURE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Usb_moisture_det");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERTIME:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Chg_overtime");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_UNDERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Under_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_CHG_OVERVOLTAGE:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Over_voltage");
			break;
		case HEALTH_NOTIFY_EVENT_TYPE_REMOVE_CHG:
			scnprintf(event_string, CHG_SHOW_MAX_SIZE,
				"BATTERY_HEALTH=Usb_connect_err");
			break;
		default:
			pr_info("Invalid notify event type %d\n", bcdev->batt_health);
			goto out;
		}

		if (bcdev->batt_uenvp[0])
			bcdev->batt_uenvp[0] = NULL;
		bcdev->batt_uenvp[0] = event_string;
		bcdev->batt_uenvp[1] = NULL;

		printk("[charge monitor work entry event_string: %s]\n", event_string);
		kobject_uevent_env(&pst->psy->dev.kobj,
					KOBJ_CHANGE,
					bcdev->batt_uenvp);
	}
out:
	kfree(event_string);
	return;
}

void battery_chg_updata_health_status(struct battery_chg_dev *bcdev)
{
	if(bcdev->vbat_over){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_OVERVOLTAGE;
	}else if(bcdev->lpd_status){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_USB_MOISTURE;
	}else if(bcdev->chging_over_time){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_CHG_OVERTIME;
	}else if(bcdev->vchg_err && bcdev->usb_voltage >= HIGH_VBUS_9V){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_CHG_OVERVOLTAGE;
	}else if(bcdev->vchg_err && bcdev->usb_voltage <= LOW_VBUS_5V){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_CHG_UNDERVOLTAGE;
	}else if(bcdev->batt_temp >= HIGH_TEMP){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_DEVICE_HOT;
	}else if(bcdev->batt_temp < LOW_TEMP){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_COLD;
	}else if(bcdev->usb_temp >= FCNT_USB_TEMP){
		bcdev->batt_health = FCNT_BATTERY_HEALTH_REMOVE_CHG;
	}else{
		bcdev->batt_health = FCNT_BATTERY_HEALTH_GOOD;
	}

	if (bcdev->batt_health != FCNT_BATTERY_HEALTH_GOOD) {
		battery_notify_charger_health_event(bcdev);
	}
}

void charger_detect_check(struct battery_chg_dev *bcdev)
{
	if(bcdev->usb_present){
		if (bcdev->charger_type == POWER_SUPPLY_USB_TYPE_UNKNOWN){
			battery_chg_variables_reset(bcdev, true);
		}
	}else{
		battery_chg_variables_reset(bcdev, false);
	}
}

void charger_usb_cc_open_check(struct battery_chg_dev *bcdev)
{
       int rc;

       if (bcdev->usb_temp >= FCNT_USB_TEMP) {
               if(!bcdev->cc_open) {
                       rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_CC_OPEN, 1);
                       if (rc < 0) {
                               pr_err("%s:%d failed to write property", __func__, __LINE__);
                       } else {
                               bcdev->cc_open = true;
                               pr_info("%s:%d cc open", __func__, __LINE__);
                       }
               }
       } else if(bcdev->cc_open) {
               rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_CC_OPEN, 0);
               if (rc < 0) {
                       pr_err("%s:%d failed to write property", __func__, __LINE__);
               } else {
                       bcdev->cc_open = false;
                       pr_info("%s:%d cc open restore", __func__, __LINE__);
               }
       }
}

static void battery_chg_monitor_status_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work,
					struct battery_chg_dev,
					battery_monitor_work.work);

	update_battery_charge_data(bcdev);
	charger_detect_check(bcdev);

	if(bcdev->usb_present){
		battery_chg_protection_check(bcdev);
		battery_chg_updata_health_status(bcdev);
	}

	if(bcdev->otg_enable) {
		battery_chg_otg_check(bcdev);
	}

	battery_chg_print_log(bcdev);

	charger_usb_cc_open_check(bcdev);

	schedule_delayed_work(&bcdev->battery_monitor_work,msecs_to_jiffies(5000));
}

#define INTERVAL_TIME	4000
static void battery_chg_full_charge_setting_work(struct work_struct *work)
{
	int rc;
	int soc;

	struct battery_chg_dev *bcdev = container_of(work,
					struct battery_chg_dev, full_charge_setting_work.work);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];

	rc = read_property_id(bcdev, pst, BATT_CAPACITY); /* read the SoC */
	if (rc < 0) {
		pr_err("%s:%d failed to read BATT_CAPACITY property\n", __func__, __LINE__);
	}

	soc = DIV_ROUND_CLOSEST(pst->prop[BATT_CAPACITY], 100);
	pr_err("%s:%d full charge read chip_soc = %d ,ui_soc = %d\n", __func__, __LINE__, pst->prop[BATT_CAPACITY], soc);

	/* Fitst time */
	if (bcdev->high_soc && !!bcdev->full_charge_setting) { /* stop charging untill limit SoC */
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, FULL_CHG_ON);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}

		if (soc > STOP_CHARGE_85_LOW_SOC) {
			goto rerun;
		} else {	/* reach limit soc, allow charging */
			bcdev->high_soc = false;
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, FULL_CHG_OFF);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
		}
	}

	/* Rerun time */
	if (!!bcdev->full_charge_setting) { /* function keep running */
		/* fake */
		if (soc >= STOP_CHARGE_85_LOW_SOC && soc <= STOP_CHARGE_85_HIGH_SOC) {
			bcdev->fake_soc = STOP_CHARGE_85_LOW_SOC;
			power_supply_changed(pst->psy);
		} else if (soc >= 0 && soc <= STOP_CHARGE_85_LOW_SOC) {
			bcdev->fake_soc = -10;
		}

		if (soc >= STOP_CHARGE_85_HIGH_SOC && soc <= 100) {
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, FULL_CHG_ON);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
		}

		if (soc >= 0 && soc <= STOP_CHARGE_85_LOW_SOC) {
			rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, FULL_CHG_OFF);
			if (rc < 0) {
				pr_err("%s:%d failed to write property", __func__, __LINE__);
			}
		}
	} else { /* allow charging */
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, FULL_CHG_OFF);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}

		/* soc jump preventation */
		if (likely(bcdev->fake_soc < soc)) {
			bcdev->fake_soc += 1;
			power_supply_changed(pst->psy);
		} else {
			bcdev->fake_soc -= 1;
			power_supply_changed(pst->psy);
		}

		if (bcdev->fake_soc == soc) {
			bcdev->fake_soc = -10;
			goto out;
		}
	}

rerun:
	schedule_delayed_work(&bcdev->full_charge_setting_work, msecs_to_jiffies(INTERVAL_TIME));
out:
	pr_err("%s:%d exit full-charge-setting\n", __func__, __LINE__);
}

//#define FV_REDUCTION 4350
static ssize_t fv_reduction_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_BATTERY],
					BATT_FV_REDUCTION, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t fv_reduction_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_FV_REDUCTION);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
				pst->prop[BATT_FV_REDUCTION]);
}
static CLASS_ATTR_RW(fv_reduction);

static ssize_t cycle_count_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	bcdev->oem_cycle_count = val;

	return count;
}

static ssize_t cycle_count_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);

	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_CYCLE_COUNT);
	if (rc < 0)
		return rc;
	bcdev->cycle_count = pst->prop[BATT_CYCLE_COUNT];

	return scnprintf(buf, PAGE_SIZE, "%d\n",bcdev->cycle_count);
}
static CLASS_ATTR_RW(cycle_count);

static ssize_t cycle_clear_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	bcdev->cycle_clear = val;

	return count;
}

static ssize_t cycle_clear_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);

	return scnprintf(buf, PAGE_SIZE, "%d\n",bcdev->cycle_clear);
}
static CLASS_ATTR_RW(cycle_clear);

static ssize_t age_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	int rc;

	rc = read_property_id(bcdev, pst, BATT_CHG_FULL_DESIGN);
	if (rc < 0)
		return rc;
	bcdev->charge_full_design = pst->prop[BATT_CHG_FULL_DESIGN];

	rc = read_property_id(bcdev, pst, BATT_CHG_FULL);
	if (rc < 0)
		return rc;
	bcdev->charge_full = pst->prop[BATT_CHG_FULL];

	bcdev->age = (bcdev->charge_full * 100)/BATTERY_DESIGNCAP;

	if(bcdev->age > 100) {
		bcdev->age = 100;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n",bcdev->age);
}
static CLASS_ATTR_RO(age);

static ssize_t full_charge_setting_store(struct class *c, struct class_attribute *attr,
                    const char *buf, size_t count)
{
	int val;
	int soc;
	int rc;

	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev, oem_battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_BATTERY];
	sscanf(buf, "%d", &val);
	if (TURN_OFF_LIMIT_CHG != val && TURN_ON_85_LIMIT_CHG != val) { /* invalid input */
		pr_err("%s:%d invalid input\n", __func__, __LINE__);
		return sizeof(int);
	}

	bcdev->full_charge_setting = val; /* get new status */

	if (true == !!bcdev->full_charge_setting) { /* turn on SoC detection */
		rc = read_property_id(bcdev, pst, BATT_CAPACITY); /* read the SoC */
		if (rc < 0) {
			pr_err("%s:%d failed to read BATT_CAPACITY property\n", __func__, __LINE__);
		}

		soc = DIV_ROUND_CLOSEST(pst->prop[BATT_CAPACITY], 100);
		if (soc >=0 && soc <= STOP_CHARGE_85_LOW_SOC) {
			bcdev->high_soc = 0;
		} else if (soc > STOP_CHARGE_85_LOW_SOC && soc <= 100) {
			bcdev->high_soc = 1;
		}

		schedule_delayed_work(&bcdev->full_charge_setting_work, 0);
	}

	return count;
}

static ssize_t full_charge_setting_show(struct class *c, struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						oem_battery_class);

	pr_info("%s:%d read successful, current_status:%s\n", __func__,
				__LINE__, bcdev->full_charge_setting ? "ON" : "OFF");
	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->full_charge_setting);
}
static CLASS_ATTR_RW(full_charge_setting);

static ssize_t schedule_chg_store(struct class *c, struct class_attribute *attr,
                    const char *buf, size_t count)
{
	bool val;
	int rc;
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev, oem_battery_class);
	sscanf(buf, "%d", &val);
	if (1 != val && 0 != val) { /* invalid input */
		pr_err("%s:%d invalid input\n", __func__, __LINE__);
		return sizeof(int);
	}

	if (val == bcdev->schedule_chg) { /* repeating input */
		pr_err("%s:%d charging is already allowed\n", __func__, __LINE__);
	}

	bcdev->schedule_chg = val; /* get new status */

	if (0 == bcdev->schedule_chg) { /* allow charging */
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, SCHEDULE_CHG_OFF);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}
	} else { /* stop charging */
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_USB], USB_FULL_CHARGE_SET, SCHEDULE_CHG_ON);
		if (rc < 0) {
			pr_err("%s:%d failed to write property", __func__, __LINE__);
		}
	}

	pr_info("%s:%d store successful, current_status:%s\n", __func__,
			__LINE__, bcdev->schedule_chg ? "Stop charging" : "Allow charging");
	return count;
}

static ssize_t schedule_chg_show(struct class *c, struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev, oem_battery_class);

	pr_info("%s:%d read successful, current_status:%s\n", __func__,
			__LINE__, bcdev->schedule_chg ? "Stop charging" : "Allow charging");
	return scnprintf(buf, PAGE_SIZE, "%d\n", bcdev->schedule_chg);
}
static CLASS_ATTR_RW(schedule_chg);

static struct attribute *oem_battery_class_attrs[] = {
	&class_attr_fv_reduction.attr,
	&class_attr_cycle_count.attr,
	&class_attr_cycle_clear.attr,
	&class_attr_age.attr,
	&class_attr_full_charge_setting.attr,
	&class_attr_schedule_chg.attr,
	NULL,
};
ATTRIBUTE_GROUPS(oem_battery_class);

static void battery_chg_variables_init(struct battery_chg_dev *bcdev)
{
	bcdev->charger_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	bcdev->chg_stop = false;
	bcdev->temp_err = false;
	bcdev->vbat_over = false;
	bcdev->vchg_err = false;
	bcdev->chging_over_time = false;
	bcdev->total_time = 0;
}

static int battery_chg_probe(struct platform_device *pdev)
{
	struct battery_chg_dev *bcdev;
	struct device *dev = &pdev->dev;
	struct pmic_glink_client_data client_data = { };
	int rc, i;

	bcdev = devm_kzalloc(&pdev->dev, sizeof(*bcdev), GFP_KERNEL);
	if (!bcdev)
		return -ENOMEM;

	bcdev->psy_list[PSY_TYPE_BATTERY].map = battery_prop_map;
	bcdev->psy_list[PSY_TYPE_BATTERY].prop_count = BATT_PROP_MAX;
	bcdev->psy_list[PSY_TYPE_BATTERY].opcode_get = BC_BATTERY_STATUS_GET;
	bcdev->psy_list[PSY_TYPE_BATTERY].opcode_set = BC_BATTERY_STATUS_SET;
	bcdev->psy_list[PSY_TYPE_USB].map = usb_prop_map;
	bcdev->psy_list[PSY_TYPE_USB].prop_count = USB_PROP_MAX;
	bcdev->psy_list[PSY_TYPE_USB].opcode_get = BC_USB_STATUS_GET;
	bcdev->psy_list[PSY_TYPE_USB].opcode_set = BC_USB_STATUS_SET;
	bcdev->psy_list[PSY_TYPE_WLS].map = wls_prop_map;
	bcdev->psy_list[PSY_TYPE_WLS].prop_count = WLS_PROP_MAX;
	bcdev->psy_list[PSY_TYPE_WLS].opcode_get = BC_WLS_STATUS_GET;
	bcdev->psy_list[PSY_TYPE_WLS].opcode_set = BC_WLS_STATUS_SET;

	for (i = 0; i < PSY_TYPE_MAX; i++) {
		bcdev->psy_list[i].prop =
			devm_kcalloc(&pdev->dev, bcdev->psy_list[i].prop_count,
					sizeof(u32), GFP_KERNEL);
		if (!bcdev->psy_list[i].prop)
			return -ENOMEM;
	}

	bcdev->psy_list[PSY_TYPE_BATTERY].model =
		devm_kzalloc(&pdev->dev, MAX_STR_LEN, GFP_KERNEL);
	if (!bcdev->psy_list[PSY_TYPE_BATTERY].model)
		return -ENOMEM;

	mutex_init(&bcdev->rw_lock);
	init_rwsem(&bcdev->state_sem);
	init_completion(&bcdev->ack);
	init_completion(&bcdev->fw_buf_ack);
	init_completion(&bcdev->fw_update_ack);
	INIT_WORK(&bcdev->subsys_up_work, battery_chg_subsys_up_work);
	INIT_WORK(&bcdev->usb_type_work, battery_chg_update_usb_type_work);
	INIT_WORK(&bcdev->battery_check_work, battery_chg_check_status_work);
	INIT_DELAYED_WORK(&bcdev->battery_monitor_work, battery_chg_monitor_status_work);
	INIT_DELAYED_WORK(&bcdev->full_charge_setting_work, battery_chg_full_charge_setting_work);
	bcdev->dev = dev;

	rc = battery_chg_register_panel_notifier(bcdev);
	if (rc < 0)
		return rc;

	client_data.id = MSG_OWNER_BC;
	client_data.name = "battery_charger";
	client_data.msg_cb = battery_chg_callback;
	client_data.priv = bcdev;
	client_data.state_cb = battery_chg_state_cb;

	bcdev->client = pmic_glink_register_client(dev, &client_data);
	if (IS_ERR(bcdev->client)) {
		rc = PTR_ERR(bcdev->client);
		if (rc != -EPROBE_DEFER)
			dev_err(dev, "Error in registering with pmic_glink %d\n",
				rc);
		goto reg_error;
	}

	down_write(&bcdev->state_sem);
	atomic_set(&bcdev->state, PMIC_GLINK_STATE_UP);
	/*
	 * This should be initialized here so that battery_chg_callback
	 * can run successfully when battery_chg_parse_dt() starts
	 * reading BATT_CHG_CTRL_LIM_MAX parameter and waits for a response.
	 */
	bcdev->initialized = true;
	up_write(&bcdev->state_sem);

	bcdev->reboot_notifier.notifier_call = battery_chg_ship_mode;
	bcdev->reboot_notifier.priority = 255;
	register_reboot_notifier(&bcdev->reboot_notifier);

	rc = battery_chg_parse_dt(bcdev);
	if (rc < 0) {
		dev_err(dev, "Failed to parse dt rc=%d\n", rc);
		goto error;
	}

	bcdev->restrict_fcc_ua = DEFAULT_RESTRICT_FCC_UA;
	platform_set_drvdata(pdev, bcdev);
	bcdev->fake_soc = -EINVAL;
	rc = battery_chg_init_psy(bcdev);
	if (rc < 0)
		goto error;

	bcdev->battery_class.name = "qcom-battery";

	if (bcdev->wls_not_supported)
		bcdev->battery_class.class_groups = battery_class_no_wls_groups;
	else
		bcdev->battery_class.class_groups = battery_class_groups;

	rc = class_register(&bcdev->battery_class);
	if (rc < 0) {
		dev_err(dev, "Failed to create battery_class rc=%d\n", rc);
		goto error;
	}

	bcdev->wls_fw_update_time_ms = WLS_FW_UPDATE_TIME_MS;
	battery_chg_add_debugfs(bcdev);
	bcdev->notify_en = false;
	battery_chg_notify_enable(bcdev);
	device_init_wakeup(bcdev->dev, true);
	rc = register_extcon_conn_type(bcdev);
	if (rc < 0)
		dev_warn(dev, "Failed to register extcon rc=%d\n", rc);

	if (bcdev->connector_type == USB_CONNECTOR_TYPE_MICRO_USB) {
		bcdev->typec_class = qti_typec_class_init(bcdev->dev);
		if (IS_ERR_OR_NULL(bcdev->typec_class)) {
			dev_err(dev, "Failed to init typec class err=%d\n",
				PTR_ERR(bcdev->typec_class));
			return PTR_ERR(bcdev->typec_class);
		}
	}

	schedule_work(&bcdev->usb_type_work);
	if (bcdev->fcnt_charging_feature_supported) {
		bcdev->oem_battery_class.name = "oem-battery";
		bcdev->oem_battery_class.class_groups = oem_battery_class_groups;
		class_register(&bcdev->oem_battery_class);
		bcdev->dbg_health = -EINVAL;
		battery_chg_variables_init(bcdev);
		schedule_delayed_work(&bcdev->battery_monitor_work,msecs_to_jiffies(5000));
	}

	return 0;
error:
	down_write(&bcdev->state_sem);
	atomic_set(&bcdev->state, PMIC_GLINK_STATE_DOWN);
	bcdev->initialized = false;
	up_write(&bcdev->state_sem);

	pmic_glink_unregister_client(bcdev->client);
	cancel_work_sync(&bcdev->usb_type_work);
	cancel_work_sync(&bcdev->subsys_up_work);
	cancel_work_sync(&bcdev->battery_check_work);
	cancel_delayed_work_sync(&bcdev->battery_monitor_work);
	cancel_delayed_work_sync(&bcdev->full_charge_setting_work);
	complete(&bcdev->ack);
	unregister_reboot_notifier(&bcdev->reboot_notifier);
reg_error:
	if (bcdev->notifier_cookie)
		panel_event_notifier_unregister(bcdev->notifier_cookie);
	return rc;
}

static int battery_chg_remove(struct platform_device *pdev)
{
	struct battery_chg_dev *bcdev = platform_get_drvdata(pdev);

	down_write(&bcdev->state_sem);
	atomic_set(&bcdev->state, PMIC_GLINK_STATE_DOWN);
	bcdev->initialized = false;
	up_write(&bcdev->state_sem);

	qti_typec_class_deinit(bcdev->typec_class);
	if (bcdev->notifier_cookie)
		panel_event_notifier_unregister(bcdev->notifier_cookie);

	device_init_wakeup(bcdev->dev, false);
	debugfs_remove_recursive(bcdev->debugfs_dir);
	class_unregister(&bcdev->battery_class);
	if (bcdev->fcnt_charging_feature_supported) {
		class_unregister(&bcdev->oem_battery_class);
	}
	pmic_glink_unregister_client(bcdev->client);
	cancel_work_sync(&bcdev->subsys_up_work);
	cancel_work_sync(&bcdev->usb_type_work);
	cancel_work_sync(&bcdev->battery_check_work);
	unregister_reboot_notifier(&bcdev->reboot_notifier);

	return 0;
}

static const struct of_device_id battery_chg_match_table[] = {
	{ .compatible = "qcom,battery-charger" },
	{},
};

static struct platform_driver battery_chg_driver = {
	.driver = {
		.name = "qti_battery_charger",
		.of_match_table = battery_chg_match_table,
	},
	.probe = battery_chg_probe,
	.remove = battery_chg_remove,
};
module_platform_driver(battery_chg_driver);

MODULE_DESCRIPTION("QTI Glink battery charger driver");
MODULE_LICENSE("GPL v2");
