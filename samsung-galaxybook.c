// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung Galaxy Book series extras driver
 *
 * Copyright (c) 2024 Joshua Grisham <josh@joshuagrisham.com>
 * Copyright (c) 2024 Giulio Girardi <giulio.girardi@protechgroup.it>
 *
 * Implementation inspired by existing x86 platform drivers.
 * Thank you to the authors!
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/i8042.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/nls.h>
#include <linux/version.h>

#include <acpi/battery.h>

#define SAMSUNG_GALAXYBOOK_CLASS  "samsung-galaxybook"
#define SAMSUNG_GALAXYBOOK_NAME   "Samsung Galaxy Book Extras"

/*
 * Module parameters
 */

static bool kbd_backlight = true;
static bool battery_threshold = true;
static bool performance_mode = true;
static bool allow_recording = true;
static bool fan_speed = true;
static bool i8042_filter = true;

module_param(kbd_backlight, bool, 0644);
MODULE_PARM_DESC(kbd_backlight, "Enable Keyboard Backlight control (default on)");
module_param(battery_threshold, bool, 0644);
MODULE_PARM_DESC(battery_threshold, "Enable battery charge threshold control (default on)");
module_param(performance_mode, bool, 0644);
MODULE_PARM_DESC(performance_mode, "Enable Performance Mode control (default on)");
module_param(allow_recording, bool, 0644);
MODULE_PARM_DESC(allow_recording, "Enable control to allow or block access to camera and microphone (default on)");
module_param(fan_speed, bool, 0644);
MODULE_PARM_DESC(fan_speed, "Enable fan speed (default on)");
module_param(i8042_filter, bool, 0644);
MODULE_PARM_DESC(i8042_filter, "Enable capturing keyboard hotkey events (default on)");

/*
 * Device definitions and matching
 */

static const struct acpi_device_id galaxybook_device_ids[] = {
	{ "SAM0427" },
	{ "SAM0428" },
	{ "SAM0429" },
	{ "SAM0430" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, galaxybook_device_ids);

static const struct dmi_system_id galaxybook_dmi_ids[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
			DMI_MATCH(DMI_CHASSIS_TYPE, "10"), /* Notebook */
		},
	},
	{}
};

struct galaxybook_fan {
	struct acpi_device fan;
	char *description;
	bool supports_fst;
	unsigned int *fan_speeds;
	int fan_speeds_count;
	struct dev_ext_attribute fan_speed_rpm_ext_attr;
};

#define MAX_FAN_COUNT 5

struct samsung_galaxybook {
	struct platform_device *platform;
	struct acpi_device *acpi;

	struct led_classdev kbd_backlight;
	struct work_struct kbd_backlight_hotkey_work;

	struct input_dev *input;
	struct key_entry *keymap;

	u8 *profile_performance_modes;
	struct platform_profile_handler profile_handler;
	struct work_struct performance_mode_hotkey_work;

	struct work_struct allow_recording_hotkey_work;

	struct galaxybook_fan fans[MAX_FAN_COUNT];
	int fans_count;

#if IS_ENABLED(CONFIG_HWMON)
	struct device *hwmon;
#endif
};
static struct samsung_galaxybook *galaxybook_ptr;

struct sawb {
	u16 safn;
	u16 sasb;
	u8 rflg;
	union {
		struct {
			u8 gunm;
			u8 guds[250];
		};
		struct {
			u8 caid[16];
			u8 fncn;
			u8 subn;
			u8 iob0;
			u8 iob1;
			u8 iob2;
			u8 iob3;
			u8 iob4;
			u8 iob5;
			u8 iob6;
			u8 iob7;
			u8 iob8;
			u8 iob9;
		};
		struct {
			u8 iob_prefix[18];
			u8 iob_values[10];
		};
	};
};

#define SAWB_LEN_SETTINGS         0x15
#define SAWB_LEN_PERFORMANCE_MODE 0x100

#define SAFN  0x5843

#define SASB_KBD_BACKLIGHT     0x78
#define SASB_POWER_MANAGEMENT  0x7a
#define SASB_USB_CHARGE_GET    0x67
#define SASB_USB_CHARGE_SET    0x68
#define SASB_NOTIFICATIONS     0x86
#define SASB_ALLOW_RECORDING   0x8a
#define SASB_PERFORMANCE_MODE  0x91

#define SAWB_RFLG_POS  4
#define SAWB_GUNM_POS  5

#define RFLG_SUCCESS  0xaa
#define GUNM_FAIL     0xff

#define GUNM_FEATURE_ENABLE          0xbb
#define GUNM_FEATURE_ENABLE_SUCCESS  0xdd
#define GUDS_FEATURE_ENABLE          0xaa
#define GUDS_FEATURE_ENABLE_SUCCESS  0xcc

#define GUNM_GET  0x81
#define GUNM_SET  0x82

#define GUNM_POWER_MANAGEMENT  0x82

#define GUNM_USB_CHARGE_GET              0x80
#define GUNM_USB_CHARGE_ON               0x81
#define GUNM_USB_CHARGE_OFF              0x80
#define GUDS_START_ON_LID_OPEN           0xa3
#define GUDS_START_ON_LID_OPEN_GET       0x81
#define GUDS_START_ON_LID_OPEN_SET       0x80
#define GUDS_BATTERY_CHARGE_CONTROL      0xe9
#define GUDS_BATTERY_CHARGE_CONTROL_GET  0x91
#define GUDS_BATTERY_CHARGE_CONTROL_SET  0x90
#define GUNM_ACPI_NOTIFY_ENABLE          0x80
#define GUDS_ACPI_NOTIFY_ENABLE          0x02

#define FNCN_PERFORMANCE_MODE       0x51
#define SUBN_PERFORMANCE_MODE_LIST  0x01
#define SUBN_PERFORMANCE_MODE_GET   0x02
#define SUBN_PERFORMANCE_MODE_SET   0x03

/* guid 8246028d-8bca-4a55-ba0f-6f1e6b921b8f */
static const guid_t performance_mode_guid_value =
	GUID_INIT(0x8246028d, 0x8bca, 0x4a55, 0xba, 0x0f, 0x6f, 0x1e, 0x6b, 0x92, 0x1b, 0x8f);
#define PERFORMANCE_MODE_GUID performance_mode_guid_value

#define PERFORMANCE_MODE_ULTRA               0x16
#define PERFORMANCE_MODE_PERFORMANCE         0x15
#define PERFORMANCE_MODE_SILENT              0xb
#define PERFORMANCE_MODE_QUIET               0xa
#define PERFORMANCE_MODE_OPTIMIZED           0x2
#define PERFORMANCE_MODE_PERFORMANCE_LEGACY  0x1
#define PERFORMANCE_MODE_OPTIMIZED_LEGACY    0x0
#define PERFORMANCE_MODE_UNKNOWN             0xff

#define DEFAULT_PLATFORM_PROFILE PLATFORM_PROFILE_BALANCED

#define ACPI_METHOD_ENABLE           "SDLS"
#define ACPI_METHOD_ENABLE_ON        1
#define ACPI_METHOD_ENABLE_OFF       0
#define ACPI_METHOD_SETTINGS         "CSFI"
#define ACPI_METHOD_PERFORMANCE_MODE "CSXI"

#define ACPI_FAN_DEVICE_ID    "PNP0C0B"
#define ACPI_FAN_SPEED_LIST   "FANT"
#define ACPI_FAN_SPEED_VALUE  "\\_SB.PC00.LPCB.H_EC.FANS"

#define KBD_BACKLIGHT_MAX_BRIGHTNESS  3

