// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec-uart.c - Wiren Board Embedded Controller SPI-UART driver
 *
 * Copyright (c) 2024 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/wbec.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/device/bus.h>

#define DRIVER_NAME				"wbec-uart"
#define WBEC_UART_PORT_COUNT			2
#define WBEC_REGMAP_READ_BIT			BIT(15)
#define WBEC_UART_REGMAP_BUFFER_SIZE		64
#define WBEC_UART_REGMAP_BUFFER_SIZE_W_ERRORS	(WBEC_UART_REGMAP_BUFFER_SIZE / 2)

#define WBEC_UART_RX_BYTE_ERROR_PE		BIT(0)
#define WBEC_UART_RX_BYTE_ERROR_FE		BIT(1)
#define WBEC_UART_RX_BYTE_ERROR_NE		BIT(2)
#define WBEC_UART_RX_BYTE_ERROR_ORE		BIT(3)

struct wbec_uart_regmap_address {
	u16 ctrl;
	u16 tx_start;
	u16 exchange;
};

static const struct wbec_uart_regmap_address wbec_uart_regmap_address[WBEC_UART_PORT_COUNT] = {
	[0] = {
		.ctrl = 0x100,
		.tx_start = 0x120,
		.exchange = 0x180,
	},
	[1] = {
		.ctrl = 0x108,
		.tx_start = 0x121,
		.exchange = 0x1A1,
	}
};

struct wbec_uart_one_port {
	struct uart_port port;
	struct wbec_uart *wbec_uart;
	struct work_struct start_tx_work;
	struct completion tx_complete;
	struct regmap *regmap;
};

static struct wbec_uart_one_port *wbec_uart_ports[WBEC_UART_PORT_COUNT] = {};
static struct mutex wbec_uart_mutex;

struct wbec_regmap_header {
	u16 address : 15;
	u16 read_bit : 1;
	u16 pad[WBEC_REGMAP_PAD_WORDS_COUNT];
} __packed;

struct uart_rx {
	u8 read_bytes_count;
	u8 ready_for_tx : 1;
	u8 tx_completed : 1;
	u8 data_format : 1;
	u8 reserved : 5;
	union {
		struct {
			uint8_t err_flags;
			uint8_t byte;
		} bytes_with_errors[WBEC_UART_REGMAP_BUFFER_SIZE_W_ERRORS];
		uint8_t read_bytes[WBEC_UART_REGMAP_BUFFER_SIZE];
	};
} __packed;

struct uart_tx {
	u8 bytes_to_send_count;
	u8 reserved;
	u8 bytes_to_send[WBEC_UART_REGMAP_BUFFER_SIZE];
} __packed;

struct uart_ctrl {
	/* offset 0x00 */
	uint16_t enable : 1;
	uint16_t ctrl_applyed : 1;
	uint16_t res1 : 14;
	/* offset 0x01 */
	uint16_t baud_x100;
	/* offset 0x02 */
	uint16_t parity : 2;
	uint16_t stop_bits : 2;
	uint16_t rs485_enabled : 1;
	uint16_t rs485_rx_during_tx : 1;
	uint16_t res2 : 10;
} __packed;

union uart_ctrl_regs {
	struct uart_ctrl ctrl;
	u16 buf[3];
};

union uart_exchange_regs_tx {
	struct {
		struct wbec_regmap_header header;
		struct uart_tx tx[WBEC_UART_PORT_COUNT];
	};
	u16 buf[(sizeof(struct uart_tx) * WBEC_UART_PORT_COUNT + sizeof(struct wbec_regmap_header)) / 2];
};

union uart_exchange_regs_rx {
	struct {
		struct wbec_regmap_header header;
		struct uart_rx rx[WBEC_UART_PORT_COUNT];
	};
	u16 buf[(sizeof(struct uart_rx) * WBEC_UART_PORT_COUNT + sizeof(struct wbec_regmap_header)) / 2];
};

static void swap_bytes(u16 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		buf[i] = __fswab16(buf[i]);
}

