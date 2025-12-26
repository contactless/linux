// SPDX-License-Identifier: GPL-2.0-only
/*
 * Holtek HT42B416 UART-to-CAN bridge driver
 *
 * The controller is configured with ASCII commands terminated by carriage
 * return characters. Payload bytes are transferred in binary form.
 *
 * Copyright (C) 2024 Wiren Board
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/skb.h>

#define HT42B416_MAX_DATA_LEN		8
#define HT42B416_MAX_CMD_LEN		16
#define HT42B416_MAX_FRAME_LEN		(1 + 4 + 1 + HT42B416_MAX_DATA_LEN + 1)
#define HT42B416_RX_BUF_LEN		64

#define HT42B416_CMD_TIMEOUT_MS		250
#define HT42B416_POWERUP_DELAY_US	200
#define HT42B416_TX_TIMEOUT		(2 * HZ)

static bool ht42b416_debug;
module_param_named(debug, ht42b416_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable verbose UART logging");

enum ht42b416_wait_ack {
	HT42B416_WAIT_NONE,
	HT42B416_WAIT_CR,
};

enum ht42b416_tx_expect {
	HT42B416_TX_NONE,
	HT42B416_TX_EXPECT_Z,
	HT42B416_TX_EXPECT_CAP_Z,
};

enum ht42b416_status {
	HT42B416_STATUS_TX_OK		= 6,
	HT42B416_STATUS_BUS_OFF		= -8,
	HT42B416_STATUS_ERR_CRC		= -10,
	HT42B416_STATUS_ERR_BIT1	= -11,
	HT42B416_STATUS_ERR_BIT0	= -12,
	HT42B416_STATUS_ERR_ACK		= -13,
	HT42B416_STATUS_ERR_FORM	= -14,
	HT42B416_STATUS_ERR_STUFF	= -15,
	HT42B416_STATUS_ERR_UNKNOWN	= -16,
};

struct ht42b416_priv {
	struct can_priv can;
	struct serdev_device *serdev;
	struct gpio_desc *enable_gpiod;

	struct work_struct tx_work;
	spinlock_t tx_lock;	/* protects below fields */
	u8 tx_buf[HT42B416_MAX_FRAME_LEN];
	size_t tx_len;
	size_t tx_pos;
	enum ht42b416_tx_expect tx_expect;
	u8 tx_dlc;
	bool tx_busy;

	u8 rx_buf[HT42B416_RX_BUF_LEN];
	size_t rx_len;

	struct completion ack_complete;
	enum ht42b416_wait_ack wait_ack;
	struct mutex cmd_lock;	/* serialises configuration commands */

	u32 uart_baud;
	bool running;
};

static const u32 ht42b416_bitrate_const[] = {
	  5000,   10000,   20000,   50000,   100000,
	125000,  250000,  500000,  800000, 1000000,
};

static const u8 ht42b416_filter_all_std[] = { 'm', 0x00, 0x00 };
static const u8 ht42b416_code_all_std[]   = { 'M', 0x00, 0x00 };
static const u8 ht42b416_filter_all_ext[] = { 'm', 0x00, 0x00, 0x00, 0x00 };
static const u8 ht42b416_code_all_ext[]   = { 'M', 0x00, 0x00, 0x00, 0x00 };

static void ht42b416_log_uart(struct device *dev, const char *prefix,
			      const u8 *buf, size_t len)
{
	char hex[HT42B416_RX_BUF_LEN * 3 + 1];
	size_t i, pos = 0;

	for (i = 0; i < len && pos + 3 < sizeof(hex); i++)
		pos += scnprintf(hex + pos, sizeof(hex) - pos,
				 "%02x ", buf[i]);

	if (pos)
		hex[pos - 1] = '\0';
	else
		hex[0] = '\0';

	dev_info(dev, "%s[%zu]: %s\n", prefix, len, hex);
}

static int ht42b416_write_raw(struct ht42b416_priv *priv,
			      const u8 *buf, size_t len)
{
	int ret;

	ret = serdev_device_write(priv->serdev, buf, len,
				  msecs_to_jiffies(HT42B416_CMD_TIMEOUT_MS));
	if (ret < 0)
		return ret;
	if (ret != len)
		return -ETIMEDOUT;

	return 0;
}