#define ACPI_NOTIFY_BATTERY_STATE_CHANGED    0x61
#define ACPI_NOTIFY_DEVICE_ON_TABLE          0x6c
#define ACPI_NOTIFY_DEVICE_OFF_TABLE         0x6d
#define ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE  0x70

#define GB_KEY_KBD_BACKLIGHT_KEYDOWN    0x2c
#define GB_KEY_KBD_BACKLIGHT_KEYUP      0xac
#define GB_KEY_ALLOW_RECORDING_KEYDOWN  0x1f
#define GB_KEY_ALLOW_RECORDING_KEYUP    0x9f

static const struct key_entry galaxybook_acpi_keymap[] = {
	{KE_KEY, ACPI_NOTIFY_BATTERY_STATE_CHANGED, { KEY_BATTERY } },
	{KE_KEY, ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE, { KEY_PROG3 } },
	{KE_KEY, ACPI_NOTIFY_DEVICE_ON_TABLE, { KEY_F14 } },
	{KE_KEY, ACPI_NOTIFY_DEVICE_OFF_TABLE, { KEY_F15 } },
	{KE_END, 0},
};

/*
 * ACPI method handling
 */

#define pr_debug_prefixed(...) pr_debug("[DEBUG] " __VA_ARGS__)

#define print_acpi_object_buffer_debug(header_str, buf_ptr, buf_len) 	\
	do {																\
		pr_debug_prefixed("%s\n", header_str);							\
		print_hex_dump_debug("samsung_galaxybook: [DEBUG]   ",			\
				DUMP_PREFIX_NONE, 16, 1, buf_ptr, buf_len, false);		\
	} while (0)

static char * get_acpi_device_description(struct acpi_device *acpi_dev)
{
	struct acpi_buffer name_buf = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = acpi_get_name(acpi_dev->handle, ACPI_SINGLE_NAME, &name_buf);
	if (ACPI_SUCCESS(status) &&	name_buf.length > 0)
		return name_buf.pointer;

	if (name_buf.pointer)
		kfree(name_buf.pointer);

	return NULL;
}

static int galaxybook_acpi_method(struct samsung_galaxybook *galaxybook, acpi_string method,
				struct sawb *buf, u32 len, const char *purpose_str, struct sawb *ret)
{
	union acpi_object in_obj, *out_obj;
	struct acpi_object_list input;
	struct acpi_buffer output = {ACPI_ALLOCATE_BUFFER, NULL};
	acpi_status status;

	in_obj.type = ACPI_TYPE_BUFFER;
	in_obj.buffer.length = len;
	in_obj.buffer.pointer = (u8 *) buf;

	input.count = 1;
	input.pointer = &in_obj;

	print_acpi_object_buffer_debug(purpose_str, in_obj.buffer.pointer, in_obj.buffer.length);

	status = acpi_evaluate_object(galaxybook->acpi->handle, method, &input, &output);

	if (ACPI_SUCCESS(status)) {
		out_obj = output.pointer;
		if (out_obj->type != ACPI_TYPE_BUFFER) {
			pr_err("failed %s with ACPI method %s; response was not a buffer\n",
					purpose_str,
					method);
			status = -EIO;
		} else {
			print_acpi_object_buffer_debug("response was: ",
					out_obj->buffer.pointer, out_obj->buffer.length);
		}
		if (out_obj->buffer.length != len) {
			pr_err("failed %s with ACPI method %s; response length mismatch\n",
					purpose_str,
					method);
			status = -EIO;
		} else if (out_obj->buffer.length < SAWB_GUNM_POS + 1) {
			pr_err("failed %s with ACPI method %s; response from device was too short\n",
					purpose_str,
					method);
			status = -EIO;
		} else if (out_obj->buffer.pointer[SAWB_RFLG_POS] != RFLG_SUCCESS) {
			pr_err("failed %s with ACPI method %s; device did not respond with success code 0x%x\n",
					purpose_str,
					method,
					RFLG_SUCCESS);
			status = -EIO;
		} else if (out_obj->buffer.pointer[SAWB_GUNM_POS] == GUNM_FAIL) {
			pr_err("failed %s with ACPI method %s; device responded with failure code 0x%x\n",
					purpose_str,
					method,
					GUNM_FAIL);
			status = -EIO;
		} else {
			memcpy(ret, out_obj->buffer.pointer, len);
		}
		kfree(output.pointer);
		return status;
	} else {
		pr_err("failed %s with ACPI method %s; got %s\n",
				purpose_str,
				method,
				acpi_format_exception(status));
		return status;
	}
}

static int galaxybook_enable_acpi_feature(struct samsung_galaxybook *galaxybook, const u16 sasb)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = sasb;
	buf.gunm = GUNM_FEATURE_ENABLE;
	buf.guds[0] = GUDS_FEATURE_ENABLE;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"enabling ACPI feature", &buf);
	if (err)
		return err;

	if (buf.gunm != GUNM_FEATURE_ENABLE_SUCCESS && buf.guds[0] != GUDS_FEATURE_ENABLE_SUCCESS)
		return -ENODEV;

	return 0;
}

/*
 * Keyboard Backlight
 */

static int kbd_backlight_acpi_set(struct samsung_galaxybook *galaxybook,
				const enum led_brightness brightness)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_KBD_BACKLIGHT;
	buf.gunm = GUNM_SET;

	buf.guds[0] = brightness;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting kbd_backlight brightness", &buf);
	if (err)
		return err;

	galaxybook->kbd_backlight.brightness = brightness;

	pr_debug_prefixed("set kbd_backlight brightness to %d\n", brightness);

	return 0;
}

static int kbd_backlight_acpi_get(struct samsung_galaxybook *galaxybook,
				enum led_brightness *brightness)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_KBD_BACKLIGHT;
	buf.gunm = GUNM_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting kbd_backlight brightness", &buf);
	if (err)
		return err;

	*brightness = buf.gunm;
	galaxybook->kbd_backlight.brightness = buf.gunm;

	pr_debug_prefixed("current kbd_backlight brightness is %d\n", buf.gunm);

	return 0;
}

static int kbd_backlight_store(struct led_classdev *led,
		const enum led_brightness brightness)
{
	struct samsung_galaxybook *galaxybook = container_of(led,
			struct samsung_galaxybook, kbd_backlight);
	int err;

	err = kbd_backlight_acpi_set(galaxybook, brightness);
	if (err)
		return err;

	return 0;
}

static enum led_brightness kbd_backlight_show(struct led_classdev *led)
{
	struct samsung_galaxybook *galaxybook = container_of(led,
			struct samsung_galaxybook, kbd_backlight);
	enum led_brightness brightness;
	int err;

	err = kbd_backlight_acpi_get(galaxybook, &brightness);
	if (err)
		return err;

	return brightness;
}

static int galaxybook_kbd_backlight_init(struct samsung_galaxybook *galaxybook)
{
	enum led_brightness brightness;
	struct led_init_data init_data = {};
	int err;

	err = galaxybook_enable_acpi_feature(galaxybook, SASB_KBD_BACKLIGHT);
	if (err)
		return err;

	/* verify we can read the value, otherwise init should stop and fail */
	err = kbd_backlight_acpi_get(galaxybook, &brightness);
	if (err)
		return err;

	init_data.devicename = SAMSUNG_GALAXYBOOK_CLASS;
	init_data.default_label = ":kbd_backlight";
	init_data.devname_mandatory = true;

	galaxybook->kbd_backlight = (struct led_classdev) {
		.brightness_get = kbd_backlight_show,
		.brightness_set_blocking = kbd_backlight_store,
		.flags = LED_BRIGHT_HW_CHANGED,
		.max_brightness = KBD_BACKLIGHT_MAX_BRIGHTNESS,
	};

	pr_info("registering LED class using default name of %s:%s\n",
			init_data.devicename, init_data.default_label);

	return led_classdev_register_ext(&galaxybook->platform->dev, &galaxybook->kbd_backlight,
			&init_data);
}

