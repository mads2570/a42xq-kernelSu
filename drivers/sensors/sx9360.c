/*
 * Copyright (C) 2018 Semtech Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/pm_wakeup.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/sensor/sensors_core.h>
#include "sx9360_reg.h"
#if IS_ENABLED(CONFIG_CCIC_NOTIFIER)
#include <linux/usb/typec/common/pdic_notifier.h>
#endif
#if IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
#include <linux/usb/typec/manager/usb_typec_manager_notifier.h>
#endif

#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
#include <linux/vbus_notifier.h>
#endif

#define VENDOR_NAME              "SEMTECH"
#define MODEL_NAME               "SX9360"
#define MODULE_NAME              "grip_sensor"

#define I2C_M_WR                 0 /* for i2c Write */
#define I2c_M_RD                 1 /* for i2c Read */

#define IDLE                     0
#define ACTIVE                   1

#define SX9360_MODE_SLEEP        0
#define SX9360_MODE_NORMAL       1

#define DIFF_READ_NUM            10
#define GRIP_LOG_TIME            15 /* 30 sec */
#define ZERO_DETECT_TIME         5 /* 10 sec */

/* CS Main */
#define ENABLE_CSX               0x03
#define REFERENCE_DISABLE        0x02

#define CSX_STATUS_REG           SX9360_STAT_PROXSTAT_FLAG

#define IRQ_PROCESS_CONDITION   (SX9360_IRQSTAT_TOUCH_FLAG	\
				| SX9360_IRQSTAT_RELEASE_FLAG	\
				| SX9360_IRQSTAT_COMPDONE_FLAG)

#if defined(CONFIG_FOLDER_HALL)
#define HALLIC_PATH		"/sys/class/sec/sec_flip/flipStatus"
#else
#define HALLIC_PATH		"/sys/class/sec/hall_ic/hall_detect"
#endif

#if defined(CONFIG_FLIP_COVER_DETECTOR_FACTORY)
#define HALLIC_CERT_PATH	"/sys/class/sensors/flip_cover_detector_sensor/nfc_cover_status"
#else
#define HALLIC_CERT_PATH	"/sys/class/sec/hall_ic/certify_hall_detect"
#endif

struct sx9360_p {
	struct i2c_client *client;
	struct input_dev *input;
	struct device *factory_device;
	struct delayed_work init_work;
	struct delayed_work irq_work;
	struct delayed_work debug_work;
	struct wakeup_source *grip_ws;
	struct mutex mode_mutex;
	struct mutex read_mutex;
#if IS_ENABLED(CONFIG_CCIC_NOTIFIER)
	struct notifier_block ccic_nb;
#endif
#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
	struct notifier_block vbus_nb;
#endif

	bool skip_data;

	int irq;
	int gpio_nirq;
	int state;
	int init_done;

	atomic_t enable;
#ifdef CONFIG_SUPPORT_CAMERA_FREEFALL
	int poll_delay;
#endif

	int again_m;
	int dgain_m;

	u16 detect_threshold;
	u16 offset;
	s32 capMain;
	s32 useful;
	s16 avg;
	s16 diff;

	s16 diff_avg;
	int diff_cnt;
	s32 useful_avg;

	int irq_count;
	int abnormal_mode;
	s16 max_diff;
	s16 max_normal_diff;

	int debug_count;
	int debug_zero_count;
	char hall_ic[6];

	u32 hallic_cert_detect;
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	int mcc;
	u8 default_threshold;
	u8 mcc_threshold;
#endif
};

static int sx9360_check_hallic_state(char *file_path, char hall_ic_status[])
{
	int iRet = 0;
	mm_segment_t old_fs;
	struct file *filep;
	char hall_sysfs[5];

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	filep = filp_open(file_path, O_RDONLY, 0440);
	if (IS_ERR(filep)) {
		iRet = PTR_ERR(filep);
		GRIP_ERR("file open fail %s(%d)\n", file_path, iRet);
		set_fs(old_fs);
		return iRet;
	}

	iRet = filep->f_op->read(filep, hall_sysfs,
				 sizeof(hall_sysfs), &filep->f_pos);

	if (iRet <= 0) {
		GRIP_ERR("file read fail %d\n", iRet);
		filp_close(filep, current->files);
		set_fs(old_fs);
		return -EIO;
	} else {
		strncpy(hall_ic_status, hall_sysfs, sizeof(hall_sysfs));
	}

	filp_close(filep, current->files);
	set_fs(old_fs);

	return iRet;
}

static int sx9360_get_nirq_state(struct sx9360_p *data)
{
	return gpio_get_value_cansleep(data->gpio_nirq);
}

static int sx9360_i2c_write(struct sx9360_p *data, u8 reg_addr, u8 buf)
{
	int ret;
	struct i2c_msg msg;
	unsigned char w_buf[2];

	w_buf[0] = reg_addr;
	w_buf[1] = buf;

	msg.addr = data->client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 2;
	msg.buf = (char *)w_buf;

	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret < 0)
		GRIP_ERR("i2c write error %d\n", ret);

	return 0;
}

static int sx9360_i2c_read(struct sx9360_p *data, u8 reg_addr, u8 *buf)
{
	int ret;
	struct i2c_msg msg[2];

	msg[0].addr = data->client->addr;
	msg[0].flags = I2C_M_WR;
	msg[0].len = 1;
	msg[0].buf = &reg_addr;

	msg[1].addr = data->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = buf;

	ret = i2c_transfer(data->client->adapter, msg, 2);
	if (ret < 0)
		GRIP_ERR("i2c read error %d\n", ret);

	return ret;
}

static u8 sx9360_read_irqstate(struct sx9360_p *data)
{
	u8 val = 0;

	if (sx9360_i2c_read(data, SX9360_IRQSTAT_REG, &val) >= 0)
		return (val & 0xFF);

	return 0;
}

static void sx9360_initialize_register(struct sx9360_p *data)
{
	u8 val = 0;
	unsigned int idx;

	for (idx = 0; idx < (int)(sizeof(setup_reg) >> 1); idx++) {
		sx9360_i2c_write(data, setup_reg[idx].reg, setup_reg[idx].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[idx].reg, setup_reg[idx].val);

		sx9360_i2c_read(data, setup_reg[idx].reg, &val);
		GRIP_INFO("Read Reg: 0x%x Value: 0x%x\n\n",
			  setup_reg[idx].reg, val);
	}

	sx9360_i2c_read(data, SX9360_PROXCTRL5_REG, &val);
	data->detect_threshold = (u16)val * (u16)val / 2;

	sx9360_i2c_read(data, SX9360_PROXCTRL4_REG, &val);
	val = (val & 0x30) >> 4;

	if (val)
		data->detect_threshold += data->detect_threshold >> (5 - val);

	GRIP_INFO("detect threshold: %u\n", data->detect_threshold);

	data->init_done = ON;
}