static void wbec_collect_data_for_exchange(struct wbec_uart_one_port *wbec_one_port, struct uart_tx *tx)
{
	struct uart_port *port = &wbec_one_port->port;
	struct circ_buf *xmit;

	if (port == NULL) {
		tx->bytes_to_send_count = 0;
		return;
	}

	xmit = &port->state->xmit;

	if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		tx->bytes_to_send_count = 0;
	} else {
		int i;
		unsigned int to_send = uart_circ_chars_pending(xmit);

		to_send = min(to_send, WBEC_UART_REGMAP_BUFFER_SIZE);

		tx->bytes_to_send_count = to_send;

		// Convert to linear buffer
		for (i = 0; i < to_send; ++i) {
			u8 c = xmit->buf[(xmit->tail + i) & (UART_XMIT_SIZE - 1)];

			tx->bytes_to_send[i] = c;
		}

		if (to_send)
			reinit_completion(&wbec_one_port->tx_complete);
	}
}

static void wbec_process_received_exchange(struct wbec_uart_one_port *wbec_one_port,
				      struct uart_rx *rx,
				      u8 bytes_sent_in_exchange)
{
	struct uart_port *port = &wbec_one_port->port;
	struct circ_buf *xmit = &port->state->xmit;
	u8 max_read_bytes = WBEC_UART_REGMAP_BUFFER_SIZE;

	if (rx->data_format == 1)
		max_read_bytes = WBEC_UART_REGMAP_BUFFER_SIZE_W_ERRORS;

	if (rx->read_bytes_count > max_read_bytes) {
		dev_err_ratelimited(port->dev, "received_bytes_count bigger than buffer size\n");
		rx->read_bytes_count = max_read_bytes;
	}

	if (rx->read_bytes_count > 0) {
		int i;

		for (i = 0; i < rx->read_bytes_count; i++) {
			u8 c;
			u8 flag = TTY_NORMAL;

			if (rx->data_format == 1) {
				c = rx->bytes_with_errors[i].byte;
				if (rx->bytes_with_errors[i].err_flags & WBEC_UART_RX_BYTE_ERROR_PE) {
					flag |= TTY_PARITY;
					port->icount.parity++;
				}
				if (rx->bytes_with_errors[i].err_flags & WBEC_UART_RX_BYTE_ERROR_FE) {
					flag |= TTY_FRAME;
					port->icount.frame++;
				}
				if (rx->bytes_with_errors[i].err_flags & WBEC_UART_RX_BYTE_ERROR_ORE) {
					flag |= TTY_OVERRUN;
					port->icount.overrun++;
				}
			} else {
				c = rx->read_bytes[i];
			}

			port->icount.rx++;
			uart_insert_char(port, 0, 0, c, flag);
		}
		tty_flip_buffer_push(&port->state->port);
	}

	/* check if tx data was accepted by wbec */
	if (rx->ready_for_tx) {
		unsigned long flags;

		if (bytes_sent_in_exchange > 0)
			uart_xmit_advance(port, bytes_sent_in_exchange);

		uart_port_lock_irqsave(port, &flags);
		if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
			uart_write_wakeup(port);
		uart_port_unlock_irqrestore(port, flags);
	}

	/* check if tx was completed */
	if (rx->tx_completed && (bytes_sent_in_exchange == 0))
		complete(&wbec_one_port->tx_complete);
}

