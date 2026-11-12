// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Linaro Ltd
 */
#include <linux/unaligned.h>
#include <linux/auxiliary_bus.h>
#include <linux/ctype.h>
#include <linux/hex.h>
#include <linux/swab.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/math.h>
#include <linux/units.h>

#define BATTMGR_CHEMISTRY_LEN	4
#define BATTMGR_STRING_LEN	128

enum qcom_battmgr_variant {
	QCOM_BATTMGR_SM8350,
	QCOM_BATTMGR_SM8475,
	QCOM_BATTMGR_SC8280XP,
};

#define BATTMGR_BAT_STATUS		0x1

#define BATTMGR_REQUEST_NOTIFICATION	0x4

#define BATTMGR_NOTIFICATION		0x7
#define NOTIF_BAT_PROPERTY		0x30
#define NOTIF_USB_PROPERTY		0x32
#define NOTIF_WLS_PROPERTY		0x34
#define NOTIF_XM_PROPERTY		0x50
#define NOTIF_BAT_INFO			0x81
#define NOTIF_BAT_STATUS		0x80

#define BATTMGR_BAT_INFO		0x9

#define BATTMGR_BAT_DISCHARGE_TIME	0xc

#define BATTMGR_BAT_CHARGE_TIME		0xd

#define BATTMGR_BAT_PROPERTY_GET	0x30
#define BATTMGR_BAT_PROPERTY_SET	0x31
#define BATT_STATUS			0
#define BATT_HEALTH			1
#define BATT_PRESENT			2
#define BATT_CHG_TYPE			3
#define BATT_CAPACITY			4
#define BATT_SOH			5
#define BATT_VOLT_OCV			6
#define BATT_VOLT_NOW			7
#define BATT_VOLT_MAX			8
#define BATT_CURR_NOW			9
#define BATT_CHG_CTRL_LIM		10
#define BATT_CHG_CTRL_LIM_MAX		11
#define BATT_TEMP			12
#define BATT_TECHNOLOGY			13
#define BATT_CHG_COUNTER		14
#define BATT_CYCLE_COUNT		15
#define BATT_CHG_FULL_DESIGN		16
#define BATT_CHG_FULL			17
#define BATT_MODEL_NAME			18
#define BATT_TTF_AVG			19
#define BATT_TTE_AVG			20
#define BATT_RESISTANCE			21
#define BATT_POWER_NOW			22
#define BATT_POWER_AVG			23

#define BATTMGR_USB_PROPERTY_GET	0x32
#define BATTMGR_USB_PROPERTY_SET	0x33
#define USB_ONLINE			0
#define USB_VOLT_NOW			1
#define USB_VOLT_MAX			2
#define USB_CURR_NOW			3
#define USB_CURR_MAX			4
#define USB_INPUT_CURR_LIMIT		5
#define USB_TYPE			6
#define USB_ADAP_TYPE			7
#define USB_MOISTURE_DET_EN		8
#define USB_MOISTURE_DET_STS		9
#define USB_TEMP			10
#define USB_REAL_TYPE			11

#define BATTMGR_XM_PROPERTY_GET		0x50
#define BATTMGR_XM_PROPERTY_SET		0x51
#define XM_AUTHENTIC			3
#define XM_REAL_TYPE			6
#define XM_VERIFY_PROCESS		7
#define XM_VDM_SESSION_SEED		11
#define XM_VDM_AUTHENTICATION		12
#define XM_VDM_VERIFIED		13
#define XM_CURRENT_STATE		16
#define XM_ADAPTER_ID			17
#define XM_ADAPTER_SVID			18
#define XM_PD_VERIFIED			19
#define XM_PDO2			20
#define XM_FASTCHG_MODE			38
#define XM_QUICK_CHARGE_TYPE		42
#define XM_APDO_MAX			43
#define XM_POWER_MAX			44
/* Additional XM properties used by the adapter/battery authentication path. */
#define XM_VERIFY_DIGEST		1
#define XM_VDM_CHARGER_VERSION		8
#define XM_VDM_CHARGER_VOLTAGE		9
#define XM_VDM_CHARGER_TEMP		10
#define XM_VDM_REMOVE_COMPENSATION	14
#define XM_VDM_REVERSE_AUTHEN		15
#define XM_UVDM_STATE			21
#define XM_BQ2597X_BUS_CURRENT		24
#define XM_BQ2597X_SLAVE_BUS_CURRENT	25
#define XM_BQ2597X_BUS_VOLTAGE		27
#define XM_SLAVE_AUTHENTIC		139
#define XM_DIGEST_LEN			32

/* Vendor UVDM command numbers written to request_vdm_cmd. */
#define XM_UVDM_CHARGER_VERSION		1
#define XM_UVDM_CHARGER_VOLTAGE		2
#define XM_UVDM_CHARGER_TEMP		3
#define XM_UVDM_SESSION_SEED		4
#define XM_UVDM_AUTHENTICATION		5
#define XM_UVDM_VERIFIED		6
#define XM_UVDM_REMOVE_COMPENSATION	7
#define XM_UVDM_REVERSE_AUTHEN		8

#define BATTMGR_WLS_PROPERTY_GET	0x34
#define BATTMGR_WLS_PROPERTY_SET	0x35
#define WLS_ONLINE			0
#define WLS_VOLT_NOW			1
#define WLS_VOLT_MAX			2
#define WLS_CURR_NOW			3
#define WLS_CURR_MAX			4
#define WLS_TYPE			5
#define WLS_BOOST_EN			6

struct qcom_battmgr_enable_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 power_state;
	__le32 low_capacity;
	__le32 high_capacity;
};

struct qcom_battmgr_property_request {
	struct pmic_glink_hdr hdr;
	__le32 battery;
	__le32 property;
	__le32 value;
};

/*
 * Xiaomi's charger-PD service uses this 32 byte XM frame for the session
 * seed and authentication exchanges.  Unlike the normal 24 byte property
 * response, this frame has no ret_code field; the wire format is confirmed
 * by liuqin's exact qti_battery_charger.c implementation.
 */
struct qcom_battmgr_xm_words_message {
	struct pmic_glink_hdr hdr;
	__le32 property;
	__le32 words[4];
};
static_assert(sizeof(struct qcom_battmgr_xm_words_message) == 32);

/*
 * The fuel-gauge authentication exchange carries a 32 byte digest plus a
 * slave-cell flag in a 52 byte frame (qti_battery_charger.c
 * xm_verify_digest_resp_msg).  Like the words frame it has no ret_code.
 */
struct qcom_battmgr_xm_digest_message {
	struct pmic_glink_hdr hdr;
	__le32 property;
	u8 digest[XM_DIGEST_LEN];
	u8 slave_fg;
	u8 reserved[3];
};
static_assert(sizeof(struct qcom_battmgr_xm_digest_message) == 52);

enum qcom_battmgr_xm_response_kind {
	QCOM_BATTMGR_XM_VALUE_RESPONSE,
	QCOM_BATTMGR_XM_WORDS_RESPONSE,
	QCOM_BATTMGR_XM_DIGEST_RESPONSE,
};

struct qcom_battmgr_xm_pending {
	bool active;
	u32 opcode;
	u32 property;
	enum qcom_battmgr_xm_response_kind response_kind;
	u32 value;
	u32 words[4];
	u8 digest[XM_DIGEST_LEN];
};

struct qcom_battmgr_update_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
};

struct qcom_battmgr_charge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 percent;
	__le32 reserved;
};

struct qcom_battmgr_discharge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 rate; /* 0 for current rate */
	__le32 reserved;
};