static void sx9360_initialize_chip(struct sx9360_p *data)
{
	int cnt = 0;

	while ((sx9360_get_nirq_state(data) == 0) && (cnt++ < 10)) {
		sx9360_read_irqstate(data);
		msleep(20);
	}

	if (cnt >= 10)
		GRIP_ERR("s/w reset fail(%d)\n", cnt);

	sx9360_initialize_register(data);
}

static int sx9360_set_offset_calibration(struct sx9360_p *data)
{
	int ret = 0;

	GRIP_INFO("\n");
	ret = sx9360_i2c_write(data, SX9360_STAT_REG,
			       SX9360_STAT_COMPSTAT_ALL_FLAG);

	return ret;
}

static void sx9360_send_event(struct sx9360_p *data, u8 state)
{
	if (data->skip_data == true) {
		GRIP_INFO("skip grip event\n");
		return;
	}

	if (state == ACTIVE) {
		data->state = ACTIVE;
		GRIP_INFO("touched\n");
	} else {
		data->state = IDLE;
		GRIP_INFO("released\n");
	}

	if (state == ACTIVE)
		input_report_rel(data->input, REL_MISC, 1);
	else
		input_report_rel(data->input, REL_MISC, 2);

	input_sync(data->input);
}

static void sx9360_display_data_reg(struct sx9360_p *data)
{
	u8 val, reg;

	GRIP_INFO("############# %d reference #############\n", 0);
	for (reg = SX9360_REGUSEMSBPHR; reg <= SX9360_REGOFFSETLSBPHR; reg++) {
		sx9360_i2c_read(data, reg, &val);
		GRIP_INFO("Register(0x%2x) data(0x%2x)\n", reg, val);
	}
	GRIP_INFO("############# %d Main #############\n", 0);
	for (reg = SX9360_REGUSEMSBPHM; reg <= SX9360_REGOFFSETLSBPHM; reg++) {
		sx9360_i2c_read(data, reg, &val);
		GRIP_INFO("Register(0x%2x) data(0x%2x)\n",
			  reg, val);
	}
}

static void sx9360_get_gain(struct sx9360_p *data)
{
	u8 msByte;
	static const int again_phm[] = {7500, 22500, 37500, 52500, 60000, 75000, 90000, 105000};
	sx9360_i2c_read(data, SX9360_AFEPARAM1PHM_REG, &msByte);
	msByte = (msByte >> 4) & 0x07;
	data->again_m = again_phm[msByte];

	sx9360_i2c_read(data, SX9360_PROXCTRL0PHM_REG, &msByte);
	msByte = (msByte >> 3) & 0x07;
	if (msByte)
		data->dgain_m = 1 << (msByte - 1);
	else
		data->dgain_m = 1;
}


static void sx9360_get_data(struct sx9360_p *data)
{
	u8 msByte = 0;
	u8 lsByte = 0;
	u16 offset = 0;
	s32 capMain = 0, useful = 0;
	s16 avg = 0, diff = 0;
	s16 retry = 0;
	u8 convstat = 0;

	mutex_lock(&data->read_mutex);

	sx9360_get_gain(data);

	while (1) {
		sx9360_i2c_read(data, SX9360_STAT_REG, &convstat);
		convstat &= 0x01;

		if (++retry > 5 || convstat == 0)
			break;

		usleep_range(10000, 11000);
	}
	GRIP_INFO("retry : %d, CONVSTAT : %u\n", retry, convstat);

	/* diff read */
	sx9360_i2c_read(data, SX9360_REGDIFFMSBPHM, &msByte);
	sx9360_i2c_read(data, SX9360_REGDIFFLSBPHM, &lsByte);

	diff = (s16)msByte;
	diff = (diff << 8) | ((s16)lsByte);

	/* Calculate out the Main Cap information */
	sx9360_i2c_read(data, SX9360_REGUSEMSBPHM, &msByte);
	sx9360_i2c_read(data, SX9360_REGUSELSBPHM, &lsByte);

	useful = (s32)msByte;
	useful = (useful << 8) | ((s32)lsByte);
	if (useful > 32767)
		useful -= 65536;

	sx9360_i2c_read(data, SX9360_REGOFFSETMSBPHM, &msByte);
	sx9360_i2c_read(data, SX9360_REGOFFSETLSBPHM, &lsByte);

	offset = (u16)msByte;
	offset = (offset << 8) | ((u16)lsByte);

	msByte = (u8)((offset >> 7) & 0x7F);
	lsByte = (u8)((offset)      & 0x7F);

	capMain = (((s32)msByte * 30000) + ((s32)lsByte * 500)) +
		  (s32)(((s64)useful * data->again_m) / (data->dgain_m * 32768));

	/* avg read */
	sx9360_i2c_read(data, SX9360_REGAVGMSBPHM, &msByte);
	sx9360_i2c_read(data, SX9360_REGAVGLSBPHM, &lsByte);

	avg = (s16)msByte;
	avg = (avg << 8) | ((s16)lsByte);

	data->useful = useful;
	data->offset = offset;
	data->capMain = capMain;
	data->avg = avg;
	data->diff = diff;

	mutex_unlock(&data->read_mutex);

	GRIP_INFO("capMain: %ld, useful: %ld, avg: %d, diff: %d, Offset: %u\n",
		  (long int)capMain, (long int)useful, avg, diff, offset);
}

static int sx9360_set_mode(struct sx9360_p *data, unsigned char mode)
{
	int ret = -EINVAL;

	GRIP_INFO("%u\n", mode);

	mutex_lock(&data->mode_mutex);
	if (mode == SX9360_MODE_SLEEP) {
		ret = sx9360_i2c_write(data, SX9360_GNRLCTRL0_REG, SX9360_GNRLCTRL0_VAL_PHOFF);
	} else if (mode == SX9360_MODE_NORMAL) {
		ret = sx9360_i2c_write(data, SX9360_GNRLCTRL0_REG,
				       SX9360_GNRLCTRL0_VAL_PHOFF | REFERENCE_DISABLE);
		msleep(20);

		sx9360_set_offset_calibration(data);
		msleep(450);
	}

	GRIP_INFO("change the mode : %u\n", mode);

	mutex_unlock(&data->mode_mutex);
	return ret;
}

static void sx9360_check_status(struct sx9360_p *data)
{
	u8 status = 0;

	sx9360_i2c_read(data, SX9360_STAT_REG, &status);

	GRIP_INFO("(status: 0x%x)\n", status);

	if (data->skip_data == true) {
		input_report_rel(data->input, REL_MISC, 2);
		input_sync(data->input);
		return;
	}

	if ((status & CSX_STATUS_REG) && (data->diff > data->detect_threshold)) {
		sx9360_send_event(data, ACTIVE);
	} else {
		sx9360_send_event(data, IDLE);
	}
}

