// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the SM8475 (Xiaomi Pad 6 Pro) battery manager protocol.
 *
 * This file is included by qcom_battmgr.c, so the tests drive the driver's
 * own static functions. qcom_battmgr_request() is replaced by a fake ADSP
 * that answers through the real qcom_battmgr_callback(). The replacement
 * only applies on the test thread, so every worker is invoked directly here
 * and none is ever left queued.
 *
 * Expected firmware numbers are not taken from the driver's constants. XM
 * property ids are the ids the class attributes of the shipping
 * qti_battery_charger_main_m81.ko put into their 24 byte requests (SHA256
 * b13b976e8a0d5ce2256df4f96368a6de0999aecf4dd311a9ded0f993e2cedf6b, from the
 * ROM whose ADSP firmware the liuqin images ship). Battery readings are the
 * liuqin stock-sysfs units recorded by the liuqin-nixos telemetry test.
 * The input current limit follows stock usb_psy_set_icl() and the stock PHY,
 * the reported USB type stock battery_chg_update_usb_type_work().
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#include "power_supply.h"	/* power_supply_uevent(), power_supply_attr_groups */

struct battmgr_fake {
	struct qcom_battmgr *b;
	struct device *dev;
	unsigned int requests;
	u32 log[8][2];
	u32 last_opcode;
	u32 last_property;
	int transport_error;	/* returned without any reply */
	u32 fail_opcode;	/* answered with a non-zero result */
	u32 fail_property;
	unsigned int sets;	/* USB property SET requests */
	u32 set_value;
	u32 bat[32];		/* indexed by firmware battery property id */
	u32 usb[16];
	u32 xm[256];
};

/* The 24 byte property reply on the wire. */
struct battmgr_fake_reply {
	struct pmic_glink_hdr hdr;
	__le32 property;
	__le32 value;
	__le32 result;
} __packed;

static int battmgr_fake_request(struct qcom_battmgr *battmgr, void *data, size_t len)
{
	struct kunit *test = kunit_get_current_test();
	struct battmgr_fake *fw = test->priv;
	const struct qcom_battmgr_property_request *req = data;
	struct battmgr_fake_reply reply = {};
	u32 opcode, property, value = 0;

	KUNIT_EXPECT_EQ(test, len, sizeof(*req));
	opcode = le32_to_cpu(req->hdr.opcode);
	property = le32_to_cpu(req->property);
	if (fw->requests < ARRAY_SIZE(fw->log)) {
		fw->log[fw->requests][0] = opcode;
		fw->log[fw->requests][1] = property;
	}
	fw->requests++;
	fw->last_opcode = opcode;
	fw->last_property = property;

	/* What qcom_battmgr_request() does before pmic_glink_send(). */
	reinit_completion(&battmgr->ack);
	battmgr->error = 0;
	if (fw->transport_error)
		return fw->transport_error;

	switch (opcode) {
	case BATTMGR_BAT_PROPERTY_GET:
		if (property < ARRAY_SIZE(fw->bat))
			value = fw->bat[property];
		break;
	case BATTMGR_USB_PROPERTY_GET:
		if (property < ARRAY_SIZE(fw->usb))
			value = fw->usb[property];
		break;
	case BATTMGR_XM_PROPERTY_GET:
		if (property < ARRAY_SIZE(fw->xm))
			value = fw->xm[property];
		break;
	case BATTMGR_USB_PROPERTY_SET:
		fw->sets++;
		fw->set_value = le32_to_cpu(req->value);
		break;
	}

	reply.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR);
	reply.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP);
	reply.hdr.opcode = cpu_to_le32(opcode);
	reply.property = cpu_to_le32(property);
	reply.value = cpu_to_le32(value);
	reply.result = cpu_to_le32(opcode == fw->fail_opcode &&
				   property == fw->fail_property);
	qcom_battmgr_callback(&reply, sizeof(reply), battmgr);

	return completion_done(&battmgr->ack) ? battmgr->error : -ETIMEDOUT;
}

static void battmgr_test_stop(void *data)
{
	struct qcom_battmgr *b = data;

	disable_work_sync(&b->online_work);
	disable_work_sync(&b->xm_notify_work);
	disable_delayed_work_sync(&b->refresh_work);
	if (b->charge_ws) {
		__pm_relax(b->charge_ws);
		wakeup_source_unregister(b->charge_ws);
	}
}