static int ht42b416_send_cmd_locked(struct ht42b416_priv *priv,
				    const u8 *payload, size_t len,
				    enum ht42b416_wait_ack wait)
{
	u8 buffer[HT42B416_MAX_CMD_LEN];
	int ret;

	if (!len || len >= HT42B416_MAX_CMD_LEN)
		return -EINVAL;

	memcpy(buffer, payload, len);
	buffer[len++] = '\r';

	if (wait != HT42B416_WAIT_NONE) {
		reinit_completion(&priv->ack_complete);
		WRITE_ONCE(priv->wait_ack, wait);
	}

	if (ht42b416_debug)
		ht42b416_log_uart(&priv->serdev->dev, "UART TX cmd ", buffer, len);

	ret = ht42b416_write_raw(priv, buffer, len);
	if (ret || wait == HT42B416_WAIT_NONE)
		goto out;

	if (!wait_for_completion_timeout(&priv->ack_complete,
					 msecs_to_jiffies(HT42B416_CMD_TIMEOUT_MS)))
		ret = -ETIMEDOUT;

out:
	if (ret && wait != HT42B416_WAIT_NONE)
		WRITE_ONCE(priv->wait_ack, HT42B416_WAIT_NONE);

	return ret;
}

static int ht42b416_send_cmd(struct ht42b416_priv *priv,
			     const u8 *payload, size_t len,
			     enum ht42b416_wait_ack wait)
{
	int ret;

	mutex_lock(&priv->cmd_lock);
	ret = ht42b416_send_cmd_locked(priv, payload, len, wait);
	mutex_unlock(&priv->cmd_lock);

	return ret;
}

static int ht42b416_bitrate_to_code(u32 bitrate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ht42b416_bitrate_const); i++)
		if (ht42b416_bitrate_const[i] == bitrate)
			return i;

	return -EINVAL;
}

static void ht42b416_abort_tx(struct ht42b416_priv *priv)
{
	struct net_device *ndev = priv->can.dev;
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_len = 0;
	priv->tx_pos = 0;
	priv->tx_expect = HT42B416_TX_NONE;
	priv->tx_busy = false;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	can_free_echo_skb(ndev, 0, NULL);
	netif_wake_queue(ndev);
}

static const struct ethtool_ops ht42b416_ethtool_ops = {
	.get_ts_info = ethtool_op_get_ts_info,
};

static int ht42b416_hw_stop(struct ht42b416_priv *priv)
{
	static const u8 close_cmd[] = { 'C' };

	if (!priv->running)
		goto disable_gpio;

	netif_stop_queue(priv->can.dev);
	ht42b416_abort_tx(priv);

	ht42b416_send_cmd(priv, close_cmd, sizeof(close_cmd),
			  HT42B416_WAIT_CR);
	priv->running = false;

disable_gpio:
	if (priv->enable_gpiod)
		gpiod_set_value_cansleep(priv->enable_gpiod, 0);

	priv->can.state = CAN_STATE_STOPPED;

	return 0;
}