struct qcom_battmgr_message {
	struct pmic_glink_hdr hdr;
	union {
		struct {
			__le32 property;
			__le32 value;
			__le32 result;
		} intval;
		struct {
			__le32 property;
			char model[BATTMGR_STRING_LEN];
		} strval;
		struct {
			/*
			 * 0: mWh
			 * 1: mAh
			 */
			__le32 power_unit;
			__le32 design_capacity;
			__le32 last_full_capacity;
			/*
			 * 0 nonrechargable
			 * 1 rechargable
			 */
			__le32 battery_tech;
			__le32 design_voltage; /* mV */
			__le32 capacity_low;
			__le32 capacity_warning;
			__le32 cycle_count;
			/* thousandth of percent */
			__le32 accuracy;
			__le32 max_sample_time_ms;
			__le32 min_sample_time_ms;
			__le32 max_average_interval_ms;
			__le32 min_average_interval_ms;
			/* granularity between low and warning */
			__le32 capacity_granularity1;
			/* granularity between warning and full */
			__le32 capacity_granularity2;
			/*
			 * 0: no
			 * 1: cold
			 * 2: hot
			 */
			__le32 swappable;
			__le32 capabilities;
			char model_number[BATTMGR_STRING_LEN];
			char serial_number[BATTMGR_STRING_LEN];
			char battery_type[BATTMGR_STRING_LEN];
			char oem_info[BATTMGR_STRING_LEN];
			char battery_chemistry[BATTMGR_CHEMISTRY_LEN];
			char uid[BATTMGR_STRING_LEN];
			__le32 critical_bias;
			u8 day;
			u8 month;
			__le16 year;
			__le32 battery_id;
		} info;
		struct {
			/*
			 * BIT(0) discharging
			 * BIT(1) charging
			 * BIT(2) critical low
			 */
			__le32 battery_state;
			/* mWh or mAh, based on info->power_unit */
			__le32 capacity;
			__le32 rate;
			/* mv */
			__le32 battery_voltage;
			/*
			 * BIT(0) power online
			 * BIT(1) discharging
			 * BIT(2) charging
			 * BIT(3) battery critical
			 */
			__le32 power_state;
			/*
			 * 1: AC
			 * 2: USB
			 * 3: Wireless
			 */
			__le32 charging_source;
			__le32 temperature;
		} status;
		__le32 time;
		__le32 notification;
	};
};

#define BATTMGR_CHARGING_SOURCE_AC	1
#define BATTMGR_CHARGING_SOURCE_USB	2
#define BATTMGR_CHARGING_SOURCE_WIRELESS 3

enum qcom_battmgr_unit {
	QCOM_BATTMGR_UNIT_mWh = 0,
	QCOM_BATTMGR_UNIT_mAh = 1
};

struct qcom_battmgr_info {
	bool valid;

	bool present;
	unsigned int charge_type;
	unsigned int design_capacity;
	unsigned int last_full_capacity;
	unsigned int voltage_max_design;
	unsigned int voltage_max;
	unsigned int capacity_low;
	unsigned int capacity_warning;
	unsigned int cycle_count;
	unsigned int charge_count;
	char model_number[BATTMGR_STRING_LEN];
	char serial_number[BATTMGR_STRING_LEN];
	char oem_info[BATTMGR_STRING_LEN];
	unsigned char technology;
	unsigned char day;
	unsigned char month;
	unsigned short year;
};

struct qcom_battmgr_status {
	unsigned int status;
	unsigned int health;
	unsigned int capacity;
	unsigned int percent;
	int current_now;
	int power_now;
	unsigned int voltage_now;
	unsigned int voltage_ocv;
	unsigned int temperature;

	unsigned int discharge_time;
	unsigned int charge_time;
};

struct qcom_battmgr_ac {
	bool online;
};

struct qcom_battmgr_usb {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
	unsigned int current_limit;
	unsigned int usb_type;
};

struct qcom_battmgr_wireless {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
};

struct qcom_battmgr {
	struct device *dev;
	struct pmic_glink_client *client;

	enum qcom_battmgr_variant variant;

	struct power_supply *ac_psy;
	struct power_supply *bat_psy;
	struct power_supply *usb_psy;
	struct power_supply *wls_psy;

	enum qcom_battmgr_unit unit;

	int error;
	struct completion ack;

	bool service_up;

	struct qcom_battmgr_info info;
	struct qcom_battmgr_status status;
	struct qcom_battmgr_ac ac;
	struct qcom_battmgr_usb usb;
	struct qcom_battmgr_wireless wireless;

	/* Last vendor/raw property response, serialized by @lock. */
	unsigned int raw_opcode;
	unsigned int raw_property;
	unsigned int raw_value;
	bool raw_valid;

	/*
	 * XM is a vendor service that also sends unsolicited notifications.  Keep
	 * a complete request identity, rather than accepting the next response on
	 * the shared completion.  @lock serializes its lifetime with the request.
	 */
	struct qcom_battmgr_xm_pending xm;
	/* verify_slave_flag: selects the slave fuel gauge for verify_digest. */
	bool xm_slave_fg;
	/* Last acknowledged liuqin cooling level. Invalid after a service restart. */
	int liuqin_thermal_level;
	atomic_t service_generation;
	int liuqin_level_generation;

	struct work_struct enable_work;

	/*
	 * @lock is used to prevent concurrent power supply requests to the
	 * firmware, as it then stops responding.
	 */
	struct mutex lock;
};

static int qcom_battmgr_request_property(struct qcom_battmgr *battmgr, int opcode,
					 int property, u32 value);
static int qcom_battmgr_request(struct qcom_battmgr *battmgr, void *data,
				 size_t len);

static int qcom_battmgr_xm_transact_locked(struct qcom_battmgr *battmgr,
					   u32 opcode, u32 property,
					   enum qcom_battmgr_xm_response_kind kind,
					   void *request, size_t request_len)
{
	int ret;

	if (opcode != BATTMGR_XM_PROPERTY_GET &&
	    opcode != BATTMGR_XM_PROPERTY_SET &&
	    !(opcode == BATTMGR_BAT_PROPERTY_SET && property == BATT_CHG_CTRL_LIM &&
	      kind == QCOM_BATTMGR_XM_VALUE_RESPONSE))
		return -EINVAL;

	/* The callback may run before pmic_glink_send() returns. */
	battmgr->xm.opcode = opcode;
	battmgr->xm.property = property;
	battmgr->xm.response_kind = kind;
	/* Publish the complete identity before the callback can observe active. */
	smp_store_release(&battmgr->xm.active, true); /* pairs with callback acquire */

	ret = qcom_battmgr_request(battmgr, request, request_len);

	/* Retire this request; XM has no transaction ID for immediate same-key retry. */
	WRITE_ONCE(battmgr->xm.active, false);
	return ret;
}

static int qcom_battmgr_xm_request_locked(struct qcom_battmgr *battmgr,
					  u32 opcode, u32 property,
					  u32 request_value,
					  const u32 *words,
					  enum qcom_battmgr_xm_response_kind response_kind,
					  u32 *value, u32 response_words[4])
{
	struct qcom_battmgr_property_request property_request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.battery = cpu_to_le32(0),
		.property = cpu_to_le32(property),
		.value = cpu_to_le32(request_value),
	};
	struct qcom_battmgr_xm_words_message words_request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.property = cpu_to_le32(property),
	};
	void *request = &property_request;
	size_t request_len = sizeof(property_request);
	int ret;

	if (words) {
		for (unsigned int i = 0; i < ARRAY_SIZE(words_request.words); i++)
			words_request.words[i] = cpu_to_le32(words[i]);
		request = &words_request;
		request_len = sizeof(words_request);
	}

	ret = qcom_battmgr_xm_transact_locked(battmgr, opcode, property,
					      response_kind, request, request_len);
	if (ret)
		return ret;

	if (response_kind == QCOM_BATTMGR_XM_VALUE_RESPONSE) {
		if (value)
			*value = battmgr->xm.value;
	} else if (response_words) {
		memcpy(response_words, battmgr->xm.words,
		       sizeof(battmgr->xm.words));
	}

	return 0;
}

static int qcom_battmgr_xm_get_value_locked(struct qcom_battmgr *battmgr,
					    u32 property, u32 *value)
{
	return qcom_battmgr_xm_request_locked(battmgr, BATTMGR_XM_PROPERTY_GET,
					      property, 0, NULL,
					      QCOM_BATTMGR_XM_VALUE_RESPONSE,
					      value, NULL);
}

