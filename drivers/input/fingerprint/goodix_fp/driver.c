/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2020 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt)		KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <net/netlink.h>

#ifdef CONFIG_GOODIX_FP_DRM_EVENTS
#include <drm/drm_bridge.h>
#include <linux/msm_drm_notify.h>
#endif

#include "goodix_fp.h"

#define VER_MAJOR	1
#define VER_MINOR	2
#define PATCH_LEVEL	1

#define WAKELOCK_HOLD_TIME		2000	/* in ms */
#define FP_UNLOCK_REJECTION_TIMEOUT	(WAKELOCK_HOLD_TIME - 500)

#define GF_OF_DEV_NAME		"goodix,fingerprint"
#define GF_INPUT_NAME		"uinput-goodix"	/* "goodix_fp" */

#define GF_CHRDEV_NAME		"goodix_fp_spi"
#define GF_CLASS_NAME		"goodix_fp"

#define GF_MAX_DEVS		32	/* ... up to 256 */

static int gf_dev_major;

static DECLARE_BITMAP(minors, GF_MAX_DEVS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct wakeup_source fp_wakelock;

struct gf_key_map maps[] = {
	{EV_KEY, GF_KEY_INPUT_HOME},
	{EV_KEY, GF_KEY_INPUT_MENU},
	{EV_KEY, GF_KEY_INPUT_BACK},
	{EV_KEY, GF_KEY_INPUT_POWER},
#if defined(CONFIG_GOODIX_FP_NAV_EVENT)
	{EV_KEY, GF_NAV_INPUT_UP},
	{EV_KEY, GF_NAV_INPUT_DOWN},
	{EV_KEY, GF_NAV_INPUT_RIGHT},
	{EV_KEY, GF_NAV_INPUT_LEFT},
	{EV_KEY, GF_KEY_INPUT_CAMERA},
	{EV_KEY, GF_NAV_INPUT_CLICK},
	{EV_KEY, GF_NAV_INPUT_DOUBLE_CLICK},
	{EV_KEY, GF_NAV_INPUT_LONG_PRESS},
	{EV_KEY, GF_NAV_INPUT_HEAVY},
#endif
};

static void gf_enable_irq(struct gf_dev *gf_dev)
{
	if (!gf_dev->irq_enabled) {
		enable_irq(gf_dev->irq);
		gf_dev->irq_enabled = true;
	} else {
		dev_warn(gf_dev->dev, "IRQ has been already enabled\n");
	}
}

static void gf_disable_irq(struct gf_dev *gf_dev)
{
	if (gf_dev->irq_enabled) {
		gf_dev->irq_enabled = false;
		disable_irq(gf_dev->irq);
	} else {
		dev_warn(gf_dev->dev, "IRQ has been already disabled\n");
	}
}

#ifdef CONFIG_GOODIX_FP_CLK_CTRL
static long spi_clk_max_rate(struct clk *clk, unsigned long rate)
{
	long lowest_available, nearest_low, step_size, cur;
	long step_direction = -1;
	int max_steps = 10;
	long guess = rate;

	cur = clk_round_rate(clk, rate);
	if (cur == rate)
		return rate;

	/* if we got here then: cur > rate */
	lowest_available = clk_round_rate(clk, 0);
	if (lowest_available > rate)
		return -EINVAL;

	step_size = (rate - lowest_available) >> 1;
	nearest_low = lowest_available;

	while (max_steps-- && step_size) {
		guess += step_size * step_direction;
		cur = clk_round_rate(clk, guess);

		if ((cur < rate) && (cur > nearest_low))
			nearest_low = cur;
		/*
		 * if we stepped too far, then start stepping in the other
		 * direction with half the step size
		 */
		if (((cur > rate) && (step_direction > 0))
		    || ((cur < rate) && (step_direction < 0))) {
			step_direction = -step_direction;
			step_size >>= 1;
		}
	}

	return nearest_low;
}
#endif

static int gf_clk_enable(struct gf_dev *gf_dev)
{
#ifdef CONFIG_GOODIX_FP_CLK_CTRL
	int rc;

	if (gf_dev->clk_enabled)
		return 0;

	rc = clk_prepare_enable(gf_dev->core_clk);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to enable core_clk\n");
		return rc;
	}

	rc = clk_prepare_enable(gf_dev->iface_clk);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to enable iface_clk\n");
		clk_disable_unprepare(gf_dev->core_clk);
		return -ENOENT;
	}

	gf_dev->clk_enabled = true;
