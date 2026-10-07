// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC1264 SPI sensor control for Xiaomi Pad 6 Pro (SM8475, liuqin).
 * The fpc1020 names below retain the existing device interface. Other sensors
 * using that interface have not been validated by this integration.
 *
 * Opening /dev/fpc1020 powers the sensor; the last close powers it off.
 * Biometric enrolment and matching run in the OEM trusted application.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/pm.h>
#include <linux/sizes.h>
#include <uapi/linux/spi/spidev.h>

#define FPC_IOC_MAGIC			'f'
#define FPC_IOC_RESET			_IO(FPC_IOC_MAGIC, 1)
#define FPC_IOC_GET_HWID		_IOR(FPC_IOC_MAGIC, 2, __u16)

#define FPC1020_REG_HWID		0xFC
#define FPC1020_EXPECTED_HWID		0x020A
#define FPC1020_EXPECTED_HWID_LIUQIN	0x2012

/*
 * Upper bound on one copied transfer, whether through read()/write() or one
 * SPI_IOC_MESSAGE segment. The sensor's own transfers are far smaller; this
 * only stops a caller from making the kernel allocate an arbitrary size.
 */
#define FPC1020_MAX_XFER		SZ_64K

struct fpc1020_data {
	struct spi_device	*spi;
	struct gpio_desc	*reset_gpio;
	struct regulator	*vdd_supply;
	int			irq;
	u16			hw_id;
	atomic_t		irq_count;
	struct miscdevice	miscdev;
	struct mutex		lock; /* Serializes SPI transfers. */
	struct mutex		power_lock; /* Serializes power state and open count. */
	unsigned int		users;
	wait_queue_head_t	irq_wait;
	bool			irq_fired;
	bool			powered;
};

static int fpc1020_hw_reset(struct fpc1020_data *data)
{
	if (!data->reset_gpio)
		return 0;

	/* Active-low reset: set to 1 means asserting reset */
	gpiod_set_value_cansleep(data->reset_gpio, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(data->reset_gpio, 0);
	msleep(20);

	return 0;
}

static int fpc1020_power_on(struct fpc1020_data *data)
{
	int ret;

	if (data->powered)
		return 0;

	if (data->vdd_supply) {
		ret = regulator_enable(data->vdd_supply);
		if (ret < 0) {
			dev_warn(&data->spi->dev, "Failed to enable vdd: %d\n", ret);
			return ret;
		}
	}

	data->powered = true;
	fpc1020_hw_reset(data);

	if (data->irq > 0)
		enable_irq(data->irq);

	return 0;
}

static void fpc1020_power_off(struct fpc1020_data *data)
{
	if (!data->powered)
		return;

	if (data->irq > 0)
		disable_irq(data->irq);

	/* Put sensor in reset low-power state */
	if (data->reset_gpio)
		gpiod_set_value_cansleep(data->reset_gpio, 1);

	if (data->vdd_supply)
		regulator_disable(data->vdd_supply);

	data->powered = false;
}

static int fpc1020_read_hwid(struct fpc1020_data *data, u16 *id)
{
	u8 *tx;
	u8 *rx;
	struct spi_transfer t = {
		.len = 3,
	};
	bool was_off;
	int ret;

	/* SPI payloads must be DMA-safe, even for this short transfer. */
	tx = kzalloc(t.len, GFP_KERNEL);
	if (!tx)
		return -ENOMEM;
	rx = kzalloc(t.len, GFP_KERNEL);
	if (!rx) {
		kfree(tx);
		return -ENOMEM;
	}
	tx[0] = FPC1020_REG_HWID;
	t.tx_buf = tx;
	t.rx_buf = rx;

	mutex_lock(&data->power_lock);
	was_off = !data->powered;
	if (was_off) {
		ret = fpc1020_power_on(data);
		if (ret < 0) {
			mutex_unlock(&data->power_lock);
			goto free_bufs;
		}
	}

	mutex_lock(&data->lock);
	ret = spi_sync_transfer(data->spi, &t, 1);
	mutex_unlock(&data->lock);

	if (was_off)
		fpc1020_power_off(data);
	mutex_unlock(&data->power_lock);

	if (ret < 0) {
		dev_err(&data->spi->dev, "Failed to read HWID over SPI: %d\n", ret);
		goto free_bufs;
	}

	*id = (rx[1] << 8) | rx[2];
free_bufs:
	kfree(rx);
	kfree(tx);
	return ret;
}

static irqreturn_t fpc1020_irq_thread(int irq, void *dev_id)
{
	struct fpc1020_data *data = dev_id;

	atomic_inc(&data->irq_count);
	data->irq_fired = true;
	wake_up_interruptible(&data->irq_wait);

	dev_dbg(&data->spi->dev, "Finger touch interrupt triggered! (count=%d)\n",
		atomic_read(&data->irq_count));

	return IRQ_HANDLED;
}

static ssize_t hw_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);
	u16 id = 0;
	int ret;

	ret = fpc1020_read_hwid(data, &id);
	if (ret < 0)
		return ret;

	data->hw_id = id;
	return sysfs_emit(buf, "0x%04x\n", id);
}
static DEVICE_ATTR_RO(hw_id);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);
	bool was_off;
	int ret = 0;

	mutex_lock(&data->power_lock);
	was_off = !data->powered;
	if (was_off)
		ret = fpc1020_power_on(data);
	if (!ret)
		ret = fpc1020_hw_reset(data);
	if (was_off && data->powered)
		fpc1020_power_off(data);
	mutex_unlock(&data->power_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t irq_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&data->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t power_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", data->powered ? "on" : "off");
}
static DEVICE_ATTR_RO(power_state);