static int battmgr_test_init(struct kunit *test)
{
	struct battmgr_fake *fw;
	struct qcom_battmgr *b;
	struct device *dev;

	fw = kunit_kzalloc(test, sizeof(*fw), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fw);
	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	dev = kunit_device_register(test, "qcom-battmgr-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	b->dev = dev;
	b->variant = QCOM_BATTMGR_SM8475;
	b->service_up = true;
	mutex_init(&b->lock);
	init_completion(&b->ack);
	spin_lock_init(&b->connector_lock);
	spin_lock_init(&b->charge_lock);
	INIT_WORK(&b->online_work, qcom_battmgr_check_online);
	INIT_WORK(&b->xm_notify_work, qcom_battmgr_xm_notify_worker);
	INIT_DELAYED_WORK(&b->refresh_work, qcom_battmgr_sm8475_refresh_worker);
	/* A kworker would reach the real transport: run this one by hand. */
	disable_work(&b->online_work);
	dev_set_drvdata(dev, b);

	fw->b = b;
	fw->dev = dev;
	test->priv = fw;
	kunit_activate_static_stub(test, qcom_battmgr_request, battmgr_fake_request);

	return kunit_add_action_or_reset(test, battmgr_test_stop, b);
}

struct battmgr_id_case {
	struct device_attribute *attr;
	u32 opcode;
	u32 id;
};

static void battmgr_test_property_ids(struct kunit *test)
{
	/* Ids of the stock *_show() functions; see the file comment. */
	const struct battmgr_id_case cases[] = {
		{ &dev_attr_raw_usb_real_type, 0x32, 11 },
		{ &dev_attr_raw_xm_connector_temp, 0x50, 2 },
		{ &dev_attr_authentic, 0x50, 3 },
		{ &dev_attr_raw_xm_authentic, 0x50, 3 },
		{ &dev_attr_real_type, 0x50, 6 },
		{ &dev_attr_raw_xm_real_type, 0x50, 6 },
		{ &dev_attr_verify_process, 0x50, 7 },
		{ &dev_attr_raw_xm_verify_process, 0x50, 7 },
		{ &dev_attr_current_state, 0x50, 16 },
		{ &dev_attr_raw_xm_current_state, 0x50, 16 },
		{ &dev_attr_adapter_id, 0x50, 17 },
		{ &dev_attr_raw_xm_adapter_id, 0x50, 17 },
		{ &dev_attr_adapter_svid, 0x50, 18 },
		{ &dev_attr_raw_xm_adapter_svid, 0x50, 18 },
		{ &dev_attr_pd_verifed, 0x50, 19 },
		{ &dev_attr_raw_xm_pd_verified, 0x50, 19 },
		{ &dev_attr_pdo2, 0x50, 20 },
		{ &dev_attr_raw_xm_pdo2, 0x50, 20 },
		{ &dev_attr_bq2597x_bus_current, 0x50, 24 },
		{ &dev_attr_bq2597x_slave_bus_current, 0x50, 25 },
		{ &dev_attr_bq2597x_bus_voltage, 0x50, 27 },
		{ &dev_attr_fastchg_mode, 0x50, 38 },
		{ &dev_attr_raw_xm_fastchg_mode, 0x50, 38 },
		/* No stock reader: the slot between soc_decimal_rate (43) and apdo_max. */
		{ &dev_attr_raw_xm_quick_charge_type, 0x50, 44 },
		{ &dev_attr_apdo_max, 0x50, 45 },
		{ &dev_attr_raw_xm_apdo_max, 0x50, 45 },
		{ &dev_attr_power_max, 0x50, 46 },
		{ &dev_attr_raw_xm_power_max, 0x50, 46 },
		{ &dev_attr_slave_authentic, 0x50, 139 },
	};
	struct battmgr_fake *fw = test->priv;
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	ssize_t ret;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct battmgr_id_case *c = &cases[i];
		const char *name = c->attr->attr.name;

		fw->requests = 0;
		ret = c->attr->show(fw->dev, c->attr, buf);
		KUNIT_EXPECT_GT_MSG(test, ret, 0, "%s", name);
		KUNIT_EXPECT_EQ_MSG(test, fw->requests, 1, "%s", name);
		KUNIT_EXPECT_EQ_MSG(test, fw->last_opcode, c->opcode, "%s", name);
		KUNIT_EXPECT_EQ_MSG(test, fw->last_property, c->id, "%s", name);
	}

	/* request_vdm_cmd_show(): UVDM state 21, then VDM 8, 9 or 10. */
	for (u32 state = 1; state <= 3; state++) {
		fw->xm[21] = state;
		fw->requests = 0;
		ret = dev_attr_request_vdm_cmd.show(fw->dev, &dev_attr_request_vdm_cmd, buf);
		KUNIT_EXPECT_GT(test, ret, 0);
		KUNIT_ASSERT_EQ(test, fw->requests, 2);
		KUNIT_EXPECT_EQ(test, fw->log[0][1], 21);
		KUNIT_EXPECT_EQ(test, fw->log[1][1], 7 + state);
	}
}