static int ht42b416_hw_start(struct ht42b416_priv *priv)
{
	struct net_device *ndev = priv->can.dev;
	u8 cmd[5];
	int ret, code;
	u8 open_cmd;

	if (priv->running)
		return 0;

	if (priv->enable_gpiod) {
		gpiod_set_value_cansleep(priv->enable_gpiod, 1);
		usleep_range(HT42B416_POWERUP_DELAY_US,
			     HT42B416_POWERUP_DELAY_US + 200);
	}

	priv->rx_len = 0;

	/* Close device to ensure clean state */
	cmd[0] = 'C';
	ret = ht42b416_send_cmd(priv, cmd, 1, HT42B416_WAIT_CR);
	if (ret)
		return ret;

	code = ht42b416_bitrate_to_code(priv->can.bittiming.bitrate);
	if (code < 0)
		return code;

	cmd[0] = 'S';
	cmd[1] = code;
	if (ht42b416_debug)
		dev_info(&priv->serdev->dev, "CAN bitrate %u -> S 0x%02x\n",
			 priv->can.bittiming.bitrate, code);
	ret = ht42b416_send_cmd(priv, cmd, 2, HT42B416_WAIT_CR);
	if (ret)
		return ret;

	ret = ht42b416_send_cmd(priv, ht42b416_filter_all_std,
				sizeof(ht42b416_filter_all_std),
				HT42B416_WAIT_CR);
	if (ret)
		return ret;

	ret = ht42b416_send_cmd(priv, ht42b416_code_all_std,
				sizeof(ht42b416_code_all_std),
				HT42B416_WAIT_CR);
	if (ret)
		return ret;

	ret = ht42b416_send_cmd(priv, ht42b416_filter_all_ext,
				sizeof(ht42b416_filter_all_ext),
				HT42B416_WAIT_CR);
	if (ret)
		return ret;

	ret = ht42b416_send_cmd(priv, ht42b416_code_all_ext,
				sizeof(ht42b416_code_all_ext),
				HT42B416_WAIT_CR);
	if (ret)
		return ret;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		open_cmd = 'l';
	else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		open_cmd = 'L';
	else
		open_cmd = 'O';

	ret = ht42b416_send_cmd(priv, &open_cmd, 1, HT42B416_WAIT_CR);
	if (ret)
		return ret;

	priv->tx_pos = 0;
	priv->tx_len = 0;
	priv->tx_expect = HT42B416_TX_NONE;
	priv->tx_busy = false;
	priv->running = true;
	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_wake_queue(ndev);

	return 0;
}

static int ht42b416_tx_kick_locked(struct ht42b416_priv *priv)
{
	while (priv->tx_pos < priv->tx_len) {
		int written;

		written = serdev_device_write_buf(priv->serdev,
						  priv->tx_buf + priv->tx_pos,
						  priv->tx_len - priv->tx_pos);
		if (written < 0) {
			netdev_err(priv->can.dev,
				   "unable to push UART data: %d\n", written);
			priv->tx_pos = priv->tx_len;
			return written;
		}

		if (!written)
			break;

		priv->tx_pos += written;
	}

	return 0;
}

static void ht42b416_tx_work(struct work_struct *work)
{
	struct ht42b416_priv *priv =
		container_of(work, struct ht42b416_priv, tx_work);
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (priv->tx_pos < priv->tx_len)
		ht42b416_tx_kick_locked(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

static void ht42b416_tx_wakeup(struct serdev_device *serdev)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);

	serdev_device_write_wakeup(serdev);
	schedule_work(&priv->tx_work);
}

static void ht42b416_finish_tx(struct ht42b416_priv *priv, u8 ack_byte)
{
	struct net_device *ndev = priv->can.dev;
	unsigned long flags;
	unsigned int tx_bytes;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (!priv->tx_busy || priv->tx_pos < priv->tx_len) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return;
	}

	if ((ack_byte != 'z' || priv->tx_expect != HT42B416_TX_EXPECT_Z) &&
	    (ack_byte != 'Z' || priv->tx_expect != HT42B416_TX_EXPECT_CAP_Z))
		netdev_warn(ndev, "unexpected TX ack 0x%02x\n", ack_byte);

	priv->tx_busy = false;
	priv->tx_expect = HT42B416_TX_NONE;
	priv->tx_len = 0;
	priv->tx_pos = 0;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	tx_bytes = can_get_echo_skb(ndev, 0, NULL);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += tx_bytes;

	netif_wake_queue(ndev);
}

