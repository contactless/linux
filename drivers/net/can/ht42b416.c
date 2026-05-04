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
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/build_bug.h>

#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/skb.h>

#define HT42B416_MAX_DATA_LEN		8
#define HT42B416_MAX_CMD_LEN		16
#define HT42B416_MAX_FRAME_LEN		(1 + 4 + 1 + HT42B416_MAX_DATA_LEN + 1)
#define HT42B416_RX_BUF_LEN		64
#define HT42B416_ECHO_SIZE		1

#define HT42B416_CMD_TIMEOUT_MS		250
#define HT42B416_POWERUP_DELAY_US	200
#define HT42B416_TX_TIMEOUT		(2 * HZ)
#define HT42B416_TX_RESP_TIMEOUT_MS	500

#define HT42B416_TX_QUEUE_LEN		64

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

enum ht42b416_tx_stage {
	HT42B416_TX_IDLE,
	HT42B416_TX_WAIT_Z,
	HT42B416_TX_WAIT_STATUS_RESP,
};

enum ht42b416_tx_event {
	HT42B416_TX_EVT_KICK,
	HT42B416_TX_EVT_Z,
	HT42B416_TX_EVT_STATUS_RESP,
};

struct ht42b416_tx_actions {
	bool kick;
	bool arm_timeout;
	bool cancel_timeout;
};

struct ht42b416_priv {
	struct can_priv can;
	struct serdev_device *serdev;
	struct gpio_desc *enable_gpiod;

	struct work_struct tx_work;
	struct delayed_work tx_timeout_work;
	spinlock_t tx_lock;	/* protects below fields */
	u8 tx_buf[HT42B416_MAX_FRAME_LEN];
	size_t tx_len;
	size_t tx_pos;
	bool tx_busy;
	u8 tx_fifo_head;
	u8 tx_fifo_tail;
	u8 tx_fifo_len;
	enum ht42b416_tx_expect tx_expect_fifo[HT42B416_ECHO_SIZE];
	struct sk_buff *tx_queue[HT42B416_TX_QUEUE_LEN];
	u8 tx_q_head;
	u8 tx_q_tail;
	u8 tx_q_len;
	enum ht42b416_tx_stage tx_stage;

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

static void ht42b416_kick_queue(struct ht42b416_priv *priv);
static void ht42b416_tx_fsm_locked(struct ht42b416_priv *priv,
				   enum ht42b416_tx_event ev,
				   struct ht42b416_tx_actions *act);
static void ht42b416_tx_timeout_arm(struct ht42b416_priv *priv);
static void ht42b416_tx_timeout_cancel(struct ht42b416_priv *priv);


/**
 * ht42b416_write_raw - write a raw UART buffer with timeout handling
 */
static int ht42b416_write_raw(struct ht42b416_priv *priv,
			      const u8 *buf, size_t len)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(HT42B416_CMD_TIMEOUT_MS);
	size_t pos = 0;

	while (pos < len) {
		int written = serdev_device_write_buf(priv->serdev, buf + pos, len - pos);

		if (written < 0)
			return written;
		if (!written) {
			if (time_after(jiffies, deadline))
				return -ETIMEDOUT;
			usleep_range(100, 200);
			continue;
		}
		pos += written;
	}

	return 0;
}

/**
 * ht42b416_send_cmd_locked - send a command with optional CR ack wait
 */
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

/**
 * ht42b416_send_cmd - serialized command sender
 */
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

/**
 * ht42b416_bitrate_to_code - map bitrate to chip configuration code
 */
static int ht42b416_bitrate_to_code(u32 bitrate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ht42b416_bitrate_const); i++)
		if (ht42b416_bitrate_const[i] == bitrate)
			return i;

	return -EINVAL;
}

/**
 * ht42b416_abort_tx - drop pending TX data and reset TX state
 */