static void sx9360_set_enable(struct sx9360_p *data, int enable)
{
	int pre_enable = atomic_read(&data->enable);

	GRIP_INFO("%d\n", enable);

	if (enable) {
		if (pre_enable == OFF) {
			data->diff_avg = 0;
			data->diff_cnt = 0;
			data->useful_avg = 0;
			sx9360_get_data(data);
			sx9360_check_status(data);

			msleep(20);

			/* make sure no interrupts are pending since enabling irq
			 * will only work on next falling edge */
			sx9360_read_irqstate(data);

			/* enable interrupt */
			sx9360_i2c_write(data, SX9360_IRQ_ENABLE_REG, 0x0E);

			enable_irq(data->irq);
			enable_irq_wake(data->irq);

			atomic_set(&data->enable, ON);
		}
	} else {
		if (pre_enable == ON) {
			/* disable interrupt */
			sx9360_i2c_write(data, SX9360_IRQ_ENABLE_REG, 0x00);

			disable_irq(data->irq);
			disable_irq_wake(data->irq);

			atomic_set(&data->enable, OFF);
		}
	}
}

static void sx9360_set_debug_work(struct sx9360_p *data, u8 enable,
				  unsigned int time_ms)
{
	if (enable == ON) {
		data->debug_count = 0;
		schedule_delayed_work(&data->debug_work,
				      msecs_to_jiffies(time_ms));
	} else {
		cancel_delayed_work_sync(&data->debug_work);
	}
}

static ssize_t sx9360_get_offset_calibration_show(struct device *dev,
						  struct device_attribute *attr, char *buf)
{
	u8 val = 0;
	struct sx9360_p *data = dev_get_drvdata(dev);

	sx9360_i2c_read(data, SX9360_IRQSTAT_REG, &val);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t sx9360_set_offset_calibration_store(struct device *dev,
						   struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;
	struct sx9360_p *data = dev_get_drvdata(dev);

	if (kstrtoul(buf, 10, &val)) {
		GRIP_ERR("Invalid Argument\n");
		return -EINVAL;
	}

	if (val)
		sx9360_set_offset_calibration(data);

	return count;
}

static ssize_t sx9360_register_write_store(struct device *dev,
					   struct device_attribute *attr, const char *buf, size_t count)
{
	int regist = 0, val = 0;
	struct sx9360_p *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%2x,%2x", &regist, &val) != 2) {
		GRIP_ERR("The number of data are wrong\n");
		return -EINVAL;
	}

	sx9360_i2c_write(data, (unsigned char)regist, (unsigned char)val);
	GRIP_INFO("Register(0x%2x) data(0x%2x)\n", regist, val);

	return count;
}

static ssize_t sx9360_register_read_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	u8 val = 0;
	int offset = 0, idx = 0;
	struct sx9360_p *data = dev_get_drvdata(dev);

	for (idx = 0; idx < (int)(ARRAY_SIZE(setup_reg)); idx++) {
		sx9360_i2c_read(data, setup_reg[idx].reg, &val);
		GRIP_INFO("Read Reg: 0x%x Value: 0x%x\n\n", setup_reg[idx].reg, val);

		offset += snprintf(buf + offset, PAGE_SIZE - offset,
				   "Reg: 0x%x Value: 0x%08x\n", setup_reg[idx].reg, val);
	}

	return offset;
}

static ssize_t sx9360_read_data_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	sx9360_display_data_reg(data);

	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t sx9360_sw_reset_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	GRIP_INFO("\n");
	sx9360_set_offset_calibration(data);
	msleep(450);
	sx9360_get_data(data);

	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t sx9360_vendor_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}

static ssize_t sx9360_name_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static ssize_t sx9360_touch_mode_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "1\n");
}

static ssize_t sx9360_raw_data_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	static s32 sum_diff, sum_useful;
	struct sx9360_p *data = dev_get_drvdata(dev);

	sx9360_get_data(data);

	if (data->diff_cnt == 0) {
		sum_diff = (s32)data->diff;
		sum_useful = data->useful;
	} else {
		sum_diff += (s32)data->diff;
		sum_useful += data->useful;
	}

	if (++data->diff_cnt >= DIFF_READ_NUM) {
		data->diff_avg = (s16)(sum_diff / DIFF_READ_NUM);
		data->useful_avg = sum_useful / DIFF_READ_NUM;
		data->diff_cnt = 0;
	}

	return snprintf(buf, PAGE_SIZE, "%ld,%ld,%u,%d,%d\n", (long int)data->capMain,
			(long int)data->useful, data->offset, data->diff, data->avg);
}

static ssize_t sx9360_diff_avg_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->diff_avg);
}

static ssize_t sx9360_useful_avg_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%ld\n", (long int)data->useful_avg);
}

static ssize_t sx9360_avgnegfilt_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 avgnegfilt = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL3_REG, &avgnegfilt);

	avgnegfilt = (avgnegfilt & 0x38) >> 3;

	if (avgnegfilt == 7)
		return snprintf(buf, PAGE_SIZE, "1\n");
	else if (avgnegfilt > 0 && avgnegfilt < 7)
		return snprintf(buf, PAGE_SIZE, "1-1/%d\n", 1 << avgnegfilt);
	else if (avgnegfilt == 0)
		return snprintf(buf, PAGE_SIZE, "0\n");

	return snprintf(buf, PAGE_SIZE, "not set\n");
}

static ssize_t sx9360_avgposfilt_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 avgposfilt = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL3_REG, &avgposfilt);
	avgposfilt = avgposfilt & 0x07;

	if (avgposfilt == 7)
		return snprintf(buf, PAGE_SIZE, "1\n");
	else if (avgposfilt > 1 && avgposfilt < 7)
		return snprintf(buf, PAGE_SIZE, "1-1/%d\n", 16 << avgposfilt);
	else if (avgposfilt == 1)
		return snprintf(buf, PAGE_SIZE, "1-1/16\n");
	else
		return snprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t sx9360_gain_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 gain = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL0PHM_REG, &gain);
	gain = (gain & 0x38) >> 3;

	if (gain > 0 && gain < 5)
		return snprintf(buf, PAGE_SIZE, "x%u\n", 1 << (gain - 1));

	return snprintf(buf, PAGE_SIZE, "Reserved\n");
}

static ssize_t sx9360_range_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "None\n");
}

static ssize_t sx9360_avgthresh_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 avgthresh = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL1_REG, &avgthresh);
	avgthresh = avgthresh & 0x3F;

	return snprintf(buf, PAGE_SIZE, "%ld\n", 512 * (long int)avgthresh);
}

