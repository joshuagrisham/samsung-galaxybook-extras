// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung Galaxybook Extras driver
 *
 * Copyright (c) 2024 Joshua Grisham <josh@joshuagrisham.com>
 * Copyright (c) 2024 Giulio Girardi <giulio@rapgenic.com>
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
#include <linux/version.h>

#define SAMSUNG_GALAXYBOOK_CLASS  "samsung-galaxybook"
#define SAMSUNG_GALAXYBOOK_NAME   "Samsung Galaxybook Extras"


/*
 * Module parameters
 */

static bool kbd_backlight = true;
static bool kbd_backlight_was_set;

static bool performance_mode = true;
static bool performance_mode_was_set;

static bool fan_speed = true;
static bool fan_speed_was_set;

static bool i8042_filter = true;
static bool i8042_filter_was_set;

static bool acpi_hotkeys = true;
static bool acpi_hotkeys_was_set;

static bool wmi_hotkeys = true;
static bool wmi_hotkeys_was_set;

static bool debug = false;

static void warn_param_override(const char *param_name)
{
	pr_warn("parameter '%s' has been overridden; if your device needs this in " \
			"order to function properly, please create a new issue at " \
			"https://github.com/joshuagrisham/samsung-galaxybook-extras/issues\n",
			param_name);
}
int galaxybook_param_set(const char *val, const struct kernel_param *kp) {
	if (strcmp(kp->name, "kbd_backlight") == 0)
		kbd_backlight_was_set = true;
	if (strcmp(kp->name, "performance_mode") == 0)
		performance_mode_was_set = true;
	if (strcmp(kp->name, "fan_speed") == 0)
		fan_speed_was_set = true;
	if (strcmp(kp->name, "i8042_filter") == 0)
		i8042_filter_was_set = true;
	if (strcmp(kp->name, "acpi_hotkeys") == 0)
		acpi_hotkeys_was_set = true;
	if (strcmp(kp->name, "wmi_hotkeys") == 0)
		wmi_hotkeys_was_set = true;
	warn_param_override(kp->name);
	return param_set_bool(val, kp);
}
static struct kernel_param_ops galaxybook_module_param_ops = {
	.set = galaxybook_param_set,
	.get = param_get_bool,
};

module_param_cb(kbd_backlight, &galaxybook_module_param_ops, &kbd_backlight, 0644);
MODULE_PARM_DESC(kbd_backlight, "Enable Keyboard Backlight control (default on)");
module_param_cb(performance_mode, &galaxybook_module_param_ops, &performance_mode, 0644);
MODULE_PARM_DESC(performance_mode, "Enable Performance Mode control (default on)");
module_param_cb(fan_speed, &galaxybook_module_param_ops, &fan_speed, 0644);
MODULE_PARM_DESC(fan_speed, "Enable fan speed (default on)");
module_param_cb(i8042_filter, &galaxybook_module_param_ops, &i8042_filter, 0644);
MODULE_PARM_DESC(i8042_filter, "Enable capturing keyboard hotkey events (default on)");
module_param_cb(acpi_hotkeys, &galaxybook_module_param_ops, &acpi_hotkeys, 0644);
MODULE_PARM_DESC(acpi_hotkeys, "Enable ACPI hotkey events (default on)");
module_param_cb(wmi_hotkeys, &galaxybook_module_param_ops, &wmi_hotkeys, 0644);
MODULE_PARM_DESC(wmi_hotkeys, "Enable WMI hotkey events (default on)");
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug messages (default off)");


/*
 * Device definitions, matching, and quirks
 */

#define PERFORMANCE_MODE_COUNT 4

typedef enum {
	PERFORMANCE_MODE_OPTIMIZED,
	PERFORMANCE_MODE_QUIET,
	PERFORMANCE_MODE_SILENT,
	PERFORMANCE_MODE_PERFORMANCE,
} galaxybook_performance_mode;

static const char * const performance_mode_names[] = {
	[PERFORMANCE_MODE_OPTIMIZED] = "optimized",
	[PERFORMANCE_MODE_QUIET] = "quiet",
	[PERFORMANCE_MODE_SILENT] = "silent",
	[PERFORMANCE_MODE_PERFORMANCE] = "performance",
};

static u8 performance_modes[] = {
	[PERFORMANCE_MODE_OPTIMIZED] = 0x0,
	[PERFORMANCE_MODE_QUIET] = 0xa,
	[PERFORMANCE_MODE_SILENT] = 0xb,
	[PERFORMANCE_MODE_PERFORMANCE] = 0x1,
};

struct galaxybook_device_quirks {
	bool disable_kbd_backlight;
	bool disable_performance_mode;
	bool disable_fan_speed;
	bool disable_i8042_filter;
	bool disable_acpi_hotkeys;
	bool disable_wmi_hotkeys;
	u8 *performance_modes;
};

static const struct galaxybook_device_quirks sam0427_quirks = {
	.disable_performance_mode = true,
	.disable_fan_speed = true,
	.disable_i8042_filter = true,
};

static const struct galaxybook_device_quirks sam0429_quirks = {
	.performance_modes = (u8[]){
		[PERFORMANCE_MODE_OPTIMIZED] = 0x2,
		[PERFORMANCE_MODE_QUIET] = 0xa,
		[PERFORMANCE_MODE_SILENT] = 0xb,
		[PERFORMANCE_MODE_PERFORMANCE] = 0x15,
	},
};