static void ht42b416_handle_status(struct ht42b416_priv *priv, s8 status)
{
	struct net_device *ndev = priv->can.dev;
	struct sk_buff *skb;
	struct can_frame *cf;

	switch (status) {
	case HT42B416_STATUS_TX_OK:
	case HT42B416_STATUS_ERR_UNKNOWN:
		return;
	case HT42B416_STATUS_BUS_OFF:
		if (priv->can.state != CAN_STATE_BUS_OFF) {
			priv->can.can_stats.bus_off++;
			priv->running = false;
			can_bus_off(ndev);
		}
		return;
	default:
		break;
	}

	skb = alloc_can_err_skb(ndev, &cf);
	if (!skb) {
		ndev->stats.rx_dropped++;
		return;
	}

	memset(cf->data, 0, CAN_ERR_DLC);
	cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

	switch (status) {
	case HT42B416_STATUS_ERR_CRC:
		cf->data[2] |= CAN_ERR_PROT_BIT;
		cf->data[3] = CAN_ERR_PROT_LOC_CRC_SEQ;
		break;
	case HT42B416_STATUS_ERR_BIT1:
		cf->data[2] |= CAN_ERR_PROT_BIT1;
		break;
	case HT42B416_STATUS_ERR_BIT0:
		cf->data[2] |= CAN_ERR_PROT_BIT0;
		break;
	case HT42B416_STATUS_ERR_ACK:
		cf->can_id |= CAN_ERR_ACK;
		cf->data[3] = CAN_ERR_PROT_LOC_ACK;
		break;
	case HT42B416_STATUS_ERR_FORM:
		cf->data[2] |= CAN_ERR_PROT_FORM;
		break;
	case HT42B416_STATUS_ERR_STUFF:
		cf->data[2] |= CAN_ERR_PROT_STUFF;
		break;
	default:
		break;
	}

	priv->can.can_stats.bus_error++;
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += cf->len;
	netif_rx(skb);
}

static void ht42b416_signal_ack(struct ht42b416_priv *priv)
{
	enum ht42b416_wait_ack wait;

	wait = READ_ONCE(priv->wait_ack);
	if (wait == HT42B416_WAIT_CR) {
		WRITE_ONCE(priv->wait_ack, HT42B416_WAIT_NONE);
		complete(&priv->ack_complete);
	}
}

static int ht42b416_parse_frame(struct ht42b416_priv *priv,
				const u8 *buf, size_t len)
{
	struct net_device *ndev = priv->can.dev;
	struct sk_buff *skb;
	struct can_frame *cf;
	bool is_eff, is_rtr;
	u8 dlc;
	size_t min_len, idx;
	u32 can_id;

	if (!len)
		return -EINVAL;

	is_eff = buf[0] == 'T' || buf[0] == 'R';
	is_rtr = buf[0] == 'r' || buf[0] == 'R';

	if (is_eff) {
		min_len = 1 + 4 + 1;
		if (len < min_len)
			return -EINVAL;
		can_id = ((u32)buf[1] << 24) |
			 ((u32)buf[2] << 16) |
			 ((u32)buf[3] << 8) |
			 buf[4];
		can_id &= CAN_EFF_MASK;
		dlc = buf[5];
		idx = 6;
	} else {
		min_len = 1 + 2 + 1;
		if (len < min_len)
			return -EINVAL;
		can_id = ((u32)buf[1] << 8) | buf[2];
		can_id &= CAN_SFF_MASK;
		dlc = buf[3];
		idx = 4;
	}

	if (dlc > CAN_MAX_DLEN)
		return -EINVAL;

	if (!is_rtr && len != idx + dlc)
		return -EINVAL;

	skb = alloc_can_skb(ndev, &cf);
	if (!skb) {
		ndev->stats.rx_dropped++;
		return 0;
	}

	cf->can_id = can_id;
	if (is_eff)
		cf->can_id |= CAN_EFF_FLAG;
	if (is_rtr)
		cf->can_id |= CAN_RTR_FLAG;

	cf->len = dlc;
	if (!is_rtr && dlc)
		memcpy(cf->data, &buf[idx], dlc);

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += cf->len;
	netif_rx(skb);

	return 0;
}

static void ht42b416_process_msg(struct ht42b416_priv *priv,
				 const u8 *buf, size_t len)
{
	struct net_device *ndev = priv->can.dev;

	if (ht42b416_debug) {
		if (len)
			ht42b416_log_uart(&priv->serdev->dev,
					  "UART RX msg ", buf, len);
		else
			dev_info(&priv->serdev->dev, "UART RX ack <CR>\n");
	}

	if (!len) {
		ht42b416_signal_ack(priv);
		return;
	}

	switch (buf[0]) {
	case 't':
	case 'T':
	case 'r':
	case 'R':
		if (ht42b416_parse_frame(priv, buf, len) && net_ratelimit())
			netdev_warn(ndev, "invalid frame len=%zu\n", len);
		break;
	case 'z':
	case 'Z':
		ht42b416_finish_tx(priv, buf[0]);
		break;
	case 'F':
		if (len >= 2)
			ht42b416_handle_status(priv, (s8)buf[1]);
		break;
	default:
		netdev_dbg(ndev, "dropping response 0x%02x len=%zu\n",
			   buf[0], len);
		break;
	}
}