static void battmgr_test_refresh(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;
	unsigned long now = jiffies;

	/* xm_batt_update_work() reads 86 (0x56) every 10 s, or 20 s blanked. */
	qcom_battmgr_sm8475_refresh_worker(&b->refresh_work.work);
	KUNIT_EXPECT_EQ(test, fw->requests, 1);
	KUNIT_EXPECT_EQ(test, fw->last_opcode, 0x50);
	KUNIT_EXPECT_EQ(test, fw->last_property, 86);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&b->refresh_work));
	KUNIT_EXPECT_TRUE(test, time_after_eq(b->refresh_work.timer.expires,
					       now + 10 * HZ));
	KUNIT_EXPECT_TRUE(test, time_before_eq(b->refresh_work.timer.expires,
						jiffies + 10 * HZ + 1));
	cancel_delayed_work_sync(&b->refresh_work);

	/* Charger service down: no request, same cadence. */
	WRITE_ONCE(b->service_up, false);
	fw->requests = 0;
	qcom_battmgr_sm8475_refresh_worker(&b->refresh_work.work);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&b->refresh_work));
	cancel_delayed_work_sync(&b->refresh_work);

	/* A transport failure is retried on the next period. */
	WRITE_ONCE(b->service_up, true);
	fw->transport_error = -ETIMEDOUT;
	qcom_battmgr_sm8475_refresh_worker(&b->refresh_work.work);
	KUNIT_EXPECT_EQ(test, fw->requests, 1);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&b->refresh_work));
	cancel_delayed_work_sync(&b->refresh_work);
}

static DECLARE_COMPLETION(battmgr_test_event);

static void battmgr_test_event_fn(struct work_struct *work)
{
	complete(&battmgr_test_event);
}

static void battmgr_test_refresh_on_resume(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;

	/* Resume runs the refresher at once; the stand-in only signals. */
	INIT_DELAYED_WORK(&b->refresh_work, battmgr_test_event_fn);
	reinit_completion(&battmgr_test_event);
	KUNIT_EXPECT_EQ(test, qcom_battmgr_resume(fw->dev), 0);
	KUNIT_EXPECT_GT(test, wait_for_completion_timeout(&battmgr_test_event, HZ), 0);

	b->variant = QCOM_BATTMGR_SM8350;
	reinit_completion(&battmgr_test_event);
	KUNIT_EXPECT_EQ(test, qcom_battmgr_resume(fw->dev), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&b->refresh_work));
	KUNIT_EXPECT_FALSE(test, completion_done(&battmgr_test_event));
}

static void battmgr_test_xm_notification(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;
	struct {
		struct pmic_glink_hdr hdr;
		__le32 notification;
	} __packed msg = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_NOTIFY),
		.hdr.opcode = cpu_to_le32(BATTMGR_NOTIFICATION),
		.notification = cpu_to_le32(0x50),
	};

	/* The stand-in only signals that 0x50 queued the uevent work. */
	INIT_WORK(&b->xm_notify_work, battmgr_test_event_fn);
	reinit_completion(&battmgr_test_event);
	qcom_battmgr_callback(&msg, sizeof(msg), b);
	KUNIT_EXPECT_GT(test, wait_for_completion_timeout(&battmgr_test_event, HZ), 0);

	/* Also while an XM request holds the RX path. */
	reinit_completion(&battmgr_test_event);
	smp_store_release(&b->xm.active, true); /* as qcom_battmgr_xm_transact_locked() */
	qcom_battmgr_callback(&msg, sizeof(msg), b);
	WRITE_ONCE(b->xm.active, false);
	KUNIT_EXPECT_GT(test, wait_for_completion_timeout(&battmgr_test_event, HZ), 0);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);

	/* The worker itself sends KOBJ_CHANGE on the battmgr device. */
	qcom_battmgr_xm_notify_worker(&b->xm_notify_work);
}