#endif
	return 0;
}

static int gf_clk_disable(struct gf_dev *gf_dev)
{
#ifdef CONFIG_GOODIX_FP_CLK_CTRL
	if (!gf_dev->clk_enabled)
		return 0;

	clk_disable_unprepare(gf_dev->core_clk);
	clk_disable_unprepare(gf_dev->iface_clk);
	gf_dev->clk_enabled = false;
#endif
	return 0;
}

static int gf_clk_init(struct gf_dev *gf_dev)
{
	int rc = 0;
#ifdef CONFIG_GOODIX_FP_CLK_CTRL
	long rate;

	gf_dev->core_clk = clk_get(gf_dev->dev, "core_clk");
	if (IS_ERR(gf_dev->core_clk)) {
		dev_err(gf_dev->dev, "failed to get core_clk\n");
		return PTR_ERR(gf_dev->core_clk);
	}

	gf_dev->iface_clk = clk_get(gf_dev->dev, "iface_clk");
	if (IS_ERR(gf_dev->iface_clk)) {
		dev_err(gf_dev->dev, "fail to get iface_clk\n");
		clk_put(gf_dev->core_clk);
		gf_dev->core_clk = NULL;
		return PTR_ERR(gf_dev->iface_clk);
	}

	rc = gf_clk_enable(gf_dev);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to enable clock\n");
		goto error_clk_enable;
	}

	rate = spi_clk_max_rate(gf_dev->core_clk, 1000000);
	if (rate < 0) {
		dev_err(gf_dev->dev,
			"no match found for requested clock frequency\n");
		rc = rate;
		goto error_clk_set;
	}

	rc = clk_set_rate(gf_dev->core_clk, rate);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to set clock rate\n");
		goto error_clk_set;
	}

error_clk_set:
	gf_clk_disable(gf_dev);
error_clk_enable:
	clk_put(gf_dev->iface_clk);
	clk_put(gf_dev->core_clk);
#endif
	return rc;
}

static int gf_clk_fini(struct gf_dev *gf_dev)
{
#ifdef CONFIG_GOODIX_FP_CLK_CTRL
	if (gf_dev->clk_enabled)
		gf_clk_disable(gf_dev);

	if (!IS_ERR_OR_NULL(gf_dev->core_clk)) {
		clk_put(gf_dev->core_clk);
		gf_dev->core_clk = NULL;
	}

	if (!IS_ERR_OR_NULL(gf_dev->iface_clk)) {
		clk_put(gf_dev->iface_clk);
		gf_dev->iface_clk = NULL;
	}
#endif
	return 0;
}

