// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Linaro Ltd
 */
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/soc/qcom/pdr.h>
#include <drm/bridge/aux-bridge.h>

#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>

#include <linux/soc/qcom/pmic_glink.h>

#define PMIC_GLINK_MAX_PORTS	3

#define USBC_SC8180X_NOTIFY_IND	0x13
#define USBC_CMD_WRITE_REQ      0x15
#define USBC_NOTIFY_IND		0x16

#define ALTMODE_PAN_EN		0x10
#define ALTMODE_PAN_ACK		0x11

#define PMIC_GLINK_LIUQIN_DP_SID	0xff01

struct usbc_write_req {
	struct pmic_glink_hdr   hdr;
	__le32 cmd;
	__le32 arg;
	__le32 reserved;
};

#define NOTIFY_PAYLOAD_SIZE 16
struct usbc_notify {
	struct pmic_glink_hdr hdr;
	char payload[NOTIFY_PAYLOAD_SIZE];
	u32 reserved;
};

struct usbc_sc8180x_notify {
	struct pmic_glink_hdr hdr;
	__le32 notification;
	__le32 reserved[2];
};

enum pmic_glink_altmode_pin_assignment {
	DPAM_HPD_OUT,
	DPAM_HPD_A,
	DPAM_HPD_B,
	DPAM_HPD_C,
	DPAM_HPD_D,
	DPAM_HPD_E,
	DPAM_HPD_F,
};

struct pmic_glink_altmode;

#define work_to_altmode_port(w) container_of((w), struct pmic_glink_altmode_port, work)

struct pmic_glink_altmode_port {
	struct pmic_glink_altmode *altmode;
	unsigned int index;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;
	struct typec_mux_state state;
	struct typec_retimer *typec_retimer;
	struct typec_retimer_state retimer_state;
	struct typec_altmode dp_alt;

	struct work_struct work;

	struct auxiliary_device *bridge;

	enum typec_orientation orientation;
	enum drm_connector_status conn_status;
	u16 svid;
	u8 dp_data;
	u8 mode;
	u8 hpd_state;
	u8 hpd_irq;
};

#define work_to_altmode(w) container_of((w), struct pmic_glink_altmode, enable_work)

struct pmic_glink_altmode {
	struct device *dev;

	unsigned int owner_id;

	/* To synchronize WRITE_REQ acks */
	struct mutex lock;

	struct completion pan_ack;
	struct pmic_glink_client *client;

	struct work_struct enable_work;

	struct pmic_glink_altmode_port ports[PMIC_GLINK_MAX_PORTS];
};

static int pmic_glink_altmode_request(struct pmic_glink_altmode *altmode, u32 cmd, u32 arg)
{
	struct usbc_write_req req = {};
	unsigned long left;
	int ret;

	/*
	 * The USBC_CMD_WRITE_REQ ack doesn't identify the request, so wait for
	 * one ack at a time.
	 */
	guard(mutex)(&altmode->lock);

	req.hdr.owner = cpu_to_le32(altmode->owner_id);
	req.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP);
	req.hdr.opcode = cpu_to_le32(USBC_CMD_WRITE_REQ);
	req.cmd = cpu_to_le32(cmd);
	req.arg = cpu_to_le32(arg);

	ret = pmic_glink_send(altmode->client, &req, sizeof(req));
	if (ret) {
		dev_err(altmode->dev, "failed to send altmode request: %#x (%d)\n", cmd, ret);
		return ret;
	}

	left = wait_for_completion_timeout(&altmode->pan_ack, 5 * HZ);
	if (!left) {
		dev_err(altmode->dev, "timeout waiting for altmode request ack for: %#x\n", cmd);
		return -ETIMEDOUT;
	}

	return 0;
}