static void galaxybook_kbd_backlight_exit(struct samsung_galaxybook *galaxybook)
{
	led_classdev_unregister(&galaxybook->kbd_backlight);
}

/*
 * Platform device attributes (configuration properties which can be controlled via userspace)
 */

/* Start on lid open (device should power on when lid is opened) */

static int start_on_lid_open_acpi_set(struct samsung_galaxybook *galaxybook, const bool value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_POWER_MANAGEMENT;
	buf.guds[0] = GUDS_START_ON_LID_OPEN;
	buf.guds[1] = GUDS_START_ON_LID_OPEN_SET;
	buf.guds[2] = value;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting start_on_lid_open", &buf);
	if (err)
		return err;

	pr_debug_prefixed("turned start_on_lid_open %s\n", value ? "on (1)" : "off (0)");

	return 0;
}

static int start_on_lid_open_acpi_get(struct samsung_galaxybook *galaxybook, bool *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_POWER_MANAGEMENT;
	buf.guds[0] = GUDS_START_ON_LID_OPEN;
	buf.guds[1] = GUDS_START_ON_LID_OPEN_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting start_on_lid_open", &buf);
	if (err)
		return err;

	*value = buf.guds[1];

	pr_debug_prefixed("start_on_lid_open is currently %s\n", (buf.guds[1] ? "on (1)" : "off (0)"));

	return 0;
}

static ssize_t start_on_lid_open_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	if (!count || kstrtobool(buffer, &value))
		return -EINVAL;

	err = start_on_lid_open_acpi_set(galaxybook, value);
	if (err)
		return err;

	return count;
}

static ssize_t start_on_lid_open_show(struct device *dev, struct device_attribute *attr,
				char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	err = start_on_lid_open_acpi_get(galaxybook, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%u\n", value);
}

static DEVICE_ATTR_RW(start_on_lid_open);

/* USB Charge (USB ports can charge other devices even when device is powered off) */

static int usb_charge_acpi_set(struct samsung_galaxybook *galaxybook, const bool value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_USB_CHARGE_SET;
	buf.gunm = value ? GUNM_USB_CHARGE_ON : GUNM_USB_CHARGE_OFF;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting usb_charge", &buf);
	if (err)
		return err;

	pr_debug_prefixed("turned usb_charge %s\n", value ? "on (1)" : "off (0)");

	return 0;
}

static int usb_charge_acpi_get(struct samsung_galaxybook *galaxybook, bool *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_USB_CHARGE_GET;
	buf.gunm = GUNM_USB_CHARGE_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting usb_charge", &buf);
	if (err)
		return err;

	*value = buf.gunm;

	pr_debug_prefixed("usb_charge is currently %s\n", (buf.gunm ? "on (1)" : "off (0)"));

	return 0;
}

static ssize_t usb_charge_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	if (!count || kstrtobool(buffer, &value))
		return -EINVAL;

	err = usb_charge_acpi_set(galaxybook, value);
	if (err)
		return err;

	return count;
}

static ssize_t usb_charge_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	err = usb_charge_acpi_get(galaxybook, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%u\n", value);
}

static DEVICE_ATTR_RW(usb_charge);

/* Allow recording (allows or blocks access to camera and microphone) */

static int allow_recording_acpi_set(struct samsung_galaxybook *galaxybook, const bool value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_ALLOW_RECORDING;
	buf.gunm = GUNM_SET;
	buf.guds[0] = value;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting allow_recording", &buf);
	if (err)
		return err;

	pr_debug_prefixed("turned allow_recording %s\n", value ? "on (1)" : "off (0)");

	return 0;
}

static int allow_recording_acpi_get(struct samsung_galaxybook *galaxybook, bool *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_ALLOW_RECORDING;
	buf.gunm = GUNM_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting allow_recording", &buf);
	if (err)
		return err;

	*value = buf.gunm;

	pr_debug_prefixed("allow_recording is currently %s\n", (buf.gunm ? "on (1)" : "off (0)"));

	return 0;
}

static ssize_t allow_recording_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	if (!count || kstrtobool(buffer, &value))
		return -EINVAL;

	err = allow_recording_acpi_set(galaxybook, value);
	if (err)
		return err;

	return count;
}

static ssize_t allow_recording_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	err = allow_recording_acpi_get(galaxybook, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%u\n", value);
}

static DEVICE_ATTR_RW(allow_recording);

/*
 * Battery Extension (adds charge_control_end_threshold to the battery device)
 */

static int charge_control_end_threshold_acpi_set(struct samsung_galaxybook *galaxybook,
				const u8 value)
{
	struct sawb buf = {0};
	int err;

	if (value > 100)
		return -EINVAL;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_POWER_MANAGEMENT;
	buf.guds[0] = GUDS_BATTERY_CHARGE_CONTROL;
	buf.guds[1] = GUDS_BATTERY_CHARGE_CONTROL_SET;

	buf.guds[2] = (value == 100 ? 0 : value); /* if setting to 100, it should be set to 0 (off) */

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting battery charge_control_end_threshold", &buf);
	if (err)
		return err;

	pr_debug_prefixed("set battery charge_control_end_threshold to %d\n",
			(value == 100 ? 0 : value));

	return 0;
}

static int charge_control_end_threshold_acpi_get(struct samsung_galaxybook *galaxybook, u8 *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_POWER_MANAGEMENT;
	buf.guds[0] = GUDS_BATTERY_CHARGE_CONTROL;
	buf.guds[1] = GUDS_BATTERY_CHARGE_CONTROL_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting battery charge_control_end_threshold", &buf);
	if (err)
		return err;

	*value = buf.guds[1];

	pr_debug_prefixed("battery charge control is currently %s; " \
			"battery charge_control_end_threshold is %d\n",
			(buf.guds[1] > 0 ? "on" : "off"), buf.guds[1]);

	return 0;
}

static ssize_t charge_control_end_threshold_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	u8 value;
	int err;

	if (!count || kstrtou8(buffer, 0, &value))
		return -EINVAL;

	err = charge_control_end_threshold_acpi_set(galaxybook_ptr, value);
	if (err)
		return err;

	return count;
}

static ssize_t charge_control_end_threshold_show(struct device *dev, struct device_attribute *attr,
				char *buffer)
{
	u8 value;
	int err;

	err = charge_control_end_threshold_acpi_get(galaxybook_ptr, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%d\n", value);
}

static DEVICE_ATTR_RW(charge_control_end_threshold);

static int galaxybook_battery_add(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	if (device_create_file(&battery->dev, &dev_attr_charge_control_end_threshold))
		return -ENODEV;
	return 0;
}

static int galaxybook_battery_remove(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	device_remove_file(&battery->dev, &dev_attr_charge_control_end_threshold);
	return 0;
}

static struct acpi_battery_hook galaxybook_battery_hook = {
	.add_battery = galaxybook_battery_add,
	.remove_battery = galaxybook_battery_remove,
	.name = "Samsung Galaxy Book Battery Extension",
};

static int galaxybook_battery_threshold_init(struct samsung_galaxybook *galaxybook)
{
	u8 value;
	int err;

	err = charge_control_end_threshold_acpi_get(galaxybook, &value);
	if (err)
		return err;

	battery_hook_register(&galaxybook_battery_hook);
	return 0;
}

static void galaxybook_battery_threshold_exit(struct samsung_galaxybook *galaxybook)
{
	battery_hook_unregister(&galaxybook_battery_hook);
}

/*
 * Fan speed
 */

static int fan_speed_get_fst(struct galaxybook_fan *fan, unsigned int *speed)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response_obj = NULL;
	acpi_status status;
	int ret = 0;

	status = acpi_evaluate_object(fan->fan.handle, "_FST", NULL, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("Get fan state failed\n");
		return -ENODEV;
	}

	response_obj = response.pointer;
	if (!response_obj || response_obj->type != ACPI_TYPE_PACKAGE ||
			response_obj->package.count != 3 ||
			response_obj->package.elements[2].type != ACPI_TYPE_INTEGER) {
		pr_err("Invalid _FST data\n");
		ret = -EINVAL;
		goto out_free;
	}

	*speed = response_obj->package.elements[2].integer.value;

	pr_debug_prefixed("fan device %s (%s) reporting fan speed of %d\n",
			dev_name(&fan->fan.dev), fan->description, *speed);

out_free:
	ACPI_FREE(response.pointer);
	return ret;
}