static void wbec_spi_exchange_sync(struct wbec *wbec)
{
	struct spi_device *spi = wbec->spi;
	union uart_exchange_regs_rx rx;
	union uart_exchange_regs_tx tx = {};
	struct spi_message msg;
	struct spi_transfer transfer = {};
	int ret, port_i;
	u8 bytes_sent_in_xfer[WBEC_UART_PORT_COUNT] = {};

	mutex_lock(&wbec_uart_mutex);

	/* prepare tx data */
	for (port_i = 0; port_i < WBEC_UART_PORT_COUNT; port_i++) {
		struct wbec_uart_one_port *wbec_one_port = wbec_uart_ports[port_i];
		struct uart_tx *one_port_tx = &tx.tx[port_i];

		if (!wbec_one_port)
			continue;

		wbec_collect_data_for_exchange(wbec_one_port, one_port_tx);
		bytes_sent_in_xfer[port_i] = one_port_tx->bytes_to_send_count;
	}

	/* prepare spi transfer */
	tx.header.address = wbec_uart_regmap_address[0].exchange;

	transfer.tx_buf = tx.buf;
	transfer.rx_buf = rx.buf;
	transfer.len = sizeof(tx.header) + sizeof(tx.tx[0]) * WBEC_UART_PORT_COUNT;

	spi_message_init_with_transfers(&msg, &transfer, 1);

	swap_bytes(tx.buf, transfer.len / 2);

	/* transfer */
	ret = spi_sync(spi, &msg);
	if (ret) {
		dev_err(&spi->dev, "spi_sync failed: %d\n", ret);
		goto mutex_release;
	}

	swap_bytes(rx.buf, transfer.len / 2);

	for (port_i = 0; port_i < WBEC_UART_PORT_COUNT; port_i++) {
		struct wbec_uart_one_port *wbec_one_port = wbec_uart_ports[port_i];
		struct uart_rx *one_port_rx = &rx.rx[port_i];

		if (!wbec_one_port)
			continue;

		wbec_process_received_exchange(wbec_one_port, one_port_rx, bytes_sent_in_xfer[port_i]);
	}

mutex_release:
	mutex_unlock(&wbec_uart_mutex);
}

static void wbec_start_tx_work_handler(struct work_struct *work)
{
	struct wbec_uart_one_port *p = container_of(work,
						struct wbec_uart_one_port,
						start_tx_work);

	struct uart_port *port = &p->port;
	struct regmap *regmap = p->regmap;

	regmap_write(regmap, wbec_uart_regmap_address[port->line].tx_start, 0x1);
}


static unsigned int wbec_uart_tx_empty(struct uart_port *port)
{
	struct wbec_uart_one_port *wbec_one_port = container_of(port,
					      struct wbec_uart_one_port,
					      port);

	if (completion_done(&wbec_one_port->tx_complete))
		return TIOCSER_TEMT;

	return 0;
}

static void wbec_uart_start_tx(struct uart_port *port)
{
	struct wbec_uart_one_port *wbec_one_port = container_of(port,
					      struct wbec_uart_one_port,
					      port);
	struct circ_buf *xmit = &port->state->xmit;

	if (!uart_circ_empty(xmit) && !uart_tx_stopped(port)) {
		reinit_completion(&wbec_one_port->tx_complete);
		schedule_work(&wbec_one_port->start_tx_work);
	}
}

static int wbec_uart_startup(struct uart_port *port)
{
	struct wbec_uart_one_port *p = container_of(port,
					      struct wbec_uart_one_port,
					      port);

	struct regmap *regmap = p->regmap;
	int ret, val;
	u16 ctrl_reg = wbec_uart_regmap_address[port->line].ctrl;

	mutex_lock(&wbec_uart_mutex);

	/* set enable=1; applyed=0 */
	regmap_write(regmap, ctrl_reg, 0x1);

	/* wait for applyed==1 */
	ret = regmap_read_poll_timeout(regmap, ctrl_reg, val,
				       (val & 0x0002),
				       1000, 1000000);

	if (ret == 0)
		dev_dbg(port->dev, "wbec-uart port %d is ready\n", port->line);
	else
		dev_err(port->dev, "wbec-uart port %d is not ready\n", port->line);

	complete(&p->tx_complete);

	mutex_unlock(&wbec_uart_mutex);

	return ret;
}

static void wbec_uart_shutdown(struct uart_port *port)
{
	struct wbec_uart_one_port *p = container_of(port,
					      struct wbec_uart_one_port,
					      port);

	struct regmap *regmap = p->regmap;

	int ret, val;
	u16 ctrl_reg = wbec_uart_regmap_address[port->line].ctrl;

	wait_for_completion_timeout(&p->tx_complete, msecs_to_jiffies(10000));

	mutex_lock(&wbec_uart_mutex);

	/* set enable=0; applyed=0 */
	regmap_write(regmap, ctrl_reg, 0x0);

	/* wait for applyed==1 */
	ret = regmap_read_poll_timeout(regmap, ctrl_reg, val,
				       (val & 0x0002),
				       1000, 1000000);
	if (ret)
		dev_err(port->dev, "Failed to disable port: ctrl applyed timeout\n");

	mutex_unlock(&wbec_uart_mutex);
}