static void ht42b416_abort_tx(struct ht42b416_priv *priv)
{
	struct net_device *ndev = priv->can.dev;
	unsigned long flags;
	u8 i;

	ht42b416_tx_timeout_cancel(priv);
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_len = 0;
	priv->tx_pos = 0;
	priv->tx_busy = false;
	priv->tx_fifo_head = 0;
	priv->tx_fifo_tail = 0;
	priv->tx_fifo_len = 0;
	priv->tx_stage = HT42B416_TX_IDLE;
	while (priv->tx_q_len) {
		struct sk_buff *skb = priv->tx_queue[priv->tx_q_tail];

		priv->tx_queue[priv->tx_q_tail] = NULL;
		priv->tx_q_tail = (priv->tx_q_tail + 1) % HT42B416_TX_QUEUE_LEN;
		priv->tx_q_len--;
		dev_kfree_skb_any(skb);
	}
	priv->tx_q_head = 0;
	priv->tx_q_tail = 0;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	for (i = 0; i < HT42B416_ECHO_SIZE; i++)
		can_free_echo_skb(ndev, i, NULL);
	netif_wake_queue(ndev);
}

static const struct ethtool_ops ht42b416_ethtool_ops = {
	.get_ts_info = ethtool_op_get_ts_info,
};

/**
 * ht42b416_hw_stop - stop the UART-to-CAN bridge and disable the port
 * @priv: device private data
 */
/**
 * ht42b416_hw_stop - stop the bridge and close the CAN channel
 */
static int ht42b416_hw_stop(struct ht42b416_priv *priv)
{
	static const u8 close_cmd[] = { 'C' };

	if (!priv->running)
		goto disable_gpio;

	cancel_delayed_work_sync(&priv->tx_timeout_work);
	netif_stop_queue(priv->can.dev);
	ht42b416_abort_tx(priv);

	ht42b416_send_cmd(priv, close_cmd, sizeof(close_cmd), HT42B416_WAIT_CR);
	priv->running = false;

disable_gpio:
	if (priv->enable_gpiod)
		gpiod_set_value_cansleep(priv->enable_gpiod, 0);

	priv->can.state = CAN_STATE_STOPPED;

	return 0;
}

/**
 * ht42b416_hw_start - initialize and open the UART-to-CAN bridge
 * @priv: device private data
 */
/**
 * ht42b416_hw_start - reset and configure the bridge for CAN operation
 */
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

	/* Reset chip just in case */
	cmd[0] = 'R';
	cmd[1] = 'S';
	cmd[2] = 'T';
	ret = ht42b416_send_cmd(priv, cmd, 3, HT42B416_WAIT_CR);
	if (ret)
		return ret;

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
	ret = ht42b416_send_cmd(priv, cmd, 2, HT42B416_WAIT_CR);
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

	/* Filters must be set after port is open, otherwise the IC will
	 * end up in some buggy state with a broken baudrate
	 */
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

	priv->tx_pos = 0;
	priv->tx_len = 0;
	priv->tx_busy = false;
	priv->tx_fifo_head = 0;
	priv->tx_fifo_tail = 0;
	priv->tx_fifo_len = 0;
	priv->running = true;
	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_wake_queue(ndev);

	return 0;
}

/**
 * ht42b416_tx_kick_locked - push pending TX buffer to UART
 */
static int ht42b416_tx_kick_locked(struct ht42b416_priv *priv)
{
	while (priv->tx_pos < priv->tx_len) {
		int written;
		size_t left = priv->tx_len - priv->tx_pos;

		written = serdev_device_write_buf(priv->serdev,
						  priv->tx_buf + priv->tx_pos,
						  left);
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

	if (priv->tx_pos >= priv->tx_len) {
		priv->tx_busy = false;
	}

	return 0;
}

/**
 * ht42b416_tx_work - worker that continues partial UART writes
 */
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

/**
 * ht42b416_tx_wakeup - serdev write_wakeup callback
 */
static void ht42b416_tx_wakeup(struct serdev_device *serdev)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);
	serdev_device_write_wakeup(serdev);
	schedule_work(&priv->tx_work);
}