static void nav_event_input(struct gf_dev *gf_dev, gf_nav_event_t nav_event)
{
	uint32_t nav_input = 0;

	switch (nav_event) {
	case GF_NAV_FINGER_DOWN:
		dev_dbg(gf_dev->dev, "nav finger down\n");
		break;

	case GF_NAV_FINGER_UP:
		dev_dbg(gf_dev->dev, "nav finger up\n");
		break;

	case GF_NAV_DOWN:
		nav_input = GF_NAV_INPUT_DOWN;
		dev_dbg(gf_dev->dev, "nav down\n");
		break;

	case GF_NAV_UP:
		nav_input = GF_NAV_INPUT_UP;
		dev_dbg(gf_dev->dev, "nav up\n");
		break;

	case GF_NAV_LEFT:
		nav_input = GF_NAV_INPUT_LEFT;
		dev_dbg(gf_dev->dev, "nav left\n");
		break;

	case GF_NAV_RIGHT:
		nav_input = GF_NAV_INPUT_RIGHT;
		dev_dbg(gf_dev->dev, "nav right\n");
		break;

	case GF_NAV_CLICK:
		nav_input = GF_NAV_INPUT_CLICK;
		dev_dbg(gf_dev->dev, "nav click\n");
		break;

	case GF_NAV_HEAVY:
		nav_input = GF_NAV_INPUT_HEAVY;
		dev_dbg(gf_dev->dev, "nav heavy\n");
		break;

	case GF_NAV_LONG_PRESS:
		nav_input = GF_NAV_INPUT_LONG_PRESS;
		dev_dbg(gf_dev->dev, "nav long press\n");
		break;

	case GF_NAV_DOUBLE_CLICK:
		nav_input = GF_NAV_INPUT_DOUBLE_CLICK;
		dev_dbg(gf_dev->dev, "nav double click\n");
		break;

	default:
		dev_warn(gf_dev->dev, "unknown navigation event: %d\n",
			 nav_event);
		break;
	}

	if (nav_event != GF_NAV_FINGER_DOWN && nav_event != GF_NAV_FINGER_UP) {
		input_report_key(gf_dev->input, nav_input, 1);
		input_sync(gf_dev->input);
		input_report_key(gf_dev->input, nav_input, 0);
		input_sync(gf_dev->input);
	}
}

static void gf_kernel_key_input(struct gf_dev *gf_dev, struct gf_key *gf_key)
{
	uint32_t key_input = 0;

	if (GF_KEY_HOME == gf_key->key) {
		key_input = GF_KEY_INPUT_HOME;
	} else if (GF_KEY_POWER == gf_key->key) {
		key_input = GF_KEY_INPUT_POWER;
	} else if (GF_KEY_CAMERA == gf_key->key) {
		key_input = GF_KEY_INPUT_CAMERA;
	} else {
		/* add special key define */
		key_input = gf_key->key;
	}

	dev_dbg(gf_dev->dev, "received key event[%d], key=%d, value=%d\n",
		key_input, gf_key->key, gf_key->value);

	if ((GF_KEY_POWER == gf_key->key || GF_KEY_CAMERA == gf_key->key)
	    && (gf_key->value == 1)) {
		input_report_key(gf_dev->input, key_input, 1);
		input_sync(gf_dev->input);
		input_report_key(gf_dev->input, key_input, 0);
		input_sync(gf_dev->input);
	}

	if (GF_KEY_HOME == gf_key->key) {
		input_report_key(gf_dev->input, key_input, gf_key->value);
		input_sync(gf_dev->input);
	}
}

static int gf_hw_reset(struct gf_dev *gf_dev, unsigned int delay_ms)
{
	dev_info(gf_dev->dev, "performing HW reset\n");

	gpio_direction_output(gf_dev->reset_gpio, 0);
	mdelay(3);

	gpio_set_value(gf_dev->reset_gpio, 1);
	mdelay(delay_ms);

	return 0;
}