static int battmgr_test_get(struct kunit *test, enum power_supply_property psp,
			    int *value)
{
	struct battmgr_fake *fw = test->priv;
	struct power_supply *psy = kunit_kzalloc(test, sizeof(*psy), GFP_KERNEL);
	union power_supply_propval val = { .intval = INT_MIN };
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, psy);
	psy->drv_data = fw->b;
	ret = qcom_battmgr_bat_get_property(psy, psp, &val);
	*value = val.intval;
	return ret;
}

static bool battmgr_test_has(const enum power_supply_property *props,
			     size_t count, enum power_supply_property psp)
{
	for (size_t i = 0; i < count; i++)
		if (props[i] == psp)
			return true;
	return false;
}

static void battmgr_test_units(struct kunit *test)
{
	const enum power_supply_property placeholders[] = {
		POWER_SUPPLY_PROP_POWER_NOW,
		POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
		POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	};
	struct battmgr_fake *fw = test->priv;
	int value;

	/*
	 * SM8475 firmware ids: constant-charge-current is inserted before
	 * temperature, so the counter is 15, design 17 and full 18.
	 */
	fw->bat[15] = 4688;		/* mAh */
	fw->bat[17] = 8600000;		/* uAh */
	fw->bat[18] = 7827000;		/* uAh */
	fw->bat[4] = 6600;		/* hundredths of a percent */
	fw->bat[7] = 4389000;
	fw->bat[9] = -10843000;		/* firmware sign: negative charges */

	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_COUNTER, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 4688000);
	KUNIT_EXPECT_EQ(test, fw->last_property, 15);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_NOW, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 4688000);
	KUNIT_EXPECT_EQ(test, fw->last_property, 15);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
					       &value), 0);
	KUNIT_EXPECT_EQ(test, value, 8600000);
	KUNIT_EXPECT_EQ(test, fw->last_property, 17);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_FULL, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 7827000);
	KUNIT_EXPECT_EQ(test, fw->last_property, 18);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CAPACITY, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 66);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_VOLTAGE_NOW, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 4389000);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CURRENT_NOW, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 10843000);

	/* The conversion saturates nowhere: out of range is an error. */
	fw->bat[15] = 0;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_NOW, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 0);
	fw->bat[15] = 2147483;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_COUNTER, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 2147483000);
	fw->bat[15] = 2147484;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_COUNTER, &value),
			-ERANGE);
	fw->bat[15] = -2147484;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_NOW, &value),
			-ERANGE);

	/* Placeholders are neither requested nor advertised. */
	fw->requests = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(placeholders); i++) {
		KUNIT_EXPECT_EQ(test, battmgr_test_get(test, placeholders[i], &value), -ENODATA);
		KUNIT_EXPECT_FALSE(test, battmgr_test_has(sm8475_bat_props,
							  ARRAY_SIZE(sm8475_bat_props),
							  placeholders[i]));
		KUNIT_EXPECT_TRUE(test, battmgr_test_has(sm8350_bat_props,
							 ARRAY_SIZE(sm8350_bat_props),
							 placeholders[i]));
	}
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
	KUNIT_EXPECT_TRUE(test, battmgr_test_has(sm8475_bat_props, ARRAY_SIZE(sm8475_bat_props),
						 POWER_SUPPLY_PROP_CHARGE_NOW));
	for (size_t i = 0; i < ARRAY_SIZE(sm8350_bat_props); i++)
		if (!battmgr_test_has(placeholders, ARRAY_SIZE(placeholders), sm8350_bat_props[i]))
			KUNIT_EXPECT_TRUE(test, battmgr_test_has(sm8475_bat_props,
								 ARRAY_SIZE(sm8475_bat_props),
								 sm8350_bat_props[i]));
	KUNIT_EXPECT_EQ(test, ARRAY_SIZE(sm8475_bat_props),
			ARRAY_SIZE(sm8350_bat_props) - ARRAY_SIZE(placeholders) + 1);

	/* Errors pass through; nothing is fabricated. */
	WRITE_ONCE(fw->b->service_up, false);
	fw->requests = 0;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_NOW, &value),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
	WRITE_ONCE(fw->b->service_up, true);
	fw->transport_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_NOW, &value),
			-ETIMEDOUT);
	fw->transport_error = 0;

	/* Other variants keep their own units and their power reading. */
	fw->b->variant = QCOM_BATTMGR_SM8350;
	fw->bat[14] = 4688000;
	fw->bat[22] = 12345000;
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_CHARGE_COUNTER, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 4688000);
	KUNIT_EXPECT_EQ(test, fw->last_property, 14);
	KUNIT_EXPECT_EQ(test, battmgr_test_get(test, POWER_SUPPLY_PROP_POWER_NOW, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 12345000);
}

