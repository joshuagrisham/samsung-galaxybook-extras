// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung Galaxybook Extras driver
 *
 * Copyright (c) 2023 Joshua Grisham <josh@joshuagrisham.com>
 *
 * Implementation inspired by existing x86 platform drivers.
 * Thank you to the authors!
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/i8042.h>
#include <linux/workqueue.h>

#define SAMSUNG_GALAXYBOOK_CLASS "samsung-galaxybook"
#define SAMSUNG_GALAXYBOOK_NAME "Samsung Galaxybook Extras"

#define GALAXYBOOK_ACPI_METHOD_SETTINGS "CSFI"
#define GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE "CSXI"

#define GALAXYBOOK_ACPI_FAN_DEVICE_ID   "PNP0C0B"
#define GALAXYBOOK_ACPI_FAN_SPEED_LIST  "\\_SB.PC00.LPCB.FAN0.FANT"
#define GALAXYBOOK_ACPI_FAN_SPEED_VALUE "\\_SB.PC00.LPCB.H_EC.FANS"

#define GALAXYBOOK_KBD_BACKLIGHT_MAX_BRIGHTNESS 3

#define GALAXYBOOK_STARTUP_PROFILE PLATFORM_PROFILE_BALANCED

static const struct acpi_device_id galaxybook_device_ids[] = {
	{ "SAM0429", 0 },
	{ "", 0 },
};
MODULE_DEVICE_TABLE(acpi, galaxybook_device_ids);

static const struct dmi_system_id galaxybook_dmi_ids[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
					"SAMSUNG ELECTRONICS CO., LTD."),
			/* TODO: restrict to only specific product names if it is not working for all?  */
			/* DMI_MATCH(DMI_PRODUCT_NAME,	"950XED"), */
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

	struct platform_profile_handler profile_handler;
	enum platform_profile_option current_profile;

	struct acpi_device fan;
	unsigned int *fan_speeds;
	int fan_speeds_count;

#if IS_ENABLED(CONFIG_HWMON)
	struct device *hwmon;
#endif
};
static struct samsung_galaxybook *galaxybook_ptr;


/*
 * Keyboard Backlight
 */

static enum led_brightness galaxybook_kbd_backlight_get(struct led_classdev *kbd_backlight)
{
	return kbd_backlight->brightness;
}

static int galaxybook_kbd_backlight_set(struct led_classdev *led,
		enum led_brightness brightness)
{
	struct samsung_galaxybook *galaxybook = container_of(led,
			struct samsung_galaxybook, kbd_backlight);

	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	u8 set_payload[21] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x78;

	set_payload[5] = 0x82;
	set_payload[6] = brightness;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set kbd_backlight brightness with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// "set" should reply with:
	// {0x43, 0x58, 0x78, 0x00, 0xaa, 0x00, 0x80 * brightness, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// TODO: Should we read the response buffer and check this before continuing?

	galaxybook->kbd_backlight.brightness = brightness;
	pr_info("set kbd_backlight brightness to %d\n", brightness);

	return 0;
}