static const struct acpi_device_id galaxybook_device_ids[] = {
	{ "SAM0427", (unsigned long)&sam0427_quirks },
	{ "SAM0428" },
	{ "SAM0429", (unsigned long)&sam0429_quirks },
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

struct samsung_galaxybook {
	struct platform_device *platform;
	struct acpi_device *acpi;

	struct led_classdev kbd_backlight;
	struct work_struct kbd_backlight_hotkey_work;

	struct input_dev *input;
	struct key_entry *keymap;

	struct platform_profile_handler profile_handler;
	struct work_struct performance_mode_hotkey_work;

	struct work_struct allow_recording_hotkey_work;

	struct acpi_device fan;
	unsigned int *fan_speeds;
	int fan_speeds_count;

#if IS_ENABLED(CONFIG_HWMON)
	struct device *hwmon;
#endif
};
static struct samsung_galaxybook *galaxybook_ptr;

#define ACPI_METHOD_ENABLE           "SDLS"
#define ACPI_METHOD_SETTINGS         "CSFI"
#define ACPI_METHOD_PERFORMANCE_MODE "CSXI"

/* guid 8246028d-8bca-4a55-ba0f-6f1e6b921b8f */
static const guid_t performance_mode_guid_value =
	GUID_INIT(0x8246028d, 0x8bca, 0x4a55, 0xba, 0x0f, 0x6f, 0x1e, 0x6b, 0x92, 0x1b, 0x8f);
#define PERFORMANCE_MODE_GUID performance_mode_guid_value

#define DEFAULT_PLATFORM_PROFILE PLATFORM_PROFILE_BALANCED

#define SAFN 0x5843

#define SASB_KBD_BACKLIGHT    0x78
#define SASB_POWER_MANAGEMENT 0x7a
#define SASB_USB_CHARGING_GET 0x67
#define SASB_USB_CHARGING_SET 0x68
#define SASB_NOTIFICATIONS    0x86
#define SASB_ALLOW_RECORDING  0x8a

#define GUNM_SET 0x82
#define GUNM_GET 0x81

#define SAWB_LEN_SETTINGS 0x15
#define SAWB_LEN_PERFORMANCE_MODE 0x100

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
	};
};

#define ACPI_FAN_DEVICE_ID    "PNP0C0B"
#define ACPI_FAN_SPEED_LIST   "\\_SB.PC00.LPCB.FAN0.FANT"
#define ACPI_FAN_SPEED_VALUE  "\\_SB.PC00.LPCB.H_EC.FANS"

#define KBD_BACKLIGHT_MAX_BRIGHTNESS  3

#define ACPI_NOTIFY_BATTERY_STATE_CHANGED    0x61
#define ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE  0x70

static const struct key_entry galaxybook_acpi_keymap[] = {
	{KE_KEY, ACPI_NOTIFY_BATTERY_STATE_CHANGED, { KEY_BATTERY } },
	{KE_KEY, ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE, { KEY_PROG3 } },
	{KE_END, 0},
};


/*
 * ACPI method handling
 */

static void debug_print_acpi_object_buffer(const char *level, const char *header_str,
				const union acpi_object *obj)
{
	if (debug) {
		printk("%ssamsung_galaxybook: [DEBUG] %s\n", level, header_str);
		print_hex_dump(level, "samsung_galaxybook: [DEBUG]     ", DUMP_PREFIX_NONE, 16, 1,
				obj->buffer.pointer, obj->buffer.length, false);
	}
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

	debug_print_acpi_object_buffer(KERN_WARNING, purpose_str, &in_obj);

	status = acpi_evaluate_object(galaxybook->acpi->handle, method, &input, &output);

	if (ACPI_SUCCESS(status)) {
		out_obj = output.pointer;
		if (out_obj->type != ACPI_TYPE_BUFFER) {
			pr_err("failed %s with ACPI method %s; response was not a buffer\n",
					purpose_str,
					method);
			status = -EIO;
		} else {
			debug_print_acpi_object_buffer(KERN_WARNING, "response was:", out_obj);
		}
		if (out_obj->buffer.length != len) {
			pr_err("failed %s with ACPI method %s; response length mismatch\n",
					purpose_str,
					method);
			status = -EIO;
		} else if (out_obj->buffer.length < 6) {
			pr_err("failed %s with ACPI method %s; response from device was too short\n",
					purpose_str,
					method);
			status = -EIO;
		} else if (out_obj->buffer.pointer[5] == 0xff) {
			pr_err("failed %s with ACPI method %s; failure code 0xff reported from device\n",
					purpose_str,
					method);
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
	buf.gunm = 0xbb;
	buf.guds[0] = 0xaa;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"enabling ACPI feature", &buf);
	if (err)
		return err;

	if (buf.gunm != 0xdd && buf.guds[0] != 0xcc)
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

	pr_info("set kbd_backlight brightness to %d\n", brightness);

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

	if (debug)
		pr_warn("[DEBUG] current kbd_backlight brightness is %d\n", buf.gunm);

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
	int err;

	err = galaxybook_enable_acpi_feature(galaxybook, SASB_KBD_BACKLIGHT);
	if (err)
		return err;

	galaxybook->kbd_backlight = (struct led_classdev) {
		.brightness_get = kbd_backlight_show,
		.brightness_set_blocking = kbd_backlight_store,
		.flags = LED_BRIGHT_HW_CHANGED,
		.name = SAMSUNG_GALAXYBOOK_CLASS "::kbd_backlight",
		.max_brightness = KBD_BACKLIGHT_MAX_BRIGHTNESS,
	};

	pr_info("registering LED class %s\n",
			galaxybook->kbd_backlight.name);

	return led_classdev_register(&galaxybook->platform->dev, &galaxybook->kbd_backlight);
}

static void galaxybook_kbd_backlight_exit(struct samsung_galaxybook *galaxybook)
{
	led_classdev_unregister(&galaxybook->kbd_backlight);
}


/*
 * Platform Attributes (configuration properties which can be controlled via userspace)
 */


/* Battery saver percent (stop charging battery at given percentage value) */

/* 
 * Note that as of this writing, the Samsung Settings app in Windows expects a value of 0x50 (80%)
 * to mean "on" and a value of 0 to mean "off", but the device seems to support any number between
 * 0 and 100. Use only values 0 or 80 if the user wishes for full compatibility with Windows;
 * otherwise, the user is free to set whatever arbitrary percentage that they would like here.
 */
static int battery_saver_percent_acpi_set(struct samsung_galaxybook *galaxybook, const u8 value)
{
	struct sawb buf = {0};
	int err;

	if (value > 100)
		return -EINVAL;

	if (value == 100) {
		pr_warn("setting battery_saver_percent to 100 " \
				"will effectively just turn off battery saver; value will be set to 0 (off)\n");
	}

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_SET;
	buf.guds[0] = 0xe9;
	buf.guds[1] = 0x90;

	buf.guds[2] = (value == 100 ? 0 : value);

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting battery_saver_percent", &buf);
	if (err)
		return err;

	if (buf.guds[1] != 0x90 && buf.guds[2] != (value == 100 ? 0 : value)) {
		pr_err("invalid response when setting battery_saver_percent; " \
				"returned value was: 0x%02x 0x%02x\n",
				buf.guds[1], buf.guds[2]);
		return -EINVAL;
	}

	pr_info("set battery_saver_percent to %d\n", buf.guds[2]);

	return 0;
}

static int battery_saver_percent_acpi_get(struct samsung_galaxybook *galaxybook, u8 *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = 0x82;
	buf.guds[0] = 0xe9;
	buf.guds[1] = 0x91;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting battery_saver_percent", &buf);
	if (err)
		return err;

	*value = buf.guds[1];

	if (debug) {
		pr_warn("[DEBUG] battery saver is currently %s; battery_saver_percent is %d\n",
				(buf.guds[1] > 0 ? "on" : "off"), buf.guds[1]);
	}

	return 0;
}

static ssize_t battery_saver_percent_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	u8 value;
	int err;

	if (!count || kstrtou8(buffer, 0, &value))
		return -EINVAL;

	err = battery_saver_percent_acpi_set(galaxybook, value);
	if (err)
		return err;

	return count;
}