static int qcom_battmgr_xm_get_words_locked(struct qcom_battmgr *battmgr,
					    u32 property, u32 words[4])
{
	const u32 empty_words[4] = {};

	/* The vendor uses a zeroed 32 byte request to read authentication words. */
	return qcom_battmgr_xm_request_locked(battmgr, BATTMGR_XM_PROPERTY_GET,
					      property, 0, empty_words,
					      QCOM_BATTMGR_XM_WORDS_RESPONSE,
					      NULL, words);
}

/*
 * SET is restricted to the properties the vendor authentication daemon
 * (batterysecret) writes: battery/adapter authentication verdicts, the
 * verification-in-progress flag and the UVDM command channel.  There is no
 * arbitrary XM-property write path and no current, voltage or PMIC write.
 */
static bool qcom_battmgr_xm_set_allowed(u32 property)
{
	switch (property) {
	case XM_AUTHENTIC:
	case XM_SLAVE_AUTHENTIC:
	case XM_VERIFY_PROCESS:
	case XM_PD_VERIFIED:
	case XM_VDM_CHARGER_VERSION:
	case XM_VDM_CHARGER_VOLTAGE:
	case XM_VDM_CHARGER_TEMP:
	case XM_VDM_VERIFIED:
	case XM_VDM_REMOVE_COMPENSATION:
		return true;
	default:
		return false;
	}
}

static int qcom_battmgr_xm_set_value_locked(struct qcom_battmgr *battmgr,
					    u32 property, u32 value)
{
	if (!qcom_battmgr_xm_set_allowed(property))
		return -EPERM;

	return qcom_battmgr_xm_request_locked(battmgr, BATTMGR_XM_PROPERTY_SET,
					      property, value, NULL,
					      QCOM_BATTMGR_XM_VALUE_RESPONSE,
					      NULL, NULL);
}

static int qcom_battmgr_xm_set_words_locked(struct qcom_battmgr *battmgr,
					    u32 property, const u32 words[4])
{
	if (property != XM_VDM_SESSION_SEED &&
	    property != XM_VDM_AUTHENTICATION &&
	    property != XM_VDM_REVERSE_AUTHEN)
		return -EPERM;

	return qcom_battmgr_xm_request_locked(battmgr, BATTMGR_XM_PROPERTY_SET,
					      property, 0, words,
					      QCOM_BATTMGR_XM_WORDS_RESPONSE,
					      NULL, NULL);
}

static int qcom_battmgr_xm_digest_locked(struct qcom_battmgr *battmgr,
					 u32 opcode, bool slave_fg,
					 const u8 *digest_in, u8 *digest_out)
{
	struct qcom_battmgr_xm_digest_message request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.property = cpu_to_le32(XM_VERIFY_DIGEST),
		.slave_fg = slave_fg,
	};
	int ret;

	if (digest_in)
		memcpy(request.digest, digest_in, sizeof(request.digest));

	ret = qcom_battmgr_xm_transact_locked(battmgr, opcode, XM_VERIFY_DIGEST,
					      QCOM_BATTMGR_XM_DIGEST_RESPONSE,
					      &request, sizeof(request));
	if (ret)
		return ret;

	if (digest_out)
		memcpy(digest_out, battmgr->xm.digest, XM_DIGEST_LEN);

	return 0;
}

static int qcom_battmgr_xm_get(struct qcom_battmgr *battmgr, u32 property,
			       u32 *value)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_get_value_locked(battmgr, property, value);
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_xm_set(struct qcom_battmgr *battmgr, u32 property,
			       u32 value)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_set_value_locked(battmgr, property, value);
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_xm_get_words(struct qcom_battmgr *battmgr,
				     u32 property, u32 words[4])
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_get_words_locked(battmgr, property, words);
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_xm_set_words(struct qcom_battmgr *battmgr,
				     u32 property, const u32 words[4])
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_set_words_locked(battmgr, property, words);
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_xm_digest(struct qcom_battmgr *battmgr, u32 opcode,
				  bool slave_fg, const u8 *digest_in,
				  u8 *digest_out)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_digest_locked(battmgr, opcode, slave_fg,
					    digest_in, digest_out);
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_raw_property(struct qcom_battmgr *battmgr,
				     unsigned int opcode,
				     unsigned int property,
				     unsigned int *value)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	battmgr->raw_valid = false;
	if (opcode == BATTMGR_XM_PROPERTY_GET)
		ret = qcom_battmgr_xm_get_value_locked(battmgr, property, value);
	else
		ret = qcom_battmgr_request_property(battmgr, opcode, property, 0);
	if (!ret && opcode != BATTMGR_XM_PROPERTY_GET &&
	    (!battmgr->raw_valid || battmgr->raw_opcode != opcode ||
	     battmgr->raw_property != property))
		ret = -ENODATA;
	if (!ret && opcode != BATTMGR_XM_PROPERTY_GET)
		*value = battmgr->raw_value;
	mutex_unlock(&battmgr->lock);

	return ret;
}

#define QCOM_BATTMGR_RAW_ATTR(_name, _opcode, _property) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev); \
	unsigned int value; \
	int ret; \
	ret = qcom_battmgr_raw_property(battmgr, _opcode, _property, &value); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, "%u\n", value); \
} \
static DEVICE_ATTR_RO(_name)

QCOM_BATTMGR_RAW_ATTR(raw_usb_real_type, BATTMGR_USB_PROPERTY_GET, USB_REAL_TYPE);
QCOM_BATTMGR_RAW_ATTR(raw_xm_authentic, BATTMGR_XM_PROPERTY_GET, XM_AUTHENTIC);
QCOM_BATTMGR_RAW_ATTR(raw_xm_real_type, BATTMGR_XM_PROPERTY_GET, XM_REAL_TYPE);
QCOM_BATTMGR_RAW_ATTR(raw_xm_verify_process, BATTMGR_XM_PROPERTY_GET, XM_VERIFY_PROCESS);
QCOM_BATTMGR_RAW_ATTR(raw_xm_current_state, BATTMGR_XM_PROPERTY_GET, XM_CURRENT_STATE);
QCOM_BATTMGR_RAW_ATTR(raw_xm_adapter_id, BATTMGR_XM_PROPERTY_GET, XM_ADAPTER_ID);
QCOM_BATTMGR_RAW_ATTR(raw_xm_adapter_svid, BATTMGR_XM_PROPERTY_GET, XM_ADAPTER_SVID);
QCOM_BATTMGR_RAW_ATTR(raw_xm_pd_verified, BATTMGR_XM_PROPERTY_GET, XM_PD_VERIFIED);
QCOM_BATTMGR_RAW_ATTR(raw_xm_pdo2, BATTMGR_XM_PROPERTY_GET, XM_PDO2);
QCOM_BATTMGR_RAW_ATTR(raw_xm_fastchg_mode, BATTMGR_XM_PROPERTY_GET, XM_FASTCHG_MODE);
QCOM_BATTMGR_RAW_ATTR(raw_xm_quick_charge_type, BATTMGR_XM_PROPERTY_GET, XM_QUICK_CHARGE_TYPE);
QCOM_BATTMGR_RAW_ATTR(raw_xm_apdo_max, BATTMGR_XM_PROPERTY_GET, XM_APDO_MAX);
QCOM_BATTMGR_RAW_ATTR(raw_xm_power_max, BATTMGR_XM_PROPERTY_GET, XM_POWER_MAX);

static struct attribute *qcom_battmgr_raw_attrs[] = {
	&dev_attr_raw_usb_real_type.attr,
	&dev_attr_raw_xm_authentic.attr,
	&dev_attr_raw_xm_real_type.attr,
	&dev_attr_raw_xm_verify_process.attr,
	&dev_attr_raw_xm_current_state.attr,
	&dev_attr_raw_xm_adapter_id.attr,
	&dev_attr_raw_xm_adapter_svid.attr,
	&dev_attr_raw_xm_pd_verified.attr,
	&dev_attr_raw_xm_pdo2.attr,
	&dev_attr_raw_xm_fastchg_mode.attr,
	&dev_attr_raw_xm_quick_charge_type.attr,
	&dev_attr_raw_xm_apdo_max.attr,
	&dev_attr_raw_xm_power_max.attr,
	NULL,
};