static int fan_speed_get_fans(struct galaxybook_fan *fan, unsigned int *speed)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response_obj = NULL;
	acpi_status status;
	int ret = 0;
	int speed_level = -1;

	status = acpi_evaluate_object(NULL, ACPI_FAN_SPEED_VALUE, NULL, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("Get fan state failed\n");
		return -ENODEV;
	}

	response_obj = response.pointer;
	if (!response_obj ||
			response_obj->type != ACPI_TYPE_INTEGER ||
			response_obj->integer.value > INT_MAX ||
			(int) response_obj->integer.value > fan->fan_speeds_count) {
		pr_err("invalid fan speed data\n");
		ret = -EINVAL;
		goto out_free;
	}

	speed_level = (int) response_obj->integer.value;
	*speed = fan->fan_speeds[speed_level];

	pr_debug_prefixed("fan device %s (%s) reporting fan speed of %d (level %d)\n",
			dev_name(&fan->fan.dev), fan->description, *speed, speed_level);

out_free:
	ACPI_FREE(response.pointer);
	return ret;
}

static int fan_speed_get(struct galaxybook_fan *fan, unsigned int *speed)
{
	if (!fan)
		return -ENODEV;
	if (fan->supports_fst)
		return fan_speed_get_fst(fan, speed);
	else
		return fan_speed_get_fans(fan, speed);
}

static ssize_t fan_speed_rpm_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);
	struct galaxybook_fan *fan = ea->var;
	unsigned int speed;
	int ret = 0;

	if (!fan)
		return -ENODEV;

	ret = fan_speed_get(fan, &speed);
	if (ret)
		return ret;

	return sysfs_emit(buffer, "%u\n", speed);
}

static int __init fan_speed_list_init(acpi_handle handle, struct galaxybook_fan *fan)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response_obj = NULL;
	acpi_status status;
	unsigned int speed;

	status = acpi_evaluate_object(handle, ACPI_FAN_SPEED_LIST, NULL, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("failed to read fan speed list\n");
		return -ENODEV;
	}

	response_obj = response.pointer;
	if (!response_obj || response_obj->type != ACPI_TYPE_PACKAGE ||
			response_obj->package.count == 0) {
		pr_err("invalid fan speed list data\n");
		status = -EINVAL;
		goto out_free;
	}

	/*
	 * fan_speeds[] starts with a hard-coded 0 (fan is off), then has some "funny" logic:
	 *  - fetch the speed level values read in from FANT and add 0x0a to each value
	 *  - _FST method in the DSDT seems to indicate that level 3 and 4 should have the same value,
	 *    however real-life observation suggests that the speed actually does change
	 *  - _FST says that level 5 should give the 4th value from FANT but it seems significantly
	 *    louder -- we will just "guess" it is 1000 RPM faster than the highest value from FANT?
	 */

	fan->fan_speeds = kzalloc(sizeof(unsigned int) * (response_obj->package.count + 2),
			GFP_KERNEL);
	if (!fan->fan_speeds)
		return -ENOMEM;

	/* hard-coded "off" value (0) */
	fan->fan_speeds[0] = 0;
	fan->fan_speeds_count = 1;

	/* fetch and assign the next values from FANT response */
	int i = 0;
	for (i = 1; i <= response_obj->package.count; i++) {
		if (response_obj->package.elements[i-1].type != ACPI_TYPE_INTEGER) {
			pr_err("invalid fan speed list value at position %d (expected type %d, got type %d)\n",
					i-1, ACPI_TYPE_INTEGER, response_obj->package.elements[i-1].type);
			status = -EINVAL;
			goto err_fan_speeds_free;
		}
		fan->fan_speeds[i] = response_obj->package.elements[i-1].integer.value + 0x0a;
		fan->fan_speeds_count++;
	}

	/* add the missing final level where we "guess" 1000 RPM faster than highest from FANT */
	if (fan->fan_speeds_count > 1) {
		fan->fan_speeds[i] = fan->fan_speeds[i-1] + 1000;
		fan->fan_speeds_count++;
	}

	/* test that it actually works to read the speed, otherwise the init should fail */
	status = fan_speed_get_fans(fan, &speed);
	if (ACPI_FAILURE(status)) {
		pr_err("failed to read fan speed level from FANS\n");
		goto err_fan_speeds_free;
	}

	pr_info("initialized fan speed reporting for device %s (%s) with the following levels:\n",
			dev_name(&fan->fan.dev), fan->description);
	for (i = 0; i < fan->fan_speeds_count; i++)
		pr_info("  %s (%s) fan speed level %d = %d\n",
				dev_name(&fan->fan.dev), fan->description, i, fan->fan_speeds[i]);

out_free:
	ACPI_FREE(response.pointer);
	return status;

err_fan_speeds_free:
	kfree(fan->fan_speeds);
	goto out_free;
}

static acpi_status galaxybook_add_fan(acpi_handle handle, u32 level, void *context,
				void **return_value)
{
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);
	struct samsung_galaxybook *galaxybook = context;
	struct galaxybook_fan *fan;
	int speed = -1;

	pr_info("found fan device %s\n", dev_name(&adev->dev));

	/* if fan meets acpi4 fan device requirements, assume it is added already under ACPI */
	if (acpi_has_method(handle, "_FIF") &&
			acpi_has_method(handle, "_FPS") &&
			acpi_has_method(handle, "_FSL") &&
			acpi_has_method(handle, "_FST")) {
		pr_info("fan device %s should already be available as an ACPI fan; skipping\n",
				dev_name(&adev->dev));
		return 0;
	}

	if (galaxybook->fans_count >= MAX_FAN_COUNT) {
		pr_err("maximum number of %d fans has already been reached\n", MAX_FAN_COUNT);
		return 0;
	}

	fan = &galaxybook->fans[galaxybook->fans_count];
	fan->fan = *adev;
	fan->description = get_acpi_device_description(&fan->fan);

	/* try to get speed from _FST */
	if (ACPI_FAILURE(fan_speed_get_fst(fan, &speed))) {
		pr_debug_prefixed("_FST is present but failed on fan device %s (%s); " \
				"will attempt to add fan speed support using FANT and FANS\n",
				dev_name(&fan->fan.dev), fan->description);
		fan->supports_fst = false;
	}
	/* if speed was 0 and FANT and FANS exist, they should be used anyway due to bugs in ACPI */
	else if (speed <= 0 &&
			acpi_has_method(handle, ACPI_FAN_SPEED_LIST) &&
			acpi_has_method(NULL, ACPI_FAN_SPEED_VALUE)) {
		pr_debug_prefixed("_FST is present on fan device %s (%s) but returned value of 0; " \
				"will attempt to add fan speed support using FANT and FANS\n",
				dev_name(&fan->fan.dev), fan->description);
		fan->supports_fst = false;
	} else {
		fan->supports_fst = true;
	}

	if (!fan->supports_fst) {
		/* since FANS is a single field on the EC, it does not make sense to use more than once */
		for (int i = 0; i < galaxybook->fans_count; i++) {
			if (!galaxybook->fans[i].supports_fst) {
				pr_err("more than one fan using FANS is not supported\n");
				return 0;
			}
		}
		if (ACPI_FAILURE(fan_speed_list_init(handle, fan))) {
			pr_err("unable to initialize fan speeds for fan device %s (%s)\n",
					dev_name(&fan->fan.dev), fan->description);
			return 0;
		}
	} else {
		pr_info("initialized fan speed reporting for device %s (%s) using method _FST\n",
				dev_name(&fan->fan.dev), fan->description);
	}

	/* set up RO dev_ext_attribute */
	fan->fan_speed_rpm_ext_attr.attr.attr.name = "fan_speed_rpm";
	fan->fan_speed_rpm_ext_attr.attr.attr.mode = 0444;
	fan->fan_speed_rpm_ext_attr.attr.show = fan_speed_rpm_show;
	/* extended attribute var points to this galaxybook_fan so it can used in the show method */
	fan->fan_speed_rpm_ext_attr.var = fan;

	if (sysfs_create_file(&adev->dev.kobj, &fan->fan_speed_rpm_ext_attr.attr.attr))
		pr_err("unable to create fan_speed_rpm attribute for fan device %s (%s)\n",
				dev_name(&fan->fan.dev), fan->description);

	galaxybook->fans_count++;

	return 0;
}