static void pmic_glink_altmode_enable_dp(struct pmic_glink_altmode *altmode,
					 struct pmic_glink_altmode_port *port,
					 u8 mode, bool hpd_state,
					 bool hpd_irq)
{
	struct typec_displayport_data dp_data = {};
	int ret;

	dp_data.status = DP_STATUS_ENABLED;
	if (hpd_state)
		dp_data.status |= DP_STATUS_HPD_STATE;
	if (hpd_irq)
		dp_data.status |= DP_STATUS_IRQ_HPD;
	dp_data.conf = DP_CONF_SET_PIN_ASSIGN(mode);

	port->state.alt = &port->dp_alt;
	port->state.data = &dp_data;
	port->state.mode = TYPEC_MODAL_STATE(mode);

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to DP: %d\n", ret);

	port->retimer_state.alt = &port->dp_alt;
	port->retimer_state.data = &dp_data;
	port->retimer_state.mode = TYPEC_MODAL_STATE(mode);

	ret = typec_retimer_set(port->typec_retimer, &port->retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to DP: %d\n", ret);
}

static void pmic_glink_altmode_enable_usb(struct pmic_glink_altmode *altmode,
					  struct pmic_glink_altmode_port *port)
{
	int ret;

	port->state.alt = NULL;
	port->state.data = NULL;
	port->state.mode = TYPEC_STATE_USB;

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to USB: %d\n", ret);

	port->retimer_state.alt = NULL;
	port->retimer_state.data = NULL;
	port->retimer_state.mode = TYPEC_STATE_USB;

	ret = typec_retimer_set(port->typec_retimer, &port->retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to USB: %d\n", ret);
}

static void pmic_glink_altmode_safe(struct pmic_glink_altmode *altmode,
				    struct pmic_glink_altmode_port *port)
{
	int ret;

	port->state.alt = NULL;
	port->state.data = NULL;
	port->state.mode = TYPEC_STATE_SAFE;

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to safe mode: %d\n", ret);

	port->retimer_state.alt = NULL;
	port->retimer_state.data = NULL;
	port->retimer_state.mode = TYPEC_STATE_SAFE;

	ret = typec_retimer_set(port->typec_retimer, &port->retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to USB: %d\n", ret);
}

/*
 * The liuqin PMIC charger firmware identifies the DisplayPort alt mode with a
 * vendor svid (0xff01) in the notification opcode instead of the USB-IF
 * DisplayPort SVID (0x040e); the vendor msm_drm registers its DP altmode
 * client with id 0xff01.  Accept both so the worker routes DP notifications
 * to the DP path rather than misclassifying them as USB.
 */
static bool pmic_glink_altmode_is_dp(u16 svid)
{
	return svid == USB_TYPEC_DP_SID || svid == PMIC_GLINK_LIUQIN_DP_SID;
}

static void pmic_glink_altmode_worker(struct work_struct *work)
{
	struct pmic_glink_altmode_port *alt_port = work_to_altmode_port(work);
	struct pmic_glink_altmode *altmode = alt_port->altmode;
	enum drm_connector_status conn_status;

	typec_switch_set(alt_port->typec_switch, alt_port->orientation);

	if (pmic_glink_altmode_is_dp(alt_port->svid)) {
		if (alt_port->mode == 0xff) {
			pmic_glink_altmode_safe(altmode, alt_port);
		} else {
			pmic_glink_altmode_enable_dp(altmode, alt_port,
						     alt_port->mode,
						     alt_port->hpd_state,
						     alt_port->hpd_irq);
		}

		/*
		 * Report the connector as connected only once the sink asserts HPD
		 * (hpd_state in the notification), i.e. once the dock's DP bridge is
		 * actually up.  Driving it connected regardless makes the DP driver
		 * probe a bridge that is not ready yet, and that first AUX
		 * transaction gets no reply -- the DPCD read then fails with
		 * -ETIMEDOUT and the display never comes up.  This is why DP worked
		 * only on the plugs whose notification carried hpd_state=1.
		 *
		 * An earlier version dropped this gate because it deadlocked: the
		 * bridge asserts HPD only after the source powers the DP PHY, and
		 * mainline powers the PHY only after the DPCD read.
		 * msm_dp_ctrl_phy_init() now brings the PHY up first (as the
		 * vendor's dp_ctrl_host_init() does), which breaks that cycle.
		 *
		 * Notify *only* on a connect/disconnect transition.  The sink
		 * re-sends its notification whenever HPD-IRQ toggles, and every
		 * notify makes the DP driver run a full plug cycle, whose
		 * msm_dp_display_host_init() resets the DPU/MDSS.  Doing that every
		 * couple of seconds resets the display controller underneath the
		 * live internal DSI panel and blanks the screen.
		 */
		conn_status = (alt_port->mode == 0xff || !alt_port->hpd_state) ?
			connector_status_disconnected : connector_status_connected;

		if (alt_port->conn_status != conn_status) {
			alt_port->conn_status = conn_status;

			dev_info(altmode->dev,
				 "dp hpd bridge notify: svid=%#06x mode=%u orient=%d hpd_state=%u -> %s\n",
				 alt_port->svid, alt_port->mode, (int)alt_port->orientation,
				 alt_port->hpd_state,
				 conn_status == connector_status_connected ? "connected" : "disconnected");

			drm_aux_hpd_bridge_notify(&alt_port->bridge->dev, conn_status);
		}
	} else {
		pmic_glink_altmode_enable_usb(altmode, alt_port);
	}

	pmic_glink_altmode_request(altmode, ALTMODE_PAN_ACK, alt_port->index);
}

static enum typec_orientation pmic_glink_altmode_orientation(unsigned int orientation)
{
	if (orientation == 0)
		return TYPEC_ORIENTATION_NORMAL;
	else if (orientation == 1)
		return TYPEC_ORIENTATION_REVERSE;
	else
		return TYPEC_ORIENTATION_NONE;
}

/*
 * The liuqin PMIC charger firmware reports the cable orientation as the
 * typec_orientation enum value (1 = normal, 2 = reverse) instead of the 0/1
 * encoding the generic sc8280xp notification uses.  Decoding 1 as "reverse"
 * inverted the SBU and port-select state on every plug, so the DP AUX
 * channel was set up wrong for *both* physical cable orientations and the
 * first DPCD read always timed out.  Set liuqin_orientation_enum=0 to fall
 * back to the generic encoding.
 */
static bool liuqin_orientation_enum = true;
module_param(liuqin_orientation_enum, bool, 0644);
MODULE_PARM_DESC(liuqin_orientation_enum,
		 "liuqin PMIC firmware reports orientation as typec_orientation enum values");

static enum typec_orientation
pmic_glink_altmode_orientation_liuqin(unsigned int orientation)
{
	switch (orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		return TYPEC_ORIENTATION_NORMAL;
	case TYPEC_ORIENTATION_REVERSE:
		return TYPEC_ORIENTATION_REVERSE;
	default:
		return TYPEC_ORIENTATION_NONE;
	}
}

#define SC8180X_PORT_MASK		0x000000ff
#define SC8180X_ORIENTATION_MASK	0x0000ff00
#define SC8180X_MUX_MASK		0x00ff0000
#define SC8180X_MODE_MASK		0x3f000000
#define SC8180X_HPD_STATE_MASK		0x40000000
#define SC8180X_HPD_IRQ_MASK		0x80000000

static void pmic_glink_altmode_sc8180xp_notify(struct pmic_glink_altmode *altmode,
					       const void *data, size_t len)
{
	struct pmic_glink_altmode_port *alt_port;
	const struct usbc_sc8180x_notify *msg;
	u32 notification;
	u8 orientation;
	u8 hpd_state;
	u8 hpd_irq;
	u16 svid;
	u8 port;
	u8 mode;
	u8 mux;

	if (len != sizeof(*msg)) {
		dev_warn(altmode->dev, "invalid length of USBC_NOTIFY indication: %zd\n", len);
		return;
	}

	msg = data;
	notification = le32_to_cpu(msg->notification);
	port = FIELD_GET(SC8180X_PORT_MASK, notification);
	orientation = FIELD_GET(SC8180X_ORIENTATION_MASK, notification);
	mux = FIELD_GET(SC8180X_MUX_MASK, notification);
	mode = FIELD_GET(SC8180X_MODE_MASK, notification);
	hpd_state = FIELD_GET(SC8180X_HPD_STATE_MASK, notification);
	hpd_irq = FIELD_GET(SC8180X_HPD_IRQ_MASK, notification);

	svid = mux == 2 ? USB_TYPEC_DP_SID : 0;

	if (port >= ARRAY_SIZE(altmode->ports) || !altmode->ports[port].altmode) {
		dev_dbg(altmode->dev, "notification on undefined port %d\n", port);
		return;
	}

	alt_port = &altmode->ports[port];
	alt_port->orientation = pmic_glink_altmode_orientation(orientation);
	alt_port->svid = svid;
	alt_port->mode = mode;
	alt_port->hpd_state = hpd_state;
	alt_port->hpd_irq = hpd_irq;
	schedule_work(&alt_port->work);
}

#define SC8280XP_DPAM_MASK	0x3f
#define SC8280XP_HPD_STATE_MASK BIT(6)
#define SC8280XP_HPD_IRQ_MASK	BIT(7)

static void pmic_glink_altmode_sc8280xp_notify(struct pmic_glink_altmode *altmode,
					       u16 svid, const void *data, size_t len)
{
	struct pmic_glink_altmode_port *alt_port;
	const struct usbc_notify *notify;
	u8 orientation;
	u8 hpd_state;
	u8 hpd_irq;
	u8 mode;
	u8 port;

	if (len != sizeof(*notify)) {
		dev_warn(altmode->dev, "invalid length USBC_NOTIFY_IND: %zd\n",
			 len);
		return;
	}

	notify = data;

	port = notify->payload[0];
	orientation = notify->payload[1];
	mode = FIELD_GET(SC8280XP_DPAM_MASK, notify->payload[8]) - DPAM_HPD_A;
	hpd_state = FIELD_GET(SC8280XP_HPD_STATE_MASK, notify->payload[8]);
	hpd_irq = FIELD_GET(SC8280XP_HPD_IRQ_MASK, notify->payload[8]);

	if (port >= ARRAY_SIZE(altmode->ports) || !altmode->ports[port].altmode) {
		dev_dbg(altmode->dev, "notification on undefined port %d\n", port);
		return;
	}

	alt_port = &altmode->ports[port];
	alt_port->orientation = liuqin_orientation_enum ?
		pmic_glink_altmode_orientation_liuqin(orientation) :
		pmic_glink_altmode_orientation(orientation);
	alt_port->svid = svid;
	alt_port->mode = mode;
	alt_port->hpd_state = hpd_state;
	alt_port->hpd_irq = hpd_irq;
	dev_info(altmode->dev,
		 "sc8280xp notify: svid=%#06x port=%u orient=%u(%s) dpam=0x%02x mode=%u hpd_state=%u hpd_irq=%u\n",
		 svid, port, orientation,
		 alt_port->orientation == TYPEC_ORIENTATION_NORMAL ? "normal" :
		 alt_port->orientation == TYPEC_ORIENTATION_REVERSE ? "reverse" : "none",
		 notify->payload[8], mode, hpd_state, hpd_irq);
	schedule_work(&alt_port->work);
}

static void pmic_glink_altmode_callback(const void *data, size_t len, void *priv)
{
	struct pmic_glink_altmode *altmode = priv;
	const struct pmic_glink_hdr *hdr = data;
	u16 opcode;
	u16 svid;

	opcode = le32_to_cpu(hdr->opcode) & 0xff;
	svid = le32_to_cpu(hdr->opcode) >> 16;

	switch (opcode) {
	case USBC_CMD_WRITE_REQ:
		complete(&altmode->pan_ack);
		break;
	case USBC_NOTIFY_IND:
		pmic_glink_altmode_sc8280xp_notify(altmode, svid, data, len);
		break;
	case USBC_SC8180X_NOTIFY_IND:
		pmic_glink_altmode_sc8180xp_notify(altmode, data, len);
		break;
	}
}

static void pmic_glink_altmode_put_retimer(void *data)
{
	typec_retimer_put(data);
}

static void pmic_glink_altmode_put_mux(void *data)
{
	typec_mux_put(data);
}

static void pmic_glink_altmode_put_switch(void *data)
{
	typec_switch_put(data);
}

static void pmic_glink_altmode_enable_worker(struct work_struct *work)
{
	struct pmic_glink_altmode *altmode = work_to_altmode(work);
	int ret;

	ret = pmic_glink_altmode_request(altmode, ALTMODE_PAN_EN, 0);
	if (ret)
		dev_err(altmode->dev, "failed to request altmode notifications: %d\n", ret);
}

static void pmic_glink_altmode_pdr_notify(void *priv, int state)
{
	struct pmic_glink_altmode *altmode = priv;

	if (state == SERVREG_SERVICE_STATE_UP)
		schedule_work(&altmode->enable_work);
}

static const struct of_device_id pmic_glink_altmode_of_quirks[] = {
	{ .compatible = "qcom,sc8180x-pmic-glink", .data = (void *)PMIC_GLINK_OWNER_USBC },
	{}
};

static int pmic_glink_altmode_probe(struct auxiliary_device *adev,
				    const struct auxiliary_device_id *id)
{
	struct pmic_glink_altmode_port *alt_port;
	struct pmic_glink_altmode *altmode;
	const struct of_device_id *match;
	struct fwnode_handle *fwnode;
	struct device *dev = &adev->dev;
	u32 port;
	int ret;

	altmode = devm_kzalloc(dev, sizeof(*altmode), GFP_KERNEL);
	if (!altmode)
		return -ENOMEM;

	altmode->dev = dev;

	match = of_match_device(pmic_glink_altmode_of_quirks, dev->parent);
	if (match)
		altmode->owner_id = (unsigned long)match->data;
	else
		altmode->owner_id = PMIC_GLINK_OWNER_USBC_PAN;

	INIT_WORK(&altmode->enable_work, pmic_glink_altmode_enable_worker);
	init_completion(&altmode->pan_ack);
	mutex_init(&altmode->lock);

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_u32(fwnode, "reg", &port);
		if (ret < 0) {
			dev_err(dev, "missing reg property of %pOFn\n", fwnode);
			fwnode_handle_put(fwnode);
			return ret;
		}

		if (port >= ARRAY_SIZE(altmode->ports)) {
			dev_warn(dev, "invalid connector number, ignoring\n");
			continue;
		}

		if (altmode->ports[port].altmode) {
			dev_err(dev, "multiple connector definition for port %u\n", port);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		alt_port = &altmode->ports[port];
		alt_port->altmode = altmode;
		alt_port->index = port;
		INIT_WORK(&alt_port->work, pmic_glink_altmode_worker);

		alt_port->bridge = devm_drm_dp_hpd_bridge_alloc(dev, to_of_node(fwnode));
		if (IS_ERR(alt_port->bridge)) {
			fwnode_handle_put(fwnode);
			return PTR_ERR(alt_port->bridge);
		}

		alt_port->dp_alt.svid = USB_TYPEC_DP_SID;
		alt_port->dp_alt.mode = USB_TYPEC_DP_MODE;
		alt_port->dp_alt.active = 1;

		alt_port->typec_mux = fwnode_typec_mux_get(fwnode);
		if (IS_ERR(alt_port->typec_mux)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_mux),
					     "failed to acquire mode-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, pmic_glink_altmode_put_mux,
					       alt_port->typec_mux);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		alt_port->typec_retimer = fwnode_typec_retimer_get(fwnode);
		if (IS_ERR(alt_port->typec_retimer)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_retimer),
					     "failed to acquire retimer-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, pmic_glink_altmode_put_retimer,
					       alt_port->typec_retimer);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		alt_port->typec_switch = fwnode_typec_switch_get(fwnode);
		if (IS_ERR(alt_port->typec_switch)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_switch),
					     "failed to acquire orientation-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, pmic_glink_altmode_put_switch,
					       alt_port->typec_switch);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	for (port = 0; port < ARRAY_SIZE(altmode->ports); port++) {
		alt_port = &altmode->ports[port];
		if (!alt_port->bridge)
			continue;

		ret = devm_drm_dp_hpd_bridge_add(dev, alt_port->bridge);
		if (ret)
			return ret;
	}

	altmode->client = devm_pmic_glink_client_alloc(dev,
						       altmode->owner_id,
						       pmic_glink_altmode_callback,
						       pmic_glink_altmode_pdr_notify,
						       altmode);
	if (IS_ERR(altmode->client))
		return PTR_ERR(altmode->client);

	pmic_glink_client_register(altmode->client);

	return 0;
}

static const struct auxiliary_device_id pmic_glink_altmode_id_table[] = {
	{ .name = "pmic_glink.altmode", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, pmic_glink_altmode_id_table);

static struct auxiliary_driver pmic_glink_altmode_driver = {
	.name = "pmic_glink_altmode",
	.probe = pmic_glink_altmode_probe,
	.id_table = pmic_glink_altmode_id_table,
};

module_auxiliary_driver(pmic_glink_altmode_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK Altmode driver");
MODULE_LICENSE("GPL");