static ssize_t battery_saver_percent_show(struct device *dev, struct device_attribute *attr,
				char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	u8 value;
	int err;

	err = battery_saver_percent_acpi_get(galaxybook, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%d\n", value);
}

static DEVICE_ATTR_RW(battery_saver_percent);


/* Dolby Atmos mode for speakers - needs further investigation */
/*
static bool dolby_atmos;

static ssize_t dolby_atmos_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	if (!count || kstrtobool(buffer, &dolby_atmos))
		return -EINVAL;
	return count;
}

static ssize_t dolby_atmos_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	return sysfs_emit(buffer, "%u\n", dolby_atmos);
}

static DEVICE_ATTR_RW(dolby_atmos);
*/


/* Start on lid open (device should power on when lid is opened) */


static int start_on_lid_open_acpi_set(struct samsung_galaxybook *galaxybook, const bool value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = GUNM_SET;
	buf.guds[0] = 0xa3;
	buf.guds[1] = 0x80;
	buf.guds[2] = value;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting start_on_lid_open", &buf);
	if (err)
		return err;

	if (buf.guds[1] != 0x80 && buf.guds[2] != value) {
		pr_err("invalid response when setting start_on_lid_open; " \
				"returned value was: 0x%02x 0x%02x\n",
				buf.guds[1], buf.guds[2]);
		return -EINVAL;
	}

	pr_info("turned start_on_lid_open %s\n", value ? "on (1)" : "off (0)");

	return 0;
}

static int start_on_lid_open_acpi_get(struct samsung_galaxybook *galaxybook, bool *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_POWER_MANAGEMENT;
	buf.gunm = 0x82;
	buf.guds[0] = 0xa3;
	buf.guds[1] = 0x81;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting start_on_lid_open", &buf);
	if (err)
		return err;

	*value = buf.guds[1];

	if (debug) {
		pr_warn("[DEBUG] start_on_lid_open is currently %s\n",
				(buf.guds[1] ? "on (1)" : "off (0)"));
	}

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


/* USB Charging (USB ports can charge other devices even when device is powered off) */

static int usb_charging_acpi_set(struct samsung_galaxybook *galaxybook, const bool value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_USB_CHARGING_SET;

	/* gunm value should be 0x81 to turn on and 0x80 to turn off */
	buf.gunm = value ? 0x81 : 0x80;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"setting usb_charging", &buf);
	if (err)
		return err;

	if (buf.gunm != value) {
		pr_err("invalid response when setting usb_charging; returned value was: 0x%02x\n",
				buf.gunm);
		return -EINVAL;
	}

	pr_info("turned usb_charging %s\n", value ? "on (1)" : "off (0)");

	return 0;
}

static int usb_charging_acpi_get(struct samsung_galaxybook *galaxybook, bool *value)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = SASB_USB_CHARGING_GET;
	buf.gunm = 0x80;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"getting usb_charging", &buf);
	if (err)
		return err;

	*value = buf.gunm;

	if (debug) {
		pr_warn("[DEBUG] usb_charging is currently %s\n",
				(buf.gunm ? "on (1)" : "off (0)"));
	}

	return 0;
}

static ssize_t usb_charging_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	if (!count || kstrtobool(buffer, &value))
		return -EINVAL;

	err = usb_charging_acpi_set(galaxybook, value);
	if (err)
		return err;

	return count;
}

static ssize_t usb_charging_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	int err;

	err = usb_charging_acpi_get(galaxybook, &value);
	if (err)
		return err;

	return sysfs_emit(buffer, "%u\n", value);
}