static int __init galaxybook_fan_speed_init(struct samsung_galaxybook *galaxybook)
{
	acpi_status status;

	/* get and set up all fans matching ACPI_FAN_DEVICE_ID */	
	status = acpi_get_devices(ACPI_FAN_DEVICE_ID, galaxybook_add_fan, galaxybook, NULL);

	if (galaxybook->fans_count == 0)
		return -ENODEV;

	return status;
}

static void galaxybook_fan_speed_exit(struct samsung_galaxybook *galaxybook)
{
	for (int i = 0; i < galaxybook->fans_count; i++)
		sysfs_remove_file(&galaxybook->fans[i].fan.dev.kobj,
				&galaxybook->fans[i].fan_speed_rpm_ext_attr.attr.attr);
}

/*
 * Hwmon device
 */

#if IS_ENABLED(CONFIG_HWMON)
static umode_t galaxybook_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (channel < galaxybook_ptr->fans_count &&
				(attr == hwmon_fan_input || attr == hwmon_fan_label))
			return 0444;
		return 0;
	default:
		return 0;
	}
}

static int galaxybook_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, long *val)
{
	unsigned int speed;

	switch (type) {
	case hwmon_fan:
		if (channel < galaxybook_ptr->fans_count && attr == hwmon_fan_input) {
			if (fan_speed_get(&galaxybook_ptr->fans[channel], &speed))
				return -EIO;
			*val = speed;
			return 0;
		}
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static int galaxybook_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		if (channel < galaxybook_ptr->fans_count && attr == hwmon_fan_label) {
			*str = galaxybook_ptr->fans[channel].description;
			return 0;
		}
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops galaxybook_hwmon_ops = {
	.is_visible = galaxybook_hwmon_is_visible,
	.read = galaxybook_hwmon_read,
	.read_string = galaxybook_hwmon_read_string,
};

static const struct hwmon_channel_info * const galaxybook_hwmon_info[] = {
	/* note: number of max possible fan channel entries here should match MAX_FAN_COUNT */
	HWMON_CHANNEL_INFO(fan,
			HWMON_F_INPUT | HWMON_F_LABEL,
			HWMON_F_INPUT | HWMON_F_LABEL,
			HWMON_F_INPUT | HWMON_F_LABEL,
			HWMON_F_INPUT | HWMON_F_LABEL,
			HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info galaxybook_hwmon_chip_info = {
	.ops = &galaxybook_hwmon_ops,
	.info = galaxybook_hwmon_info,
};

static int galaxybook_hwmon_init(struct samsung_galaxybook *galaxybook)
{
	int ret = 0;

	char *hwmon_device_name = devm_hwmon_sanitize_name(&galaxybook->platform->dev,
			SAMSUNG_GALAXYBOOK_CLASS);

	galaxybook->hwmon = devm_hwmon_device_register_with_info(&galaxybook->platform->dev,
			hwmon_device_name, NULL, &galaxybook_hwmon_chip_info, NULL);
	if (PTR_ERR_OR_ZERO(galaxybook->hwmon)) {
		ret = PTR_ERR(galaxybook->hwmon);
		galaxybook->hwmon = NULL;
	}

	return ret;
}

static void galaxybook_hwmon_exit(struct samsung_galaxybook *galaxybook)
{
	if (galaxybook->hwmon)
		hwmon_device_unregister(galaxybook->hwmon);
}
#endif

/*
 * Platform Profile / Performance mode
 */

static int performance_mode_acpi_set(struct samsung_galaxybook *galaxybook,
				const u8 performance_mode)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_PERFORMANCE_MODE;
	export_guid(buf.caid, &PERFORMANCE_MODE_GUID);
	buf.fncn = FNCN_PERFORMANCE_MODE;
	buf.subn = SUBN_PERFORMANCE_MODE_SET;
	buf.iob0 = performance_mode;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &buf,
			SAWB_LEN_PERFORMANCE_MODE, "setting performance_mode", &buf);
	if (err)
		return err;

	return 0;
}

static int performance_mode_acpi_get(struct samsung_galaxybook *galaxybook, u8 *performance_mode)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_PERFORMANCE_MODE;
	export_guid(buf.caid, &PERFORMANCE_MODE_GUID);
	buf.fncn = FNCN_PERFORMANCE_MODE;
	buf.subn = SUBN_PERFORMANCE_MODE_GET;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &buf,
			SAWB_LEN_PERFORMANCE_MODE, "getting performance_mode", &buf);
	if (err)
		return err;

	*performance_mode = buf.iob0;

	return 0;
}

static enum platform_profile_option profile_performance_mode(
				struct samsung_galaxybook *galaxybook, const u8 performance_mode)
{
	for (int i = 0; i < PLATFORM_PROFILE_LAST; i++)
		if (galaxybook->profile_performance_modes[i] == performance_mode)
			return i;
	return -1;
}

/* copied from platform_profile.c; better if this could be fetched from a public function, maybe? */
static const char * const profile_names[] = {
	[PLATFORM_PROFILE_LOW_POWER] = "low-power",
	[PLATFORM_PROFILE_COOL] = "cool",
	[PLATFORM_PROFILE_QUIET] = "quiet",
	[PLATFORM_PROFILE_BALANCED] = "balanced",
	[PLATFORM_PROFILE_BALANCED_PERFORMANCE] = "balanced-performance",
	[PLATFORM_PROFILE_PERFORMANCE] = "performance",
};
static_assert(ARRAY_SIZE(profile_names) == PLATFORM_PROFILE_LAST);

static int galaxybook_platform_profile_set(struct platform_profile_handler *pprof,
				enum platform_profile_option profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	int err;

	err = performance_mode_acpi_set(galaxybook, galaxybook->profile_performance_modes[profile]);
	if (err)
		return err;

	pr_debug_prefixed("set platform profile to '%s' (performance mode 0x%x)\n",
			profile_names[profile], galaxybook->profile_performance_modes[profile]);
	return 0;
}