static struct attribute *fpc1020_attrs[] = {
	&dev_attr_hw_id.attr,
	&dev_attr_reset.attr,
	&dev_attr_irq_count.attr,
	&dev_attr_power_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpc1020);

static int fpc1020_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct fpc1020_data *data;
	int ret = 0;

	data = container_of(misc, struct fpc1020_data, miscdev);
	mutex_lock(&data->power_lock);
	if (!data->users)
		ret = fpc1020_power_on(data);
	if (!ret) {
		data->users++;
		file->private_data = data;
	}
	mutex_unlock(&data->power_lock);

	return ret;
}

static int fpc1020_release(struct inode *inode, struct file *file)
{
	struct fpc1020_data *data = file->private_data;

	mutex_lock(&data->power_lock);
	if (WARN_ON(!data->users)) {
		mutex_unlock(&data->power_lock);
		return 0;
	}

	data->users--;
	if (!data->users)
		fpc1020_power_off(data);
	mutex_unlock(&data->power_lock);

	return 0;
}

static ssize_t fpc1020_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct fpc1020_data *data = file->private_data;
	u8 *rx;
	int ret;

	if (!count)
		return 0;
	if (count > FPC1020_MAX_XFER)
		return -EMSGSIZE;

	rx = kmalloc(count, GFP_KERNEL);
	if (!rx)
		return -ENOMEM;

	mutex_lock(&data->lock);
	ret = spi_read(data->spi, rx, count);
	mutex_unlock(&data->lock);

	if (ret == 0) {
		if (copy_to_user(buf, rx, count))
			ret = -EFAULT;
		else
			ret = count;
	}

	kfree(rx);
	return ret;
}

static ssize_t fpc1020_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct fpc1020_data *data = file->private_data;
	u8 *tx;
	int ret;

	if (!count)
		return 0;
	if (count > FPC1020_MAX_XFER)
		return -EMSGSIZE;

	tx = memdup_user(buf, count);
	if (IS_ERR(tx))
		return PTR_ERR(tx);

	mutex_lock(&data->lock);
	ret = spi_write(data->spi, tx, count);
	mutex_unlock(&data->lock);

	kfree(tx);
	return (ret == 0) ? count : ret;
}

static __poll_t fpc1020_poll(struct file *file, struct poll_table_struct *wait)
{
	struct fpc1020_data *data = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &data->irq_wait, wait);
	if (data->irq_fired) {
		mask |= EPOLLIN | EPOLLRDNORM;
		data->irq_fired = false;
	}
	return mask;
}

static int fpc1020_message(struct fpc1020_data *data,
			   struct spi_ioc_transfer *u_xfers, unsigned int n_xfers)
{
	struct spi_message msg;
	struct spi_transfer *k_xfers;
	int status = 0;
	unsigned int i;
	int total = 0;

	if (n_xfers > 16)
		return -EINVAL;