static DEVICE_ATTR_RW(usb_charging);


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

	if (buf.gunm != value) {
		pr_err("invalid response when setting allow_recording; returned value was: 0x%02x\n",
				buf.gunm);
		return -EINVAL;
	}

	pr_info("turned allow_recording %s\n", value ? "on (1)" : "off (0)");

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

	if (debug) {
		pr_warn("[DEBUG] allow_recording is currently %s\n",
				(buf.gunm ? "on (1)" : "off (0)"));
	}

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


/* Add attributes to necessary groups etc */

static struct attribute *galaxybook_attrs[] = {
	&dev_attr_battery_saver_percent.attr,
	/* &dev_attr_dolby_atmos.attr, */ /* removed pending further investigation */
	&dev_attr_start_on_lid_open.attr,
	&dev_attr_usb_charging.attr,
	&dev_attr_allow_recording.attr,
	NULL
};
ATTRIBUTE_GROUPS(galaxybook);


/*
 * Fan speed
 */

static int fan_speed_get(struct samsung_galaxybook *galaxybook, unsigned int *speed)
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
			(int) response_obj->integer.value > galaxybook->fan_speeds_count) {
		pr_err("Invalid fan speed data\n");
		ret = -EINVAL;
		goto out_free;
	}

	speed_level = (int) response_obj->integer.value;
	*speed = galaxybook->fan_speeds[speed_level];

	if (debug)
		pr_warn("[DEBUG] reporting fan_speed of %d (level %d)\n", *speed, speed_level);

out_free:
	ACPI_FREE(response.pointer);
	return ret;
}

static int __init fan_speed_list_init(struct samsung_galaxybook *galaxybook)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response_obj = NULL;
	acpi_status status;

	status = acpi_evaluate_object(NULL, ACPI_FAN_SPEED_LIST, NULL, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to read fan speed list\n");
		return -ENODEV;
	}

	response_obj = response.pointer;
	if (!response_obj || response_obj->type != ACPI_TYPE_PACKAGE ||
			response_obj->package.count == 0) {
		pr_err("Invalid fan speed list data\n");
		status = -EINVAL;
		goto out_free;
	}

	/* fan_speeds[] starts with a hard-coded 0 (fan is off), then has some "funny" logic:
	 *  - fetch the speed level values read in from FANT and add 0x0a to each value
	 *  - _FST method in the DSDT seems to indicate that level 3 and 4 should have the same value,
	 *    however real-life observation suggests that the speed actually does change
	 *  - _FST says that level 5 should give the 4th value from FANT but it seems significantly
	 *    louder -- we will just "guess" it is 1000 RPM faster than the highest value from FANT?
	 */

	galaxybook->fan_speeds = kzalloc(sizeof(unsigned int) * (response_obj->package.count + 2),
			GFP_KERNEL);
	if (!galaxybook->fan_speeds)
		return -ENOMEM;

	/* hard-coded "off" value (0) */
	galaxybook->fan_speeds[0] = 0;
	galaxybook->fan_speeds_count = 1;

	/* fetch and assign the next values from FANT response */
	int i = 0;
	for (i = 1; i <= response_obj->package.count; i++) {
		if (response_obj->package.elements[i-1].type != ACPI_TYPE_INTEGER) {
			pr_err("Invalid fan speed list value at position %d (expected type %d, got type %d)\n",
					i-1, ACPI_TYPE_INTEGER, response_obj->package.elements[i-1].type);
			status = -EINVAL;
			goto err_fan_speeds_free;
		}
		galaxybook->fan_speeds[i] = response_obj->package.elements[i-1].integer.value + 0x0a;
		galaxybook->fan_speeds_count++;
	}

	/* add the missing last level speed where we "guess" it is 1000 RPM faster than the highest
	   level fetched from FANT */
	if (galaxybook->fan_speeds_count > 1) {
		galaxybook->fan_speeds[i] = galaxybook->fan_speeds[i-1] + 1000;
		galaxybook->fan_speeds_count++;
	}

	pr_info("initialized fan speed reporting with the following levels:\n");
	for (i = 0; i < galaxybook->fan_speeds_count; i++)
		pr_info("  fan speed level %d = %d\n", i, galaxybook->fan_speeds[i]);

out_free:
	ACPI_FREE(response.pointer);
	return status;

err_fan_speeds_free:
	kfree(galaxybook->fan_speeds);
	goto out_free;
}

static ssize_t fan_speed_rpm_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	unsigned int speed;
	int ret = 0;

	ret = fan_speed_get(galaxybook_ptr, &speed);
	if (ret)
		return ret;

	return sysfs_emit(buffer, "%u\n", speed);
}
static DEVICE_ATTR_RO(fan_speed_rpm);

void galaxybook_fan_speed_exit(struct samsung_galaxybook *galaxybook)
{
	sysfs_remove_file(&galaxybook->fan.dev.kobj, &dev_attr_fan_speed_rpm.attr);
}

static int __init galaxybook_fan_speed_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	galaxybook->fan = *acpi_dev_get_first_match_dev(ACPI_FAN_DEVICE_ID, NULL, -1);

	err = sysfs_create_file(&galaxybook->fan.dev.kobj, &dev_attr_fan_speed_rpm.attr);
	if (err)
		pr_err("Unable create fan_speed_rpm attribute\n");

	err = fan_speed_list_init(galaxybook);
	if (err)
		pr_err("Unable to get list of fan speeds\n");

	return 0;
}


/*
 * Hwmon device
 */

#if IS_ENABLED(CONFIG_HWMON)
static umode_t galaxybook_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	/*
	 * There is only a single fan so simple logic can be used to match it and discard anything else.
	 */
	if (type == hwmon_fan && channel == 0 && attr == hwmon_fan_input)
		return 0444;
	else
		return 0;
}