static int galaxybook_platform_profile_get(struct platform_profile_handler *pprof,
				enum platform_profile_option *profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	u8 performance_mode;
	int err;

	err = performance_mode_acpi_get(galaxybook, &performance_mode);
	if (err)
		return err;

	*profile = profile_performance_mode(galaxybook, performance_mode);
	if (*profile == -1)
		return -EINVAL;

	pr_debug_prefixed("platform profile is currently '%s' (performance mode 0x%x)\n",
			profile_names[*profile], performance_mode);

	return 0;
}

#define IGNORE_PERFORMANCE_MODE_MAPPING  -1

static int galaxybook_profile_init(struct samsung_galaxybook *galaxybook)
{
	struct sawb buf = {0};
	int mode_profile, err;

	galaxybook->profile_handler.profile_get = galaxybook_platform_profile_get;
	galaxybook->profile_handler.profile_set = galaxybook_platform_profile_set;

	/* fetch supported performance mode values from ACPI method */
	buf.safn = SAFN;
	buf.sasb = SASB_PERFORMANCE_MODE;
	export_guid(buf.caid, &PERFORMANCE_MODE_GUID);
	buf.fncn = FNCN_PERFORMANCE_MODE;
	buf.subn = SUBN_PERFORMANCE_MODE_LIST;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &buf,
			SAWB_LEN_PERFORMANCE_MODE, "get supported performance modes", &buf);
	if (err)
		return err;

	/* set up profile_performance_modes with "unknown" as init value */
	galaxybook->profile_performance_modes = kzalloc(sizeof(u8) * PLATFORM_PROFILE_LAST, GFP_KERNEL);
	if (!galaxybook->profile_performance_modes)
		return -ENOMEM;
	for (int i = 0; i < PLATFORM_PROFILE_LAST; i++)
		galaxybook->profile_performance_modes[i] = PERFORMANCE_MODE_UNKNOWN;

	/*
	 * Value returned in iob0 will have the number of supported performance modes.
	 * The performance mode values will then be given as a list after this (iob1-iobX).
	 * Loop backwards from last value to first value (to handle fallback cases which will come with
	 * smaller values) and map each supported value to its correct platform_profile_option.
	 */
	err = -ENODEV; /* set err to "no device" to signal that we have not yet mapped any profiles */
	for (int i = buf.iob0; i > 0; i--) {
		/*
		 * Prefer mapping to at least performance, balanced, and low-power profiles, as these are
		 * the ones which are typically supported by userspace tools (power-profiles-daemon, etc).
		 * - performance = "ultra", otherwise "performance"
		 * - balanced    = "optimized", otherwise "performance" when "ultra" is supported
		 * - low-power   = "silent", otherwise "quiet"
		 * Different models support different modes. Additional supported modes will be mapped to
		 * profiles that fall in between these 3.
		 */
		switch (buf.iob_values[i]) {

		case PERFORMANCE_MODE_ULTRA:
			/* ultra always maps to performance */
			mode_profile = PLATFORM_PROFILE_PERFORMANCE;
			break;

		case PERFORMANCE_MODE_PERFORMANCE:
			/* if ultra exists, map performance to balanced-performance */
			if (galaxybook->profile_performance_modes[PLATFORM_PROFILE_PERFORMANCE] !=
					PERFORMANCE_MODE_UNKNOWN)
				mode_profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
			else /* otherwise map it to performance instead */
				mode_profile = PLATFORM_PROFILE_PERFORMANCE;
			break;

		case PERFORMANCE_MODE_SILENT:
			/* silent always maps to low-power */
			mode_profile = PLATFORM_PROFILE_LOW_POWER;
			break;

		case PERFORMANCE_MODE_QUIET:
			/* if silent exists, map quiet to quiet */
			if (galaxybook->profile_performance_modes[PLATFORM_PROFILE_LOW_POWER] !=
					PERFORMANCE_MODE_UNKNOWN)
				mode_profile = PLATFORM_PROFILE_QUIET;
			else /* otherwise map it to low-power for better support in userspace tools */
				mode_profile = PLATFORM_PROFILE_LOW_POWER;
			break;

		case PERFORMANCE_MODE_OPTIMIZED:
			/* optimized always maps to balanced */
			mode_profile = PLATFORM_PROFILE_BALANCED;
			break;

		case PERFORMANCE_MODE_PERFORMANCE_LEGACY:
			/* map to performance if performance is not already supported */
			if (galaxybook->profile_performance_modes[PLATFORM_PROFILE_PERFORMANCE] ==
					PERFORMANCE_MODE_UNKNOWN)
				mode_profile = PLATFORM_PROFILE_PERFORMANCE;
			else /* otherwise, ignore */
				mode_profile = IGNORE_PERFORMANCE_MODE_MAPPING;
			break;

		case PERFORMANCE_MODE_OPTIMIZED_LEGACY:
			/* map to balanced if balanced is not already supported */
			if (galaxybook->profile_performance_modes[PLATFORM_PROFILE_BALANCED] ==
					PERFORMANCE_MODE_UNKNOWN)
				mode_profile = PLATFORM_PROFILE_BALANCED;
			else /* otherwise, ignore */
				mode_profile = IGNORE_PERFORMANCE_MODE_MAPPING;
			break;

		default: /* any other value is not supported */
			mode_profile = IGNORE_PERFORMANCE_MODE_MAPPING;
			break;
		}

		/* if current mode value was mapped to a supported platform_profile_option, set it up */
		if (mode_profile > IGNORE_PERFORMANCE_MODE_MAPPING) {
			err = 0; /* clear err to signal that at least one profile is now mapped */
			galaxybook->profile_performance_modes[mode_profile] = buf.iob_values[i];
			set_bit(mode_profile, galaxybook->profile_handler.choices);
			pr_info("will support platform profile '%s' with performance mode value 0x%x\n",
						profile_names[mode_profile], buf.iob_values[i]);
		} else {
			pr_debug_prefixed("unmapped performance mode value 0x%x will be ignored\n",
					buf.iob_values[i]);
		}
	}

	/* if no performance modes were mapped (err is still -ENODEV) then stop and fail here */
	if (err)
		return err;

	err = platform_profile_register(&galaxybook->profile_handler);
	if (err)
		return err;

	/* now check currently set performance mode; if it is not supported then set default profile */
	u8 current_performance_mode;
	err = performance_mode_acpi_get(galaxybook, &current_performance_mode);
	if (err)
		pr_warn("failed with code %d when fetching initial performance mode\n", err);
	if (profile_performance_mode(galaxybook, current_performance_mode) == -1) {
		pr_debug_prefixed("initial performance mode value is not supported by device; " \
				"setting to default\n");
		err = galaxybook_platform_profile_set(&galaxybook->profile_handler,
				DEFAULT_PLATFORM_PROFILE);
		if (err)
			return err;
	}

	return 0;
}

static void galaxybook_profile_exit(struct samsung_galaxybook *galaxybook)
{
	platform_profile_remove();
}

/*
 * Hotkey work and filters
 */

static void galaxybook_performance_mode_hotkey_work(struct work_struct *work)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	platform_profile_cycle();
#else
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, performance_mode_hotkey_work);
	u8 current_performance_mode;
	enum platform_profile_option current_profile;
	int i;

	if (!galaxybook->profile_performance_modes)
		return;

	performance_mode_acpi_get(galaxybook, &current_performance_mode);
	current_profile = profile_performance_mode(galaxybook, current_performance_mode);
	current_profile++;
	/* try setting the "next" profile starting from the current */
	for (i = current_profile; i < PLATFORM_PROFILE_LAST; i++) {
		if (galaxybook->profile_performance_modes[i] != PERFORMANCE_MODE_UNKNOWN) {
			galaxybook_platform_profile_set(&galaxybook->profile_handler, i);
			platform_profile_notify();
			return;
		}
	}
	/* if that did not work, maybe we were at the end; start again from 0 and try again */
	for (i = 0; i < PLATFORM_PROFILE_LAST; i++) {
		if (galaxybook->profile_performance_modes[i] != PERFORMANCE_MODE_UNKNOWN) {
			galaxybook_platform_profile_set(&galaxybook->profile_handler, i);
			platform_profile_notify();
			return;
		}
	}
	/* if that still did not work, there was some kind of problem */
	pr_warn("performance mode hotkey failed to find any supported profile to apply\n");