static int gf_set_power(struct gf_dev *gf_dev, bool enable)
{
	int rc = 0;

	/* No change? */
	if (!(gf_dev->avail ^ enable)) {
		dev_dbg(gf_dev->dev, "sensor has already powered-%s\n",
			enable ? "on" : "off");

		return 0;
	}

#ifdef CONFIG_GOODIX_FP_POWER_CTRL
	if (gpio_is_valid(gf_dev->pwr_gpio)) {
		rc = gpio_direction_output(gf_dev->pwr_gpio, enable ? 1 : 0);
		dev_info(gf_dev->dev, "set_power(%s) %s\n",
			 enable ? "on" : "off", !rc ? "succeeded" : "failed");
	}
	msleep(10);
#endif

	gf_dev->avail = enable;

	return rc;
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = filp->private_data;
	gf_nav_event_t nav_event __maybe_unused;
	void __user *uptr = (void __user *)arg;
	u8 netlink_route = NETLINK_GOODIX_FP;
	struct gf_ioc_chip_info info;
	struct gf_key gf_key;
	int retval = 0;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;

	if (_IOC_DIR(cmd) & _IOC_READ)
		retval = !access_ok(VERIFY_WRITE, uptr, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		retval = !access_ok(VERIFY_READ, uptr, _IOC_SIZE(cmd));
	if (retval)
		return -EFAULT;

	if (!gf_dev->avail) {
		switch (cmd) {
		case GF_IOC_ENABLE_POWER:
		case GF_IOC_DISABLE_POWER:
			dev_dbg(gf_dev->dev, "power cmd\n");
			break;
		default:
			dev_dbg(gf_dev->dev, "get cmd %d but sensor is powered off\n",
				 _IOC_NR(cmd));
			return -ENODEV;
		}
	}

	switch (cmd) {
	case GF_IOC_INIT:
		dev_dbg(gf_dev->dev, "GF_IOC_INIT\n");
		if (copy_to_user(uptr, &netlink_route, sizeof(u8)))
			return -EFAULT;
		break;
	case GF_IOC_EXIT:
		dev_dbg(gf_dev->dev, "GF_IOC_EXIT\n");
		break;
	case GF_IOC_DISABLE_IRQ:
		dev_dbg(gf_dev->dev, "GF_IOC_DISABEL_IRQ\n");
		gf_disable_irq(gf_dev);
		break;
	case GF_IOC_ENABLE_IRQ:
		dev_dbg(gf_dev->dev, "GF_IOC_ENABLE_IRQ\n");
		gf_enable_irq(gf_dev);
		break;
	case GF_IOC_RESET:
		dev_dbg(gf_dev->dev, "GF_IOC_RESET.\n");
		gf_hw_reset(gf_dev, 3);
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&gf_key, uptr, sizeof(gf_key)))
			return -EFAULT;
		gf_kernel_key_input(gf_dev, &gf_key);
		break;
	case GF_IOC_NAV_EVENT:
#if defined(CONFIG_GOODIX_FP_NAV_EVENT)
		dev_dbg(gf_dev->dev, "GF_IOC_NAV_EVENT\n");
		if (copy_from_user(&nav_event, uptr, sizeof(nav_event)))
			return -EFAULT;
		nav_event_input(gf_dev, nav_event);
#else
		dev_warn(gf_dev->dev, "navigation event is not enabled\n");
#endif
		break;
	case GF_IOC_ENABLE_SPI_CLK:
		gf_clk_enable(gf_dev);
		break;
	case GF_IOC_DISABLE_SPI_CLK:
		gf_clk_disable(gf_dev);
		break;
	case GF_IOC_ENABLE_POWER:
		dev_dbg(gf_dev->dev, "GF_IOC_ENABLE_POWER\n");
		gf_set_power(gf_dev, true);
		break;
	case GF_IOC_DISABLE_POWER:
		dev_dbg(gf_dev->dev, "GF_IOC_DISABLE_POWER\n");
		gf_set_power(gf_dev, false);
		break;
	case GF_IOC_ENTER_SLEEP_MODE:
		dev_dbg(gf_dev->dev, "GF_IOC_ENTER_SLEEP_MODE\n");
		break;
	case GF_IOC_GET_FW_INFO:
		dev_dbg(gf_dev->dev, "GF_IOC_GET_FW_INFO\n");
		break;
	case GF_IOC_REMOVE:
		dev_dbg(gf_dev->dev, "GF_IOC_REMOVE\n");
		break;
	case GF_IOC_CHIP_INFO:
		dev_dbg(gf_dev->dev, "GF_IOC_CHIP_INFO\n");
		if (copy_from_user(&info, uptr, sizeof(info)))
			return -EFAULT;
		dev_dbg(gf_dev->dev, "vendor_id : 0x%x\n", info.vendor_id);
		dev_dbg(gf_dev->dev, "mode : 0x%x\n", info.mode);
		dev_dbg(gf_dev->dev, "operation: 0x%x\n", info.operation);
		break;
	default:
		dev_warn(gf_dev->dev, "unsupported ioctl: 0x%x\n", cmd);
		break;
	}

	return retval;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /* CONFIG_COMPAT */