static int galaxybook_kbd_backlight_init(struct samsung_galaxybook *galaxybook)
{
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	u8 init_payload[21] = { 0 };

	init_payload[0] = 0x43;
	init_payload[1] = 0x58;
	init_payload[2] = 0x78;

	init_payload[5] = 0xbb;
	init_payload[6] = 0xaa;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(init_payload);
	in_obj.buffer.pointer	= init_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init kbd_backlight with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// "init" should reply with:
	// {0x43, 0x58, 0x78, 0x00, 0xaa, 0xdd, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// TODO: Should we read the response buffer and check this before continuing?

	pr_info("kbd_backlight successfully initialized via ACPI method %s\n",
			GALAXYBOOK_ACPI_METHOD_SETTINGS);

	galaxybook->kbd_backlight = (struct led_classdev) {
		.brightness_get = galaxybook_kbd_backlight_get,
		.brightness_set_blocking = galaxybook_kbd_backlight_set,
		.flags = LED_BRIGHT_HW_CHANGED,
		.name = SAMSUNG_GALAXYBOOK_CLASS "::kbd_backlight",
		.max_brightness = GALAXYBOOK_KBD_BACKLIGHT_MAX_BRIGHTNESS,
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


/* Battery saver mode (should battery stop charging at 85%) */

static ssize_t battery_saver_store(struct device *dev, struct device_attribute *attr,
								   const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	if (kstrtobool(buffer, &value) < 0)
		return -EINVAL;

	u8 set_payload[21] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x7a;

	set_payload[5] = 0x82;
	set_payload[6] = 0xe9;
	set_payload[7] = 0x90;

	// payload value should be 0x55 to turn on and 0x00 to turn off
	set_payload[8] = value ? 0x55 : 0x00;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43,0x58,0x7a,0x00,0xaa,0x00,0x00,0x90,(value ? 0x55 : 0x00),0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
	// TODO: Should we read response buffer and check this before continuing? or at least that pos 5 is not 0xff?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set battery_saver with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("turned battery_saver %s\n", value ? "on (1)" : "off (0)");
	return count;
}

static ssize_t battery_saver_show(struct device *dev, struct device_attribute *attr,
								  char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;
	struct acpi_buffer response_buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response;
	u8 response_value;
	int ret = -1;

	u8 read_payload[21] = { 0 };

	read_payload[0] = 0x43;
	read_payload[1] = 0x58;
	read_payload[2] = 0x7a;

	read_payload[5] = 0x82;
	read_payload[6] = 0xe9;
	read_payload[7] = 0x91;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(read_payload);
	in_obj.buffer.pointer	= read_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get battery_saver with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get battery_saver with ACPI method %s, response type was invalid\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 8) {
		pr_err("failed to get battery_saver with ACPI method %s, response from device was too short\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[7];
	if (response->buffer.pointer[5] == 0xff) {
		pr_err("failed to get battery_saver with ACPI method %s, failure code 0xff was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}
	if (response_value == 0x00) {
		ret = sysfs_emit(buffer, "%u\n", 0);
		goto out_free;
	}
	else if (response_value == 0x55) {
		ret = sysfs_emit(buffer, "%u\n", 1);
		goto out_free;
	}
	else {
		pr_err("failed to get battery_saver with ACPI method %s, unexpected value 0x%02x was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				response_value);
		ret = -ERANGE;
		goto out_free;
	}

out_free:
	ACPI_FREE(response_buffer.pointer);
	return ret;
}

static DEVICE_ATTR_RW(battery_saver);


/* Dolby Atmos mode for speakers */
/*
static bool dolby_atmos;

static ssize_t dolby_atmos_store(struct device *dev, struct device_attribute *attr,
								 const char *buffer, size_t count)
{
	if (kstrtobool(buffer, &dolby_atmos) < 0)
		return -EINVAL;
	return count;
}

static ssize_t dolby_atmos_show(struct device *dev, struct device_attribute *attr,
								char *buffer)
{
	return sysfs_emit(buffer, "%u\n", dolby_atmos);
}

static DEVICE_ATTR_RW(dolby_atmos);
*/

/* Start on lid open (device should power on when lid is opened) */

static ssize_t start_on_lid_open_store(struct device *dev, struct device_attribute *attr,
									   const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	if (kstrtobool(buffer, &value) < 0)
		return -EINVAL;

	u8 set_payload[21] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x7a;

	set_payload[5] = 0x82;
	set_payload[6] = 0xa3;
	set_payload[7] = 0x80;

	set_payload[8] = value;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43,0x58,0x7a,0x00,0xaa,0x00,0x00,0x80,value,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
	// TODO: Should we read response buffer and check this before continuing?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set start_on_lid_open with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("turned start_on_lid_open %s\n", value ? "on (1)" : "off (0)");
	return count;
}

static ssize_t start_on_lid_open_show(struct device *dev, struct device_attribute *attr,
									  char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;
	struct acpi_buffer response_buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response;
	u8 response_value;
	int ret = -1;

	u8 read_payload[21] = { 0 };

	read_payload[0] = 0x43;
	read_payload[1] = 0x58;
	read_payload[2] = 0x7a;

	read_payload[5] = 0x82;
	read_payload[6] = 0xa3;
	read_payload[7] = 0x81;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(read_payload);
	in_obj.buffer.pointer	= read_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, response type was invalid\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 6) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, response from device was too short\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[7];
	if (response->buffer.pointer[5] == 0xff) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, failure code 0xff was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}
	else if (response_value == 0x00) {
		ret = sysfs_emit(buffer, "%u\n", 0);
		goto out_free;
	}
	else if (response_value == 0x01) {
		ret = sysfs_emit(buffer, "%u\n", 1);
		goto out_free;
	}
	else {
		pr_err("failed to get start_on_lid_open with ACPI method %s, unexpected value 0x%02x was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				response_value);
		ret = -ERANGE;
		goto out_free;
	}

out_free:
	ACPI_FREE(response_buffer.pointer);
	return ret;
}

static DEVICE_ATTR_RW(start_on_lid_open);


/* USB Charging (USB ports can charge other devices even when device is powered off) */

static ssize_t usb_charging_store(struct device *dev, struct device_attribute *attr,
								  const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	bool value;
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	if (kstrtobool(buffer, &value) < 0)
		return -EINVAL;

	u8 set_payload[21] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x68;

	// payload value should be 0x81 to turn on and 0x80 to turn off
	set_payload[5] = value ? 0x81 : 0x80;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43, 0x58, 0x68, 0x00, 0xaa, value, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set usb_charging with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("turned usb_charging %s\n", value ? "on (1)" : "off (0)");
	return count;
}

static ssize_t usb_charging_show(struct device *dev, struct device_attribute *attr,
								 char *buffer)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;
	struct acpi_buffer response_buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *response;
	u8 response_value;
	int ret = -1;

	u8 read_payload[21] = { 0 };

	read_payload[0] = 0x43;
	read_payload[1] = 0x58;
	read_payload[2] = 0x67;

	read_payload[5] = 0x80;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(read_payload);
	in_obj.buffer.pointer	= read_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get usb_charging with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get usb_charging with ACPI method %s, response type was invalid\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 6) {
		pr_err("failed to get usb_charging with ACPI method %s, response from device was too short\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[5];
	if (response_value == 0xff) {
		pr_err("failed to get usb_charging with ACPI method %s, failure code 0xff was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}
	else if (response_value == 0x00) {
		ret = sysfs_emit(buffer, "%u\n", 0);
		goto out_free;
	}
	else if (response_value == 0x01) {
		ret = sysfs_emit(buffer, "%u\n", 1);
		goto out_free;
	}
	else {
		pr_err("failed to get usb_charging with ACPI method %s, unexpected value 0x%02x was reported from the device\n",
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				response_value);
		ret = -ERANGE;
		goto out_free;
	}

out_free:
	ACPI_FREE(response_buffer.pointer);
	return ret;
}

static DEVICE_ATTR_RW(usb_charging);


/* Add attributes to necessary groups etc */

static struct attribute *galaxybook_attrs[] = {
	&dev_attr_battery_saver.attr,
//	&dev_attr_dolby_atmos.attr,
	&dev_attr_start_on_lid_open.attr,
	&dev_attr_usb_charging.attr,
	NULL
};
ATTRIBUTE_GROUPS(galaxybook);


/*
 * Fan speed
 */

static int fan_speed_get(struct samsung_galaxybook *galaxybook, unsigned int *speed)
{
	struct acpi_buffer response_buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;
	int speed_level = -1;

	status = acpi_evaluate_object(NULL, GALAXYBOOK_ACPI_FAN_SPEED_VALUE, NULL, &response_buffer);
	if (ACPI_FAILURE(status)) {
		pr_err("Get fan state failed\n");
		return -ENODEV;
	}

	obj = response_buffer.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER || obj->integer.value > INT_MAX ||
			(int) obj->integer.value > galaxybook->fan_speeds_count) {
		pr_err("Invalid fan speed data\n");
		ret = -EINVAL;
		goto out_free;
	}

	speed_level = (int) obj->integer.value;
	*speed = galaxybook->fan_speeds[speed_level];

out_free:
	ACPI_FREE(obj);
	return ret;
}

static int __init fan_speed_list_init(struct samsung_galaxybook *galaxybook)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = acpi_evaluate_object(NULL, GALAXYBOOK_ACPI_FAN_SPEED_LIST, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to read fan speed list\n");
		return -ENODEV;
	}

	obj = buffer.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count == 0) {
		pr_err("Invalid fan speed list data\n");
		status = -EINVAL;
		goto out_free;
	}

	// fan_speeds[] starts with a hard-coded 0 (fan is off), then should be appended with the speed levels read in from FANT
	galaxybook->fan_speeds = kzalloc(sizeof(unsigned int) * (obj->package.count + 1), GFP_KERNEL);
	galaxybook->fan_speeds[0] = 0;
	galaxybook->fan_speeds_count = 1;
	for (int i = 1; i <= obj->package.count; i++) {
		if (obj->package.elements[i-1].type != ACPI_TYPE_INTEGER) {
			pr_err("Invalid fan speed list value at position %d (expected type %d, got type %d)\n",
					i-1, ACPI_TYPE_INTEGER, obj->package.elements[i-1].type);
			status = -EINVAL;
			goto err_fan_speeds_free;
		}
		galaxybook->fan_speeds[i] = obj->package.elements[i-1].integer.value;
		galaxybook->fan_speeds_count++;
	}

out_free:
	ACPI_FREE(obj);
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

	galaxybook->fan = *acpi_dev_get_first_match_dev(
		GALAXYBOOK_ACPI_FAN_DEVICE_ID, NULL, -1);

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
 * Hotkeys
 */

static void galaxybook_kbd_backlight_hotkey_work(struct work_struct *work)
{
	struct samsung_galaxybook *galaxybook = container_of(work,
			struct samsung_galaxybook, kbd_backlight_hotkey_work);

	if (galaxybook->kbd_backlight.brightness < galaxybook->kbd_backlight.max_brightness)
		galaxybook_kbd_backlight_set(&galaxybook->kbd_backlight,
				galaxybook->kbd_backlight.brightness + 1);
	else
		galaxybook_kbd_backlight_set(&galaxybook->kbd_backlight, 0);

	led_classdev_notify_brightness_hw_changed(&galaxybook->kbd_backlight,
			galaxybook->kbd_backlight.brightness);

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

		// kbd_backlight keydown
		if (data == 0x2c) {
			pr_info("hotkey: kbd_backlight keydown\n");
		}
		// kbd_backlight keyup
		if (data == 0xac) {
			pr_info("hotkey: kbd_backlight keyup\n");
			schedule_work(&galaxybook_ptr->kbd_backlight_hotkey_work);
		}
	}

	//pr_info("Keyboard input data=%c (0x%02x), str=%c (0x%02x), is AUX? %d\n", data, data, str, str, (str & I8042_STR_AUXDATA));
	return false;
}


/*
 * Platform Profile / Performance mode
 */

typedef enum {
	PERFORMANCE_MODE_SILENT = 0x0b,
	PERFORMANCE_MODE_QUIET = 0x0a,
	PERFORMANCE_MODE_OPTIMIZED = 0x02,
	PERFORMANCE_MODE_HIGH_PERFORMANCE = 0x15,
} galaxybook_performance_mode;

static const char * const galaxybook_performance_mode_names[] = {
	[PERFORMANCE_MODE_SILENT] = "silent",
	[PERFORMANCE_MODE_QUIET] = "quiet",
	[PERFORMANCE_MODE_OPTIMIZED] = "optimized",
	[PERFORMANCE_MODE_HIGH_PERFORMANCE] = "high-performance",
};

static galaxybook_performance_mode profile_performance_mode(enum platform_profile_option profile)
{
	/* Map all the profiles even though we will mark only some of them as supported */
	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		return PERFORMANCE_MODE_SILENT;
	case PLATFORM_PROFILE_COOL:
	case PLATFORM_PROFILE_QUIET:
		return PERFORMANCE_MODE_QUIET;
	case PLATFORM_PROFILE_PERFORMANCE:
		return PERFORMANCE_MODE_HIGH_PERFORMANCE;
	case PLATFORM_PROFILE_BALANCED:
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
	default:
		return PERFORMANCE_MODE_OPTIMIZED;
	}
}

static int platform_profile_set(struct platform_profile_handler *pprof,
									enum platform_profile_option profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	galaxybook_performance_mode performance_mode = profile_performance_mode(profile);

	u8 set_payload[256] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x91;

	set_payload[5] = 0x8d;
	set_payload[6] = 0x02;
	set_payload[7] = 0x46;
	set_payload[8] = 0x82;
	set_payload[9] = 0xca;
	set_payload[10] = 0x8b;
	set_payload[11] = 0x55;
	set_payload[12] = 0x4a;
	set_payload[13] = 0xba;
	set_payload[14] = 0x0f;
	set_payload[15] = 0x6f;
	set_payload[16] = 0x1e;
	set_payload[17] = 0x6b;
	set_payload[18] = 0x92;
	set_payload[19] = 0x1b;
	set_payload[20] = 0x8f;
	set_payload[21] = 0x51;
	set_payload[22] = 0x03;

	set_payload[23] = performance_mode;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	// "set" should always reply with:
	// {0x43, 0x58, 0x91, 0x00, 0xaa, 0x8d, 0x02, 0x46, 0x82, 0xca, 0x8b, 0x55, 0x4a, 0xba, 0x0f, 0x6f, 0x1e, 0x6b, 0x92, 0x1b, 0x8f, 0x51, 0x03, 0x00, ... }
	// TODO: Should we read response buffer and check this before continuing?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set performance_mode with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("set performance_mode to '%s'\n",
			galaxybook_performance_mode_names[performance_mode]);
	galaxybook->current_profile = profile;
	return 0;
}

static int platform_profile_get(struct platform_profile_handler *pprof,
									enum platform_profile_option *profile)
{
	struct samsung_galaxybook *galaxybook = container_of(pprof,
			struct samsung_galaxybook, profile_handler);
	*profile = galaxybook->current_profile;
	return 0;
}

static int galaxybook_profile_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	galaxybook->profile_handler.profile_get = platform_profile_get;
	galaxybook->profile_handler.profile_set = platform_profile_set;

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


/*
 * Platform
 */

static struct platform_driver galaxybook_platform_driver = {
	.driver = {
		.name = SAMSUNG_GALAXYBOOK_CLASS,
		.acpi_match_table = galaxybook_device_ids,
		.dev_groups = galaxybook_groups,
	},
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
 * Temp WMI stuff
 * TODO: just adding a notify handler for this unrecognized GUID and see if we can get something useful with this?
 */

#define GALAXYBOOK_WMI_EVENT_GUID "A6FEA33E-DABF-46F5-BFC8-460D961BEC9F"

static void galaxybook_wmi_notify(u32 value, void *context)
{
	pr_warn("Please create an issue with this information at https://github.com/joshuagrisham/samsung-galaxybook-extras/issues\n");
	pr_warn("WMI Event received: %u\n", value);

	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmi_get_event_data(value, &response);
	if (ACPI_FAILURE(status)) {
		pr_err("Bad event status 0x%x\n", status);
		return;
	}

	obj = (union acpi_object *)response.pointer;
	if (!obj)
		return;

	pr_warn("WMI Event Data object type: %i\n", obj->type);

	kfree(response.pointer);
}
static int __init galaxybook_wmi_init(void)
{
	if (!wmi_has_guid(GALAXYBOOK_WMI_EVENT_GUID))
		return -ENODEV;

	pr_info("installing wmi notify handler\n");
	wmi_install_notify_handler(GALAXYBOOK_WMI_EVENT_GUID, galaxybook_wmi_notify, NULL);
	return 0;
}
static int galaxybook_wmi_exit(void)
{
	if (!wmi_has_guid(GALAXYBOOK_WMI_EVENT_GUID))
		return -ENODEV;

	wmi_remove_notify_handler(GALAXYBOOK_WMI_EVENT_GUID);
	return 0;
}


/*
 * ACPI
 */

static void galaxybook_acpi_notify(struct acpi_device *device, u32 event)
{
	pr_warn("Please create an issue with this information at https://github.com/joshuagrisham/samsung-galaxybook-extras/issues\n");
	pr_warn("ACPI notification received: %x\n", event);
}

static int galaxybook_acpi_init(struct samsung_galaxybook *galaxybook)
{
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	u8 init_payload[21] = { 0 };
	u8 init_perf_payload[256] = { 0 };

	// shared init_payload setup
	// position 2 seems to point to different device control types
	// and each one needs to be initialized with 0xbb 0xaa before use

	init_payload[0] = 0x43;
	init_payload[1] = 0x58;

	init_payload[5] = 0xbb;
	init_payload[6] = 0xaa;

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(init_payload);
	in_obj.buffer.pointer	= init_payload;

	params.count = 1;
	params.pointer = &in_obj;

	// a successful "init" should always reply with:
	// {0x43, 0x58, init_payload[2], 0x00, 0xaa, 0xdd, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// TODO: if we should actually read the response buffer and check this on each init before continuing

	// init 0x7a device controls
	// (battery saver, start on lid open, dolby atmos, etc)

	init_payload[2] = 0x7a;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x86 device controls
	// (unknown but assumed necessary, perhaps related to USB Charging?)

	init_payload[2] = 0x86;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x8a device controls
	// (unknown but assumed necessary)

	init_payload[2] = 0x8a;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x78 device controls
	// (keyboard backlight)

	/* This is handled by galaxybook_kbd_backlight_init instead
	init_payload[2] = 0x78;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				GALAXYBOOK_ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}
	*/

	pr_info("device controls successfully initialized via ACPI method %s\n",
			GALAXYBOOK_ACPI_METHOD_SETTINGS);

	// init 0x91 "performance mode" device controls

	init_perf_payload[0] = 0x43;
	init_perf_payload[1] = 0x58;
	init_perf_payload[2] = 0x91;

	init_perf_payload[5] = 0x8d;
	init_perf_payload[6] = 0x02;
	init_perf_payload[7] = 0x46;
	init_perf_payload[8] = 0x82;
	init_perf_payload[9] = 0xca;
	init_perf_payload[10] = 0x8b;
	init_perf_payload[11] = 0x55;
	init_perf_payload[12] = 0x4a;
	init_perf_payload[13] = 0xba;
	init_perf_payload[14] = 0x0f;
	init_perf_payload[15] = 0x6f;
	init_perf_payload[16] = 0x1e;
	init_perf_payload[17] = 0x6b;
	init_perf_payload[18] = 0x92;
	init_perf_payload[19] = 0x1b;
	init_perf_payload[20] = 0x8f;
	init_perf_payload[21] = 0x51;

	in_obj.buffer.length 	= sizeof(init_perf_payload);
	in_obj.buffer.pointer	= init_perf_payload;

	// first init with 0x01
	init_perf_payload[22] = 0x01;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// first init should reply with:
	// 0x43,0x58,0x91,0x00,0xaa,0x8d,0x02,0x46,0x82,0xca,0x8b,0x55,0x4a,0xba
	// 0x0f,0x6f,0x1e,0x6b,0x92,0x1b,0x8f,0x51,0x01,0x07,0x00,0x01,0x02,0x0a
	// 0x0b,0x14,0x15,0x00,...
	// TODO: should we read the response buffer and check this before continuing?

	// second init with 0x00
	init_perf_payload[22] = 0x00;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// second init should reply with:
	// 0x43,0x58,0x91,0x00,0xaa,0x8d,0x02,0x46,0x82,0xca,0x8b,0x55,0x4a,0xba
	// 0x0f,0x6f,0x1e,0x6b,0x92,0x1b,0x8f,0x51,0x00,0x01,0x01,0x01,0x01,0x00,...
	// TODO: should we read the response buffer and check this before continuing?

	pr_info("performance_mode control successfully initialized via ACPI method %s\n",
			GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE);

	// set performance_mode to a default value of 'optimized'
	// without always ensuring to set a performance mode here, the kbd_backlight has intermittent problems
	// TODO: better if this is based on some user-desired value and not hard-coded to 'optimized'

	galaxybook->current_profile = GALAXYBOOK_STARTUP_PROFILE;
	init_perf_payload[22] = 0x03;
	init_perf_payload[23] = profile_performance_mode(galaxybook->current_profile);

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				GALAXYBOOK_ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("performance_mode initialized with startup value of '%s'\n",
			galaxybook_performance_mode_names[profile_performance_mode(galaxybook->current_profile)]);

	return 0;
}

static void galaxybook_acpi_exit(struct samsung_galaxybook *galaxybook)
{
	// send same payloads as init when exiting in order to properly shutdown
	pr_info("removing ACPI device (will execute init again to shutdown gracefully)\n");
	galaxybook_acpi_init(galaxybook);
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

	pr_info("initializing platform profile\n");
	err = galaxybook_profile_init(galaxybook);
	if (err)
		goto err_platform_exit;

	pr_info("initializing kbd_backlight\n");
	err = galaxybook_kbd_backlight_init(galaxybook);
	if (err)
		goto err_profile_exit;

	// initialize hotkey work queues
	INIT_WORK(&galaxybook->kbd_backlight_hotkey_work,
			galaxybook_kbd_backlight_hotkey_work);

	pr_info("installing i8402 key filter to capture hotkey input\n");
	err = i8042_install_filter(galaxybook_i8042_filter);
	if (err)
		pr_err("Unable to install key filter\n");

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

	galaxybook_wmi_init();

	// Set galaxybook_ptr reference so it can be used by hotkeys
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

	galaxybook_wmi_exit();

	i8042_remove_filter(galaxybook_i8042_filter);
	cancel_work_sync(&galaxybook->kbd_backlight_hotkey_work);

	galaxybook_kbd_backlight_exit(galaxybook);
	galaxybook_fan_speed_exit(galaxybook);
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
	.flags = ACPI_DRIVER_ALL_NOTIFY_EVENTS, // TODO: See if this helps to catch other keypresses, otherwise remove
	.ops = {
		.add = galaxybook_acpi_add,
		.remove = galaxybook_acpi_remove,
		.notify = galaxybook_acpi_notify,
		},
	.owner = THIS_MODULE,
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
MODULE_DESCRIPTION("Samsung Galaxybook Extras");
MODULE_LICENSE("GPL");