static ssize_t sx9360_rawfilt_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 rawfilt = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL0PHM_REG, &rawfilt);
	rawfilt = rawfilt & 0x07;

	if (rawfilt > 0 && rawfilt < 8)
		return snprintf(buf, PAGE_SIZE, "1-1/%d\n", 1 << rawfilt);
	else
		return snprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t sx9360_sampling_freq_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 sampling_freq = 0;
	const char *table[16] = {
		"250", "200", "166.67", "142.86", "125", "100",	"83.33", "71.43",
		"62.50", "50", "41.67", "35.71", "27.78", "20.83", "15.62", "7.81"
	};

	sx9360_i2c_read(data, SX9360_AFEPARAM1PHM_REG, &sampling_freq);
	sampling_freq = sampling_freq & 0x0F;

	return snprintf(buf, PAGE_SIZE, "%skHz\n", table[sampling_freq]);
}

static ssize_t sx9360_scan_period_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 scan_period = 0;

	sx9360_i2c_read(data, SX9360_GNRLCTRL2_REG, &scan_period);

	return snprintf(buf, PAGE_SIZE, "%ld\n",
			(long int)(((long int)scan_period << 11) / 1000));
}

static ssize_t sx9360_again_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	const char *table[8] = {
		"+/-0.75", "+/-2.25", "+/-3.75", "+/-5.25",
		"+/-6", "+/-7.5", "+/-9", "+/-10.5"
	};
	u8 again = 0;

	sx9360_i2c_read(data, SX9360_AFEPARAM1PHM_REG, &again);
	again = (again & 0x70) >> 4;

	return snprintf(buf, PAGE_SIZE, "%spF\n", table[again]);
}

static ssize_t sx9360_phase_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "1\n");
}

static ssize_t sx9360_hysteresis_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	const char *table[4] = {"None", "+/-6%", "+/-12%", "+/-25%"};
	u8 hyst = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL4_REG, &hyst);
	hyst = (hyst & 0x30) >> 4;

	return snprintf(buf, PAGE_SIZE, "%s\n", table[hyst]);
}

static ssize_t sx9360_resolution_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 resolution = 0;

	sx9360_i2c_read(data, SX9360_AFEPARAM0PHM_REG, &resolution);
	resolution = resolution & 0x7;

	return snprintf(buf, PAGE_SIZE, "%u\n", 1 << (resolution + 3));
}

static ssize_t sx9360_adc_filt_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "None\n");
}

static ssize_t sx9360_useful_filt_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 useful_filt = 0;

	sx9360_i2c_read(data, SX9360_USEFILTER4_REG, &useful_filt);
	useful_filt = useful_filt & 0x01;

	return snprintf(buf, PAGE_SIZE, "%s\n", useful_filt ? "on" : "off");
}

static ssize_t sx9360_irq_count_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	int ret = 0;
	s16 max_diff_val = 0;

	if (data->irq_count) {
		ret = -1;
		max_diff_val = data->max_diff;
	} else {
		max_diff_val = data->max_normal_diff;
	}

	GRIP_INFO("called\n");

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
			ret, data->irq_count, max_diff_val);
}

static ssize_t sx9360_irq_count_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t count)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	u8 onoff;
	int ret;

	ret = kstrtou8(buf, 10, &onoff);
	if (ret < 0) {
		GRIP_ERR("kstrtou8 failed.(%d)\n", ret);
		return count;
	}

	mutex_lock(&data->read_mutex);

	if (onoff == 0) {
		data->abnormal_mode = OFF;
	} else if (onoff == 1) {
		data->abnormal_mode = ON;
		data->irq_count = 0;
		data->max_diff = 0;
		data->max_normal_diff = 0;
	} else {
		GRIP_ERR("unknown value %d\n", onoff);
	}

	mutex_unlock(&data->read_mutex);

	GRIP_INFO("%d\n", onoff);

	return count;
}

static ssize_t sx9360_normal_threshold_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	u8 th_buf = 0, hyst = 0;
	u32 threshold = 0;

	sx9360_i2c_read(data, SX9360_PROXCTRL5_REG, &th_buf);
	threshold = (u32)th_buf * (u32)th_buf / 2;

	sx9360_i2c_read(data, SX9360_PROXCTRL4_REG, &hyst);
	hyst = (hyst & 0x30) >> 4;

	switch (hyst) {
	case 0x01: /* 6% */
		hyst = threshold >> 4;
		break;
	case 0x02: /* 12% */
		hyst = threshold >> 3;
		break;
	case 0x03: /* 25% */
		hyst = threshold >> 2;
		break;
	default:
		/* None */
		break;
	}

	return snprintf(buf, PAGE_SIZE, "%lu,%lu\n",
			(u32)threshold + (u32)hyst, (u32)threshold - (u32)hyst);
}

static ssize_t sx9360_onoff_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", !data->skip_data);
}

static ssize_t sx9360_onoff_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t count)
{
	u8 val;
	int ret;
	struct sx9360_p *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &val);
	if (ret) {
		GRIP_ERR("Invalid Argument\n");
		return ret;
	}

	if (val == 0) {
		data->skip_data = true;
		if (atomic_read(&data->enable) == ON) {
			data->state = IDLE;
			input_report_rel(data->input, REL_MISC, 2);
			input_sync(data->input);
		}
	} else {
		data->skip_data = false;
	}

	GRIP_INFO("%u\n", val);
	return count;
}

#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
static ssize_t sx9360_mcc_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, mcc;
	u8 threshold;
	struct sx9360_p *data = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &mcc);
	if (ret) {
		GRIP_ERR("Invalid Argument\n");
		return ret;
	}

	data->mcc = mcc;

	GRIP_INFO("mcc value %d\n", data->mcc);

	// 001 : call box, 440/441 : jpn, 450 : kor, 460 : chn
	if ((data->mcc != 001) && (data->mcc != 440) && (data->mcc != 441) &&
	    (data->mcc != 450) && (data->mcc != 460)) {
		GRIP_INFO("default threshold %u\n", data->default_threshold);
		threshold = data->default_threshold; /* DEFAULT KOR THRESHOLD > 1912  */
		sx9360_i2c_write(data, SX9360_PROXCTRL5_REG, threshold);
	} else {
		threshold = data->mcc_threshold;
		sx9360_i2c_write(data, SX9360_PROXCTRL5_REG, threshold);
	}

	GRIP_INFO("change threshold %u\n", threshold);
	setup_reg[SX9360_PROXTHRESH_REG_IDX].val = threshold;

	return count;
}

static ssize_t sx9360_mcc_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->mcc);
}

static DEVICE_ATTR(mcc, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_mcc_show, sx9360_mcc_store);
#endif

static DEVICE_ATTR(menual_calibrate, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_get_offset_calibration_show,
		   sx9360_set_offset_calibration_store);
static DEVICE_ATTR(register_write, S_IWUSR | S_IWGRP,
		   NULL, sx9360_register_write_store);