static irqreturn_t gf_irq(int irq, void *handle)
{
	struct gf_dev *gf_dev = handle;

	dev_info(gf_dev->dev, "interrupt received\n");

	__pm_wakeup_event(&fp_wakelock, WAKELOCK_HOLD_TIME);

	/* Sent netlink message */
	gf_sendnlmsg(GF_NET_EVENT_IRQ);

	if (gf_dev->wait_finger_down && gf_dev->avail && gf_dev->fb_black) {
		uint32_t key_input = KEY_RIGHT;

		input_report_key(gf_dev->input, key_input, 1);
		input_sync(gf_dev->input);
		input_report_key(gf_dev->input, key_input, 0);
		input_sync(gf_dev->input);

		gf_dev->wait_finger_down = false;
	}

	/* Send fasync notification */
	kill_fasync(&gf_dev->async, SIGIO, POLL_IN);

	return IRQ_HANDLED;
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;
	int rc;

	gf_dev = container_of(inode->i_cdev, struct gf_dev, cdev);

	/* Skip resource allocation except for first user */
	if (atomic_inc_return(&gf_dev->users) > 1)
		goto skip_init;

	/* request reset GPIO */
	rc = devm_gpio_request(gf_dev->dev, gf_dev->reset_gpio, "goodix_reset");
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to request RESET GPIO\n");
		return rc;
	}
	gpio_direction_output(gf_dev->reset_gpio, 0);

	/* request IRQ GPIO */
	rc = devm_gpio_request(gf_dev->dev, gf_dev->irq_gpio, "goodix_irq");
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to request IRQ GPIO\n");
		goto err_gpio_irq;
	}
	gpio_direction_input(gf_dev->irq_gpio);

	/* request IRQ */
	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
	rc = devm_request_threaded_irq(gf_dev->dev, gf_dev->irq, NULL, gf_irq,
				       IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				       "gf", gf_dev);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to register interrupt handler\n");
		goto err_irq;
	}

	/* disable IRQ that is enabled after request */
	disable_irq(gf_dev->irq);

	/* enable the interrupt to wake-up system */
	enable_irq_wake(gf_dev->irq);

	/* perform HW reset */
	gf_hw_reset(gf_dev, 3);

#ifndef CONFIG_GOODIX_FP_POWER_CTRL
	/* mark device available when power control is not enabled */
	gf_dev->avail = true;
#endif

skip_init:
	filp->private_data = gf_dev;
	nonseekable_open(inode, filp);

	dev_info(gf_dev->dev, "Succeed to open device\n");

	return 0;

err_irq:
	devm_gpio_free(gf_dev->dev, gf_dev->irq_gpio);
err_gpio_irq:
	devm_gpio_free(gf_dev->dev, gf_dev->reset_gpio);

	return rc;
}

static int gf_fasync(int fd, struct file *filp, int mode)
{
	struct gf_dev *gf_dev = filp->private_data;

	return fasync_helper(fd, filp, mode, &gf_dev->async);
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = filp->private_data;
	int status = 0;

	filp->private_data = NULL;

	/* De-init for last user */
	if (atomic_dec_return(&gf_dev->users) == 0) {
		/* Disable IRQ and release resources */
		gf_disable_irq(gf_dev);
		disable_irq_wake(gf_dev->irq);
		devm_free_irq(gf_dev->dev, gf_dev->irq, gf_dev);
		devm_gpio_free(gf_dev->dev, gf_dev->irq_gpio);
		devm_gpio_free(gf_dev->dev, gf_dev->reset_gpio);

		/* Power off the sensor */
		gf_set_power(gf_dev, false);
	}

	return status;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif /*CONFIG_COMPAT */
	.open = gf_open,
	.release = gf_release,
	.fasync = gf_fasync,
};