/**
 * ht42b416_tx_timeout_work - timeout waiting for Z or F06 response
 */
static void ht42b416_tx_timeout_work(struct work_struct *work)
{
	struct ht42b416_priv *priv =
		container_of(to_delayed_work(work),
			     struct ht42b416_priv, tx_timeout_work);
	struct net_device *ndev = priv->can.dev;
	unsigned long flags;
	enum ht42b416_tx_stage stage;
	bool expired = false;

	spin_lock_irqsave(&priv->tx_lock, flags);
	stage = priv->tx_stage;
	if (stage == HT42B416_TX_WAIT_Z ||
	    stage == HT42B416_TX_WAIT_STATUS_RESP) {
		priv->tx_stage = HT42B416_TX_IDLE;
		expired = true;
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (!expired)
		return;

	netdev_err(ndev, "TX timeout waiting for %s\n",
		   stage == HT42B416_TX_WAIT_Z ? "Z" : "status 0x06");
	priv->running = false;
	ht42b416_abort_tx(priv);
	netif_stop_queue(ndev);
	can_bus_off(ndev);
}

/**
 * ht42b416_tx_complete - handle Z/Z ack and advance TX FSM
 */
static bool ht42b416_tx_complete(struct ht42b416_priv *priv,
				 enum ht42b416_tx_expect ack_expect)
{
	struct net_device *ndev = priv->can.dev;
	unsigned long flags;
	unsigned int tx_bytes;
	enum ht42b416_tx_expect expect;
	struct ht42b416_tx_actions act = {};
	u8 slot;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (priv->tx_stage != HT42B416_TX_WAIT_Z || !priv->tx_fifo_len) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return false;
	}

	slot = priv->tx_fifo_tail;
	expect = priv->tx_expect_fifo[slot];
	priv->tx_fifo_tail = (priv->tx_fifo_tail + 1) % HT42B416_ECHO_SIZE;
	priv->tx_fifo_len--;

	if (expect != ack_expect)
		netdev_warn(ndev, "unexpected TX ack (exp=%u got=%u)\n",
			    expect, ack_expect);

	spin_unlock_irqrestore(&priv->tx_lock, flags);

	tx_bytes = can_get_echo_skb(ndev, slot, NULL);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += tx_bytes;

	spin_lock_irqsave(&priv->tx_lock, flags);
	ht42b416_tx_fsm_locked(priv, HT42B416_TX_EVT_Z, &act);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (act.cancel_timeout)
		ht42b416_tx_timeout_cancel(priv);
	if (act.arm_timeout)
		ht42b416_tx_timeout_arm(priv);
	return true;
}

/**
 * ht42b416_handle_status - handle status/error notifications from the chip
 * @priv: device private data
 * @status: status code returned by the bridge
 */
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

/**
 * ht42b416_signal_ack - wake up a command waiting for CR ack
 */
static void ht42b416_signal_ack(struct ht42b416_priv *priv)
{
	enum ht42b416_wait_ack wait;

	wait = READ_ONCE(priv->wait_ack);
	if (wait == HT42B416_WAIT_CR) {
		WRITE_ONCE(priv->wait_ack, HT42B416_WAIT_NONE);
		complete(&priv->ack_complete);
	}
}

/**
 * ht42b416_handle_status_response - process Fxx status and unblock on F06
 */
static void ht42b416_handle_status_response(struct ht42b416_priv *priv, s8 status)
{
	unsigned long flags;
	struct ht42b416_tx_actions act = {};

	if (status != HT42B416_STATUS_TX_OK) {
		ht42b416_handle_status(priv, status);
		return;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);
	ht42b416_tx_fsm_locked(priv, HT42B416_TX_EVT_STATUS_RESP, &act);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (act.cancel_timeout)
		ht42b416_tx_timeout_cancel(priv);
	if (act.arm_timeout)
		ht42b416_tx_timeout_arm(priv);
	if (act.kick)
		ht42b416_kick_queue(priv);
}