static void wbec_uart_set_termios(struct uart_port *port, struct ktermios *new,
				       const struct ktermios *old)
{
	struct wbec_uart_one_port *p = container_of(port,
					      struct wbec_uart_one_port,
					      port);
	struct regmap *regmap = p->regmap;
	int ret, val, baud;
	u16 ctrl_reg = wbec_uart_regmap_address[port->line].ctrl;
	union uart_ctrl_regs ctrl_regs;
	unsigned long flags;

	mutex_lock(&wbec_uart_mutex);

	regmap_bulk_read(regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));

	ctrl_regs.ctrl.ctrl_applyed = 0;

	/* Parity */
	if (new->c_cflag & PARENB) {
		if (new->c_cflag & PARODD)
			ctrl_regs.ctrl.parity = 0x2; /* Odd */
		else
			ctrl_regs.ctrl.parity = 0x1; /* Even */
	} else {
		ctrl_regs.ctrl.parity = 0x0; /* None */
	}

	/* Stop bits */
	if (new->c_cflag & CSTOPB)
		ctrl_regs.ctrl.stop_bits = 2; /* 2 */
	else
		ctrl_regs.ctrl.stop_bits = 0; /* 1 */

	/* Baud rate */
	baud = uart_get_baud_rate(port, new, old, 1200, 115200);
	ctrl_regs.ctrl.baud_x100 = baud / 100;

	regmap_bulk_write(regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));

	/* wait for applyed==1 */
	ret = regmap_read_poll_timeout(regmap, ctrl_reg, val,
				       (val & 0x0002),
				       100, 1000000);
	if (ret)
		dev_err(port->dev, "Failed to set termios: ctrl applyed timeout\n");

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, new->c_cflag, baud);
	uart_port_unlock_irqrestore(port, flags);

	mutex_unlock(&wbec_uart_mutex);
}

static void wbec_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_WBEC;
}

static void wbec_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* do nothing */
}

static unsigned int wbec_uart_get_mctrl(struct uart_port *port)
{
	/* do nothing */
	return 0;
}

static void wbec_uart_break_ctl(struct uart_port *port, int break_state)
{
	/* do nothing */
}

static int wbec_uart_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	/* do nothing */
	return 0;
}

static void wbec_uart_pm(struct uart_port *port, unsigned int state, unsigned int oldstate)
{
	/* do nothing */
}

static int wbec_uart_request_port(struct uart_port *port)
{
	/* do nothing */
	return 0;
}

static void wbec_uart_null_void(struct uart_port *port)
{
	/* do nothing */
}

static const char *wbec_uart_type(struct uart_port *port)
{
	return (port->type == PORT_WBEC) ? "WBEC UART" : NULL;
}

static const struct uart_ops wbec_uart_ops = {
	.tx_empty	= wbec_uart_tx_empty,
	.set_mctrl	= wbec_uart_set_mctrl,
	.get_mctrl	= wbec_uart_get_mctrl,
	.stop_tx	= wbec_uart_null_void,
	.start_tx	= wbec_uart_start_tx,
	.stop_rx	= wbec_uart_null_void,
	.break_ctl	= wbec_uart_break_ctl,
	.startup	= wbec_uart_startup,
	.shutdown	= wbec_uart_shutdown,
	.set_termios	= wbec_uart_set_termios,
	.type		= wbec_uart_type,
	.request_port	= wbec_uart_request_port,
	.release_port	= wbec_uart_null_void,
	.config_port	= wbec_uart_config_port,
	.verify_port	= wbec_uart_verify_port,
	.pm		= wbec_uart_pm,
};

static int wbec_uart_config_rs485(struct uart_port *port,
				  struct ktermios *termios,
				  struct serial_rs485 *rs485)
{
	struct wbec_uart_one_port *p = container_of(port,
					      struct wbec_uart_one_port,
					      port);
	int ret = 0;