static size_t ht42b416_receive(struct serdev_device *serdev,
			       const u8 *data, size_t count)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);
	size_t i;

	for (i = 0; i < count; i++) {
		u8 byte = data[i];

		if (byte == '\r' || byte == '\n') {
			if (priv->rx_len < HT42B416_RX_BUF_LEN)
				ht42b416_process_msg(priv,
						     priv->rx_buf,
						     priv->rx_len);
			else
				netdev_warn(priv->can.dev,
					    "RX buffer overflow (%zu bytes)\n",
					    priv->rx_len);
			priv->rx_len = 0;
			continue;
		}

		if (priv->rx_len >= HT42B416_RX_BUF_LEN) {
			priv->rx_len = HT42B416_RX_BUF_LEN;
			continue;
		}

		priv->rx_buf[priv->rx_len++] = byte;
	}

	return count;
}

static const struct serdev_device_ops ht42b416_serdev_ops = {
	.receive_buf = ht42b416_receive,
	.write_wakeup = ht42b416_tx_wakeup,
};

static netdev_tx_t ht42b416_start_xmit(struct sk_buff *skb,
				       struct net_device *ndev)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);
	struct can_frame *cf = (struct can_frame *)skb->data;
	unsigned long flags;
	bool eff, rtr;
	u8 *pos;
	int err;

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	eff = cf->can_id & CAN_EFF_FLAG;
	rtr = cf->can_id & CAN_RTR_FLAG;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (priv->tx_busy) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	pos = priv->tx_buf;
	if (eff) {
		u32 id = cf->can_id & CAN_EFF_MASK;

		*pos++ = rtr ? 'R' : 'T';
		*pos++ = id >> 24;
		*pos++ = id >> 16;
		*pos++ = id >> 8;
		*pos++ = id;
	} else {
		u16 id = cf->can_id & CAN_SFF_MASK;

		*pos++ = rtr ? 'r' : 't';
		*pos++ = id >> 8;
		*pos++ = id;
	}

	*pos++ = cf->len;

	if (!rtr && cf->len) {
		memcpy(pos, cf->data, cf->len);
		pos += cf->len;
	}

	*pos++ = '\r';

	priv->tx_len = pos - priv->tx_buf;
	priv->tx_pos = 0;
	priv->tx_busy = true;
	priv->tx_expect = eff ? HT42B416_TX_EXPECT_CAP_Z : HT42B416_TX_EXPECT_Z;
	priv->tx_dlc = cf->len;

	if (ht42b416_debug)
		ht42b416_log_uart(&priv->serdev->dev, "UART TX frame ",
				  priv->tx_buf, priv->tx_len);

	netif_stop_queue(ndev);
	can_put_echo_skb(skb, ndev, 0, 0);

	err = ht42b416_tx_kick_locked(priv);
	if (err) {
		priv->tx_busy = false;
		priv->tx_len = 0;
		priv->tx_pos = 0;
		priv->tx_expect = HT42B416_TX_NONE;
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		ndev->stats.tx_errors++;
		can_free_echo_skb(ndev, 0, NULL);
		netif_wake_queue(ndev);
		return NETDEV_TX_OK;
	}

	if (priv->tx_pos < priv->tx_len)
		schedule_work(&priv->tx_work);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_OK;
}

static int ht42b416_open(struct net_device *ndev)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);
	int ret;

	ret = open_candev(ndev);
	if (ret)
		return ret;

	ret = ht42b416_hw_start(priv);
	if (ret) {
		close_candev(ndev);
		return ret;
	}

	netif_start_queue(ndev);
	return 0;
}

static int ht42b416_close(struct net_device *ndev)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	ht42b416_hw_stop(priv);
	close_candev(ndev);

	return 0;
}