/**
 * ht42b416_parse_frame - decode a complete CAN frame from UART payload
 * @priv: device private data
 * @buf: message payload (no trailing CR/LF)
 * @len: payload length
 */
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

/**
 * ht42b416_rx_consume - drop consumed bytes from the RX buffer
 * @priv: device private data
 * @len: number of bytes to remove from the front
 */
static void ht42b416_rx_consume(struct ht42b416_priv *priv, size_t len)
{
	if (len >= priv->rx_len) {
		priv->rx_len = 0;
		return;
	}

	memmove(priv->rx_buf, priv->rx_buf + len, priv->rx_len - len);
	priv->rx_len -= len;
}

/**
 * ht42b416_rx_handle_ack - handle a lone CR ack
 */
static bool ht42b416_rx_handle_ack(struct ht42b416_priv *priv)
{
	ht42b416_signal_ack(priv);
	ht42b416_rx_consume(priv, 1);
	return true;
}

/**
 * ht42b416_rx_handle_z - handle Z/Z ack messages
 */
static bool ht42b416_rx_handle_z(struct ht42b416_priv *priv, u8 start)
{
	struct net_device *ndev = priv->can.dev;
	enum ht42b416_tx_expect ack_expect;

	if (priv->rx_len < 2)
		return false;
	if (priv->rx_buf[1] != '\r') {
		ht42b416_rx_consume(priv, 1);
		return true;
	}

	ack_expect = start == 'z' ? HT42B416_TX_EXPECT_Z
				  : HT42B416_TX_EXPECT_CAP_Z;
	if (!ht42b416_tx_complete(priv, ack_expect) && net_ratelimit())
		netdev_warn(ndev, "unexpected TX ack (empty fifo)\n");

	ht42b416_rx_consume(priv, 2);
	return true;
}

/**
 * ht42b416_rx_handle_status - handle Fxx status response
 */
static bool ht42b416_rx_handle_status(struct ht42b416_priv *priv)
{
	if (priv->rx_len < 3)
		return false;
	if (priv->rx_buf[2] != '\r') {
		ht42b416_rx_consume(priv, 1);
		return true;
	}

	ht42b416_handle_status_response(priv, (s8)priv->rx_buf[1]);
	ht42b416_rx_consume(priv, 3);
	return true;
}

/**
 * ht42b416_rx_handle_frame - handle received CAN frame payload
 */
static bool ht42b416_rx_handle_frame(struct ht42b416_priv *priv, u8 start)
{
	struct net_device *ndev = priv->can.dev;
	size_t hdr_len;
	size_t msg_len;
	bool is_eff, is_rtr;
	u8 dlc;

	is_eff = start == 'T' || start == 'R';
	is_rtr = start == 'r' || start == 'R';
	hdr_len = is_eff ? (1 + 4 + 1) : (1 + 2 + 1);
	if (priv->rx_len < hdr_len)
		return false;

	dlc = priv->rx_buf[hdr_len - 1];
	if (dlc > CAN_MAX_DLEN) {
		if (net_ratelimit())
			netdev_warn(ndev, "invalid DLC %u\n", dlc);
		ht42b416_rx_consume(priv, 1);
		return true;
	}

	msg_len = hdr_len + (is_rtr ? 0 : dlc);
	if (priv->rx_len < msg_len + 1)
		return false;
	if (priv->rx_buf[msg_len] != '\r') {
		ht42b416_rx_consume(priv, 1);
		return true;
	}

	if (ht42b416_parse_frame(priv, priv->rx_buf, msg_len) &&
	    net_ratelimit())
		netdev_warn(ndev, "invalid frame len=%zu\n", msg_len);
	ht42b416_rx_consume(priv, msg_len + 1);
	return true;
}