struct battmgr_wake_case {
	bool online;
	bool otg;
	u32 type;
	bool awake;
};

static void battmgr_test_wake_matrix(struct kunit *test)
{
	/* SM8475 USB_ADAP_TYPE: stock 5.10 power_supply_usb_type, QTI from 0x80. */
	const struct battmgr_wake_case cases[] = {
		{ true, false, 0, true },	/* unknown: detection running */
		{ true, false, 1, false },	/* SDP: computer port */
		{ true, false, 2, true },	/* DCP */
		{ true, false, 3, false },	/* CDP: computer port */
		{ true, false, 4, true },	/* ACA */
		{ true, false, 5, true },	/* Type-C current */
		{ true, false, 6, true },	/* PD */
		{ true, false, 7, true },	/* PD DRP */
		{ true, false, 8, true },	/* PD PPS */
		{ true, false, 9, true },	/* Apple brick */
		{ true, false, 0x80, true },	/* HVDCP */
		{ true, false, 0x81, true },	/* HVDCP 3 */
		{ true, false, 0x82, true },	/* HVDCP 3.5 */
		{ true, false, 0x83, true },	/* floating charger */
		{ true, false, 0x84, true },	/* HVDCP 3 class B */
		{ false, false, 2, false },	/* unplugged */
		{ false, false, 6, false },
		{ true, true, 2, false },	/* OTG */
		{ true, true, 6, false },
		{ true, true, 0, false },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct battmgr_wake_case *c = &cases[i];

		KUNIT_EXPECT_EQ_MSG(test, liuqin_should_stay_awake(c->online, c->otg, c->type),
				    c->awake, "online %d otg %d type %#x",
				    c->online, c->otg, c->type);
	}
}

/* One USB property notification: the online work, run on this thread. */
static void battmgr_test_usb_event(struct kunit *test, u32 online, u32 type)
{
	struct battmgr_fake *fw = test->priv;

	fw->usb[0] = online;	/* USB_ONLINE */
	fw->usb[7] = type;	/* USB_ADAP_TYPE */
	fw->requests = 0;
	set_bit(ONLINE_USB, &fw->b->online_pending);
	qcom_battmgr_check_online(&fw->b->online_work);
}

static bool battmgr_test_held(struct qcom_battmgr *b)
{
	return b->charge_awake && b->charge_ws->active;
}

static void battmgr_test_wake_session(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;

	if (!IS_ENABLED(CONFIG_PM_SLEEP))
		kunit_skip(test, "wakeup sources need CONFIG_PM_SLEEP");
	b->charge_ws = wakeup_source_register(fw->dev, "qcom-battmgr-charge-test");
	KUNIT_ASSERT_NOT_NULL(test, b->charge_ws);

	/* Wall charger: both inputs read for this event, then held. */
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	KUNIT_ASSERT_EQ(test, fw->requests, 2);
	KUNIT_EXPECT_EQ(test, fw->log[0][0], 0x32);
	KUNIT_EXPECT_EQ(test, fw->log[0][1], 0);
	KUNIT_EXPECT_EQ(test, fw->log[1][0], 0x32);
	KUNIT_EXPECT_EQ(test, fw->log[1][1], 7);

	/* Detection settles on a computer port: released at once. */
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_FALSE(test, battmgr_test_held(b));
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	battmgr_test_usb_event(test, 1, 6);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	battmgr_test_usb_event(test, 1, 3);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);

	/* Unplug: released without reading a stale type. */
	battmgr_test_usb_event(test, 1, 8);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	battmgr_test_usb_event(test, 0, 8);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	KUNIT_EXPECT_EQ(test, fw->requests, 1);

	/* OTG: released from the role callback, kept released, rechecked after. */
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	qcom_battmgr_source_changed(b, BIT(0));
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	clear_bit(ONLINE_USB, &b->online_pending);
	qcom_battmgr_source_changed(b, 0);
	KUNIT_EXPECT_TRUE(test, test_bit(ONLINE_USB, &b->online_pending));
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));

	/* Charger service down: released from the PDR callback. */
	qcom_battmgr_pdr_notify(b, SERVREG_SERVICE_STATE_DOWN);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	WRITE_ONCE(b->service_up, true);

	/* Online unreadable: never hold on cached telemetry. */
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	fw->transport_error = -ETIMEDOUT;
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);
	fw->transport_error = 0;
	fw->fail_opcode = 0x32;
	fw->fail_property = 0;
	battmgr_test_usb_event(test, 1, 2);
	KUNIT_EXPECT_FALSE(test, b->charge_ws->active);

	/* Type unreadable on a live source: unknown, so it stays awake. */
	fw->fail_property = 7;
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_TRUE(test, battmgr_test_held(b));
	fw->fail_opcode = 0;

	KUNIT_EXPECT_GT(test, b->charge_ws->active_count, 0);
}

