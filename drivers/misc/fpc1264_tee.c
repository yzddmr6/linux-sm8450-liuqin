// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC1264 normal-world power/reset/IRQ control for OEM TEE clients.
 * SPI transfers and hardware identification belong to the trusted application.
 * The fpc1020 device name preserves the existing OEM client interface.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

struct fpc1264_tee {
	struct kref refcount;
	struct device *dev;
	struct gpio_desc *reset;
	struct regulator *vdd;
	struct miscdevice miscdev;
	struct mutex lock;
	wait_queue_head_t irq_wait;
	atomic_t irq_count;
	unsigned int users;
	int irq;
	bool powered;
	bool irq_enabled;
	bool irq_pending;
	bool removed;
};

static void fpc1264_tee_release_data(struct kref *ref)
{
	struct fpc1264_tee *data = container_of(ref, struct fpc1264_tee, refcount);

	kfree(data);
}

static void fpc1264_tee_put(void *ptr)
{
	struct fpc1264_tee *data = ptr;

	kref_put(&data->refcount, fpc1264_tee_release_data);
}

/* Caller holds lock; a failed disable retains the regulator reference. */
static int fpc1264_tee_power_off(struct fpc1264_tee *data)
{
	int ret;

	if (data->irq_enabled) {
		disable_irq(data->irq);
		data->irq_enabled = false;
	}
	gpiod_set_value_cansleep(data->reset, 1);
	if (!data->powered)
		return 0;
	ret = regulator_disable(data->vdd);
	if (ret)
		return ret;
	data->powered = false;
	return 0;
}

static int fpc1264_tee_power_on(struct fpc1264_tee *data)
{
	int ret;

	if (!data->powered) {
		ret = regulator_enable(data->vdd);
		if (ret)
			return ret;
		data->powered = true;
	}
	gpiod_set_value_cansleep(data->reset, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(data->reset, 0);
	msleep(20);
	WRITE_ONCE(data->irq_pending, false);
	if (!data->irq_enabled) {
		enable_irq(data->irq);
		data->irq_enabled = true;
	}
	return 0;
}

static irqreturn_t fpc1264_tee_irq(int irq, void *ptr)
{
	struct fpc1264_tee *data = ptr;

	atomic_inc(&data->irq_count);
	WRITE_ONCE(data->irq_pending, true);
	wake_up_interruptible(&data->irq_wait);
	return IRQ_HANDLED;
}

static int fpc1264_tee_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct fpc1264_tee *data = container_of(misc, struct fpc1264_tee, miscdev);
	int ret = 0;

	mutex_lock(&data->lock);
	if (data->removed)
		ret = -ENODEV;
	else if (!data->users)
		ret = fpc1264_tee_power_on(data);
	if (!ret) {
		kref_get(&data->refcount);
		data->users++;
		file->private_data = data;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int fpc1264_tee_release(struct inode *inode, struct file *file)
{
	struct fpc1264_tee *data = file->private_data;
	int ret = 0;

	mutex_lock(&data->lock);
	data->users--;
	if (!data->users && !data->removed)
		ret = fpc1264_tee_power_off(data);
	mutex_unlock(&data->lock);
	fpc1264_tee_put(data);
	return ret;
}

static __poll_t fpc1264_tee_poll(struct file *file, poll_table *wait)
{
	struct fpc1264_tee *data = file->private_data;

	poll_wait(file, &data->irq_wait, wait);
	if (READ_ONCE(data->removed))
		return EPOLLERR | EPOLLHUP;
	if (xchg(&data->irq_pending, false))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations fpc1264_tee_fops = {
	.owner = THIS_MODULE,
	.open = fpc1264_tee_open,
	.release = fpc1264_tee_release,
	.poll = fpc1264_tee_poll,
	.llseek = noop_llseek,
};

static ssize_t power_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fpc1264_tee *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(data->powered) ? "on" : "off");
}
static DEVICE_ATTR_RO(power_state);

static ssize_t irq_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct fpc1264_tee *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&data->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static struct attribute *fpc1264_tee_attrs[] = {
	&dev_attr_power_state.attr,
	&dev_attr_irq_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpc1264_tee);

static int fpc1264_tee_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpc1264_tee *data;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	kref_init(&data->refcount);
	/* The probe ref is dropped after IRQ, GPIO and regulator devres. */
	ret = devm_add_action_or_reset(dev, fpc1264_tee_put, data);
	if (ret)
		return ret;
	data->dev = dev;
	mutex_init(&data->lock);
	init_waitqueue_head(&data->irq_wait);
	atomic_set(&data->irq_count, 0);
	platform_set_drvdata(pdev, data);

	data->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(data->vdd))
		return dev_err_probe(dev, PTR_ERR(data->vdd), "Failed to get sensor supply\n");
	data->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset))
		return dev_err_probe(dev, PTR_ERR(data->reset), "Failed to get reset GPIO\n");
	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0)
		return data->irq;
	ret = devm_request_threaded_irq(dev, data->irq, NULL, fpc1264_tee_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(dev), data);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request sensor IRQ\n");

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = "fpc1020";
	data->miscdev.fops = &fpc1264_tee_fops;
	data->miscdev.parent = dev;
	data->miscdev.mode = 0600;
	ret = misc_register(&data->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register sensor control device\n");
	return 0;
}

static void fpc1264_tee_remove(struct platform_device *pdev)
{
	struct fpc1264_tee *data = platform_get_drvdata(pdev);
	int ret;

	misc_deregister(&data->miscdev);
	mutex_lock(&data->lock);
	ret = fpc1264_tee_power_off(data);
	if (ret)
		dev_err(data->dev, "Failed to disable sensor supply: %d\n", ret);
	WRITE_ONCE(data->removed, true);
	mutex_unlock(&data->lock);
	wake_up_interruptible(&data->irq_wait);
}

static int __maybe_unused fpc1264_tee_suspend(struct device *dev)
{
	struct fpc1264_tee *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	ret = fpc1264_tee_power_off(data);
	mutex_unlock(&data->lock);
	return ret;
}

static int __maybe_unused fpc1264_tee_resume(struct device *dev)
{
	struct fpc1264_tee *data = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&data->lock);
	if (data->users)
		ret = fpc1264_tee_power_on(data);
	mutex_unlock(&data->lock);
	return ret;
}

static SIMPLE_DEV_PM_OPS(fpc1264_tee_pm, fpc1264_tee_suspend, fpc1264_tee_resume);

static const struct of_device_id fpc1264_tee_of_match[] = {
	{ .compatible = "fpc,fpc1264-tee" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1264_tee_of_match);

static struct platform_driver fpc1264_tee_driver = {
	.driver = {
		.name = "fpc1264_tee",
		.of_match_table = fpc1264_tee_of_match,
		.dev_groups = fpc1264_tee_groups,
		.pm = &fpc1264_tee_pm,
	},
	.probe = fpc1264_tee_probe,
	.remove = fpc1264_tee_remove,
};
module_platform_driver(fpc1264_tee_driver);

MODULE_AUTHOR("reisa");
MODULE_DESCRIPTION("FPC1264 GPIO and power control for OEM TEE clients");
MODULE_LICENSE("GPL");