static const struct attribute_group qcom_battmgr_raw_group = {
	.name = "sm8475_raw",
	.attrs = qcom_battmgr_raw_attrs,
};

/*
 * Vendor-compatible "xiaomi" attribute group.  Names, formats and the
 * request_vdm_cmd protocol follow Xiaomi's qti_battery_charger.c class
 * attributes so the MiPPS authentication daemon runs unchanged.
 */
static ssize_t qcom_battmgr_xm_parse_hex(const char *str, u8 *out, size_t max)
{
	size_t n = 0;

	while (n < max && isxdigit(str[0]) && isxdigit(str[1])) {
		out[n++] = (hex_to_bin(str[0]) << 4) | hex_to_bin(str[1]);
		str += 2;
	}
	return n;
}

static ssize_t request_vdm_cmd_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	char payload[65] = {};
	u8 bytes[16] = {};
	u32 words[4];
	u32 property;
	int cmd;
	int ret;

	if (sscanf(buf, "%d,%64s", &cmd, payload) < 1)
		return -EINVAL;

	qcom_battmgr_xm_parse_hex(payload, bytes, sizeof(bytes));
	for (unsigned int i = 0; i < ARRAY_SIZE(words); i++)
		words[i] = get_unaligned_le32(bytes + 4 * i);

	switch (cmd) {
	case XM_UVDM_CHARGER_VERSION:
		ret = qcom_battmgr_xm_set(battmgr, XM_VDM_CHARGER_VERSION, 0);
		break;
	case XM_UVDM_CHARGER_VOLTAGE:
		ret = qcom_battmgr_xm_set(battmgr, XM_VDM_CHARGER_VOLTAGE, 0);
		break;
	case XM_UVDM_CHARGER_TEMP:
		ret = qcom_battmgr_xm_set(battmgr, XM_VDM_CHARGER_TEMP, 0);
		break;
	case XM_UVDM_SESSION_SEED:
		property = XM_VDM_SESSION_SEED;
		goto words;
	case XM_UVDM_AUTHENTICATION:
		property = XM_VDM_AUTHENTICATION;
		goto words;
	case XM_UVDM_REVERSE_AUTHEN:
		property = XM_VDM_REVERSE_AUTHEN;
words:
		/* The vendor byte-swaps each 32 bit word before sending. */
		for (unsigned int i = 0; i < ARRAY_SIZE(words); i++)
			words[i] = swab32(words[i]);
		ret = qcom_battmgr_xm_set_words(battmgr, property, words);
		break;
	case XM_UVDM_VERIFIED:
		ret = qcom_battmgr_xm_set(battmgr, XM_VDM_VERIFIED, words[0]);
		break;
	case XM_UVDM_REMOVE_COMPENSATION:
		ret = qcom_battmgr_xm_set(battmgr, XM_VDM_REMOVE_COMPENSATION,
					  words[0]);
		break;
	default:
		return -EINVAL;
	}

	return ret ? ret : count;
}

static ssize_t request_vdm_cmd_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 words[4];
	u32 property;
	u32 state;
	u32 value;
	int ret;

	ret = qcom_battmgr_xm_get(battmgr, XM_UVDM_STATE, &state);
	if (ret)
		return ret;

	switch (state) {
	case XM_UVDM_CHARGER_VERSION:
		property = XM_VDM_CHARGER_VERSION;
		goto value;
	case XM_UVDM_CHARGER_VOLTAGE:
		property = XM_VDM_CHARGER_VOLTAGE;
		goto value;
	case XM_UVDM_CHARGER_TEMP:
		property = XM_VDM_CHARGER_TEMP;
value:
		ret = qcom_battmgr_xm_get(battmgr, property, &value);
		if (ret)
			return ret;
		return sysfs_emit(buf, "%u,%u\n", state, value);
	case XM_UVDM_AUTHENTICATION:
		ret = qcom_battmgr_xm_get_words(battmgr, XM_VDM_AUTHENTICATION,
						words);
		if (ret)
			return ret;
		return sysfs_emit(buf, "%u,%08x%08x%08x%08x\n", state,
				  words[0], words[1], words[2], words[3]);
	default:
		return sysfs_emit(buf, "%u,Null\n", state);
	}
}
static DEVICE_ATTR_RW(request_vdm_cmd);

static ssize_t verify_slave_flag_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	battmgr->xm_slave_fg = val;
	return count;
}

static ssize_t verify_slave_flag_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", battmgr->xm_slave_fg);
}
static DEVICE_ATTR_RW(verify_slave_flag);

static ssize_t verify_digest_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u8 digest[XM_DIGEST_LEN];
	int ret;

	if (qcom_battmgr_xm_parse_hex(buf, digest, sizeof(digest)) != sizeof(digest))
		return -EINVAL;

	ret = qcom_battmgr_xm_digest(battmgr, BATTMGR_XM_PROPERTY_SET,
				     battmgr->xm_slave_fg, digest, NULL);
	return ret ? ret : count;
}

static ssize_t verify_digest_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u8 digest[XM_DIGEST_LEN];
	int ret;

	ret = qcom_battmgr_xm_digest(battmgr, BATTMGR_XM_PROPERTY_GET,
				     battmgr->xm_slave_fg, NULL, digest);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%*phN\n", (int)sizeof(digest), digest);
}
static DEVICE_ATTR_RW(verify_digest);

#define QCOM_BATTMGR_XM_BOOL_ATTR(_name, _property) \
static ssize_t _name##_store(struct device *dev, \
			     struct device_attribute *attr, \
			     const char *buf, size_t count) \
{ \
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev); \
	bool val; \
	int ret; \
	if (kstrtobool(buf, &val)) \
		return -EINVAL; \
	ret = qcom_battmgr_xm_set(battmgr, _property, val); \
	return ret ? ret : count; \
} \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev); \
	u32 value; \
	int ret; \
	ret = qcom_battmgr_xm_get(battmgr, _property, &value); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, "%u\n", value); \
} \
static DEVICE_ATTR_RW(_name)

QCOM_BATTMGR_XM_BOOL_ATTR(authentic, XM_AUTHENTIC);
QCOM_BATTMGR_XM_BOOL_ATTR(slave_authentic, XM_SLAVE_AUTHENTIC);
QCOM_BATTMGR_XM_BOOL_ATTR(verify_process, XM_VERIFY_PROCESS);
QCOM_BATTMGR_XM_BOOL_ATTR(pd_verifed, XM_PD_VERIFIED);

#define QCOM_BATTMGR_XM_RO_ATTR(_name, _property, _fmt) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev); \
	u32 value; \
	int ret; \
	ret = qcom_battmgr_xm_get(battmgr, _property, &value); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, _fmt, value); \
} \
static DEVICE_ATTR_RO(_name)

QCOM_BATTMGR_XM_RO_ATTR(adapter_id, XM_ADAPTER_ID, "%08x\n");
QCOM_BATTMGR_XM_RO_ATTR(adapter_svid, XM_ADAPTER_SVID, "%04x\n");
QCOM_BATTMGR_XM_RO_ATTR(pdo2, XM_PDO2, "%08x\n");
QCOM_BATTMGR_XM_RO_ATTR(fastchg_mode, XM_FASTCHG_MODE, "%u\n");
QCOM_BATTMGR_XM_RO_ATTR(apdo_max, XM_APDO_MAX, "%u\n");
QCOM_BATTMGR_XM_RO_ATTR(power_max, XM_POWER_MAX, "%u\n");
QCOM_BATTMGR_XM_RO_ATTR(bq2597x_bus_current, XM_BQ2597X_BUS_CURRENT, "%u\n");
QCOM_BATTMGR_XM_RO_ATTR(bq2597x_slave_bus_current, XM_BQ2597X_SLAVE_BUS_CURRENT, "%u\n");
QCOM_BATTMGR_XM_RO_ATTR(bq2597x_bus_voltage, XM_BQ2597X_BUS_VOLTAGE, "%u\n");