static int battmgr_test_set_icl(struct kunit *test, int ua)
{
	struct battmgr_fake *fw = test->priv;
	struct power_supply *psy = kunit_kzalloc(test, sizeof(*psy), GFP_KERNEL);
	union power_supply_propval val = { .intval = ua };

	KUNIT_ASSERT_NOT_NULL(test, psy);
	psy->drv_data = fw->b;
	return qcom_battmgr_sm8475_usb_set_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
						    &val);
}

static void battmgr_test_icl_set(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;

	KUNIT_EXPECT_TRUE(test, qcom_battmgr_sm8475_usb_property_is_writeable(NULL,
				POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT));
	KUNIT_EXPECT_FALSE(test, qcom_battmgr_sm8475_usb_property_is_writeable(NULL,
				POWER_SUPPLY_PROP_ONLINE));
	KUNIT_EXPECT_FALSE(test, qcom_battmgr_sm8475_usb_property_is_writeable(NULL,
				POWER_SUPPLY_PROP_CURRENT_MAX));

	/* SDP: the type is read for this request, then USB_INPUT_CURR_LIMIT set. */
	fw->usb[7] = 1;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 500000), 0);
	KUNIT_ASSERT_EQ(test, fw->requests, 2);
	KUNIT_EXPECT_EQ(test, fw->log[0][0], 0x32);
	KUNIT_EXPECT_EQ(test, fw->log[0][1], 7);
	KUNIT_EXPECT_EQ(test, fw->log[1][0], 0x33);
	KUNIT_EXPECT_EQ(test, fw->log[1][1], 5);
	KUNIT_EXPECT_EQ(test, fw->set_value, 500000);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);

	/* An explicit lower request is honoured (host-port A/B). */
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 100000), 0);
	KUNIT_EXPECT_EQ(test, fw->set_value, 100000);

	/* PD takes it too. */
	fw->usb[7] = 6;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 1500000), 0);
	KUNIT_EXPECT_EQ(test, fw->set_value, 1500000);

	/* Negative lifts the limit. */
	fw->usb[7] = 1;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, -1), 0);
	KUNIT_EXPECT_EQ(test, fw->set_value, UINT_MAX);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, UINT_MAX);

	/* 0 and 2 mA (USB disconnect, suspend) change nothing. */
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 500000), 0);
	fw->requests = 0;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 0), 0);
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 2000), 0);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);

	/* Any other source refuses, without a write. */
	for (u32 type = 0; type <= 0x84; type++) {
		if (type == 1 || type == 6 || (type > 10 && type < 0x80))
			continue;
		fw->usb[7] = type;
		fw->sets = 0;
		KUNIT_EXPECT_EQ_MSG(test, battmgr_test_set_icl(test, 500000), -EINVAL,
				    "type %#x", type);
		KUNIT_EXPECT_EQ_MSG(test, fw->sets, 0, "type %#x", type);
	}

	/* Firmware errors and an unreadable type are errors, not successes. */
	fw->usb[7] = 1;
	fw->fail_opcode = 0x33;
	fw->fail_property = 5;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 900000), -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);
	fw->fail_opcode = 0x32;
	fw->fail_property = 7;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 900000), -EREMOTEIO);
	fw->fail_opcode = 0;
	fw->transport_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 900000), -ETIMEDOUT);
	fw->transport_error = 0;
	WRITE_ONCE(b->service_up, false);
	fw->requests = 0;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 900000), -EAGAIN);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
}