/**
 * ht42b416_rx_step - assemble and handle one complete message from RX buffer
 * @priv: device private data
 *
 * The UART delivers a byte stream in uneven chunks, so a single message may
 * arrive in pieces. This function keeps buffering until it can identify a
 * full message (based on type and DLC), then handles it in one place.
 *
 * Return: true if a message was consumed (success or drop), false if more
 * data is needed.
 */
static bool ht42b416_rx_step(struct ht42b416_priv *priv)
{
	u8 start;

	if (!priv->rx_len)
		return false;

	start = priv->rx_buf[0];

	if (start == '\r')
		return ht42b416_rx_handle_ack(priv);

	switch (start) {
	case 'z':
	case 'Z':
		return ht42b416_rx_handle_z(priv, start);
	case 'F':
		return ht42b416_rx_handle_status(priv);
	case 't':
	case 'r':
	case 'T':
	case 'R':
		return ht42b416_rx_handle_frame(priv, start);
	default:
		ht42b416_rx_consume(priv, 1);
		return true;
	}
}

static size_t ht42b416_receive(struct serdev_device *serdev,
			       const u8 *data, size_t count)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);
	size_t orig_count = count;
	size_t space;
	size_t copy;
	size_t tail;

	if (count > HT42B416_RX_BUF_LEN) {
		data += count - HT42B416_RX_BUF_LEN;
		count = HT42B416_RX_BUF_LEN;
	}

	if (priv->rx_len + count > HT42B416_RX_BUF_LEN) {
		if (net_ratelimit())
			netdev_warn(priv->can.dev,
				    "RX buffer overflow (%zu bytes)\n",
				    priv->rx_len + count);
		if (priv->rx_len >= HT42B416_RX_BUF_LEN / 2) {
			tail = HT42B416_RX_BUF_LEN / 2;
			memmove(priv->rx_buf,
				priv->rx_buf + priv->rx_len - tail,
				tail);
			priv->rx_len = tail;
		} else {
			priv->rx_len = 0;
		}
	}

	space = HT42B416_RX_BUF_LEN - priv->rx_len;
	copy = min(count, space);
	memcpy(priv->rx_buf + priv->rx_len, data, copy);
	priv->rx_len += copy;

	while (ht42b416_rx_step(priv))
		;

	return orig_count;
}

static const struct serdev_device_ops ht42b416_serdev_ops = {
	.receive_buf = ht42b416_receive,
	.write_wakeup = ht42b416_tx_wakeup,
};

/**
 * ht42b416_tx_queue_full - check if software TX queue is full
 */
static bool ht42b416_tx_queue_full(struct ht42b416_priv *priv)
{
	return priv->tx_q_len >= HT42B416_TX_QUEUE_LEN;
}

/**
 * ht42b416_can_send_locked - check if a new frame can be sent now
 */
static bool ht42b416_can_send_locked(struct ht42b416_priv *priv)
{
	return priv->running &&
	       !priv->tx_busy &&
	       priv->tx_fifo_len < HT42B416_ECHO_SIZE &&
	       priv->tx_stage == HT42B416_TX_IDLE;
}

/**
 * ht42b416_queue_wake_if_needed - wake netdev queue when space is available
 */
static void ht42b416_queue_wake_if_needed(struct ht42b416_priv *priv)
{
	struct net_device *ndev = priv->can.dev;

	if (priv->running && !ht42b416_tx_queue_full(priv) &&
	    netif_queue_stopped(ndev))
		netif_wake_queue(ndev);
}

/**
 * ht42b416_send_next_locked - pop one skb and start UART transmission
 */