	if (rs485->flags & SER_RS485_ENABLED) {
		u16 gpio_af_mode = 0b010000 << (port->line * 6);
		u16 gpio_af_mask = 0b110000 << (port->line * 6);
		union uart_ctrl_regs ctrl_regs;
		u16 ctrl_reg = wbec_uart_regmap_address[port->line].ctrl;
		int val;

		// gpio mode
		regmap_update_bits(p->regmap, WBEC_REG_GPIO_AF, gpio_af_mask, gpio_af_mode);

		regmap_bulk_read(p->regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));

		ctrl_regs.ctrl.ctrl_applyed = 0;
		ctrl_regs.ctrl.rs485_enabled = 1;

		if (rs485->flags & SER_RS485_RX_DURING_TX)
			ctrl_regs.ctrl.rs485_rx_during_tx = 1;
		else
			ctrl_regs.ctrl.rs485_rx_during_tx = 0;

		regmap_bulk_write(p->regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));

		/* wait for applyed==1 */
		ret = regmap_read_poll_timeout(p->regmap, ctrl_reg, val,
				       (val & 0x0002),
				       1000, 1000000);
		if (ret)
			dev_err(port->dev, "Failed to set rs485 config: ctrl applyed timeout\n");
	}

	return ret;
}

static const struct serial_rs485 wbec_uart_rs485_supported = {
	.flags = SER_RS485_ENABLED | SER_RS485_RX_DURING_TX,
};

static struct uart_driver wbec_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = DRIVER_NAME,
	.dev_name = "ttyWBE",
	.nr = WBEC_UART_PORT_COUNT,
};