static int galaxybook_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				   u32 attr, int channel, long *val)
{
	/*
	 * There is only a single fan so simple logic can be used to match it and discard anything else.
	 */
	if (type == hwmon_fan && channel == 0 && attr == hwmon_fan_input) {
		unsigned int speed;
		int ret = 0;

		ret = fan_speed_get(galaxybook_ptr, &speed);
		if (ret)
			return ret;

		*val = speed;
		return ret;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info * const galaxybook_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops galaxybook_hwmon_ops = {
	.is_visible = galaxybook_hwmon_is_visible,
	.read = galaxybook_hwmon_read,
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
#endif


/*
 * Platform Profile / Performance mode
 */

static galaxybook_performance_mode profile_to_performance_mode(
				const enum platform_profile_option profile)
{
	/* Map all the profiles even though we will mark only some of them as supported */
	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		return PERFORMANCE_MODE_SILENT;
	case PLATFORM_PROFILE_COOL:
	case PLATFORM_PROFILE_QUIET:
		return PERFORMANCE_MODE_QUIET;
	case PLATFORM_PROFILE_PERFORMANCE:
		return PERFORMANCE_MODE_PERFORMANCE;
	case PLATFORM_PROFILE_BALANCED:
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
	default:
		return PERFORMANCE_MODE_OPTIMIZED;
	}
}

static enum platform_profile_option performance_mode_to_profile(
				const galaxybook_performance_mode mode)
{
	switch (mode) {
	case PERFORMANCE_MODE_SILENT:
		return PLATFORM_PROFILE_LOW_POWER;
	case PERFORMANCE_MODE_QUIET:
		return PLATFORM_PROFILE_QUIET;
	case PERFORMANCE_MODE_PERFORMANCE:
		return PLATFORM_PROFILE_PERFORMANCE;
	default:
		return PLATFORM_PROFILE_BALANCED;
	}
}

static galaxybook_performance_mode performance_mode_lookup_by_value(
				struct samsung_galaxybook *galaxybook, const u8 value)
{
	for (int i = 0; i < PERFORMANCE_MODE_COUNT; i++)
		if (performance_modes[i] == value)
			return i;
	return -1;
}

static int performance_mode_acpi_set(struct samsung_galaxybook *galaxybook,
				const galaxybook_performance_mode performance_mode)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = 0x91;
	export_guid(buf.caid, &PERFORMANCE_MODE_GUID);
	buf.fncn = 0x51;
	buf.subn = 0x03;
	buf.iob0 = performance_modes[performance_mode];

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &buf,
			SAWB_LEN_PERFORMANCE_MODE, "setting performance_mode", &buf);
	if (err)
		return err;

	pr_info("set performance_mode to '%s' (0x%02x)\n", performance_mode_names[performance_mode],
			performance_modes[performance_mode]);

	return 0;
}

static int performance_mode_acpi_get(struct samsung_galaxybook *galaxybook,
				galaxybook_performance_mode *performance_mode)
{
	struct sawb buf = {0};
	int err;

	buf.safn = SAFN;
	buf.sasb = 0x91;
	export_guid(buf.caid, &PERFORMANCE_MODE_GUID);
	buf.fncn = 0x51;
	buf.subn = 0x02;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &buf,
			SAWB_LEN_PERFORMANCE_MODE, "getting performance_mode", &buf);
	if (err)
		return err;

	*performance_mode = performance_mode_lookup_by_value(galaxybook, buf.iob0);
	if (*performance_mode == -1)
		return -EINVAL;

	if (debug)
		pr_warn("[DEBUG] performance_mode is currently '%s' (0x%02x)\n",
			performance_mode_names[*performance_mode], buf.iob0);

	return 0;
}

static int galaxybook_platform_profile_set(struct platform_profile_handler *pprof,
				enum platform_profile_option profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	galaxybook_performance_mode performance_mode = profile_to_performance_mode(profile);

	return performance_mode_acpi_set(galaxybook, performance_mode);
}

static int galaxybook_platform_profile_get(struct platform_profile_handler *pprof,
				enum platform_profile_option *profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	galaxybook_performance_mode performance_mode;
	int err;

	err = performance_mode_acpi_get(galaxybook, &performance_mode);
	if (err)
		return err;

	*profile = performance_mode_to_profile(performance_mode);

	return 0;
}

static int galaxybook_profile_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	galaxybook->profile_handler.profile_get = galaxybook_platform_profile_get;
	galaxybook->profile_handler.profile_set = galaxybook_platform_profile_set;

	set_bit(PLATFORM_PROFILE_LOW_POWER, galaxybook->profile_handler.choices);
	set_bit(PLATFORM_PROFILE_QUIET, galaxybook->profile_handler.choices);
	set_bit(PLATFORM_PROFILE_BALANCED, galaxybook->profile_handler.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, galaxybook->profile_handler.choices);

	err = platform_profile_register(&galaxybook->profile_handler);
	if (err)
		return err;

	return 0;
}

static void galaxybook_profile_exit(struct samsung_galaxybook *galaxybook)
{
	platform_profile_remove();
}