static const char * const qcom_battmgr_xm_usb_type_text[] = {
	"Unknown", "SDP", "DCP", "CDP", "ACA", "C",
	"PD", "PD_DRP", "PD_PPS", "BrickID", "USB_FLOAT",
};

static const char * const qcom_battmgr_xm_qc_type_text[] = {
	"HVDCP", "HVDCP_3", "HVDCP_3P5", "USB_FLOAT", "HVDCP_3",
};

static ssize_t real_type_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	const char *name = "Unknown";
	u32 value;
	int ret;

	ret = qcom_battmgr_xm_get(battmgr, XM_REAL_TYPE, &value);
	if (ret)
		return ret;
	if (value >= 0x80 &&
	    value - 0x80 < ARRAY_SIZE(qcom_battmgr_xm_qc_type_text))
		name = qcom_battmgr_xm_qc_type_text[value - 0x80];
	else if (value < ARRAY_SIZE(qcom_battmgr_xm_usb_type_text))
		name = qcom_battmgr_xm_usb_type_text[value];
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(real_type);

static ssize_t current_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	const char *name;
	u32 value;
	int ret;

	ret = qcom_battmgr_xm_get(battmgr, XM_CURRENT_STATE, &value);
	if (ret)
		return ret;
	switch (value) {
	case 25:
		name = "SNK_Startup";
		break;
	case 31:
		name = "SNK_Ready";
		break;
	case 5:
		name = "SRC_Ready";
		break;
	default:
		name = "UNKNOWN";
		break;
	}
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(current_state);

/*
 * The M81 battery firmware accepts a cooling level from 0 through 15. This
 * property is an index, not a current in microamps. Report only an acknowledged
 * request and discard it when the charger service goes offline.
 */
static ssize_t thermal_level_requested_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	int level = READ_ONCE(battmgr->liuqin_thermal_level);

	if (!READ_ONCE(battmgr->service_up) || level < 0 ||
	    READ_ONCE(battmgr->liuqin_level_generation) != atomic_read(&battmgr->service_generation))
		return -ENODATA;
	return sysfs_emit(buf, "%d\n", level);
}

static ssize_t thermal_level_requested_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	unsigned int level;
	int generation, ret;

	if (kstrtouint(buf, 10, &level) || level > 15)
		return -EINVAL;

	mutex_lock(&battmgr->lock);
	generation = atomic_read(&battmgr->service_generation);
	if (!READ_ONCE(battmgr->service_up))
		ret = -EAGAIN;
	else
		ret = qcom_battmgr_xm_request_locked(battmgr, BATTMGR_BAT_PROPERTY_SET,
						     BATT_CHG_CTRL_LIM, level, NULL,
						     QCOM_BATTMGR_XM_VALUE_RESPONSE,
						     NULL, NULL);
	if (!ret && generation != atomic_read(&battmgr->service_generation))
		ret = -EAGAIN;
	if (!ret) {
		WRITE_ONCE(battmgr->liuqin_thermal_level, level);
		WRITE_ONCE(battmgr->liuqin_level_generation, generation);
	}
	mutex_unlock(&battmgr->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(thermal_level_requested);

static struct attribute *qcom_battmgr_xiaomi_attrs[] = {
	&dev_attr_thermal_level_requested.attr,
	&dev_attr_request_vdm_cmd.attr,
	&dev_attr_verify_slave_flag.attr,
	&dev_attr_verify_digest.attr,
	&dev_attr_authentic.attr,
	&dev_attr_slave_authentic.attr,
	&dev_attr_verify_process.attr,
	&dev_attr_pd_verifed.attr,
	&dev_attr_adapter_id.attr,
	&dev_attr_adapter_svid.attr,
	&dev_attr_pdo2.attr,
	&dev_attr_fastchg_mode.attr,
	&dev_attr_apdo_max.attr,
	&dev_attr_power_max.attr,
	&dev_attr_bq2597x_bus_current.attr,
	&dev_attr_bq2597x_slave_bus_current.attr,
	&dev_attr_bq2597x_bus_voltage.attr,
	&dev_attr_real_type.attr,
	&dev_attr_current_state.attr,
	NULL,
};

static umode_t qcom_battmgr_xiaomi_attr_visible(struct kobject *kobj,
					       struct attribute *attr, int index)
{
	if (attr == &dev_attr_thermal_level_requested.attr &&
	    !of_machine_is_compatible("xiaomi,liuqin"))
		return 0;
	return attr->mode;
}

static const struct attribute_group qcom_battmgr_xiaomi_group = {
	.is_visible = qcom_battmgr_xiaomi_attr_visible,
	.name = "xiaomi",
	.attrs = qcom_battmgr_xiaomi_attrs,
};

static int qcom_battmgr_request(struct qcom_battmgr *battmgr, void *data, size_t len)
{
	unsigned long left;
	int ret;

	reinit_completion(&battmgr->ack);

	battmgr->error = 0;

	ret = pmic_glink_send(battmgr->client, data, len);
	if (ret < 0)
		return ret;

	left = wait_for_completion_timeout(&battmgr->ack, HZ);
	if (!left)
		return -ETIMEDOUT;

	return battmgr->error;
}

static int qcom_battmgr_request_property(struct qcom_battmgr *battmgr, int opcode,
					 int property, u32 value)
{
	struct qcom_battmgr_property_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.battery = cpu_to_le32(0),
		.property = cpu_to_le32(property),
		.value = cpu_to_le32(value),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_status(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_STATUS),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_info(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_INFO),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_charge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_charge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_CHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.percent = cpu_to_le32(100),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_discharge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_discharge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_DISCHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.rate = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static const u8 sm8350_bat_prop_map[] = {
	[POWER_SUPPLY_PROP_STATUS] = BATT_STATUS,
	[POWER_SUPPLY_PROP_HEALTH] = BATT_HEALTH,
	[POWER_SUPPLY_PROP_PRESENT] = BATT_PRESENT,
	[POWER_SUPPLY_PROP_CHARGE_TYPE] = BATT_CHG_TYPE,
	[POWER_SUPPLY_PROP_CAPACITY] = BATT_CAPACITY,
	[POWER_SUPPLY_PROP_VOLTAGE_OCV] = BATT_VOLT_OCV,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = BATT_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = BATT_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = BATT_CURR_NOW,
	[POWER_SUPPLY_PROP_TEMP] = BATT_TEMP,
	[POWER_SUPPLY_PROP_TECHNOLOGY] = BATT_TECHNOLOGY,
	[POWER_SUPPLY_PROP_CHARGE_COUNTER] =  BATT_CHG_COUNTER,
	[POWER_SUPPLY_PROP_CYCLE_COUNT] = BATT_CYCLE_COUNT,
	[POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN] =  BATT_CHG_FULL_DESIGN,
	[POWER_SUPPLY_PROP_CHARGE_FULL] = BATT_CHG_FULL,
	[POWER_SUPPLY_PROP_MODEL_NAME] = BATT_MODEL_NAME,
	[POWER_SUPPLY_PROP_TIME_TO_FULL_AVG] = BATT_TTF_AVG,
	[POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG] = BATT_TTE_AVG,
	[POWER_SUPPLY_PROP_POWER_NOW] = BATT_POWER_NOW,
};

static unsigned int qcom_battmgr_bat_fw_property(struct qcom_battmgr *battmgr,
						 unsigned int property)
{
	/* SM8475 inserts constant-charge-current before temperature. */
	if (battmgr->variant == QCOM_BATTMGR_SM8475 && property >= BATT_TEMP)
		property++;

	return property;
}

static unsigned int qcom_battmgr_bat_host_property(struct qcom_battmgr *battmgr,
						   unsigned int property)
{
	if (battmgr->variant == QCOM_BATTMGR_SM8475 && property > BATT_TEMP)
		property--;

	return property;
}

static int qcom_battmgr_bat_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_bat_prop_map))
		return -EINVAL;

	prop = sm8350_bat_prop_map[psp];
	prop = qcom_battmgr_bat_fw_property(battmgr, prop);

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_bat_sc8280xp_update(struct qcom_battmgr *battmgr,
					    enum power_supply_property psp)
{
	int ret;