static DEVICE_ATTR(register_read, S_IRUGO, sx9360_register_read_show, NULL);
static DEVICE_ATTR(readback, S_IRUGO, sx9360_read_data_show, NULL);
static DEVICE_ATTR(reset, S_IRUGO, sx9360_sw_reset_show, NULL);

static DEVICE_ATTR(name, S_IRUGO, sx9360_name_show, NULL);
static DEVICE_ATTR(vendor, S_IRUGO, sx9360_vendor_show, NULL);
static DEVICE_ATTR(mode, S_IRUGO, sx9360_touch_mode_show, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, sx9360_raw_data_show, NULL);
static DEVICE_ATTR(diff_avg, S_IRUGO, sx9360_diff_avg_show, NULL);
static DEVICE_ATTR(useful_avg, S_IRUGO, sx9360_useful_avg_show, NULL);
static DEVICE_ATTR(onoff, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_onoff_show, sx9360_onoff_store);
static DEVICE_ATTR(normal_threshold, S_IRUGO,
		   sx9360_normal_threshold_show, NULL);

static DEVICE_ATTR(avg_negfilt, S_IRUGO, sx9360_avgnegfilt_show, NULL);
static DEVICE_ATTR(avg_posfilt, S_IRUGO, sx9360_avgposfilt_show, NULL);
static DEVICE_ATTR(avg_thresh, S_IRUGO, sx9360_avgthresh_show, NULL);
static DEVICE_ATTR(rawfilt, S_IRUGO, sx9360_rawfilt_show, NULL);
static DEVICE_ATTR(sampling_freq, S_IRUGO, sx9360_sampling_freq_show, NULL);
static DEVICE_ATTR(scan_period, S_IRUGO, sx9360_scan_period_show, NULL);
static DEVICE_ATTR(gain, S_IRUGO, sx9360_gain_show, NULL);
static DEVICE_ATTR(range, S_IRUGO, sx9360_range_show, NULL);
static DEVICE_ATTR(analog_gain, S_IRUGO, sx9360_again_show, NULL);
static DEVICE_ATTR(phase, S_IRUGO, sx9360_phase_show, NULL);
static DEVICE_ATTR(hysteresis, S_IRUGO, sx9360_hysteresis_show, NULL);
static DEVICE_ATTR(irq_count, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_irq_count_show, sx9360_irq_count_store);
static DEVICE_ATTR(resolution, S_IRUGO, sx9360_resolution_show, NULL);
static DEVICE_ATTR(adc_filt, S_IRUGO, sx9360_adc_filt_show, NULL);
static DEVICE_ATTR(useful_filt, S_IRUGO, sx9360_useful_filt_show, NULL);

static struct device_attribute *sensor_attrs[] = {
	&dev_attr_menual_calibrate,
	&dev_attr_register_write,
	&dev_attr_register_read,
	&dev_attr_readback,
	&dev_attr_reset,
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_mode,
	&dev_attr_raw_data,
	&dev_attr_diff_avg,
	&dev_attr_useful_avg,
	&dev_attr_onoff,
	&dev_attr_normal_threshold,
	&dev_attr_avg_negfilt,
	&dev_attr_avg_posfilt,
	&dev_attr_avg_thresh,
	&dev_attr_rawfilt,
	&dev_attr_sampling_freq,
	&dev_attr_scan_period,
	&dev_attr_gain,
	&dev_attr_range,
	&dev_attr_analog_gain,
	&dev_attr_phase,
	&dev_attr_hysteresis,
	&dev_attr_irq_count,
	&dev_attr_resolution,
	&dev_attr_adc_filt,
	&dev_attr_useful_filt,
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	&dev_attr_mcc,
#endif
	NULL,
};

/*****************************************************************************/
#ifdef CONFIG_SUPPORT_CAMERA_FREEFALL
void sx9360_set_camera_freefall_mode(struct sx9360_p *data, int enable)
{
	u8 val = 0;

	if (enable) {
		sx9360_i2c_write(data, setup_reg[2].reg, 0x07);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[2].reg, 0x07);

		sx9360_i2c_write(data, setup_reg[8].reg, 0x20);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[8].reg, 0x20);

		sx9360_i2c_write(data, setup_reg[9].reg, 0x20);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[9].reg, 0x20);

		sx9360_i2c_write(data, setup_reg[13].reg, 0x10);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[13].reg, 0x10);

		sx9360_i2c_write(data, setup_reg[14].reg, 0x96);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[14].reg, 0x96);
	} else {
		sx9360_i2c_write(data, setup_reg[2].reg, setup_reg[2].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[2].reg, setup_reg[2].val);

		sx9360_i2c_write(data, setup_reg[8].reg, setup_reg[8].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[8].reg, setup_reg[8].val);

		sx9360_i2c_write(data, setup_reg[9].reg, setup_reg[9].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[9].reg, setup_reg[9].val);

		sx9360_i2c_write(data, setup_reg[13].reg, setup_reg[13].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[13].reg, setup_reg[13].val);

		sx9360_i2c_write(data, setup_reg[14].reg, setup_reg[14].val);
		GRIP_INFO("Write Reg: 0x%x Value: 0x%x\n",
			  setup_reg[14].reg, setup_reg[14].val);
	}

	sx9360_i2c_read(data, SX9360_PROXCTRL5_REG, &val);
	data->detect_threshold = (u16)val * (u16)val / 2;

	sx9360_i2c_read(data, SX9360_PROXCTRL4_REG, &val);
	val = (val & 0x30) >> 4;

	if (val)
		data->detect_threshold += data->detect_threshold >> (5 - val);

	GRIP_INFO("detect threshold: %u\n", data->detect_threshold);
}
#endif
static ssize_t sx9360_enable_store(struct device *dev,
				   struct device_attribute *attr, const char *buf, size_t size)
{
	u8 enable;
	int ret;
	struct sx9360_p *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &enable);
	if (ret) {
		GRIP_ERR("Invalid Argument\n");
		return ret;
	}

	GRIP_INFO("new_value = %u\n", enable);
	if ((enable == 0) || (enable == 1))
		sx9360_set_enable(data, (int)enable);
#ifdef CONFIG_SUPPORT_CAMERA_FREEFALL
	if (enable == 0)
		sx9360_set_camera_freefall_mode(data, OFF);
#endif
	return size;
}

static ssize_t sx9360_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enable));
}
#ifdef CONFIG_SUPPORT_CAMERA_FREEFALL
static ssize_t sx9360_delay_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t size)
{
	int ret, delay;
	struct sx9360_p *data = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &delay);

	data->poll_delay = delay / NSEC_PER_MSEC;

	GRIP_INFO("delay = %d %d\n", delay, data->poll_delay);

	if (delay == 7700000L)
		sx9360_set_camera_freefall_mode(data, ON);
	else
		sx9360_set_camera_freefall_mode(data, OFF);

	return size;
}