static void battmgr_test_icl_default(struct kunit *test)
{
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;

	/* SDP attach: 500 mA from the kernel, no enumeration needed. */
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_ASSERT_EQ(test, fw->requests, 3);
	KUNIT_EXPECT_EQ(test, fw->log[2][0], 0x33);
	KUNIT_EXPECT_EQ(test, fw->log[2][1], 5);
	KUNIT_EXPECT_EQ(test, fw->set_value, 500000);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);

	/* Once per attach: later notifications do not rewrite it. */
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, fw->requests, 2);

	/* An explicit value stays for the attach. */
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 100000), 0);
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, fw->requests, 2);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 100000);

	/* Unplug forgets it; the next SDP gets the default, which 0 mA keeps. */
	battmgr_test_usb_event(test, 0, 0);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 0);
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, fw->set_value, 500000);
	fw->requests = 0;
	KUNIT_EXPECT_EQ(test, battmgr_test_set_icl(test, 0), 0);
	KUNIT_EXPECT_EQ(test, fw->requests, 0);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);

	/* Other sources keep the firmware's own limit; PD only on request. */
	fw->sets = 0;
	battmgr_test_usb_event(test, 1, 2);
	battmgr_test_usb_event(test, 1, 3);
	battmgr_test_usb_event(test, 1, 6);
	battmgr_test_usb_event(test, 1, 0x81);
	KUNIT_EXPECT_EQ(test, fw->sets, 0);

	/* After a charger service restart the default is set again. */
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, fw->sets, 1);
	qcom_battmgr_pdr_notify(b, SERVREG_SERVICE_STATE_DOWN);
	WRITE_ONCE(b->service_up, true);
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, fw->sets, 2);
	KUNIT_EXPECT_EQ(test, fw->set_value, 500000);

	/* A refused write is retried on the next notification. */
	battmgr_test_usb_event(test, 0, 0);
	fw->fail_opcode = 0x33;
	fw->fail_property = 5;
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 0);
	fw->fail_opcode = 0;
	battmgr_test_usb_event(test, 1, 1);
	KUNIT_EXPECT_EQ(test, b->usb_icl_ua, 500000);
}

static void battmgr_test_usb_type_values(struct kunit *test)
{
	/* Raw USB_ADAP_TYPE, then the usb_type the class can report. */
	const u32 cases[][2] = {
		{ 0, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 1, POWER_SUPPLY_USB_TYPE_SDP },
		{ 2, POWER_SUPPLY_USB_TYPE_DCP },
		{ 3, POWER_SUPPLY_USB_TYPE_CDP },
		{ 4, POWER_SUPPLY_USB_TYPE_ACA },
		{ 5, POWER_SUPPLY_USB_TYPE_C },
		{ 6, POWER_SUPPLY_USB_TYPE_PD },
		{ 7, POWER_SUPPLY_USB_TYPE_PD_DRP },
		{ 8, POWER_SUPPLY_USB_TYPE_PD_PPS },
		{ 9, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID },
		{ 0x80, POWER_SUPPLY_USB_TYPE_DCP },		/* HVDCP */
		{ 0x81, POWER_SUPPLY_USB_TYPE_DCP },		/* HVDCP 3 */
		{ 0x82, POWER_SUPPLY_USB_TYPE_DCP },		/* HVDCP 3.5 */
		{ 0x83, POWER_SUPPLY_USB_TYPE_UNKNOWN },	/* floating */
		{ 0x84, POWER_SUPPLY_USB_TYPE_DCP },		/* HVDCP 3 class B */
		{ 10, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x85, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0xffffffff, POWER_SUPPLY_USB_TYPE_UNKNOWN },
	};
	struct battmgr_fake *fw = test->priv;
	struct power_supply *psy = kunit_kzalloc(test, sizeof(*psy), GFP_KERNEL);
	union power_supply_propval val;

	KUNIT_ASSERT_NOT_NULL(test, psy);
	psy->drv_data = fw->b;
	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		fw->usb[7] = cases[i][0];
		val.intval = INT_MIN;
		KUNIT_EXPECT_EQ(test, qcom_battmgr_usb_get_property(psy, POWER_SUPPLY_PROP_USB_TYPE,
								    &val), 0);
		KUNIT_EXPECT_EQ(test, fw->last_property, 7);
		KUNIT_EXPECT_EQ_MSG(test, val.intval, cases[i][1], "raw %#x", cases[i][0]);
	}
}