#ifdef CONFIG_GOODIX_FP_DRM_EVENTS
static int gf_drm_notify(struct notifier_block *nb, unsigned long val,
			 void *data)
{
	struct msm_drm_notifier *evdata = data;
	struct gf_dev *gf_dev;
	unsigned int blank;

	if (val != MSM_DRM_EVENT_BLANK)
		return NOTIFY_DONE;

	gf_dev = container_of(nb, struct gf_dev, notifier);

	dev_dbg(gf_dev->dev, "DRM notification with value: %lu\n", val);

	if (evdata && evdata->data) {
		blank = *(int *)(evdata->data);

		switch (blank) {
		case MSM_DRM_BLANK_POWERDOWN:
			if (gf_dev->avail) {
				dev_info(gf_dev->dev,
					 "received MSM_DRM_BLANK_POWERDOWN\n");

				gf_dev->fb_black = true;
				gf_dev->wait_finger_down = true;

				/* Send netlink message */
				gf_sendnlmsg(GF_NET_EVENT_FB_BLACK);

				/* Send fasync notification */
				kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
			}
			break;
		case MSM_DRM_BLANK_UNBLANK:
			if (gf_dev->avail) {
				dev_info(gf_dev->dev,
					 "received MSM_DRM_BLANK_UNBLANK\n");

				gf_dev->fb_black = false;

				/* Send netlink message */
				gf_sendnlmsg(GF_NET_EVENT_FB_UNBLACK);

				/* Send fasync notification */
				kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
			}
			break;
		default:
			dev_dbg(gf_dev->dev, "received blank: %u\n", blank);
			break;
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
	.notifier_call = gf_drm_notify,
};
#endif

#ifdef CONFIG_GOODIX_FP_PROXIMITY_STATE
static ssize_t proximity_state_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct gf_dev *gf_dev = dev_get_drvdata(dev);
	int rc, val;

	rc = kstrtoint(buf, 10, &val);
	if (rc)
		return -EINVAL;

	gf_dev->proximity_state = !!val;

	if (gf_dev->proximity_state)
		gf_disable_irq(gf_dev);
	else
		gf_enable_irq(gf_dev);

	return count;
}

static DEVICE_ATTR_WO(proximity_state);

static struct attribute *attributes[] = {
	&dev_attr_proximity_state.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};
#endif

static struct class *gf_class;

static int gf_add_cdev(struct gf_dev *gf_dev)
{
	struct device *classdev;
	unsigned long minor;
	int rc = -ENODEV;

	/* Get first available minor */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, GF_MAX_DEVS);
	if (minor < GF_MAX_DEVS)
		set_bit(minor, minors);
	mutex_unlock(&device_list_lock);

	if (minor == GF_MAX_DEVS) {
		dev_err(gf_dev->dev, "no minor number available\n");
		return -ENODEV;
	}

	/* Initialize char device structure */
	cdev_init(&gf_dev->cdev, &gf_fops);
	gf_dev->cdev.owner = THIS_MODULE;

	/* Add the char device to system */
	rc = cdev_add(&gf_dev->cdev, MKDEV(gf_dev_major, minor), 1);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to add char device to system\n");
		goto error_cdev;
	}

	/* Create class device */
	classdev = device_create(gf_class, gf_dev->dev,
				 MKDEV(gf_dev_major, minor), gf_dev,
				 GF_DEV_NAME);
	if (IS_ERR(classdev)) {
		dev_err(gf_dev->dev, "failed to create class device\n");
		rc = PTR_ERR(classdev);
		goto error_class;
	}

	mutex_lock(&device_list_lock);
	list_add(&gf_dev->device_entry, &device_list);
	mutex_unlock(&device_list_lock);

	return 0;

error_class:
	cdev_del(&gf_dev->cdev);
error_cdev:
	clear_bit(minor, minors);

	return rc;
}