static int wbec_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wbec *wbec;
	struct wbec_uart_one_port *p;
	struct device_node *parent_node;
	struct device *parent_dev;
	int ret, val;
	u32 reg;
	u16 gpio_af_mode = 0;
	u16 gpio_af_mask = 0;
	union uart_ctrl_regs ctrl_regs;
	u16 ctrl_reg;

	if (!of_device_is_available(pdev->dev.of_node))
		return dev_err_probe(dev, -ENODEV, "Device is not available\n");

	parent_node = of_get_parent(pdev->dev.of_node);
	if (!parent_node)
		return dev_err_probe(dev, -EINVAL, "Failed to get parent node\n");

	parent_dev = bus_find_device_by_of_node(&spi_bus_type, parent_node);
	if (!parent_dev)
		return dev_err_probe(dev, -EINVAL, "Failed to get parent SPI device\n");

	wbec = dev_get_drvdata(parent_dev);
	if (!wbec)
		return dev_err_probe(dev, -EINVAL, "Failed to get parent device data\n");

	if (!wbec->spi->irq || !wbec->support_v2_protocol)
		return dev_err_probe(dev, -EINVAL, "WBEC firmware does not support UART\n");

	if (device_property_read_u32(dev, "reg", &reg))
		return dev_err_probe(dev, -EINVAL, "Failed to read 'reg' property\n");

	if (reg >= WBEC_UART_PORT_COUNT)
		return dev_err_probe(dev, -EINVAL, "Invalid reg value: %u\n", reg);

	p = devm_kzalloc(&pdev->dev, sizeof(struct wbec_uart_one_port),
				GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->regmap = dev_get_regmap(parent_dev, NULL);
	if (!p->regmap)
		return dev_err_probe(dev, -ENODEV, "Failed to get regmap\n");

	mutex_lock(&wbec_uart_mutex);

	/* set RX and TX pins mode to AF */
	gpio_af_mode |= 1 << (reg * 6 + 0);	// TX
	gpio_af_mode |= 1 << (reg * 6 + 2);	// RX
	/* RTS pin mode will be set in the future in wbec_uart_config_rs485 */
	gpio_af_mask = 0b111111 << (reg * 6);

	regmap_update_bits(p->regmap, WBEC_REG_GPIO_AF, gpio_af_mask, gpio_af_mode);

	/* Disable UART */
	ctrl_reg = wbec_uart_regmap_address[reg].ctrl;
	regmap_bulk_read(p->regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));
	ctrl_regs.ctrl.ctrl_applyed = 0;
	ctrl_regs.ctrl.enable = 0;
	ctrl_regs.ctrl.rs485_enabled = 0;
	regmap_bulk_write(p->regmap, ctrl_reg, ctrl_regs.buf, ARRAY_SIZE(ctrl_regs.buf));
	/* wait for applyed==1 */
	ret = regmap_read_poll_timeout(p->regmap, ctrl_reg, val,
			       (val & 0x0002),
			       1000, 1000000);
	if (ret) {
		dev_err(dev, "Failed to init port: ctrl applyed timeout\n");
		goto mutex_release;
	}

	init_completion(&p->tx_complete);

	// Initialize the UART port
	p->port.ops = &wbec_uart_ops;
	p->port.dev = &pdev->dev;
	p->port.type = PORT_WBEC;
	p->port.irq = wbec->spi->irq;
	/*
	 * Use all ones as membase to make sure uart_configure_port() in
	 * serial_core.c does not abort for SPI/I2C devices where the
	 * membase address is not applicable.
	 */
	p->port.membase	= (void __iomem *)~0;
	p->port.iobase = reg;
	p->port.iotype = UPIO_PORT;
	p->port.flags = UPF_FIXED_TYPE | UPF_AUTO_RTS /*| UPF_LOW_LATENCY*/;
	p->port.rs485_config = wbec_uart_config_rs485;
	p->port.rs485_supported = wbec_uart_rs485_supported;
	p->port.uartclk = 64000000;
	p->port.fifosize = 64;
	p->port.line = reg;

	ret = uart_get_rs485_mode(&p->port);
	if (ret) {
		dev_err(dev, "Failed to get RS485 mode\n");
		goto mutex_release;
	}

	ret = uart_add_one_port(&wbec_uart_driver, &p->port);
	if (ret) {
		dev_err(dev, "Failed to register UART port\n");
		goto mutex_release;
	}

	INIT_WORK(&p->start_tx_work, wbec_start_tx_work_handler);

	platform_set_drvdata(pdev, p);
	wbec_uart_ports[reg] = p;

	wbec->irq_handler = wbec_spi_exchange_sync;

	dev_dbg(&pdev->dev, "port %s registered\n", p->port.name);

mutex_release:
	mutex_unlock(&wbec_uart_mutex);

	return ret;
}

static void wbec_uart_remove(struct platform_device *pdev)
{
	struct wbec_uart_one_port *p = platform_get_drvdata(pdev);
	u16 gpio_af_mode = 0;
	u16 gpio_af_mask = 0;
	int line = p->port.line;

	dev_dbg(&pdev->dev, "port %s unregistered\n", p->port.name);

	mutex_lock(&wbec_uart_mutex);

	gpio_af_mask = 0b111111 << (line * 6);
	regmap_update_bits(p->regmap, WBEC_REG_GPIO_AF, gpio_af_mask, gpio_af_mode);

	wbec_uart_ports[line] = NULL;
	uart_remove_one_port(&wbec_uart_driver, &p->port);

	mutex_unlock(&wbec_uart_mutex);
}

static const struct of_device_id wbec_uart_of_match[] = {
	{ .compatible = "wirenboard,wbec-uart" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_uart_of_match);

static struct platform_driver wbec_uart_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = wbec_uart_of_match,
	},
	.probe = wbec_uart_probe,
	.remove_new = wbec_uart_remove,
};

static int __init wbec_uart_init(void)
{
	int ret;

	mutex_init(&wbec_uart_mutex);

	ret = uart_register_driver(&wbec_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&wbec_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&wbec_uart_driver);

	return ret;
}

static void __exit wbec_uart_exit(void)
{
	platform_driver_unregister(&wbec_uart_platform_driver);
	uart_unregister_driver(&wbec_uart_driver);
}

module_init(wbec_uart_init);
module_exit(wbec_uart_exit);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board Embedded Controller SPI-UART driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-uart");