static ssize_t sx9360_delay_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	return snprintf(buf, 16, "%lld\n",
			(int64_t)data->poll_delay * (int64_t)NSEC_PER_MSEC);
}
static DEVICE_ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_delay_show, sx9360_delay_store);
#endif
static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
		   sx9360_enable_show, sx9360_enable_store);

static struct attribute *sx9360_attributes[] = {
#ifdef CONFIG_SUPPORT_CAMERA_FREEFALL
	&dev_attr_poll_delay.attr,
#endif
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group sx9360_attribute_group = {
	.attrs = sx9360_attributes
};

static void sx9360_touch_process(struct sx9360_p *data)
{
	u8 status = 0;

	sx9360_i2c_read(data, SX9360_STAT_REG, &status);
	GRIP_INFO("0x%x\n", status);

	sx9360_get_data(data);

	if (data->abnormal_mode) {
		if (status & CSX_STATUS_REG) {
			if (data->max_diff < data->diff)
				data->max_diff = data->diff;
			data->irq_count++;
		}
	}

	if (data->state == IDLE) {
		if (status & CSX_STATUS_REG)
			sx9360_send_event(data, ACTIVE);
		else
			GRIP_INFO("0x%x already released.\n", status);
	} else { /* User released button */
		if (!(status & CSX_STATUS_REG)) {
			sx9360_send_event(data, IDLE);
		} else {
			GRIP_INFO("0x%x still touched\n", status);
		}
	}
}

static void sx9360_process_interrupt(struct sx9360_p *data)
{
	u8 status = 0;

	/* since we are not in an interrupt don't need to disable irq. */
	status = sx9360_read_irqstate(data);

	GRIP_INFO("status %d\n", status);

	if (status & IRQ_PROCESS_CONDITION)
		sx9360_touch_process(data);
}

static void sx9360_init_work_func(struct work_struct *work)
{
	struct sx9360_p *data = container_of((struct delayed_work *)work,
					     struct sx9360_p, init_work);

	sx9360_initialize_chip(data);

	sx9360_set_mode(data, SX9360_MODE_NORMAL);
	/* make sure no interrupts are pending since enabling irq
	 * will only work on next falling edge */
	sx9360_read_irqstate(data);
}

static void sx9360_irq_work_func(struct work_struct *work)
{
	struct sx9360_p *data = container_of((struct delayed_work *)work,
					     struct sx9360_p, irq_work);

	if (sx9360_get_nirq_state(data) == 0)
		sx9360_process_interrupt(data);
	else
		GRIP_ERR("nirq read high %d\n", sx9360_get_nirq_state(data));
}

static void sx9360_read_register(struct sx9360_p *data)
{
	u8 val, offset = 0;
	int idx = 0, array_size = 0;
	char buf[52] = {0,};

	array_size = (int)(ARRAY_SIZE(setup_reg));
	while (idx < array_size) {
		sx9360_i2c_read(data, setup_reg[idx].reg, &val);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "[0x%02x]:0x%02x ",
				   setup_reg[idx].reg, val);
		idx++;
		if (!(idx & 0x03) || (idx == array_size)) {
			GRIP_INFO("%s\n", buf);
			offset = 0;
		}
	}
}

static void sx9360_debug_work_func(struct work_struct *work)
{
	struct sx9360_p *data = container_of((struct delayed_work *)work,
					     struct sx9360_p, debug_work);
	static int hall_flag = 1;
	static int hall_cert_flag = 1;
	int ret;
	u8 value = 0;

#if defined(CONFIG_FOLDER_HALL)
	char str[2] = "0";
#else
	char str[6] = "CLOSE";
#endif

	sx9360_check_hallic_state(HALLIC_PATH, data->hall_ic);

	data->hall_ic[sizeof(str) - 1] = '\0';

	if (strcmp(data->hall_ic, str) == 0) {
		if (hall_flag) {
			GRIP_INFO("hall IC is closed\n");
			sx9360_set_offset_calibration(data);
			hall_flag = 0;
		}
	} else {
		hall_flag = 1;
	}

	if (data->hallic_cert_detect) {
		sx9360_check_hallic_state(HALLIC_CERT_PATH, data->hall_ic);

		data->hall_ic[sizeof(str) - 1] = '\0';

		if (strcmp(data->hall_ic, str) == 0) {
			if (hall_cert_flag) {
				GRIP_INFO("cert hall IC is closed\n");
				sx9360_set_offset_calibration(data);
				hall_cert_flag = 0;
			}
		} else {
			hall_cert_flag = 1;
		}
	}

	if (atomic_read(&data->enable) == ON) {
		if (data->abnormal_mode) {
			sx9360_get_data(data);
			if (data->max_normal_diff < data->diff)
				data->max_normal_diff = data->diff;
		} else {
			if (data->debug_count >= GRIP_LOG_TIME) {
				sx9360_get_data(data);
				data->debug_count = 0;
			} else {
				data->debug_count++;
			}
		}
	}

	/* Zero Detect Defence code*/
	if (data->debug_zero_count >= ZERO_DETECT_TIME) {
		ret = sx9360_i2c_read(data, SX9360_GNRLCTRL0_REG, &value);
		if (ret < 0)
			GRIP_ERR("fail to read PHEN :0x%02x (%d)\n", value, ret);
		else if (value == 0) {
			GRIP_INFO("detected all data zero!!!\n");
			sx9360_read_register(data);

			sx9360_i2c_write(data, SX9360_SOFTRESET_REG, SX9360_SOFTRESET);
			msleep(300);
			sx9360_initialize_chip(data);
			sx9360_set_mode(data, SX9360_MODE_NORMAL);
			sx9360_read_irqstate(data);
			msleep(20);
		}
		data->debug_zero_count = 0;
	} else {
		data->debug_zero_count++;
	}

	schedule_delayed_work(&data->debug_work, msecs_to_jiffies(2000));
}

static irqreturn_t sx9360_interrupt_thread(int irq, void *pdata)
{
	struct sx9360_p *data = pdata;

	__pm_wakeup_event(data->grip_ws, jiffies_to_msecs(3 * HZ));
	schedule_delayed_work(&data->irq_work, msecs_to_jiffies(100));

	return IRQ_HANDLED;
}

static int sx9360_input_init(struct sx9360_p *data)
{
	int ret = 0;
	struct input_dev *dev = NULL;

	/* Create the input device */
	dev = input_allocate_device();
	if (!dev)
		return -ENOMEM;

	dev->name = MODULE_NAME;
	dev->id.bustype = BUS_I2C;

	input_set_capability(dev, EV_REL, REL_MISC);
	input_set_drvdata(dev, data);

	ret = input_register_device(dev);
	if (ret < 0) {
		input_free_device(dev);
		return ret;
	}

	ret = sensors_create_symlink(&dev->dev.kobj, dev->name);
	if (ret < 0) {
		input_unregister_device(dev);
		return ret;
	}

	ret = sysfs_create_group(&dev->dev.kobj, &sx9360_attribute_group);
	if (ret < 0) {
		sensors_remove_symlink(&data->input->dev.kobj,
				       data->input->name);
		input_unregister_device(dev);
		return ret;
	}

	/* save the input pointer and finish initialization */
	data->input = dev;

	return 0;
}

