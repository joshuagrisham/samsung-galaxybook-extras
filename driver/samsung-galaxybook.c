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
#include <linux/leds.h>
#include <linux/platform_device.h>

#define SAMSUNG_GALAXYBOOK_CLASS "samsung-galaxybook"
#define SAMSUNG_GALAXYBOOK_NAME "Samsung Galaxybook Extras"

#define ACPI_METHOD_SETTINGS "CSFI"
#define ACPI_METHOD_PERFORMANCE_MODE "CSXI"

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
	struct acpi_device *acpi;
	struct platform_device *platform;
	struct led_classdev kbd_backlight;
};

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
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set kbd_backlight brightness with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init kbd_backlight with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// "init" should reply with:
	// {0x43, 0x58, 0x78, 0x00, 0xaa, 0xdd, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// TODO: Should we read the response buffer and check this before continuing?

	pr_info("kbd_backlight successfully initialized via ACPI method %s\n",
			ACPI_METHOD_SETTINGS);

	galaxybook->kbd_backlight = (struct led_classdev) {
		.brightness_get = galaxybook_kbd_backlight_get,
		.brightness_set_blocking = galaxybook_kbd_backlight_set,
		.name = SAMSUNG_GALAXYBOOK_CLASS "::kbd_backlight",
		.max_brightness = 3,
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
			ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43,0x58,0x7a,0x00,0xaa,0x00,0x00,0x90,(value ? 0x55 : 0x00),0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
	// TODO: Should we read response buffer and check this before continuing? or at least that pos 5 is not 0xff?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set battery_saver with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get battery_saver with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get battery_saver with ACPI method %s, response type was invalid\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 8) {
		pr_err("failed to get battery_saver with ACPI method %s, response from device was too short\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[7];
	if (response->buffer.pointer[5] == 0xff) {
		pr_err("failed to get battery_saver with ACPI method %s, failure code 0xff was reported from the device\n",
				ACPI_METHOD_SETTINGS);
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
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43,0x58,0x7a,0x00,0xaa,0x00,0x00,0x80,value,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
	// TODO: Should we read response buffer and check this before continuing?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set start_on_lid_open with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, response type was invalid\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 6) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, response from device was too short\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[7];
	if (response->buffer.pointer[5] == 0xff) {
		pr_err("failed to get start_on_lid_open with ACPI method %s, failure code 0xff was reported from the device\n",
				ACPI_METHOD_SETTINGS);
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
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, NULL);

	// "set" should reply with:
	// {0x43, 0x58, 0x68, 0x00, 0xaa, value, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set usb_charging with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
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
			ACPI_METHOD_SETTINGS, &params, &response_buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to get usb_charging with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		ret = -ENXIO;
		goto out_free;
	}

	response = response_buffer.pointer;
	if (response->type != ACPI_TYPE_BUFFER) {
		pr_err("failed to get usb_charging with ACPI method %s, response type was invalid\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	if (response->buffer.length < 6) {
		pr_err("failed to get usb_charging with ACPI method %s, response from device was too short\n",
				ACPI_METHOD_SETTINGS);
		ret = -EIO;
		goto out_free;
	}

	response_value = response->buffer.pointer[5];
	if (response_value == 0xff) {
		pr_err("failed to get usb_charging with ACPI method %s, failure code 0xff was reported from the device\n",
				ACPI_METHOD_SETTINGS);
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
				ACPI_METHOD_SETTINGS,
				response_value);
		ret = -ERANGE;
		goto out_free;
	}

out_free:
	ACPI_FREE(response_buffer.pointer);
	return ret;
}

static DEVICE_ATTR_RW(usb_charging);


/* Performance mode */

typedef enum {
	PERFORMANCE_MODE_SILENT,
	PERFORMANCE_MODE_QUIET,
	PERFORMANCE_MODE_OPTIMIZED,
	PERFORMANCE_MODE_HIGH_PERFORMANCE,
} galaxybook_performance_mode;

static u8 galaxybook_performance_mode_values[] = {
	0x0b, // PERFORMANCE_MODE_SILENT
	0x0a, // PERFORMANCE_MODE_QUIET
	0x02, // PERFORMANCE_MODE_OPTIMIZED
	0x15, // PERFORMANCE_MODE_HIGH_PERFORMANCE
};

const static struct {
    galaxybook_performance_mode val;
    const char *str;
} performance_mode_conversion[] = {
    {PERFORMANCE_MODE_SILENT, "silent"},
    {PERFORMANCE_MODE_QUIET, "quiet"},
    {PERFORMANCE_MODE_OPTIMIZED, "optimized"},
    {PERFORMANCE_MODE_HIGH_PERFORMANCE, "high"},
    {PERFORMANCE_MODE_HIGH_PERFORMANCE, "high performance"},
    {PERFORMANCE_MODE_HIGH_PERFORMANCE, "highperformance"},
};

galaxybook_performance_mode performance_mode_from_str (const char *str)
{
	// if input string has trailing newline then it should be ignored
	int compare_size = (str[strlen(str) - 1] == '\n') ? strlen(str) - 1 : strlen(str);
	for (int i = 0; i < sizeof (performance_mode_conversion) / sizeof (performance_mode_conversion[0]); i++)
		if (!strncasecmp (str, performance_mode_conversion[i].str, compare_size))
			return performance_mode_conversion[i].val;
	return -1;
}

static galaxybook_performance_mode performance_mode;
static bool performance_mode_was_set;

static ssize_t performance_mode_store(struct device *dev, struct device_attribute *attr,
									  const char *buffer, size_t count)
{
	struct samsung_galaxybook *galaxybook = dev_get_drvdata(dev);
	union acpi_object in_obj;
	struct acpi_object_list params;
	acpi_status status;

	int value = performance_mode_from_str(buffer);
	if (value < 0)
		if (kstrtoint(buffer, 0, &value) < 0)
			return -EINVAL;

	if (value < 0 || value > PERFORMANCE_MODE_HIGH_PERFORMANCE)
		return -ERANGE;

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

	set_payload[23] = galaxybook_performance_mode_values[value];

	in_obj.type 			= ACPI_TYPE_BUFFER;
	in_obj.buffer.length 	= sizeof(set_payload);
	in_obj.buffer.pointer	= set_payload;

	params.count = 1;
	params.pointer = &in_obj;

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	// "set" should always reply with:
	// {0x43, 0x58, 0x91, 0x00, 0xaa, 0x8d, 0x02, 0x46, 0x82, 0xca, 0x8b, 0x55, 0x4a, 0xba, 0x0f, 0x6f, 0x1e, 0x6b, 0x92, 0x1b, 0x8f, 0x51, 0x03, 0x00, ... }
	// TODO: Should we read response buffer and check this before continuing?

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set performance_mode with ACPI method %s, got %s\n",
				ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	pr_info("set performance_mode to %d\n", value);
	performance_mode = value;
	performance_mode_was_set = true;
	return count;
}

static ssize_t performance_mode_show(struct device *dev, struct device_attribute *attr,
									 char *buffer)
{
	// current performance_mode is not known until it has been set by the driver
	if (performance_mode_was_set)
		return sysfs_emit(buffer, "%u\n", performance_mode);
	else
		return sysfs_emit(buffer, "%s\n", "unknown");
}

static DEVICE_ATTR_RW(performance_mode);

/* Add attributes to necessary groups etc */

static struct attribute *galaxybook_attrs[] = {
	&dev_attr_battery_saver.attr,
//	&dev_attr_dolby_atmos.attr,
	&dev_attr_start_on_lid_open.attr,
	&dev_attr_usb_charging.attr,
	&dev_attr_performance_mode.attr,
	NULL
};

static const struct attribute_group galaxybook_attr_group = {
	.attrs = galaxybook_attrs,
};

static const struct attribute_group *galaxybook_attr_groups[] = {
	&galaxybook_attr_group,
	NULL
};


/*
 * Platform
 */

static struct platform_driver galaxybook_platform_driver = {
	.driver = {
		.name = SAMSUNG_GALAXYBOOK_CLASS,
		.acpi_match_table = galaxybook_device_ids,
		.dev_groups = galaxybook_attr_groups,
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
 * ACPI
 */

static void galaxybook_acpi_notify(struct acpi_device *device, u32 event)
{
	pr_info("ACPI notification received: %x\n", event);
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
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x86 device controls
	// (unknown but assumed necessary, perhaps related to USB Charging?)

	init_payload[2] = 0x86;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x8a device controls
	// (unknown but assumed necessary)

	init_payload[2] = 0x8a;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// init 0x78 device controls
	// (keyboard backlight)

	/* This is handled by galaxybook_kbd_backlight_init instead
	init_payload[2] = 0x78;
	status = acpi_evaluate_object(galaxybook->acpi->handle,
			ACPI_METHOD_SETTINGS, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init 0x%02x controls with ACPI method %s, got %s\n",
				init_payload[2],
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}
	*/

	pr_info("device controls successfully initialized via ACPI method %s\n",
			ACPI_METHOD_SETTINGS);
	
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
			ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				ACPI_METHOD_PERFORMANCE_MODE,
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
			ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// second init should reply with:
	// 0x43,0x58,0x91,0x00,0xaa,0x8d,0x02,0x46,0x82,0xca,0x8b,0x55,0x4a,0xba
	// 0x0f,0x6f,0x1e,0x6b,0x92,0x1b,0x8f,0x51,0x00,0x01,0x01,0x01,0x01,0x00,...
	// TODO: should we read the response buffer and check this before continuing?

	pr_info("performance_mode control successfully initialized via ACPI method %s\n",
			ACPI_METHOD_PERFORMANCE_MODE);

	// set performance_mode to a default value of 'optimized'
	// without always ensuring to set a performance mode here, the kbd_backlight has intermittent problems
	// TODO: better if this is based on some user-desired value and not hard-coded to 'optimized'
	performance_mode = PERFORMANCE_MODE_OPTIMIZED;

	init_perf_payload[22] = 0x03;
	init_perf_payload[23] = galaxybook_performance_mode_values[performance_mode];

	status = acpi_evaluate_object(galaxybook->acpi->handle,
			ACPI_METHOD_PERFORMANCE_MODE, &params, NULL);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to init performance_mode control with ACPI method %s, got %s\n",
				ACPI_METHOD_PERFORMANCE_MODE,
				acpi_format_exception(status));
		return -ENXIO;
	}

	performance_mode_was_set = true;
	pr_info("performance_mode initialized with startup value of %d\n",
		performance_mode);

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

	pr_info("initializing kbd_backlight\n");
	err = galaxybook_kbd_backlight_init(galaxybook);
	if (err)
		goto err_platform_exit;

	return 0;

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

	galaxybook_kbd_backlight_exit(galaxybook);
	galaxybook_platform_exit(galaxybook);
	galaxybook_acpi_exit(galaxybook);

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
		goto err_unregister_acpi;

	ret = acpi_bus_register_driver(&galaxybook_acpi_driver);
	if (ret < 0)
		goto err_unregister_platform;

	pr_info("driver successfully loaded\n");

	return 0;

err_unregister_platform:
	platform_driver_unregister(&galaxybook_platform_driver);
err_unregister_acpi:
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