static int galaxybook_performance_mode_init(struct samsung_galaxybook *galaxybook)
{
	struct sawb perf_modes_buf = {0};
	struct sawb perf_mode_values_buf = {0};
	int err;

	perf_modes_buf.safn = SAFN;
	perf_modes_buf.sasb = 0x91;
	export_guid(perf_modes_buf.caid, &PERFORMANCE_MODE_GUID);
	perf_modes_buf.fncn = 0x51;
	perf_modes_buf.subn = 0x00;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &perf_modes_buf,
			SAWB_LEN_PERFORMANCE_MODE, "get supported performance modes", &perf_modes_buf);
	if (err)
		return err;

	perf_mode_values_buf.safn = SAFN;
	perf_mode_values_buf.sasb = 0x91;
	export_guid(perf_mode_values_buf.caid, &PERFORMANCE_MODE_GUID);
	perf_mode_values_buf.fncn = 0x51;
	perf_mode_values_buf.subn = 0x01;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_PERFORMANCE_MODE, &perf_mode_values_buf,
			SAWB_LEN_PERFORMANCE_MODE, "get performance mode values", &perf_mode_values_buf);
	if (err)
		return err;

	/* Ideally, we could use the above to dynamically read performance_modes from the device.
	 * For now, though, we will just set hard-coded value tables in quirks and then print the values
	 * here. */

	for (int i = 0; i < PERFORMANCE_MODE_COUNT; i++)
		pr_info("will support performance_mode '%s' with value 0x%x\n", performance_mode_names[i],
				performance_modes[i]);

	/* now check currently set performance_mode; if it is not supported then set default profile */
	galaxybook_performance_mode current_performance_mode;
	err = performance_mode_acpi_get(galaxybook, &current_performance_mode);
	if (err) {
		pr_info("initial performance_mode value is not supported by device; setting to default\n");
		err = galaxybook_platform_profile_set(&galaxybook->profile_handler,
				DEFAULT_PLATFORM_PROFILE);
		if (err)
			return err;
	}

	return 0;
}

/*
 * Platform device
 */

/* resolve quirks vs module parameters to final values before doing anything in the module */
static int galaxybook_platform_probe(struct platform_device *pdev)
{
	const struct galaxybook_device_quirks *quirks;
	quirks = device_get_match_data(&pdev->dev);
	if (quirks) {
		if (debug) {
			pr_warn("[DEBUG] received following device quirks:\n");
			pr_warn("[DEBUG]   disable_kbd_backlight       = %s\n",
					quirks->disable_kbd_backlight ? "true" : "false");
			pr_warn("[DEBUG]   disable_performance_mode    = %s\n",
					quirks->disable_performance_mode ? "true" : "false");
			pr_warn("[DEBUG]   disable_fan_speed           = %s\n",
					quirks->disable_fan_speed ? "true" : "false");
			pr_warn("[DEBUG]   disable_i8042_filter        = %s\n",
					quirks->disable_i8042_filter ? "true" : "false");
			pr_warn("[DEBUG]   disable_acpi_hotkeys        = %s\n",
					quirks->disable_acpi_hotkeys ? "true" : "false");
			pr_warn("[DEBUG]   disable_wmi_hotkeys         = %s\n",
					quirks->disable_wmi_hotkeys ? "true" : "false");
		}
		if (quirks->disable_kbd_backlight && !kbd_backlight_was_set)
			kbd_backlight = false;
		if (quirks->disable_performance_mode && !performance_mode_was_set)
			performance_mode = false;
		if (quirks->disable_fan_speed && !fan_speed_was_set)
			fan_speed = false;
		if (quirks->disable_i8042_filter && !i8042_filter_was_set)
			i8042_filter = false;
		if (quirks->disable_acpi_hotkeys && !acpi_hotkeys_was_set)
			acpi_hotkeys = false;
		if (quirks->disable_wmi_hotkeys && !wmi_hotkeys_was_set)
			wmi_hotkeys = false;

		/* set performance_modes values if given in quirks */
		if (quirks->performance_modes) {
			if (debug)
				pr_warn("[DEBUG] performance_modes override exist in quirks\n");
			for (int i = 0; i < PERFORMANCE_MODE_COUNT; i++) {
				performance_modes[i] = quirks->performance_modes[i];
			}
		}
	}
	return 0;
}

static struct platform_driver galaxybook_platform_driver = {
	.driver = {
		.name = SAMSUNG_GALAXYBOOK_CLASS,
		.acpi_match_table = galaxybook_device_ids,
		.dev_groups = galaxybook_groups,
	},
	.probe = galaxybook_platform_probe
};

static int galaxybook_platform_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	galaxybook->platform = platform_device_alloc(SAMSUNG_GALAXYBOOK_CLASS, PLATFORM_DEVID_NONE);
	if (!galaxybook->platform)
		return -ENOMEM;

	platform_set_drvdata(galaxybook->platform, galaxybook);

	err = platform_device_add(galaxybook->platform);
	if (err)
		goto err_device_put;

	return 0;

err_device_put:
	platform_device_put(galaxybook->platform);
	return err;
}

static void galaxybook_platform_exit(struct samsung_galaxybook *galaxybook)
{
	platform_device_unregister(galaxybook->platform);
}


/*
 * Hotkey work and filters
 */

static const enum platform_profile_option performance_mode_hotkey_next_profile[] = {
	[PLATFORM_PROFILE_LOW_POWER] = PLATFORM_PROFILE_QUIET,
	[PLATFORM_PROFILE_QUIET] = PLATFORM_PROFILE_BALANCED,
	[PLATFORM_PROFILE_BALANCED] = PLATFORM_PROFILE_PERFORMANCE,
	[PLATFORM_PROFILE_PERFORMANCE] = PLATFORM_PROFILE_LOW_POWER,
};
static void galaxybook_performance_mode_hotkey_work(struct work_struct *work)
{
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, performance_mode_hotkey_work);
	enum platform_profile_option current_profile;

	galaxybook_platform_profile_get(&galaxybook->profile_handler, &current_profile);
	galaxybook_platform_profile_set(&galaxybook->profile_handler,
			performance_mode_hotkey_next_profile[current_profile]);
	platform_profile_notify();

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

	return;
}

static void galaxybook_allow_recording_hotkey_work(struct work_struct *work)
{
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, allow_recording_hotkey_work);
	bool value;

	allow_recording_acpi_get(galaxybook, &value);
	allow_recording_acpi_set(galaxybook, !value);

	return;
}

static bool galaxybook_i8042_filter(unsigned char data, unsigned char str,
				    				struct serio *port)
{
	static bool extended;