static void battmgr_test_unregister_psy(void *psy)
{
	power_supply_unregister(psy);
}

static const char *battmgr_test_env(struct kobj_uevent_env *env, const char *key)
{
	for (int i = 0; i < env->envp_idx; i++)
		if (str_has_prefix(env->envp[i], key))
			return env->envp[i] + strlen(key);
	return NULL;
}

static void battmgr_test_usb_type_sysfs(struct kunit *test)
{
	const struct {
		u32 raw;
		const char *shown;
	} cases[] = {
		{ 0x80, "[DCP]" }, { 0x81, "[DCP]" }, { 0x82, "[DCP]" },
		{ 0x84, "[DCP]" }, { 0x83, "[Unknown]" }, { 1, "[SDP]" },
	};
	struct battmgr_fake *fw = test->priv;
	struct qcom_battmgr *b = fw->b;
	struct power_supply_config cfg = { .drv_data = b, .no_wakeup_source = true };
	struct device_attribute *usb_type = NULL;
	struct kobj_uevent_env *env;
	struct power_supply *psy;
	struct attribute **attr;
	const char *value;
	char *buf;

	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	env = kunit_kzalloc(test, sizeof(*env), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_NOT_NULL(test, env);

	/* Registration uevents run on other threads: keep them off the transport. */
	WRITE_ONCE(b->service_up, false);
	psy = power_supply_register(fw->dev, &sm8475_usb_psy_desc, &cfg);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, psy);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, battmgr_test_unregister_psy, psy), 0);
	flush_delayed_work(&psy->deferred_register_work);
	flush_work(&psy->changed_work);
	WRITE_ONCE(b->service_up, true);

	for (attr = power_supply_attr_groups[0]->attrs; *attr; attr++)
		if (!strcmp((*attr)->name, "usb_type"))
			usb_type = container_of(*attr, struct device_attribute, attr);
	KUNIT_ASSERT_NOT_NULL(test, usb_type);

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		fw->usb[7] = cases[i].raw;

		/* The usb_type attribute itself, as userspace reads it. */
		memset(buf, 0, PAGE_SIZE);
		KUNIT_EXPECT_GT_MSG(test, usb_type->show(&psy->dev, usb_type, buf), 0,
				    "raw %#x", cases[i].raw);
		KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(buf, cases[i].shown), "raw %#x: %s",
					  cases[i].raw, buf);

		/* The change uevent carries it. */
		memset(env, 0, sizeof(*env));
		KUNIT_EXPECT_EQ(test, power_supply_uevent(&psy->dev, env), 0);
		value = battmgr_test_env(env, "POWER_SUPPLY_USB_TYPE=");
		KUNIT_EXPECT_NOT_NULL_MSG(test, value, "raw %#x", cases[i].raw);
		if (value)
			KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(value, cases[i].shown),
						  "raw %#x: %s", cases[i].raw, value);
		KUNIT_EXPECT_NOT_NULL(test, battmgr_test_env(env, "POWER_SUPPLY_ONLINE="));
	}
}

static struct kunit_case qcom_battmgr_test_cases[] = {
	KUNIT_CASE(battmgr_test_property_ids),
	KUNIT_CASE(battmgr_test_refresh),
	KUNIT_CASE(battmgr_test_refresh_on_resume),
	KUNIT_CASE(battmgr_test_xm_notification),
	KUNIT_CASE(battmgr_test_units),
	KUNIT_CASE(battmgr_test_wake_matrix),
	KUNIT_CASE(battmgr_test_wake_session),
	KUNIT_CASE(battmgr_test_icl_set),
	KUNIT_CASE(battmgr_test_icl_default),
	KUNIT_CASE(battmgr_test_usb_type_values),
	KUNIT_CASE(battmgr_test_usb_type_sysfs),
	{}
};

static struct kunit_suite qcom_battmgr_test_suite = {
	.name = "qcom_battmgr_sm8475",
	.init = battmgr_test_init,
	.test_cases = qcom_battmgr_test_cases,
};
kunit_test_suite(qcom_battmgr_test_suite);