static void ht42b416_tx_timeout(struct net_device *ndev,
				unsigned int txqueue)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);

	netdev_warn(ndev, "TX timeout\n");
	ndev->stats.tx_errors++;
	ht42b416_abort_tx(priv);
}

static const struct net_device_ops ht42b416_netdev_ops = {
	.ndo_open		= ht42b416_open,
	.ndo_stop		= ht42b416_close,
	.ndo_start_xmit		= ht42b416_start_xmit,
	.ndo_tx_timeout		= ht42b416_tx_timeout,
};

static int ht42b416_set_mode(struct net_device *ndev, enum can_mode mode)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);

	switch (mode) {
	case CAN_MODE_START:
		return ht42b416_hw_start(priv);
	case CAN_MODE_STOP:
		return ht42b416_hw_stop(priv);
	default:
		return -EOPNOTSUPP;
	}
}

static int ht42b416_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct net_device *ndev;
	struct ht42b416_priv *priv;
	u32 baud = 115200;
	int ret;

	ndev = alloc_candev(sizeof(*priv), 1);
	if (!ndev)
		return -ENOMEM;

	ndev->flags |= IFF_ECHO; /* we support local echo */

	priv = netdev_priv(ndev);
	priv->serdev = serdev;
	priv->uart_baud = baud;
	priv->running = false;
	priv->rx_len = 0;
	mutex_init(&priv->cmd_lock);
	init_completion(&priv->ack_complete);
	spin_lock_init(&priv->tx_lock);
	INIT_WORK(&priv->tx_work, ht42b416_tx_work);
	priv->wait_ack = HT42B416_WAIT_NONE;

	priv->enable_gpiod = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable_gpiod)) {
		ret = PTR_ERR(priv->enable_gpiod);
		goto err_free;
	}

	device_property_read_u32(dev, "current-speed", &priv->uart_baud);

	serdev_device_set_drvdata(serdev, priv);
	serdev_device_set_client_ops(serdev, &ht42b416_serdev_ops);

	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		goto err_free;

	ret = serdev_device_set_baudrate(serdev, priv->uart_baud);
	if (ret > 0 && ret != priv->uart_baud)
		dev_warn(dev, "UART requested %u baud, got %d\n",
			 priv->uart_baud, ret);
	if (ret > 0)
		priv->uart_baud = ret;
	serdev_device_set_flow_control(serdev, false);
	serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);

	priv->can.clock.freq = 0;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
				       CAN_CTRLMODE_LISTENONLY;
	priv->can.do_set_mode = ht42b416_set_mode;
	priv->can.bitrate_const = ht42b416_bitrate_const;
	priv->can.bitrate_const_cnt = ARRAY_SIZE(ht42b416_bitrate_const);

	ndev->netdev_ops = &ht42b416_netdev_ops;
	ndev->ethtool_ops = &ht42b416_ethtool_ops;
	ndev->watchdog_timeo = HT42B416_TX_TIMEOUT;

	SET_NETDEV_DEV(ndev, dev);

	ret = register_candev(ndev);
	if (ret)
		goto err_free;

	dev_info(dev, "Holtek HT42B416 bridge at %u baud\n", priv->uart_baud);

	return 0;

err_free:
	free_candev(ndev);
	return ret;
}

static void ht42b416_remove(struct serdev_device *serdev)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);
	struct net_device *ndev = priv->can.dev;

	cancel_work_sync(&priv->tx_work);
	ht42b416_hw_stop(priv);
	unregister_candev(ndev);
	free_candev(ndev);
}

static const struct of_device_id ht42b416_of_match[] = {
	{ .compatible = "holtek,ht42b416" },
	{ }
};
MODULE_DEVICE_TABLE(of, ht42b416_of_match);

static struct serdev_device_driver ht42b416_driver = {
	.driver = {
		.name = "ht42b416",
		.of_match_table = ht42b416_of_match,
	},
	.probe = ht42b416_probe,
	.remove = ht42b416_remove,
};

module_serdev_device_driver(ht42b416_driver);

MODULE_AUTHOR("Anton Tarasov <ant0@mail.ru>");
MODULE_DESCRIPTION("Holtek HT42B416 UART-to-CAN bridge driver");
MODULE_LICENSE("GPL");