static void ht42b416_send_next_locked(struct ht42b416_priv *priv)
{
	struct net_device *ndev = priv->can.dev;
	struct sk_buff *skb;
	struct can_frame *cf;
	bool eff, rtr;
	enum ht42b416_tx_expect expect;
	u8 slot;
	u8 *pos;
	int err;

	if (!priv->tx_q_len || !ht42b416_can_send_locked(priv)) {
		return;
	}

	skb = priv->tx_queue[priv->tx_q_tail];
	priv->tx_queue[priv->tx_q_tail] = NULL;
	priv->tx_q_tail = (priv->tx_q_tail + 1) % HT42B416_TX_QUEUE_LEN;
	priv->tx_q_len--;

	cf = (struct can_frame *)skb->data;
	eff = cf->can_id & CAN_EFF_FLAG;
	rtr = cf->can_id & CAN_RTR_FLAG;
	expect = eff ? HT42B416_TX_EXPECT_CAP_Z : HT42B416_TX_EXPECT_Z;

	slot = priv->tx_fifo_head;
	priv->tx_fifo_head = (priv->tx_fifo_head + 1) % HT42B416_ECHO_SIZE;
	priv->tx_fifo_len++;
	priv->tx_expect_fifo[slot] = expect;

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
	priv->tx_stage = HT42B416_TX_WAIT_Z;

	can_put_echo_skb(skb, ndev, slot, 0);

	err = ht42b416_tx_kick_locked(priv);
	if (err) {
		priv->tx_busy = false;
		priv->tx_len = 0;
		priv->tx_pos = 0;
		priv->tx_fifo_head = slot;
		priv->tx_fifo_len--;
		priv->tx_stage = HT42B416_TX_IDLE;
		ndev->stats.tx_errors++;
		can_free_echo_skb(ndev, slot, NULL);
		netdev_err(ndev, "TX write failed: %d\n", err);
		return;
	}

	if (priv->tx_pos < priv->tx_len)
		schedule_work(&priv->tx_work);
}

/**
 * ht42b416_tx_fsm_locked - advance TX FSM for a given event
 */
static void ht42b416_tx_fsm_locked(struct ht42b416_priv *priv,
				   enum ht42b416_tx_event ev,
				   struct ht42b416_tx_actions *act)
{
	switch (priv->tx_stage) {
	case HT42B416_TX_IDLE:
		if (ev == HT42B416_TX_EVT_KICK) {
			ht42b416_send_next_locked(priv);
			if (priv->tx_stage == HT42B416_TX_WAIT_Z)
				act->arm_timeout = true;
		}
		break;
	case HT42B416_TX_WAIT_Z:
		if (ev == HT42B416_TX_EVT_Z) {
			priv->tx_stage = HT42B416_TX_WAIT_STATUS_RESP;
			act->cancel_timeout = true;
			act->arm_timeout = true;
		}
		break;
	case HT42B416_TX_WAIT_STATUS_RESP:
		if (ev == HT42B416_TX_EVT_STATUS_RESP) {
			act->cancel_timeout = true;
			priv->tx_stage = HT42B416_TX_IDLE;
			act->kick = true;
		}
		break;
	}
}

/**
 * ht42b416_tx_timeout_arm - arm timeout for Z/F06 response
 */
static void ht42b416_tx_timeout_arm(struct ht42b416_priv *priv)
{
	mod_delayed_work(system_wq, &priv->tx_timeout_work,
			 msecs_to_jiffies(HT42B416_TX_RESP_TIMEOUT_MS));
}

/**
 * ht42b416_tx_timeout_cancel - cancel outstanding TX response timeout
 */
static void ht42b416_tx_timeout_cancel(struct ht42b416_priv *priv)
{
	cancel_delayed_work(&priv->tx_timeout_work);
}

/**
 * ht42b416_kick_queue - try to send the next queued frame
 */