#endif
	return;
}

static void galaxybook_kbd_backlight_hotkey_work(struct work_struct *work)
{
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, kbd_backlight_hotkey_work);

	if (galaxybook->kbd_backlight.brightness < galaxybook->kbd_backlight.max_brightness)
		kbd_backlight_acpi_set(galaxybook, galaxybook->kbd_backlight.brightness + 1);
	else
		kbd_backlight_acpi_set(galaxybook, 0);

	led_classdev_notify_brightness_hw_changed(&galaxybook->kbd_backlight,
			galaxybook->kbd_backlight.brightness);
}

static void galaxybook_allow_recording_hotkey_work(struct work_struct *work)
{
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, allow_recording_hotkey_work);
	bool value;

	allow_recording_acpi_get(galaxybook, &value);
	allow_recording_acpi_set(galaxybook, !value);
}

static bool galaxybook_i8042_filter(unsigned char data, unsigned char str,
				    				struct serio *port)
{
	static bool extended;

	if (str & I8042_STR_AUXDATA)
		return false;

	if (unlikely(data == 0xe0)) {
		extended = true;
		return false;
	} else if (unlikely(extended)) {
		extended = false;

		switch (data) {
		case GB_KEY_KBD_BACKLIGHT_KEYDOWN:
			pr_debug_prefixed("hotkey: kbd_backlight keydown\n");
			break;
		case GB_KEY_KBD_BACKLIGHT_KEYUP:
			pr_debug_prefixed("hotkey: kbd_backlight keyup\n");
			if (kbd_backlight)
				schedule_work(&galaxybook_ptr->kbd_backlight_hotkey_work);
			break;
		case GB_KEY_ALLOW_RECORDING_KEYDOWN:
			pr_debug_prefixed("hotkey: allow_recording keydown\n");
			break;
		case GB_KEY_ALLOW_RECORDING_KEYUP:
			pr_debug_prefixed("hotkey: allow_recording keyup\n");
			if (allow_recording)
				schedule_work(&galaxybook_ptr->allow_recording_hotkey_work);
			break;
		}
	}

	return false;
}

/*
 * Input device (hotkeys and notifications)
 */

static void galaxybook_input_notify(struct samsung_galaxybook *galaxybook, int event)
{
	if (!galaxybook->input)
		return;
	pr_debug_prefixed("input notification event: 0x%x\n", event);
	if (!sparse_keymap_report_event(galaxybook->input, event, 1, true))
		pr_warn("unknown input notification event: 0x%x\n", event);
}

static int galaxybook_input_init(struct samsung_galaxybook *galaxybook)
{
	struct input_dev *input;
	int error;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = "Samsung Galaxy Book Extra Buttons";
	input->phys = SAMSUNG_GALAXYBOOK_CLASS "/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = &galaxybook->platform->dev;

	error = sparse_keymap_setup(input, galaxybook_acpi_keymap, NULL);
	if (error) {
		pr_err("Unable to setup input device keymap\n");
		goto err_free_dev;
	}
	error = input_register_device(input);
	if (error) {
		pr_err("Unable to register input device\n");
		goto err_free_dev;
	}

	galaxybook->input = input;
	return 0;

err_free_dev:
	input_free_device(input);
	return error;
}

static void galaxybook_input_exit(struct samsung_galaxybook *galaxybook)
{
	if (galaxybook->input)
		input_unregister_device(galaxybook->input);
	galaxybook->input = NULL;
}

/*
 * Platform device attributes
 */

/* galaxybook_attrs can include start_on_lid_open, usb_charge, and/or allow_recording */
#define MAX_NUM_DEVICE_ATTRIBUTES 3

static struct attribute *galaxybook_attrs[MAX_NUM_DEVICE_ATTRIBUTES+1] = { NULL };
static const struct attribute_group galaxybook_attrs_group = {
	.attrs = galaxybook_attrs,
};

static int galaxybook_device_attrs_init(struct samsung_galaxybook *galaxybook)
{
	bool value;
	int err;
	int i = 0;

	/* attempt to get each attribute's value and add them if the get does not fail */

	err = start_on_lid_open_acpi_get(galaxybook, &value);
	if (err)
		pr_debug_prefixed("failed to get start_on_lid_open value; " \
				"this feature will not be enabled\n");
	else
		galaxybook_attrs[i++] = &dev_attr_start_on_lid_open.attr;

	err = usb_charge_acpi_get(galaxybook, &value);
	if (err)
		pr_debug_prefixed("failed to get usb_charge value; this feature will not be enabled\n");
	else
		galaxybook_attrs[i++] = &dev_attr_usb_charge.attr;

	if (allow_recording) {
		pr_debug_prefixed("initializing ACPI allow_recording feature\n");
		err = galaxybook_enable_acpi_feature(galaxybook, SASB_ALLOW_RECORDING);
		if (err) {
			pr_debug_prefixed("failed to initialize ACPI allow_recording feature\n");
			allow_recording = false;
			return 0;
		}

		err = allow_recording_acpi_get(galaxybook, &value);
		if (err) {
			pr_debug_prefixed("failed to get allow_recording value; " \
					"this feature will not be enabled\n");
			allow_recording = false;
		} else {
			galaxybook_attrs[i++] = &dev_attr_allow_recording.attr;
		}
	}

	return device_add_group(&galaxybook->platform->dev, &galaxybook_attrs_group);
};

static void galaxybook_device_attrs_exit(struct samsung_galaxybook *galaxybook)
{
	device_remove_group(&galaxybook->platform->dev, &galaxybook_attrs_group);
}

/*
 * ACPI device setup
 */

static void galaxybook_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	struct samsung_galaxybook *galaxybook = data;

	if (event == ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE) {
		pr_debug_prefixed("hotkey: performance_mode keydown\n");
		if (performance_mode)
			schedule_work(&galaxybook->performance_mode_hotkey_work);
	}

	galaxybook_input_notify(galaxybook, event);
}

static int galaxybook_enable_acpi_notify(struct samsung_galaxybook *galaxybook)
{
	struct sawb buf = {0};
	int err;

	err = galaxybook_enable_acpi_feature(galaxybook, SASB_NOTIFICATIONS);
	if (err)
		return err;

	buf.safn = SAFN;
	buf.sasb = SASB_NOTIFICATIONS;
	buf.gunm = GUNM_ACPI_NOTIFY_ENABLE;
	buf.guds[0] = GUDS_ACPI_NOTIFY_ENABLE;

	return galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"activate ACPI notifications", &buf);
}

static int galaxybook_acpi_init(struct samsung_galaxybook *galaxybook)
{
	return acpi_execute_simple_method(galaxybook->acpi->handle,
			ACPI_METHOD_ENABLE, ACPI_METHOD_ENABLE_ON);
}

static void galaxybook_acpi_exit(struct samsung_galaxybook *galaxybook)
{
	acpi_execute_simple_method(galaxybook->acpi->handle,
			ACPI_METHOD_ENABLE, ACPI_METHOD_ENABLE_OFF);
}