static void gf_del_cdev(struct gf_dev *gf_dev)
{
	/* Remove device from the list */
	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	mutex_unlock(&device_list_lock);

	/* Free minor number */
	clear_bit(MINOR(gf_dev->cdev.dev), minors);

	/* Destroy device */
	device_destroy(gf_class, gf_dev->cdev.dev);

	/* Delete char device structure from system */
	cdev_del(&gf_dev->cdev);
}

static int gf_add_input(struct gf_dev *gf_dev)
{
	int rc, i;

	/* Allocate associated input device */
	gf_dev->input = input_allocate_device();
	if (!gf_dev->input) {
		dev_err(gf_dev->dev, "failed to allocate input device\n");

		return -ENOMEM;
	}

	/* Set input device name */
	gf_dev->input->name = GF_INPUT_NAME;

	/* Set capabilities */
	for (i = 0; i < ARRAY_SIZE(maps); i++)
		input_set_capability(gf_dev->input, maps[i].type, maps[i].code);

	/* Register input device */
	rc = input_register_device(gf_dev->input);
	if (rc) {
		dev_err(gf_dev->dev, "failed to register input device\n");
		input_free_device(gf_dev->input);
	}

	return rc;
}

static void gf_del_input(struct gf_dev *gf_dev)
{
	/* Unregister input device from system */
	input_unregister_device(gf_dev->input);

	/* Free input device */
	input_free_device(gf_dev->input);
}

static int gf_parse_dts(struct gf_dev *gf_dev)
{
	int rc;

#ifdef CONFIG_GOODIX_FP_POWER_CTRL
	/* get pwr resource */
	rc = of_get_named_gpio(gf_dev->dev->of_node, "fp-gpio-pwr", 0);
	if (gpio_is_valid(rc)) {
		gf_dev->pwr_gpio = rc;

		rc = devm_gpio_request(gf_dev->dev, gf_dev->pwr_gpio,
				       "goodix_pwr");
		if (rc < 0) {
			dev_err(gf_dev->dev, "failed to request PWR GPIO\n");
			return rc;
		}
	} else {
		gf_dev->pwr_gpio = -1; /* do not use pwr gpio */
	}
#endif

	/* get reset resource */
	rc = of_get_named_gpio(gf_dev->dev->of_node, "goodix,gpio-reset", 0);
	if (!gpio_is_valid(rc)) {
		dev_err(gf_dev->dev, "RESET GPIO is invalid\n");
		return rc;
	}
	gf_dev->reset_gpio = rc;

	/* get irq resourece */
	rc = of_get_named_gpio(gf_dev->dev->of_node, "goodix,gpio-irq", 0);
	if (!gpio_is_valid(rc)) {
		dev_info(gf_dev->dev, "IRQ GPIO is invalid\n");
		return rc;
	}
	gf_dev->irq_gpio = rc;

	return 0;
}

int gf_probe_common(struct device *dev)
{
	struct gf_dev *gf_dev;
	int rc;

	/* Allocate device instance */
	gf_dev = devm_kzalloc(dev, sizeof(struct gf_dev), GFP_KERNEL);
	if (!gf_dev) {
		dev_err(dev, "failed to allocate device data\n");
		return -ENOMEM;
	}

	dev_set_drvdata(dev, gf_dev);

	/* Initialize the driver data */
	INIT_LIST_HEAD(&gf_dev->device_entry);
	gf_dev->dev = dev;
	gf_dev->irq_gpio = -EINVAL;
	gf_dev->reset_gpio = -EINVAL;
	gf_dev->pwr_gpio = -EINVAL;
	atomic_set(&gf_dev->users, 0);

	rc = gf_parse_dts(gf_dev);
	if (rc < 0)
		return rc;

	/* Create and associate char device */
	rc = gf_add_cdev(gf_dev);
	if (rc < 0)
		return rc;

	/* Create and associate input device */
	rc = gf_add_input(gf_dev);
	if (rc < 0)
		goto error_input;

	/* Initialize and enable SPI clock */
	if (gf_clk_init(gf_dev))
		goto error_clk_init;

#ifdef CONFIG_GOODIX_FP_DRM_EVENTS
	gf_dev->notifier = goodix_noti_block;
	rc = msm_drm_register_client(&gf_dev->notifier);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to register DRM client\n");
		goto error_drm_reg;
	}