	if (data == 0xe0) {
		extended = true;
	} else if (likely(extended)) {
		extended = false;

		/* kbd_backlight keydown */
		if (data == 0x2c) {
			if (debug) {
				pr_warn("[DEBUG] hotkey: kbd_backlight keydown\n");
			}
		}
		/* kbd_backlight keyup */
		if (data == 0xac) {
			if (debug) {
				pr_warn("[DEBUG] hotkey: kbd_backlight keyup\n");
			}
			schedule_work(&galaxybook_ptr->kbd_backlight_hotkey_work);
		}

		/* allow_recording keydown */
		if (data == 0x1f) {
			if (debug) {
				pr_warn("[DEBUG] hotkey: allow_recording keydown\n");
			}
		}
		/* allow_recording keyup */
		if (data == 0x9f) {
			if (debug) {
				pr_warn("[DEBUG] hotkey: allow_recording keyup\n");
			}
			schedule_work(&galaxybook_ptr->allow_recording_hotkey_work);
		}
	}

	return false;
}


/*
 * Input device (hotkeys)
 */

static void galaxybook_input_notify(struct samsung_galaxybook *galaxybook, int event)
{
	if (!galaxybook->input)
		return;
	if (debug) {
		pr_warn("[DEBUG] input notification event: 0x%x\n", event);
	}
	if (!sparse_keymap_report_event(galaxybook->input, event, 1, true)) {
		pr_warn("unknown input notification event: 0x%x\n", event);
		pr_warn("Please create an issue with this information at " \
				"https://github.com/joshuagrisham/samsung-galaxybook-extras/issues\n");
	}
}

static int galaxybook_input_init(struct samsung_galaxybook *galaxybook)
{
	struct input_dev *input;
	int error;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = "Samsung Galaxybook extra buttons";
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
		pr_warn("Unable to register input device\n");
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
 * WMI notifications
 *
 * Have never seen this ever actually get any notifications. For now, just add a notify handler
 * for this unrecognized WMI GUID and see if we can ever get something useful with this.
 */

#define GALAXYBOOK_WMI_EVENT_GUID "A6FEA33E-DABF-46F5-BFC8-460D961BEC9F"

static void galaxybook_wmi_notify(u32 value, void *context)
{
	pr_warn("WMI Event received: %u\n", value);
	pr_warn("Please create an issue with this information at " \
			"https://github.com/joshuagrisham/samsung-galaxybook-extras/issues\n");

	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response_obj = NULL;
	acpi_status status;

	status = wmi_get_event_data(value, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("Bad event status 0x%x\n", status);
		return;
	}

	response_obj = (union acpi_object *)response.pointer;
	if (!response_obj)
		return;

	pr_warn("WMI Event Data object type: %i\n", response_obj->type);
	debug_print_acpi_object_buffer(KERN_WARNING, "WMI Event Data response:", response_obj);

	kfree(response.pointer);
}
static int __init galaxybook_wmi_init(void)
{
	if (!wmi_has_guid(GALAXYBOOK_WMI_EVENT_GUID))
		return -ENODEV;

	pr_info("installing WMI notify handler\n");
	return wmi_install_notify_handler(GALAXYBOOK_WMI_EVENT_GUID, galaxybook_wmi_notify, NULL);
}
static int galaxybook_wmi_exit(void)
{
	if (!wmi_has_guid(GALAXYBOOK_WMI_EVENT_GUID))
		return -ENODEV;

	wmi_remove_notify_handler(GALAXYBOOK_WMI_EVENT_GUID);
	return 0;
}


/*
 * ACPI device
 */

static void galaxybook_acpi_notify(struct acpi_device *device, u32 event)
{
	struct samsung_galaxybook *galaxybook = acpi_driver_data(device);

	if (!acpi_hotkeys)
		return;

	if (event == ACPI_NOTIFY_HOTKEY_PERFORMANCE_MODE)
		schedule_work(&galaxybook_ptr->performance_mode_hotkey_work);

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
	buf.gunm = 0x80;
	buf.guds[0] = 0x02;

	err = galaxybook_acpi_method(galaxybook, ACPI_METHOD_SETTINGS, &buf, SAWB_LEN_SETTINGS,
			"activate ACPI notifications", &buf);
	if (err)
		return err;

	return 0;
}

static int galaxybook_acpi_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	err = acpi_execute_simple_method(galaxybook->acpi->handle, ACPI_METHOD_ENABLE, 1);
	if (err)
		return err;

	err = galaxybook_enable_acpi_feature(galaxybook, SASB_POWER_MANAGEMENT);
	if (err)
		return err;

	err = galaxybook_enable_acpi_feature(galaxybook, SASB_ALLOW_RECORDING);
	if (err)
		return err;

	return 0;
}

static void galaxybook_acpi_exit(struct samsung_galaxybook *galaxybook)
{
	acpi_execute_simple_method(galaxybook->acpi->handle, ACPI_METHOD_ENABLE, 0);
	return;
}