	k_xfers = kcalloc(n_xfers, sizeof(*k_xfers), GFP_KERNEL);
	if (!k_xfers)
		return -ENOMEM;

	spi_message_init(&msg);

	for (i = 0; i < n_xfers; i++) {
		unsigned int len = u_xfers[i].len;

		if (len > FPC1020_MAX_XFER) {
			status = -EMSGSIZE;
			goto free_bufs;
		}

		k_xfers[i].len = len;
		total += len;

		if (u_xfers[i].tx_buf) {
			k_xfers[i].tx_buf = memdup_user((const void __user *)
							(uintptr_t)u_xfers[i].tx_buf, len);
			if (IS_ERR(k_xfers[i].tx_buf)) {
				status = PTR_ERR(k_xfers[i].tx_buf);
				k_xfers[i].tx_buf = NULL;
				goto free_bufs;
			}
		}

		if (u_xfers[i].rx_buf) {
			k_xfers[i].rx_buf = kzalloc(len, GFP_KERNEL);
			if (!k_xfers[i].rx_buf) {
				status = -ENOMEM;
				goto free_bufs;
			}
		}

		k_xfers[i].cs_change = !!u_xfers[i].cs_change;
		k_xfers[i].bits_per_word = u_xfers[i].bits_per_word;
		k_xfers[i].speed_hz = u_xfers[i].speed_hz;
		k_xfers[i].delay.value = u_xfers[i].delay_usecs;
		k_xfers[i].delay.unit = SPI_DELAY_UNIT_USECS;

		spi_message_add_tail(&k_xfers[i], &msg);
	}

	mutex_lock(&data->lock);
	status = spi_sync(data->spi, &msg);
	mutex_unlock(&data->lock);

	if (status == 0) {
		for (i = 0; i < n_xfers; i++) {
			if (u_xfers[i].rx_buf) {
				if (copy_to_user((void __user *)(uintptr_t)u_xfers[i].rx_buf,
						 k_xfers[i].rx_buf, u_xfers[i].len)) {
					status = -EFAULT;
					break;
				}
			}
		}
	}

free_bufs:
	for (i = 0; i < n_xfers; i++) {
		kfree(k_xfers[i].tx_buf);
		kfree(k_xfers[i].rx_buf);
	}
	kfree(k_xfers);

	return (status == 0) ? total : status;
}

static long fpc1020_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpc1020_data *data = file->private_data;
	int ret = 0;

	switch (cmd) {
	case FPC_IOC_RESET:
		/*
		 * Serialise against power changes, and do not toggle reset on a
		 * sensor that suspend has already powered down.
		 */
		mutex_lock(&data->power_lock);
		if (data->powered)
			fpc1020_hw_reset(data);
		mutex_unlock(&data->power_lock);
		return 0;

	case FPC_IOC_GET_HWID: {
		u16 hwid = 0;

		ret = fpc1020_read_hwid(data, &hwid);
		if (ret < 0)
			return ret;
		if (put_user(hwid, (__u16 __user *)arg))
			return -EFAULT;
		return 0;
	}

	default:
		/* Check for SPI_IOC_MESSAGE(N) */
		if (_IOC_TYPE(cmd) == SPI_IOC_MAGIC &&
		    _IOC_NR(cmd) == _IOC_NR(SPI_IOC_MESSAGE(0))) {
			unsigned int n_xfers = _IOC_SIZE(cmd) / sizeof(struct spi_ioc_transfer);
			struct spi_ioc_transfer *u_xfers;

			if (n_xfers == 0 || n_xfers > 16)
				return -EINVAL;

			u_xfers = memdup_user((void __user *)arg, _IOC_SIZE(cmd));
			if (IS_ERR(u_xfers))
				return PTR_ERR(u_xfers);

			ret = fpc1020_message(data, u_xfers, n_xfers);
			kfree(u_xfers);
			return ret;
		}
		return -ENOTTY;
	}
}

static const struct file_operations fpc1020_fops = {
	.owner		= THIS_MODULE,
	.open		= fpc1020_open,
	.release	= fpc1020_release,
	.read		= fpc1020_read,
	.write		= fpc1020_write,
	.unlocked_ioctl	= fpc1020_ioctl,
	.poll		= fpc1020_poll,
	.llseek		= noop_llseek,
};