#endif

#ifdef CONFIG_GOODIX_FP_PROXIMITY_STATE
	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc < 0) {
		dev_err(gf_dev->dev, "failed to create sysfs node\n");
		goto error_sysfs;
	}
#endif

	wakeup_source_init(&fp_wakelock, "fp_wakelock");

	dev_dbg(gf_dev->dev, "version V%d.%d.%02d\n", VER_MAJOR, VER_MINOR,
		PATCH_LEVEL);

	return rc;

#ifdef CONFIG_GOODIX_FP_PROXIMITY_STATE
error_sysfs:
#endif
#ifdef CONFIG_GOODIX_FP_DRM_EVENTS
	msm_drm_unregister_client(&gf_dev->notifier);
error_drm_reg:
#endif
	gf_clk_fini(gf_dev);
error_clk_init:
	gf_del_input(gf_dev);
error_input:
	gf_del_cdev(gf_dev);

	return rc;
}

int gf_remove_common(struct device *dev)
{
	struct gf_dev *gf_dev = dev_get_drvdata(dev);

	wakeup_source_trash(&fp_wakelock);

	/* Disable interrupt and wake-up ability */
	gf_disable_irq(gf_dev);
	disable_irq_wake(gf_dev->irq);

	/* Unregister and delete associated input device */
	gf_del_input(gf_dev);

	/* Unregister and delete associated char device */
	gf_del_cdev(gf_dev);

#ifdef CONFIG_GOODIX_FP_DRM_EVENTS
	/* Unregister DRM notifier */
	msm_drm_unregister_client(&gf_dev->notifier);
#endif

	return 0;
}

static struct of_device_id gf_match_table[] = {
	{ .compatible = GF_OF_DEV_NAME },
	{ },
};

static int __init gf_init(void)
{
	dev_t dev;
	int rc;

	/* Allocate chardev region and assign major number */
	BUILD_BUG_ON(GF_MAX_DEVS > 256);
	rc = alloc_chrdev_region(&dev, 0, GF_MAX_DEVS, GF_CHRDEV_NAME);
	if (rc < 0) {
		pr_err("failed to alloc char device region\n");
		return rc;
	}
	gf_dev_major = MAJOR(dev);

	/* Create class */
	gf_class = class_create(THIS_MODULE, GF_CLASS_NAME);
	if (IS_ERR(gf_class)) {
		pr_err("failed to create device class\n");
		rc = PTR_ERR(gf_class);
		goto error_class;
	}

	/* Register platform driver */
	rc = gf_register_platform_driver(gf_match_table);
	if (rc < 0) {
		pr_err("failed to register platform driver\n");
		goto error_plat;
	}

	/* Register SPI driver */
	rc = gf_register_spi_driver(gf_match_table);
	if (rc < 0) {
		pr_err("failed to register SPI driver\n");
		goto error_spi;
	}

	/* Initialize netlink interface */
	rc = gf_netlink_init();
	if (rc < 0) {
		pr_err("failed to initialize netlink\n");
		goto error_netlink;
	}

	pr_debug("initialization successfully done\n");

	return 0;

error_netlink:
	gf_unregister_spi_driver();
error_spi:
	gf_unregister_platform_driver();
error_plat:
	class_destroy(gf_class);
error_class:
	unregister_chrdev_region(MKDEV(gf_dev_major, 0), GF_MAX_DEVS);

	return rc;
}

module_init(gf_init);

static void __exit gf_exit(void)
{
	gf_netlink_exit();
	gf_unregister_spi_driver();
	gf_unregister_platform_driver();
	class_destroy(gf_class);
	unregister_chrdev_region(MKDEV(gf_dev_major, 0), GF_MAX_DEVS);
}

module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