	mutex_lock(&battmgr->lock);

	if (!battmgr->info.valid) {
		ret = qcom_battmgr_update_info(battmgr);
		if (ret < 0)
			goto out_unlock;
		battmgr->info.valid = true;
	}

	ret = qcom_battmgr_update_status(battmgr);
	if (ret < 0)
		goto out_unlock;

	if (psp == POWER_SUPPLY_PROP_TIME_TO_FULL_AVG) {
		ret = qcom_battmgr_update_charge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

	if (psp == POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG) {
		ret = qcom_battmgr_update_discharge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_bat_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	enum qcom_battmgr_unit unit = battmgr->unit;
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_bat_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battmgr->status.status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = battmgr->info.charge_type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = battmgr->status.health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battmgr->info.present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = battmgr->info.technology;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = battmgr->info.cycle_count;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = battmgr->info.voltage_max_design;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->info.voltage_max;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->status.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = battmgr->status.voltage_ocv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->status.current_now;
		/* Xiaomi charger firmware uses positive current for discharge. */
		if (battmgr->variant == QCOM_BATTMGR_SM8475)
			val->intval = -val->intval;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = battmgr->status.power_now;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = battmgr->info.charge_count;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (battmgr->status.percent == (unsigned int)-1)
			return -ENODATA;
		val->intval = battmgr->status.percent;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battmgr->status.temperature;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		val->intval = battmgr->status.discharge_time;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		val->intval = battmgr->status.charge_time;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_YEAR:
		val->intval = battmgr->info.year;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_MONTH:
		val->intval = battmgr->info.month;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_DAY:
		val->intval = battmgr->info.day;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = battmgr->info.model_number;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = battmgr->info.oem_info;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = battmgr->info.serial_number;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct power_supply_desc sc8280xp_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sc8280xp_bat_props,
	.num_properties = ARRAY_SIZE(sc8280xp_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static const enum power_supply_property sm8350_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_POWER_NOW,
};

static const struct power_supply_desc sm8350_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8350_bat_props,
	.num_properties = ARRAY_SIZE(sm8350_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static int qcom_battmgr_ac_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->ac.online;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_ac_psy_desc = {
	.name = "qcom-battmgr-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sc8280xp_ac_props,
	.num_properties = ARRAY_SIZE(sc8280xp_ac_props),
	.get_property = qcom_battmgr_ac_get_property,
};

static const u8 sm8350_usb_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = USB_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = USB_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = USB_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = USB_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = USB_CURR_MAX,
	[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT] = USB_INPUT_CURR_LIMIT,
	[POWER_SUPPLY_PROP_USB_TYPE] = USB_TYPE,
};

static int qcom_battmgr_usb_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_usb_prop_map))
		return -EINVAL;

	prop = sm8350_usb_prop_map[psp];
	if (battmgr->variant == QCOM_BATTMGR_SM8475 && prop == USB_TYPE)
		prop = USB_ADAP_TYPE;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_usb_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_usb_sm8350_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->usb.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->usb.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->usb.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->usb.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->usb.current_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = battmgr->usb.current_limit;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = battmgr->usb.usb_type;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sc8280xp_usb_props,
	.num_properties = ARRAY_SIZE(sc8280xp_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const enum power_supply_property sm8350_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const struct power_supply_desc sm8350_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm8350_usb_props,
	.num_properties = ARRAY_SIZE(sm8350_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const u8 sm8350_wls_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = WLS_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = WLS_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = WLS_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = WLS_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = WLS_CURR_MAX,
};

static int qcom_battmgr_wls_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_wls_prop_map))
		return -EINVAL;

	prop = sm8350_wls_prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_WLS_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_wls_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_wls_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->wireless.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->wireless.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->wireless.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->wireless.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->wireless.current_max;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sc8280xp_wls_props,
	.num_properties = ARRAY_SIZE(sc8280xp_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static const enum power_supply_property sm8350_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc sm8350_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sm8350_wls_props,
	.num_properties = ARRAY_SIZE(sm8350_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static void qcom_battmgr_notification(struct qcom_battmgr *battmgr,
				      const struct qcom_battmgr_message *msg,
				      int len)
{
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int notification;

	if (payload_len != sizeof(msg->notification)) {
		dev_warn(battmgr->dev, "ignoring notification with invalid length\n");
		return;
	}

	notification = le32_to_cpu(msg->notification);
	switch (notification) {
	case NOTIF_BAT_INFO:
		battmgr->info.valid = false;
		fallthrough;
	case NOTIF_BAT_STATUS:
	case NOTIF_BAT_PROPERTY:
		power_supply_changed(battmgr->bat_psy);
		break;
	case NOTIF_USB_PROPERTY:
		power_supply_changed(battmgr->usb_psy);
		break;
	case NOTIF_WLS_PROPERTY:
		power_supply_changed(battmgr->wls_psy);
		break;
	case NOTIF_XM_PROPERTY:
		if (battmgr->variant == QCOM_BATTMGR_SM8475)
			break;
		fallthrough;
	default:
		dev_err(battmgr->dev, "unknown notification: %#x\n", notification);
		break;
	}
}

static void qcom_battmgr_sc8280xp_strcpy(char *dest, const char *src)
{
	size_t len = src[0];

	/* Some firmware versions return Pascal-style strings */
	if (len < BATTMGR_STRING_LEN && len == strnlen(src + 1, BATTMGR_STRING_LEN - 1)) {
		memcpy(dest, src + 1, len);
		dest[len] = '\0';
	} else {
		memcpy(dest, src, BATTMGR_STRING_LEN);
	}
}

static unsigned int qcom_battmgr_sc8280xp_parse_technology(const char *chemistry)
{
	if (!strncmp(chemistry, "LIO", BATTMGR_CHEMISTRY_LEN))
		return POWER_SUPPLY_TECHNOLOGY_LION;
	if (!strncmp(chemistry, "LIP", BATTMGR_CHEMISTRY_LEN))
		return POWER_SUPPLY_TECHNOLOGY_LIPO;

	pr_err("Unknown battery technology '%s'\n", chemistry);
	return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
}

static unsigned int qcom_battmgr_sc8280xp_convert_temp(unsigned int temperature)
{
	return DIV_ROUND_CLOSEST(temperature, 10);
}

static void qcom_battmgr_sc8280xp_callback(struct qcom_battmgr *battmgr,
					   const struct qcom_battmgr_message *resp,
					   size_t len)
{
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	unsigned int source;
	unsigned int state;
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_REQUEST_NOTIFICATION:
		battmgr->error = 0;
		break;
	case BATTMGR_BAT_INFO:
		/* some firmware versions report an extra __le32 at the end of the payload */
		if (payload_len != sizeof(resp->info) &&
		    payload_len != (sizeof(resp->info) + sizeof(__le32))) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery information request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->unit = le32_to_cpu(resp->info.power_unit);

		battmgr->info.present = true;
		battmgr->info.design_capacity = le32_to_cpu(resp->info.design_capacity) * 1000;
		battmgr->info.last_full_capacity = le32_to_cpu(resp->info.last_full_capacity) * 1000;
		battmgr->info.voltage_max_design = le32_to_cpu(resp->info.design_voltage) * 1000;
		battmgr->info.capacity_low = le32_to_cpu(resp->info.capacity_low) * 1000;
		battmgr->info.cycle_count = le32_to_cpu(resp->info.cycle_count);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.model_number, resp->info.model_number);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.serial_number, resp->info.serial_number);
		battmgr->info.technology = qcom_battmgr_sc8280xp_parse_technology(resp->info.battery_chemistry);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.oem_info, resp->info.oem_info);
		battmgr->info.day = resp->info.day;
		battmgr->info.month = resp->info.month;
		battmgr->info.year = le16_to_cpu(resp->info.year);
		break;
	case BATTMGR_BAT_STATUS:
		if (payload_len != sizeof(resp->status)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery status request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		state = le32_to_cpu(resp->status.battery_state);
		if (state & BIT(0))
			battmgr->status.status = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (state & BIT(1))
			battmgr->status.status = POWER_SUPPLY_STATUS_CHARGING;
		else
			battmgr->status.status = POWER_SUPPLY_STATUS_NOT_CHARGING;

		battmgr->status.capacity = le32_to_cpu(resp->status.capacity) * 1000;
		battmgr->status.power_now = le32_to_cpu(resp->status.rate) * 1000;
		battmgr->status.voltage_now = le32_to_cpu(resp->status.battery_voltage) * 1000;
		battmgr->status.temperature = qcom_battmgr_sc8280xp_convert_temp(le32_to_cpu(resp->status.temperature));

		source = le32_to_cpu(resp->status.charging_source);
		battmgr->ac.online = source == BATTMGR_CHARGING_SOURCE_AC;
		battmgr->usb.online = source == BATTMGR_CHARGING_SOURCE_USB;
		battmgr->wireless.online = source == BATTMGR_CHARGING_SOURCE_WIRELESS;
		if (battmgr->info.last_full_capacity != 0) {
			/*
			 * 100 * battmgr->status.capacity can overflow a 32bit
			 * unsigned integer. FW readings are in m{W/A}h, which
			 * are multiplied by 1000 converting them to u{W/A}h,
			 * the format the power_supply API expects.
			 * To avoid overflow use the original value for dividend
			 * and convert the divider back to m{W/A}h, which can be
			 * done without any loss of precision.
			 */
			battmgr->status.percent =
				(100 * le32_to_cpu(resp->status.capacity)) /
				(battmgr->info.last_full_capacity / 1000);
		} else {
			/*
			 * Let the sysfs handler know no data is available at
			 * this time.
			 */
			battmgr->status.percent = (unsigned int)-1;
		}
		break;
	case BATTMGR_BAT_DISCHARGE_TIME:
		battmgr->status.discharge_time = le32_to_cpu(resp->time);
		break;
	case BATTMGR_BAT_CHARGE_TIME:
		battmgr->status.charge_time = le32_to_cpu(resp->time);
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

	complete(&battmgr->ack);
}