static void ht42b416_kick_queue(struct ht42b416_priv *priv)
{
	unsigned long flags;
	bool wake = false;
	struct ht42b416_tx_actions act = {};

	spin_lock_irqsave(&priv->tx_lock, flags);
	ht42b416_tx_fsm_locked(priv, HT42B416_TX_EVT_KICK, &act);
	wake = priv->running && !ht42b416_tx_queue_full(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (act.cancel_timeout)
		ht42b416_tx_timeout_cancel(priv);
	if (act.arm_timeout)
		ht42b416_tx_timeout_arm(priv);
	if (wake)
		ht42b416_queue_wake_if_needed(priv);
}

/**
 * ht42b416_start_xmit - enqueue a CAN frame for UART transmission
 * @skb: CAN skb to transmit
 * @ndev: net device
 */
static netdev_tx_t ht42b416_start_xmit(struct sk_buff *skb,
				       struct net_device *ndev)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);
	unsigned long flags;
	bool stop;

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (ht42b416_tx_queue_full(priv)) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	priv->tx_queue[priv->tx_q_head] = skb;
	priv->tx_q_head = (priv->tx_q_head + 1) % HT42B416_TX_QUEUE_LEN;
	priv->tx_q_len++;
	stop = ht42b416_tx_queue_full(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (stop)
		netif_stop_queue(ndev);

	ht42b416_kick_queue(priv);
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

/**
 * ht42b416_close - stop the device and close the CAN channel
 */
static int ht42b416_close(struct net_device *ndev)
{
	struct ht42b416_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	ht42b416_hw_stop(priv);
	close_candev(ndev);

	return 0;
}

/**
 * ht42b416_tx_timeout - netdev TX watchdog handler
 */
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

/**
 * ht42b416_set_mode - handle CAN mode switches
 */
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

/**
 * ht42b416_probe - bind and initialize the serdev-backed CAN device
 */
static int ht42b416_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct net_device *ndev;
	struct ht42b416_priv *priv;
	u32 baud = 115200;
	int ret;

	/*
	 * WBEC UART can deliver uneven RX chunks under load, so we keep a
	 * single-frame TX window and reassemble RX data. For simplicity the
	 * same policy is used for all UARTs; if higher throughput is needed on
	 * SoC UARTs, consider applying the small TX window only to WBEC.
	 */
	ndev = alloc_candev(sizeof(*priv), HT42B416_ECHO_SIZE);
	if (!ndev)
		return -ENOMEM;

	ndev->flags |= IFF_ECHO; /* we support local echo */

	priv = netdev_priv(ndev);
	priv->serdev = serdev;
	priv->uart_baud = baud;
	priv->running = false;
	priv->rx_len = 0;
	priv->tx_fifo_head = 0;
	priv->tx_fifo_tail = 0;
	priv->tx_fifo_len = 0;
	priv->tx_q_head = 0;
	priv->tx_q_tail = 0;
	priv->tx_q_len = 0;
	priv->tx_stage = HT42B416_TX_IDLE;
	mutex_init(&priv->cmd_lock);
	init_completion(&priv->ack_complete);
	spin_lock_init(&priv->tx_lock);
	INIT_WORK(&priv->tx_work, ht42b416_tx_work);
	INIT_DELAYED_WORK(&priv->tx_timeout_work, ht42b416_tx_timeout_work);
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
		dev_warn(dev, "UART requested %u baud, got %d\n", priv->uart_baud, ret);
	if (ret > 0)
		priv->uart_baud = ret;
	serdev_device_set_flow_control(serdev, false);
	serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);

	priv->can.clock.freq = 0;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY;
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

/**
 * ht42b416_remove - unbind and teardown the CAN device
 */
static void ht42b416_remove(struct serdev_device *serdev)
{
	struct ht42b416_priv *priv = serdev_device_get_drvdata(serdev);
	struct net_device *ndev = priv->can.dev;

	cancel_work_sync(&priv->tx_work);
	cancel_delayed_work_sync(&priv->tx_timeout_work);
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

MODULE_AUTHOR("Anton Tarasov <anton.tarasov@wirenboard.com>");
MODULE_DESCRIPTION("Holtek HT42B416 UART-to-CAN bridge driver");
MODULE_LICENSE("GPL");