/*
 * Platform driver
 */

static int galaxybook_probe(struct platform_device *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	struct samsung_galaxybook *galaxybook;
	acpi_status status;
	int err;

	dmi_check_system(galaxybook_dmi_ids);

	pr_info("found matched device %s; loading driver\n", dev_name(&adev->dev));

	galaxybook = kzalloc(sizeof(struct samsung_galaxybook), GFP_KERNEL);
	if (!galaxybook)
		return -ENOMEM;
	/* set static pointer here so it can be used in various methods for hotkeys, hwmon, etc */
	galaxybook_ptr = galaxybook;

	galaxybook->platform = pdev;
	galaxybook->acpi = adev;

	dev_set_drvdata(&galaxybook->platform->dev, galaxybook);

	pr_debug_prefixed("initializing ACPI device\n");
	err = galaxybook_acpi_init(galaxybook);
	if (err) {
		pr_err("failed to initialize the ACPI device\n");
		goto err_free;
	}

	pr_debug_prefixed("initializing ACPI power management features\n");
	err = galaxybook_enable_acpi_feature(galaxybook, SASB_POWER_MANAGEMENT);
	if (err) {
		pr_warn("failed to initialize ACPI power management features; " \
				"many features of this driver will not be available\n");
		performance_mode = false;
		battery_threshold = false;
	}

	if (performance_mode) {
		pr_debug_prefixed("initializing performance mode and platform profile\n");
		err = galaxybook_profile_init(galaxybook);
		if (err) {
			pr_debug_prefixed("failed to initialize performance mode and platform profile\n");
			performance_mode = false;
		}
	} else {
		pr_debug_prefixed("performance_mode is disabled\n");
	}

	if (battery_threshold) {
		pr_debug_prefixed("initializing battery charge threshold control\n");
		err = galaxybook_battery_threshold_init(galaxybook);
		if (err) {
			pr_debug_prefixed("failed to initialize battery charge threshold control\n");
			battery_threshold = false;
		}
	} else {
		pr_debug_prefixed("battery_threshold is disabled\n");
	}

	pr_debug_prefixed("adding platform device attributes\n");
	err = galaxybook_device_attrs_init(galaxybook);
	if (err)
		pr_err("failed to add platform device attributes\n");

	if (kbd_backlight) {
		pr_debug_prefixed("initializing kbd_backlight\n");
		err = galaxybook_kbd_backlight_init(galaxybook);
		if (err) {
			pr_debug_prefixed("failed to initialize kbd_backlight\n");
			kbd_backlight = false;
		}
	} else {
		pr_debug_prefixed("kbd_backlight is disabled\n");
	}

	if (fan_speed) {
		pr_debug_prefixed("initializing fan speed\n");
		err = galaxybook_fan_speed_init(galaxybook);
		if (err) {
			pr_debug_prefixed("failed to initialize fan speed\n");
			fan_speed = false;
		} else {
#if IS_ENABLED(CONFIG_HWMON)
			pr_debug_prefixed("initializing hwmon device\n");
			err = galaxybook_hwmon_init(galaxybook);
			if (err)
				pr_warn("failed to initialize hwmon device\n");
#endif
		}
	} else {
		pr_debug_prefixed("fan_speed is disabled\n");
	}

	/* i8042_filter should be disabled if kbd_backlight and allow_recording are disabled */
	if (!kbd_backlight && !allow_recording)
		i8042_filter = false;

	if (i8042_filter) {
		pr_debug_prefixed("installing i8402 key filter to capture hotkey input\n");

		/* initialize hotkey work queues */
		if (kbd_backlight)
			INIT_WORK(&galaxybook->kbd_backlight_hotkey_work,
					galaxybook_kbd_backlight_hotkey_work);
		if (allow_recording)
			INIT_WORK(&galaxybook->allow_recording_hotkey_work,
					galaxybook_allow_recording_hotkey_work);

		err = i8042_install_filter(galaxybook_i8042_filter);
		if (err) {
			pr_err("failed to install i8402 key filter\n");
			cancel_work_sync(&galaxybook->kbd_backlight_hotkey_work);
			cancel_work_sync(&galaxybook->allow_recording_hotkey_work);
			i8042_filter = false;
		}
	} else {
		pr_debug_prefixed("i8042_filter is disabled\n");
	}

	pr_debug_prefixed("installing ACPI notify handler\n");
	status = acpi_install_notify_handler(galaxybook->acpi->handle, ACPI_ALL_NOTIFY,
			galaxybook_acpi_notify, galaxybook);
	if (ACPI_SUCCESS(status)) {
		pr_debug_prefixed("enabling ACPI notifications\n");
		err = galaxybook_enable_acpi_notify(galaxybook);
		if (err) {
			pr_warn("failed to enable ACPI notifications; some hotkeys will not be supported\n");
		} else {
			/* initialize ACPI hotkey work queues */
			INIT_WORK(&galaxybook->performance_mode_hotkey_work,
					galaxybook_performance_mode_hotkey_work);

			pr_debug_prefixed("initializing input device\n");
			err = galaxybook_input_init(galaxybook);
			if (err) {
				pr_err("failed to initialize input device\n");
				cancel_work_sync(&galaxybook->performance_mode_hotkey_work);
				galaxybook_input_exit(galaxybook);
			}
		}
	} else {
		pr_debug_prefixed("failed to install ACPI notify handler\n");
	}

	pr_info("driver successfully loaded\n");

	return 0;

err_free:
	kfree(galaxybook);
	return err;
}

static void galaxybook_remove(struct platform_device *pdev)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(&pdev->dev);

	pr_info("removing driver\n");

	galaxybook_device_attrs_exit(galaxybook);

	galaxybook_input_exit(galaxybook);
	cancel_work_sync(&galaxybook->performance_mode_hotkey_work);

	if (i8042_filter) {
		i8042_remove_filter(galaxybook_i8042_filter);
		cancel_work_sync(&galaxybook->kbd_backlight_hotkey_work);
		cancel_work_sync(&galaxybook->allow_recording_hotkey_work);
	}

	acpi_remove_notify_handler(galaxybook->acpi->handle, ACPI_ALL_NOTIFY,
			galaxybook_acpi_notify);

	if (fan_speed) {
		galaxybook_fan_speed_exit(galaxybook);
#if IS_ENABLED(CONFIG_HWMON)
		galaxybook_hwmon_exit(galaxybook);
#endif
	}

	if (kbd_backlight)
		galaxybook_kbd_backlight_exit(galaxybook);

	if (battery_threshold)
		galaxybook_battery_threshold_exit(galaxybook);

	if (performance_mode)
		galaxybook_profile_exit(galaxybook);

	galaxybook_acpi_exit(galaxybook);

	if (galaxybook_ptr)
		galaxybook_ptr = NULL;

	kfree(galaxybook);

	pr_info("driver successfully removed\n");
}

static struct platform_driver galaxybook_platform_driver = {
	.driver = {
		.name = SAMSUNG_GALAXYBOOK_CLASS,
		.acpi_match_table = galaxybook_device_ids,
	},
	.probe = galaxybook_probe,
	.remove_new = galaxybook_remove,
};

static int __init samsung_galaxybook_init(void)
{
	return platform_driver_register(&galaxybook_platform_driver);
}

static void __exit samsung_galaxybook_exit(void)
{
	platform_driver_unregister(&galaxybook_platform_driver);
}

module_init(samsung_galaxybook_init);
module_exit(samsung_galaxybook_exit);

MODULE_AUTHOR("Joshua Grisham, Giulio Girardi");
MODULE_DESCRIPTION(SAMSUNG_GALAXYBOOK_NAME);
MODULE_LICENSE("GPL");