static int sx9360_setup_pin(struct sx9360_p *data)
{
	int ret;

	ret = gpio_request(data->gpio_nirq, "SX9360_nIRQ");
	if (ret < 0) {
		GRIP_ERR("gpio %d request failed (%d)\n", data->gpio_nirq, ret);
		return ret;
	}

	ret = gpio_direction_input(data->gpio_nirq);
	if (ret < 0) {
		GRIP_ERR("failed to set gpio %d as input (%d)\n", data->gpio_nirq, ret);
		gpio_free(data->gpio_nirq);
		return ret;
	}

	return 0;
}

static void sx9360_initialize_variable(struct sx9360_p *data)
{
	data->init_done = OFF;
	data->skip_data = false;
	data->state = IDLE;

	atomic_set(&data->enable, OFF);
}

static int sx9360_read_setupreg(struct device_node *dnode, char *str, u32 *val)
{
	u32 temp_val;
	int ret;

	ret = of_property_read_u32(dnode, str, &temp_val);

	if (!ret)
		*val = temp_val;
	else
		GRIP_ERR("%s: property read err 0x%2x (%d)\n", str, temp_val, ret);

	return ret;
}

static int sx9360_parse_dt(struct sx9360_p *data, struct device *dev)
{
	struct device_node *dNode = dev->of_node;
	enum of_gpio_flags flags;

	u32 val = 0;

	if (dNode == NULL)
		return -ENODEV;

	data->gpio_nirq = of_get_named_gpio_flags(dNode,
						  "sx9360,nirq-gpio", 0, &flags);
	if (data->gpio_nirq < 0) {
		GRIP_ERR("get gpio_nirq error\n");
		return -ENODEV;
	} else {
		GRIP_INFO("get gpio_nirq %d\n", data->gpio_nirq);
	}

	if (!sx9360_read_setupreg(dNode, SX9360_REGGNRLCTL2, &val))
		setup_reg[SX9360_REGGNRLCTL2_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_REFRESOLUTION, &val))
		setup_reg[SX9360_REFRESOLUTION_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_REFAGAINFREQ, &val))
		setup_reg[SX9360_REFAGAINFREQ_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_RESOLUTION, &val))
		setup_reg[SX9360_RESOLUTION_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_AGAINFREQ, &val))
		setup_reg[SX9360_AGAINFREQ_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_REFGAINRAWFILT, &val))
		setup_reg[SX9360_REFGAINRAWFILT_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_GAINRAWFILT, &val))
		setup_reg[SX9360_GAINRAWFILT_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_REGPROXCTRL3, &val))
		setup_reg[SX9360_REGPROXCTRL3_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_HYST, &val))
		setup_reg[SX9360_HYST_REG_IDX].val = (u8)val;
	if (!sx9360_read_setupreg(dNode, SX9360_PROXTHRESH, &val)) {
		setup_reg[SX9360_PROXTHRESH_REG_IDX].val = (u8)val;
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
		data->default_threshold = val;
	}
	if (!sx9360_read_setupreg(dNode, SX9360_PROXTHRESH_MCC, &val)) {
		data->mcc_threshold = val;
	} else {
		data->mcc_threshold = data->default_threshold;
#endif
	}
	if (sx9360_read_setupreg(dNode, SX9360_HALLIC_CERT, &data->hallic_cert_detect))
		data->hallic_cert_detect = 0;

	return 0;
}

#if IS_ENABLED(CONFIG_CCIC_NOTIFIER) && IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static int sx9360_ccic_handle_notification(struct notifier_block *nb,
					   unsigned long action, void *data)
{
	CC_NOTI_USB_STATUS_TYPEDEF usb_status =
		*(CC_NOTI_USB_STATUS_TYPEDEF *) data;
	struct sx9360_p *pdata =
		container_of(nb, struct sx9360_p, ccic_nb);
	static int pre_attach;

	if ((usb_status.drp != USB_STATUS_NOTIFY_ATTACH_DFP) &&
	    (usb_status.drp != USB_STATUS_NOTIFY_DETACH))
		return 0;

	if (pre_attach == usb_status.drp)
		return 0;

	if (pdata->init_done == ON) {
		switch (usb_status.drp) {
		case USB_STATUS_NOTIFY_ATTACH_DFP:
		case USB_STATUS_NOTIFY_DETACH:
			GRIP_INFO("accept attach = %d\n", usb_status.drp);
			sx9360_set_offset_calibration(pdata);
			break;
		default:
			GRIP_INFO("skip attach = %d\n", usb_status.drp);
			break;
		}
	}

	pre_attach = usb_status.drp;

	return 0;
}
#endif
#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
static int sx9360_vbus_handle_notification(struct notifier_block *nb,
					   unsigned long action, void *data)
{
	vbus_status_t vbus_type = *(vbus_status_t *) data;
	struct sx9360_p *pdata =
		container_of(nb, struct sx9360_p, vbus_nb);
	static int pre_attach;

	if (pre_attach == vbus_type)
		return 0;

	if (pdata->init_done == ON) {
		switch (vbus_type) {
		case STATUS_VBUS_HIGH:
		case STATUS_VBUS_LOW:
			GRIP_INFO("accept attach = %d\n", vbus_type);
			sx9360_set_offset_calibration(pdata);
			break;
		default:
			GRIP_INFO("skip attach = %d\n", vbus_type);
			break;
		}
	}

	pre_attach = vbus_type;

	return 0;
}
#endif

static int sx9360_check_chip_id(struct sx9360_p *data)
{
	int ret;
	u8 value = 0;

	ret = sx9360_i2c_read(data, SX9360_WHOAMI_REG, &value);
	if (ret < 0) {
		GRIP_ERR("whoami[0x%x] read failed %d\n", value, ret);
		return ret;
	}
	if (value != WHO_AM_I) {
		GRIP_ERR("invalid whoami(%x)\n", value);
		return -1;
	}

	return 0;
}