static void qcom_battmgr_sm8350_callback(struct qcom_battmgr *battmgr,
					 const struct qcom_battmgr_message *resp,
					 size_t len)
{
	unsigned int property;
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int val;

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_BAT_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (property == qcom_battmgr_bat_fw_property(battmgr, BATT_MODEL_NAME)) {
			if (payload_len != sizeof(resp->strval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for BATT_MODEL_NAME request: %zd\n",
					 payload_len);
				battmgr->error = -ENODATA;
				return;
			}
		} else {
			if (payload_len != sizeof(resp->intval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for %#x request: %zd\n",
					 property, payload_len);
				battmgr->error = -ENODATA;
				return;
			}

			battmgr->error = le32_to_cpu(resp->intval.result);
			if (battmgr->error)
				goto out_complete;
		}
		property = qcom_battmgr_bat_host_property(battmgr, property);

		switch (property) {
		case BATT_STATUS:
			battmgr->status.status = le32_to_cpu(resp->intval.value);
			break;
		case BATT_HEALTH:
			battmgr->status.health = le32_to_cpu(resp->intval.value);
			break;
		case BATT_PRESENT:
			battmgr->info.present = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_TYPE:
			battmgr->info.charge_type = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CAPACITY:
			battmgr->status.percent = le32_to_cpu(resp->intval.value) / 100;
			break;
		case BATT_VOLT_OCV:
			battmgr->status.voltage_ocv = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_NOW:
			battmgr->status.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_MAX:
			battmgr->info.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CURR_NOW:
			battmgr->status.current_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TEMP:
			val = le32_to_cpu(resp->intval.value);
			battmgr->status.temperature = DIV_ROUND_CLOSEST(val, 10);
			break;
		case BATT_TECHNOLOGY:
			battmgr->info.technology = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_COUNTER:
			battmgr->info.charge_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CYCLE_COUNT:
			battmgr->info.cycle_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL_DESIGN:
			battmgr->info.design_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL:
			battmgr->info.last_full_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_MODEL_NAME:
			strscpy(battmgr->info.model_number, resp->strval.model, BATTMGR_STRING_LEN);
			break;
		case BATT_TTF_AVG:
			battmgr->status.charge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TTE_AVG:
			battmgr->status.discharge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_POWER_NOW:
			battmgr->status.power_now = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_USB_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;
		if (battmgr->variant == QCOM_BATTMGR_SM8475 &&
		    property == USB_ADAP_TYPE)
			property = USB_TYPE;

		switch (property) {
		case USB_ONLINE:
			battmgr->usb.online = le32_to_cpu(resp->intval.value);
			break;
		case USB_VOLT_NOW:
			battmgr->usb.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_VOLT_MAX:
			battmgr->usb.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_NOW:
			battmgr->usb.current_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_MAX:
			battmgr->usb.current_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_INPUT_CURR_LIMIT:
			battmgr->usb.current_limit = le32_to_cpu(resp->intval.value);
			break;
		case USB_TYPE:
			battmgr->usb.usb_type = le32_to_cpu(resp->intval.value);
			break;
		case USB_REAL_TYPE:
			battmgr->raw_opcode = opcode;
			battmgr->raw_property = property;
			battmgr->raw_value = le32_to_cpu(resp->intval.value);
			battmgr->raw_valid = true;
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_WLS_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;

		switch (property) {
		case WLS_ONLINE:
			battmgr->wireless.online = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_NOW:
			battmgr->wireless.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_MAX:
			battmgr->wireless.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_NOW:
			battmgr->wireless.current_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_MAX:
			battmgr->wireless.current_max = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_REQUEST_NOTIFICATION:
		battmgr->error = 0;
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

out_complete:
	complete(&battmgr->ack);
}

/*
 * Return true when an XM request is in flight, whether or not @data is a
 * valid reply.  In the invalid case it is intentionally not completed: a
 * stale reply, an XM notification, or a frame for another property must time
 * out rather than be attributed to the request currently holding @lock.
 */
static bool qcom_battmgr_xm_handle_response(struct qcom_battmgr *battmgr,
					    const void *data, size_t len)
{
	const struct pmic_glink_hdr *hdr = data;
	const struct qcom_battmgr_message *value_response = data;
	const struct qcom_battmgr_xm_words_message *words_response = data;
	const struct qcom_battmgr_xm_digest_message *digest_response = data;
	const size_t value_len = sizeof(value_response->hdr) +
				 sizeof(value_response->intval);
	u32 owner;
	u32 type;
	u32 opcode;
	u32 property;
	size_t expected_len;

	if (len < sizeof(*hdr))
		return true;

	owner = le32_to_cpu(hdr->owner);
	type = le32_to_cpu(hdr->type);
	opcode = le32_to_cpu(hdr->opcode);
	switch (battmgr->xm.response_kind) {
	case QCOM_BATTMGR_XM_WORDS_RESPONSE:
		expected_len = sizeof(*words_response);
		break;
	case QCOM_BATTMGR_XM_DIGEST_RESPONSE:
		expected_len = sizeof(*digest_response);
		break;
	default:
		expected_len = value_len;
		break;
	}

	/*
	 * The vendor accepts a SET acknowledgment either as an echo of the
	 * request frame (32 byte words, 52 byte digest; no ret_code) or as the
	 * plain 24 byte property reply carrying ret_code.
	 */
	if (owner != PMIC_GLINK_OWNER_BATTMGR || type != PMIC_GLINK_REQ_RESP ||
	    opcode != battmgr->xm.opcode ||
	    (len != expected_len &&
	     !(opcode == BATTMGR_XM_PROPERTY_SET && len == value_len)))
		return true;

	property = le32_to_cpu(value_response->intval.property);
	if (property != battmgr->xm.property)
		return true;

	if (len == value_len) {
		/* The 24 byte property reply is the only format carrying ret_code. */
		if (le32_to_cpu(value_response->intval.result))
			battmgr->error = -EREMOTEIO;
		else
			battmgr->xm.value = le32_to_cpu(value_response->intval.value);
	} else if (battmgr->xm.response_kind == QCOM_BATTMGR_XM_DIGEST_RESPONSE) {
		/* The 52 byte digest reply has no ret_code field. */
		memcpy(battmgr->xm.digest, digest_response->digest,
		       sizeof(battmgr->xm.digest));
		battmgr->error = 0;
	} else {
		/* The exact 32 byte session/auth reply has no ret_code field. */
		for (unsigned int i = 0; i < ARRAY_SIZE(battmgr->xm.words); i++)
			battmgr->xm.words[i] = le32_to_cpu(words_response->words[i]);
		battmgr->error = 0;
	}

	complete(&battmgr->ack);
	return true;
}

static void qcom_battmgr_callback(const void *data, size_t len, void *priv)
{
	const struct pmic_glink_hdr *hdr = data;
	struct qcom_battmgr *battmgr = priv;
	bool xm_active;
	unsigned int opcode;

	if (len < sizeof(*hdr)) {
		dev_warn(battmgr->dev, "short PMIC GLINK frame: %zu\n", len);
		return;
	}

	opcode = le32_to_cpu(hdr->opcode);
	xm_active = smp_load_acquire(&battmgr->xm.active); /* pairs with request publish */

	/* Do this before the generic callback, whose old completion is broad. */
	if (xm_active) {
		if (opcode == BATTMGR_NOTIFICATION)
			qcom_battmgr_notification(battmgr, data, len);
		else
			qcom_battmgr_xm_handle_response(battmgr, data, len);
		return;
	}

	/* XM must never reach the generic, broad completion path. */
	if (opcode == BATTMGR_XM_PROPERTY_GET || opcode == BATTMGR_XM_PROPERTY_SET ||
	    (battmgr->variant == QCOM_BATTMGR_SM8475 &&
	     opcode == BATTMGR_BAT_PROPERTY_SET)) {
		dev_warn_ratelimited(battmgr->dev,
				     "unsolicited XM response %#x ignored\n", opcode);
		return;
	}

	if (opcode == BATTMGR_NOTIFICATION)
		qcom_battmgr_notification(battmgr, data, len);
	else if (battmgr->variant == QCOM_BATTMGR_SC8280XP)
		qcom_battmgr_sc8280xp_callback(battmgr, data, len);
	else
		qcom_battmgr_sm8350_callback(battmgr, data, len);
}

static void qcom_battmgr_enable_worker(struct work_struct *work)
{
	struct qcom_battmgr *battmgr = container_of(work, struct qcom_battmgr, enable_work);
	struct qcom_battmgr_enable_request req = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_NOTIFY),
		.hdr.opcode = cpu_to_le32(BATTMGR_REQUEST_NOTIFICATION),
	};
	int ret;

	ret = qcom_battmgr_request(battmgr, &req, sizeof(req));
	if (ret)
		dev_err(battmgr->dev, "failed to request power notifications\n");
}

static void qcom_battmgr_pdr_notify(void *priv, int state)
{
	struct qcom_battmgr *battmgr = priv;

	if (state == SERVREG_SERVICE_STATE_UP) {
		WRITE_ONCE(battmgr->service_up, true);
		schedule_work(&battmgr->enable_work);
	} else {
		WRITE_ONCE(battmgr->service_up, false);
		atomic_inc(&battmgr->service_generation);
		WRITE_ONCE(battmgr->liuqin_thermal_level, -1);
	}
}

static const struct of_device_id qcom_battmgr_of_variants[] = {
	{ .compatible = "qcom,sm8475-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8475 },
	{ .compatible = "qcom,sc8180x-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,sc8280xp-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,x1e80100-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	/* Unmatched devices falls back to QCOM_BATTMGR_SM8350 */
	{}
};

static char *qcom_battmgr_battery[] = { "battery" };

static int qcom_battmgr_probe(struct auxiliary_device *adev,
			      const struct auxiliary_device_id *id)
{
	struct power_supply_config psy_cfg_supply = {};
	struct power_supply_config psy_cfg = {};
	const struct of_device_id *match;
	struct qcom_battmgr *battmgr;
	struct device *dev = &adev->dev;

	battmgr = devm_kzalloc(dev, sizeof(*battmgr), GFP_KERNEL);
	if (!battmgr)
		return -ENOMEM;

	battmgr->dev = dev;
	dev_set_drvdata(dev, battmgr);

	psy_cfg.drv_data = battmgr;
	psy_cfg.fwnode = dev_fwnode(&adev->dev);

	psy_cfg_supply.drv_data = battmgr;
	psy_cfg_supply.fwnode = dev_fwnode(&adev->dev);
	psy_cfg_supply.supplied_to = qcom_battmgr_battery;
	psy_cfg_supply.num_supplicants = 1;
	/*
	 * The charger firmware sends a notification on every change, and
	 * power_supply_changed() reports each one as a wakeup event, which
	 * aborts suspend-to-idle within minutes. Those updates aren't a reason
	 * to wake the system.
	 */
	psy_cfg.no_wakeup_source = true;
	psy_cfg_supply.no_wakeup_source = true;


	INIT_WORK(&battmgr->enable_work, qcom_battmgr_enable_worker);
	mutex_init(&battmgr->lock);
	init_completion(&battmgr->ack);
	battmgr->liuqin_thermal_level = -1;

	match = of_match_device(qcom_battmgr_of_variants, dev->parent);
	if (match)
		battmgr->variant = (unsigned long)match->data;
	else
		battmgr->variant = QCOM_BATTMGR_SM8350;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP) {
		battmgr->bat_psy = devm_power_supply_register(dev, &sc8280xp_bat_psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		battmgr->ac_psy = devm_power_supply_register(dev, &sc8280xp_ac_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->ac_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->ac_psy),
					     "failed to register AC power supply\n");

		battmgr->usb_psy = devm_power_supply_register(dev, &sc8280xp_usb_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		battmgr->wls_psy = devm_power_supply_register(dev, &sc8280xp_wls_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	} else {
		battmgr->bat_psy = devm_power_supply_register(dev, &sm8350_bat_psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		battmgr->usb_psy = devm_power_supply_register(dev, &sm8350_usb_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		battmgr->wls_psy = devm_power_supply_register(dev, &sm8350_wls_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	}

	battmgr->client = devm_pmic_glink_client_alloc(dev, PMIC_GLINK_OWNER_BATTMGR,
						       qcom_battmgr_callback,
						       qcom_battmgr_pdr_notify,
						       battmgr);
	if (IS_ERR(battmgr->client))
		return PTR_ERR(battmgr->client);

	pmic_glink_client_register(battmgr->client);

	if (battmgr->variant == QCOM_BATTMGR_SM8475) {
		int ret;

		ret = devm_device_add_group(dev, &qcom_battmgr_raw_group);
		if (ret)
			return ret;
		return devm_device_add_group(dev, &qcom_battmgr_xiaomi_group);
	}

	return 0;
}

static const struct auxiliary_device_id qcom_battmgr_id_table[] = {
	{ .name = "pmic_glink.power-supply", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, qcom_battmgr_id_table);

static struct auxiliary_driver qcom_battmgr_driver = {
	.name = "pmic_glink_power_supply",
	.probe = qcom_battmgr_probe,
	.id_table = qcom_battmgr_id_table,
};

module_auxiliary_driver(qcom_battmgr_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK battery manager driver");
MODULE_LICENSE("GPL");