static int fpc1020_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct fpc1020_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	mutex_init(&data->lock);
	mutex_init(&data->power_lock);
	init_waitqueue_head(&data->irq_wait);
	spi_set_drvdata(spi, data);

	/* 1. Regulator setup */
	data->vdd_supply = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(data->vdd_supply)) {
		ret = PTR_ERR(data->vdd_supply);
		if (ret != -ENODEV && ret != -ENOENT)
			return dev_err_probe(dev, ret, "Failed to get vdd supply\n");

		dev_info(dev, "No dedicated vdd supply specified in DT\n");
		data->vdd_supply = NULL;
	}

	/* 2. GPIO Reset line */
	data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(data->reset_gpio)) {
		ret = PTR_ERR(data->reset_gpio);
		dev_err(dev, "Failed to request reset GPIO: %d\n", ret);
		return ret;
	}

	/* 3. SPI bus configuration */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = 5000000;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "Failed to setup SPI device: %d\n", ret);
		return ret;
	}

	/* 4. Interrupt configuration */
	if (spi->irq > 0) {
		data->irq = spi->irq;
		irq_set_status_flags(data->irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(dev, data->irq, NULL,
						fpc1020_irq_thread,
						IRQF_TRIGGER_RISING | IRQF_ONESHOT,
						"fpc1020", data);
		if (ret < 0) {
			dev_warn(dev, "Failed to request IRQ %d: %d\n", data->irq, ret);
			data->irq = 0;
		} else {
			dev_info(dev, "Registered threaded IRQ on pin %d\n", data->irq);
		}
	}

	/* 5. Probe Hardware ID: power on, read, and power off */
	ret = fpc1020_read_hwid(data, &data->hw_id);
	if (ret == 0) {
		dev_info(dev, "Hardware ID read returned: 0x%04x\n", data->hw_id);
		if (data->hw_id == FPC1020_EXPECTED_HWID)
			dev_info(dev, "Identified FPC1020 sensor (0x%04x)\n",
				 data->hw_id);
		else if (data->hw_id == FPC1020_EXPECTED_HWID_LIUQIN ||
			 (data->hw_id & 0xff00) == 0x0200)
			dev_info(dev, "Identified FPC-compatible sensor (0x%04x)\n",
				 data->hw_id);
		else
			dev_warn(dev, "Unexpected HWID 0x%04x (expected 0x%04x)\n",
				 data->hw_id, FPC1020_EXPECTED_HWID);
	} else {
		dev_warn(dev, "SPI HWID read failed (err=%d)\n", ret);
	}

	/* 6. Misc device registration */
	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = "fpc1020";
	data->miscdev.fops = &fpc1020_fops;
	data->miscdev.parent = dev;

	ret = misc_register(&data->miscdev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register misc device\n");

	return 0;
}

static void fpc1020_remove(struct spi_device *spi)
{
	struct fpc1020_data *data = spi_get_drvdata(spi);

	misc_deregister(&data->miscdev);
	fpc1020_power_off(data);
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static const struct spi_device_id fpc1020_id[] = {
	{ "fpc1020", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, fpc1020_id);

static int __maybe_unused fpc1020_suspend(struct device *dev)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->power_lock);
	fpc1020_power_off(data);
	mutex_unlock(&data->power_lock);

	return 0;
}

static int __maybe_unused fpc1020_resume(struct device *dev)
{
	struct fpc1020_data *data = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&data->power_lock);
	if (data->users)
		ret = fpc1020_power_on(data);
	mutex_unlock(&data->power_lock);

	return ret;
}

static SIMPLE_DEV_PM_OPS(fpc1020_pm_ops, fpc1020_suspend, fpc1020_resume);

static struct spi_driver fpc1020_driver = {
	.driver = {
		.name = "fpc1264_spi_diag",
		.of_match_table = fpc1020_of_match,
		.dev_groups = fpc1020_groups,
		.pm = &fpc1020_pm_ops,
	},
	.probe = fpc1020_probe,
	.remove = fpc1020_remove,
	.id_table = fpc1020_id,
};

module_spi_driver(fpc1020_driver);

MODULE_AUTHOR("reisa");
MODULE_DESCRIPTION("FPC1020 SPI Fingerprint Sensor Driver with Power Management");
MODULE_LICENSE("GPL");