static int sx9360_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int ret = -ENODEV;
	struct sx9360_p *data = NULL;

	GRIP_INFO("Probe Start!\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		GRIP_ERR("i2c_check_functionality error\n");
		goto exit;
	}

	/* create memory for main struct */
	data = kzalloc(sizeof(struct sx9360_p), GFP_KERNEL);
	if (data == NULL) {
		GRIP_ERR("kzalloc error\n");
		ret = -ENOMEM;
		goto exit_kzalloc;
	}

	i2c_set_clientdata(client, data);
	data->client = client;
	data->factory_device = &client->dev;

	ret = sx9360_input_init(data);
	if (ret < 0)
		goto exit_input_init;

	data->grip_ws = wakeup_source_register(&client->dev, "grip_wake_lock");
	mutex_init(&data->mode_mutex);
	mutex_init(&data->read_mutex);

	ret = sx9360_parse_dt(data, &client->dev);
	if (ret < 0) {
		GRIP_ERR("of_node error\n");
		ret = -ENODEV;
		goto exit_of_node;
	}

	ret = sx9360_setup_pin(data);
	if (ret) {
		GRIP_ERR("could not setup pin\n");
		goto exit_setup_pin;
	}

	/* read chip id */
	ret = sx9360_check_chip_id(data);
	if (ret < 0) {
		GRIP_ERR("chip id check failed %d\n", ret);
		goto exit_chip_reset;
	}

	ret = sx9360_i2c_write(data, SX9360_SOFTRESET_REG, SX9360_SOFTRESET);
	if (ret < 0) {
		GRIP_ERR("chip reset failed %d\n", ret);
		goto exit_chip_reset;
	}

	sx9360_initialize_variable(data);
	INIT_DELAYED_WORK(&data->init_work, sx9360_init_work_func);
	INIT_DELAYED_WORK(&data->irq_work, sx9360_irq_work_func);
	INIT_DELAYED_WORK(&data->debug_work, sx9360_debug_work_func);

	data->irq = gpio_to_irq(data->gpio_nirq);
	/* initailize interrupt reporting */
	ret = request_threaded_irq(data->irq, NULL, sx9360_interrupt_thread,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "sx9360_irq", data);
	if (ret < 0) {
		GRIP_ERR("failed to set request_threaded_irq %d as returning (%d)\n", data->irq,
			 ret);
		goto exit_request_threaded_irq;
	}
	disable_irq(data->irq);

	ret = sensors_register(&data->factory_device,
			       data, sensor_attrs, MODULE_NAME);
	if (ret) {
		GRIP_ERR("cound not register sensor(%d).\n", ret);
		goto exit_register_failed;
	}

	schedule_delayed_work(&data->init_work, msecs_to_jiffies(300));
	sx9360_set_debug_work(data, ON, 20000);

#if IS_ENABLED(CONFIG_CCIC_NOTIFIER) && IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	GRIP_INFO("register ccic notifier\n");
	manager_notifier_register(&data->ccic_nb,
				  sx9360_ccic_handle_notification,
				  MANAGER_NOTIFY_CCIC_USB);
#endif
#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
	GRIP_INFO("register vbus notifier\n");
	vbus_notifier_register(&data->vbus_nb, sx9360_vbus_handle_notification,
			       VBUS_NOTIFY_DEV_CHARGER);
#endif

	GRIP_INFO("Probe done!\n");

	return 0;

exit_register_failed:
	free_irq(data->irq, data);
exit_request_threaded_irq:
exit_chip_reset:
	gpio_free(data->gpio_nirq);
exit_setup_pin:
exit_of_node:
	mutex_destroy(&data->mode_mutex);
	mutex_destroy(&data->read_mutex);
	wakeup_source_unregister(data->grip_ws);
	sysfs_remove_group(&data->input->dev.kobj, &sx9360_attribute_group);
	sensors_remove_symlink(&data->input->dev.kobj, data->input->name);
	input_unregister_device(data->input);
exit_input_init:
	kfree(data);
exit_kzalloc:
exit:
	GRIP_ERR("Probe fail!\n");
	return ret;
}

static int sx9360_remove(struct i2c_client *client)
{
	struct sx9360_p *data = (struct sx9360_p *)i2c_get_clientdata(client);

	if (atomic_read(&data->enable) == ON)
		sx9360_set_enable(data, OFF);

	sx9360_set_mode(data, SX9360_MODE_SLEEP);

	cancel_delayed_work_sync(&data->init_work);
	cancel_delayed_work_sync(&data->irq_work);
	cancel_delayed_work_sync(&data->debug_work);
	free_irq(data->irq, data);
	gpio_free(data->gpio_nirq);

	wakeup_source_unregister(data->grip_ws);
	sensors_unregister(data->factory_device, sensor_attrs);
	sensors_remove_symlink(&data->input->dev.kobj, data->input->name);
	sysfs_remove_group(&data->input->dev.kobj, &sx9360_attribute_group);
	input_unregister_device(data->input);
	mutex_destroy(&data->mode_mutex);
	mutex_destroy(&data->read_mutex);

	kfree(data);

	return 0;
}

static int sx9360_suspend(struct device *dev)
{
	struct sx9360_p *data = dev_get_drvdata(dev);
	int cnt = 0;

	GRIP_INFO("\n");
	/* before go to sleep, make the interrupt pin as high*/
	while ((sx9360_get_nirq_state(data) == 0) && (cnt++ < 3)) {
		sx9360_read_irqstate(data);
		msleep(20);
	}
	if (cnt >= 3)
		GRIP_ERR("s/w reset fail(%d)\n", cnt);

	sx9360_set_debug_work(data, OFF, 1000);

	return 0;
}

static int sx9360_resume(struct device *dev)
{
	struct sx9360_p *data = dev_get_drvdata(dev);

	GRIP_INFO("\n");
	sx9360_set_debug_work(data, ON, 1000);

	return 0;
}

static void sx9360_shutdown(struct i2c_client *client)
{
	struct sx9360_p *data = i2c_get_clientdata(client);

	GRIP_INFO("\n");
	sx9360_set_debug_work(data, OFF, 1000);
	if (atomic_read(&data->enable) == ON)
		sx9360_set_enable(data, OFF);

	sx9360_set_mode(data, SX9360_MODE_SLEEP);
}

static struct of_device_id sx9360_match_table[] = {
	{ .compatible = "sx9360",},
	{},
};

static const struct i2c_device_id sx9360_id[] = {
	{ "sx9360_match_table", 0 },
	{ }
};

static const struct dev_pm_ops sx9360_pm_ops = {
	.suspend = sx9360_suspend,
	.resume = sx9360_resume,
};

static struct i2c_driver sx9360_driver = {
	.driver = {
		.name	= MODEL_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = sx9360_match_table,
		.pm = &sx9360_pm_ops
	},
	.probe		= sx9360_probe,
	.remove		= sx9360_remove,
	.shutdown	= sx9360_shutdown,
	.id_table	= sx9360_id,
};

static int __init sx9360_init(void)
{
	return i2c_add_driver(&sx9360_driver);
}

static void __exit sx9360_exit(void)
{
	i2c_del_driver(&sx9360_driver);
}

module_init(sx9360_init);
module_exit(sx9360_exit);

MODULE_DESCRIPTION("Semtech Corp. SX9360 Capacitive Touch Controller Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