static int galaxybook_acpi_add(struct acpi_device *device)
{
	struct samsung_galaxybook *galaxybook;
	int err;

	dmi_check_system(galaxybook_dmi_ids);

	galaxybook = kzalloc(sizeof(struct samsung_galaxybook), GFP_KERNEL);
	if (!galaxybook)
		return -ENOMEM;

	strcpy(acpi_device_name(device), "Galaxybook Extras Controller");
	strcpy(acpi_device_class(device), SAMSUNG_GALAXYBOOK_CLASS);
	device->driver_data = galaxybook;
	galaxybook->acpi = device;

	pr_info("initializing ACPI device\n");
	err = galaxybook_acpi_init(galaxybook);
	if (err)
		goto err_free;

	pr_info("initializing platform device\n");
	err = galaxybook_platform_init(galaxybook);
	if (err)
		goto err_acpi_exit;

	if (performance_mode) {
		pr_info("initializing performance_mode\n");
		err = galaxybook_performance_mode_init(galaxybook);
		if (err)
			goto err_platform_exit;

		pr_info("initializing platform profile\n");
		err = galaxybook_profile_init(galaxybook);
		if (err)
			goto err_platform_exit;
	} else {
		pr_warn("performance_mode is disabled\n");
	}

	if (kbd_backlight) {
		pr_info("initializing kbd_backlight\n");
		err = galaxybook_kbd_backlight_init(galaxybook);
		if (err)
			goto err_profile_exit;
	} else {
		pr_warn("kbd_backlight is disabled\n");
	}

	if (i8042_filter) {
		/* initialize hotkey work queues */
		INIT_WORK(&galaxybook->kbd_backlight_hotkey_work,
				galaxybook_kbd_backlight_hotkey_work);
		INIT_WORK(&galaxybook->allow_recording_hotkey_work,
				galaxybook_allow_recording_hotkey_work);

		pr_info("installing i8402 key filter to capture hotkey input\n");
		err = i8042_install_filter(galaxybook_i8042_filter);
		if (err)
			pr_err("Unable to install key filter\n");
	} else {
		pr_warn("i8042_filter is disabled\n");
	}

	if (fan_speed) {
		pr_info("initializing fan speed\n");
		err = galaxybook_fan_speed_init(galaxybook);
		if (err)
			pr_err("Unable to initialize fan speed\n");

#if IS_ENABLED(CONFIG_HWMON)
		pr_info("initializing hwmon device\n");
		err = galaxybook_hwmon_init(galaxybook);
		if (err)
			pr_err("Unable to initialize hwmon device\n");
#endif
	} else {
		pr_warn("fan_speed is disabled\n");
	}

	if (acpi_hotkeys) {
		pr_info("enabling ACPI notifications\n");
		err = galaxybook_enable_acpi_notify(galaxybook);
		if (err)
			pr_err("Unable to enable ACPI notifications\n");

		pr_info("initializing hotkey input device\n");
		err = galaxybook_input_init(galaxybook);
		if (err) {
			pr_err("Unable to initialize hotkey input device\n");
			galaxybook_input_exit(galaxybook);
		}

		/* initialize hotkey work queues */
		INIT_WORK(&galaxybook->performance_mode_hotkey_work,
				galaxybook_performance_mode_hotkey_work);
	} else {
		pr_warn("acpi_hotkeys is disabled\n");
	}

	if (wmi_hotkeys) {
		pr_info("enabling WMI notifications\n");
		err = galaxybook_wmi_init();
		if (err)
			pr_err("Unable to enable WMI notifications\n");
	} else {
		pr_warn("wmi_hotkeys is disabled\n");
	}

	/* set galaxybook_ptr reference so it can be used by hotkeys */
	galaxybook_ptr = galaxybook;

	return 0;

err_profile_exit:
	galaxybook_profile_exit(galaxybook);
err_platform_exit:
	galaxybook_platform_exit(galaxybook);
err_acpi_exit:
	galaxybook_acpi_exit(galaxybook);
err_free:
	kfree(galaxybook);
	return err;
}

static void galaxybook_acpi_remove(struct acpi_device *device)
{
	struct samsung_galaxybook *galaxybook = acpi_driver_data(device);

	if (wmi_hotkeys)
		galaxybook_wmi_exit();

	if (acpi_hotkeys) {
		galaxybook_input_exit(galaxybook);
		cancel_work_sync(&galaxybook->performance_mode_hotkey_work);
	}

	if (i8042_filter) {
		i8042_remove_filter(galaxybook_i8042_filter);
		cancel_work_sync(&galaxybook->kbd_backlight_hotkey_work);
		cancel_work_sync(&galaxybook->allow_recording_hotkey_work);
	}

	if (kbd_backlight)
		galaxybook_kbd_backlight_exit(galaxybook);

	if (fan_speed)
		galaxybook_fan_speed_exit(galaxybook);

	if (performance_mode)
		galaxybook_profile_exit(galaxybook);

	galaxybook_platform_exit(galaxybook);

	galaxybook_acpi_exit(galaxybook);

	if (galaxybook_ptr)
		galaxybook_ptr = NULL;

	kfree(galaxybook);
}

static struct acpi_driver galaxybook_acpi_driver = {
	.name = SAMSUNG_GALAXYBOOK_NAME,
	.class = SAMSUNG_GALAXYBOOK_CLASS,
	.ids = galaxybook_device_ids,
	.flags = ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops = {
		.add = galaxybook_acpi_add,
		.remove = galaxybook_acpi_remove,
		.notify = galaxybook_acpi_notify,
		},

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
	.owner = THIS_MODULE,
#endif
};

static int __init samsung_galaxybook_init(void)
{
	int ret;

	pr_info("loading driver\n");

	ret = platform_driver_register(&galaxybook_platform_driver);
	if (ret < 0)
		goto err_unregister_platform;

	ret = acpi_bus_register_driver(&galaxybook_acpi_driver);
	if (ret < 0)
		goto err_unregister_acpi;

	pr_info("driver successfully loaded\n");

	return 0;

err_unregister_acpi:
	acpi_bus_unregister_driver(&galaxybook_acpi_driver);
err_unregister_platform:
	platform_driver_unregister(&galaxybook_platform_driver);

	return ret;
}

static void __exit samsung_galaxybook_exit(void)
{
	pr_info("removing driver\n");
	acpi_bus_unregister_driver(&galaxybook_acpi_driver);
	platform_driver_unregister(&galaxybook_platform_driver);
	pr_info("driver successfully removed\n");
}

module_init(samsung_galaxybook_init);
module_exit(samsung_galaxybook_exit);

MODULE_AUTHOR("Joshua Grisham");
MODULE_DESCRIPTION(SAMSUNG_GALAXYBOOK_NAME);
MODULE_LICENSE("GPL");
