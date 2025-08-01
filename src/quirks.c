/*
 * Copyright © 2018 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

/* This has the hallmarks of a library to make it re-usable from the tests
 * and from the list-quirks tool. It doesn't have all of the features from a
 * library you'd expect though
 */

#undef NDEBUG /* You don't get to disable asserts here */
#include <assert.h>
#include <dirent.h>
#include <fnmatch.h>
#include <libgen.h>
#include <libudev.h>
#include <stdlib.h>
#ifdef __FreeBSD__
#include <kenv.h>
#endif

#include "libinput-util.h"
#include "libinput-versionsort.h"
#include "quirks.h"

/* Custom logging so we can have detailed output for the tool but minimal
 * logging for libinput itself. */
#define qlog_debug(ctx_, ...) quirk_log_msg((ctx_), QLOG_NOISE, __VA_ARGS__)
#define qlog_info(ctx_, ...) quirk_log_msg((ctx_),  QLOG_INFO, __VA_ARGS__)
#define qlog_error(ctx_, ...) quirk_log_msg((ctx_), QLOG_ERROR, __VA_ARGS__)
#define qlog_parser(ctx_, ...) quirk_log_msg((ctx_), QLOG_PARSER_ERROR, __VA_ARGS__)

enum property_type {
	PT_UINT,
	PT_INT,
	PT_STRING,
	PT_BOOL,
	PT_DIMENSION,
	PT_RANGE,
	PT_DOUBLE,
	PT_TUPLES,
	PT_UINT_ARRAY,
};

struct quirk_array {
	union {
		uint32_t u[32];
	} data;
	size_t nelements;
};

/**
 * Generic value holder for the property types we support. The type
 * identifies which value in the union is defined and we expect callers to
 * already know which type yields which value.
 */
struct property {
	size_t refcount;
	struct list link; /* struct sections.properties */

	enum quirk id;
	enum property_type type;
	union {
		bool b;
		uint32_t u;
		int32_t i;
		char *s;
		double d;
		struct quirk_dimensions dim;
		struct quirk_range range;
		struct quirk_tuples tuples;
		struct quirk_array array;
	} value;
};

enum match_flags {
	M_NAME = bit(0),
	M_BUS = bit(1),
	M_VID = bit(2),
	M_PID = bit(3),
	M_DMI = bit(4),
	M_UDEV_TYPE = bit(5),
	M_DT = bit(6),
	M_VERSION = bit(7),
	M_UNIQ = bit(8),

	M_LAST = M_UNIQ,
};

enum bustype {
	BT_UNKNOWN,
	BT_USB,
	BT_BLUETOOTH,
	BT_PS2,
	BT_RMI,
	BT_I2C,
	BT_SPI,
};

enum udev_type {
	UDEV_MOUSE = bit(1),
	UDEV_POINTINGSTICK = bit(2),
	UDEV_TOUCHPAD = bit(3),
	UDEV_TABLET = bit(4),
	UDEV_TABLET_PAD = bit(5),
	UDEV_JOYSTICK = bit(6),
	UDEV_KEYBOARD = bit(7),
};

/**
 * Contains the combined set of matches for one section or the values for
 * one device.
 *
 * bits defines which fields are set, the rest is zero.
 */
struct match {
	uint32_t bits;

	char *name;
	char *uniq;
	enum bustype bus;
	uint32_t vendor;
	uint32_t product[64]; /* zero-terminated */
	uint32_t version;

	char *dmi; /* dmi modalias with preceding "dmi:" */

	/* We can have more than one type set, so this is a bitfield */
	uint32_t udev_type;

	char *dt; /* device tree compatible (first) string */
};

/**
 * Represents one section in the .quirks file.
 */
struct section {
	struct list link;

	bool has_match;    /* to check for empty sections */
	bool has_property; /* to check for empty sections */

	char *name; /* the [Section Name] */
	struct match match;
	struct list properties;
};

/**
 * The struct returned to the caller. It contains the
 * properties for a given device.
 */
struct quirks {
	size_t refcount;
	struct list link; /* struct quirks_context.quirks */

	/* These are not ref'd, just a collection of pointers */
	struct property **properties;
	size_t nproperties;

	/* Special properties for AttrEventCode and AttrInputCode, these are
	 * owned by us, not the section */
	struct list floating_properties;
};

/**
 * Quirk matching context, initialized once with quirks_init_subsystem()
 */
struct quirks_context {
	size_t refcount;

	libinput_log_handler log_handler;
	enum quirks_log_type log_type;
	struct libinput *libinput; /* for logging */

	char *dmi;
	char *dt;

	struct list sections;

	/* list of quirks handed to libinput, just for bookkeeping */
	struct list quirks;
};

LIBINPUT_ATTRIBUTE_PRINTF(3, 0)
static inline void
quirk_log_msg_va(struct quirks_context *ctx,
		 enum quirks_log_priorities priority,
		 const char *format,
		 va_list args)
{
	switch (priority) {
	/* We don't use this if we're logging through libinput */
	default:
	case QLOG_NOISE:
	case QLOG_PARSER_ERROR:
		if (ctx->log_type == QLOG_LIBINPUT_LOGGING)
			return;
		break;
	case QLOG_DEBUG: /* These map straight to libinput priorities */
	case QLOG_INFO:
	case QLOG_ERROR:
		break;
	}

	ctx->log_handler(ctx->libinput,
			 (enum libinput_log_priority)priority,
			 format,
			 args);
}

LIBINPUT_ATTRIBUTE_PRINTF(3, 4)
static inline void
quirk_log_msg(struct quirks_context *ctx,
	      enum quirks_log_priorities priority,
	      const char *format,
	      ...)
{
	va_list args;

	va_start(args, format);
	quirk_log_msg_va(ctx, priority, format, args);
	va_end(args);
}

const char *
quirk_get_name(enum quirk q)
{
	switch (q) {
	case QUIRK_MODEL_ALPS_SERIAL_TOUCHPAD:
		return "ModelALPSSerialTouchpad";
	case QUIRK_MODEL_APPLE_TOUCHPAD:
		return "ModelAppleTouchpad";
	case QUIRK_MODEL_APPLE_TOUCHPAD_ONEBUTTON:
		return "ModelAppleTouchpadOneButton";
	case QUIRK_MODEL_BOUNCING_KEYS:
		return "ModelBouncingKeys";
	case QUIRK_MODEL_CHROMEBOOK:
		return "ModelChromebook";
	case QUIRK_MODEL_CLEVO_W740SU:
		return "ModelClevoW740SU";
	case QUIRK_MODEL_DELL_CANVAS_TOTEM:
		return "ModelDellCanvasTotem";
	case QUIRK_MODEL_HP_PAVILION_DM4_TOUCHPAD:
		return "ModelHPPavilionDM4Touchpad";
	case QUIRK_MODEL_HP_ZBOOK_STUDIO_G3:
		return "ModelHPZBookStudioG3";
	case QUIRK_MODEL_INVERT_HORIZONTAL_SCROLLING:
		return "ModelInvertHorizontalScrolling";
	case QUIRK_MODEL_LENOVO_SCROLLPOINT:
		return "ModelLenovoScrollPoint";
	case QUIRK_MODEL_LENOVO_T450_TOUCHPAD:
		return "ModelLenovoT450Touchpad";
	case QUIRK_MODEL_LENOVO_X1GEN6_TOUCHPAD:
		return "ModelLenovoX1Gen6Touchpad";
	case QUIRK_MODEL_LENOVO_X230:
		return "ModelLenovoX230";
	case QUIRK_MODEL_SYNAPTICS_SERIAL_TOUCHPAD:
		return "ModelSynapticsSerialTouchpad";
	case QUIRK_MODEL_SYSTEM76_BONOBO:
		return "ModelSystem76Bonobo";
	case QUIRK_MODEL_SYSTEM76_GALAGO:
		return "ModelSystem76Galago";
	case QUIRK_MODEL_SYSTEM76_KUDU:
		return "ModelSystem76Kudu";
	case QUIRK_MODEL_TABLET_MODE_NO_SUSPEND:
		return "ModelTabletModeNoSuspend";
	case QUIRK_MODEL_TABLET_MODE_SWITCH_UNRELIABLE:
		return "ModelTabletModeSwitchUnreliable";
	case QUIRK_MODEL_TOUCHPAD_VISIBLE_MARKER:
		return "ModelTouchpadVisibleMarker";
	case QUIRK_MODEL_TOUCHPAD_PHANTOM_CLICKS:
		return "ModelTouchpadPhantomClicks";
	case QUIRK_MODEL_TRACKBALL:
		return "ModelTrackball";
	case QUIRK_MODEL_WACOM_TOUCHPAD:
		return "ModelWacomTouchpad";
	case QUIRK_MODEL_PRESSURE_PAD:
		return "ModelPressurePad";
	case QUIRK_MODEL_LOGITECH_MX_MASTER_3:
		return "ModelLogitechMXMaster3";

	case QUIRK_ATTR_SIZE_HINT:
		return "AttrSizeHint";
	case QUIRK_ATTR_TOUCH_SIZE_RANGE:
		return "AttrTouchSizeRange";
	case QUIRK_ATTR_PALM_SIZE_THRESHOLD:
		return "AttrPalmSizeThreshold";
	case QUIRK_ATTR_LID_SWITCH_RELIABILITY:
		return "AttrLidSwitchReliability";
	case QUIRK_ATTR_KEYBOARD_INTEGRATION:
		return "AttrKeyboardIntegration";
	case QUIRK_ATTR_TRACKPOINT_INTEGRATION:
		return "AttrPointingStickIntegration";
	case QUIRK_ATTR_TPKBCOMBO_LAYOUT:
		return "AttrTPKComboLayout";
	case QUIRK_ATTR_PRESSURE_RANGE:
		return "AttrPressureRange";
	case QUIRK_ATTR_PALM_PRESSURE_THRESHOLD:
		return "AttrPalmPressureThreshold";
	case QUIRK_ATTR_RESOLUTION_HINT:
		return "AttrResolutionHint";
	case QUIRK_ATTR_TRACKPOINT_MULTIPLIER:
		return "AttrTrackpointMultiplier";
	case QUIRK_ATTR_THUMB_PRESSURE_THRESHOLD:
		return "AttrThumbPressureThreshold";
	case QUIRK_ATTR_USE_VELOCITY_AVERAGING:
		return "AttrUseVelocityAveraging";
	case QUIRK_ATTR_TABLET_SMOOTHING:
		return "AttrTabletSmoothing";
	case QUIRK_ATTR_THUMB_SIZE_THRESHOLD:
		return "AttrThumbSizeThreshold";
	case QUIRK_ATTR_MSC_TIMESTAMP:
		return "AttrMscTimestamp";
	case QUIRK_ATTR_EVENT_CODE:
		return "AttrEventCode";
	case QUIRK_ATTR_INPUT_PROP:
		return "AttrInputProp";
	case QUIRK_ATTR_IS_VIRTUAL:
		return "AttrIsVirtual";
	default:
		abort();
	}
}

static inline const char *
matchflagname(enum match_flags f)
{
	switch (f) {
	case M_NAME:
		return "MatchName";
		break;
	case M_BUS:
		return "MatchBus";
		break;
	case M_VID:
		return "MatchVendor";
		break;
	case M_PID:
		return "MatchProduct";
		break;
	case M_VERSION:
		return "MatchVersion";
		break;
	case M_DMI:
		return "MatchDMIModalias";
		break;
	case M_UDEV_TYPE:
		return "MatchUdevType";
		break;
	case M_DT:
		return "MatchDeviceTree";
		break;
	case M_UNIQ:
		return "MatchUniq";
		break;
	default:
		abort();
	}
}

static inline struct property *
property_new(void)
{
	struct property *p;

	p = zalloc(sizeof *p);
	p->refcount = 1;
	list_init(&p->link);

	return p;
}

static inline struct property *
property_ref(struct property *p)
{
	assert(p->refcount > 0);
	p->refcount++;
	return p;
}

static inline struct property *
property_unref(struct property *p)
{
	/* Note: we don't cleanup here, that is a separate call so we
	   can abort if we haven't cleaned up correctly.  */
	assert(p->refcount > 0);
	p->refcount--;

	return NULL;
}

/* Separate call so we can verify that the caller unrefs the property
 * before shutting down the subsystem.
 */
static inline void
property_cleanup(struct property *p)
{
	/* If we get here, the quirks must've been removed already */
	property_unref(p);
	assert(p->refcount == 0);

	list_remove(&p->link);
	if (p->type == PT_STRING)
		free(p->value.s);
	free(p);
}

/**
 * Return the system DMI info in modalias format.
 */
#ifdef __linux__
static inline char *
init_dmi_linux(void)
{
	const char *modalias = NULL;
	const char *syspath = "/sys/devices/virtual/dmi/id";

	_unref_(udev) *udev = udev_new();
	if (!udev)
		return NULL;

	_unref_(udev_device) *udev_device = udev_device_new_from_syspath(udev, syspath);
	if (udev_device)
		modalias = udev_device_get_property_value(udev_device, "MODALIAS");

	/* Not sure whether this could ever really fail, if so we should
	 * open the sysfs file directly. But then udev wouldn't have failed,
	 * so... */
	if (!modalias)
		modalias = "dmi:*";

	return safe_strdup(modalias);
}
#endif

#ifdef __FreeBSD__
static inline char *
init_dmi_freebsd(void)
{
#define LEN (KENV_MVALLEN + 1)
	char *modalias;
	char bios_vendor[LEN], bios_version[LEN], bios_date[LEN];
	char sys_vendor[LEN], product_name[LEN], product_version[LEN];
	char board_vendor[LEN], board_name[LEN], board_version[LEN];
	char chassis_vendor[LEN], chassis_type[LEN], chassis_version[LEN];
	int chassis_type_num = 0x2;

	kenv(KENV_GET, "smbios.bios.vendor", bios_vendor, LEN);
	kenv(KENV_GET, "smbios.bios.version", bios_version, LEN);
	kenv(KENV_GET, "smbios.bios.reldate", bios_date, LEN);
	kenv(KENV_GET, "smbios.system.maker", sys_vendor, LEN);
	kenv(KENV_GET, "smbios.system.product", product_name, LEN);
	kenv(KENV_GET, "smbios.system.version", product_version, LEN);
	kenv(KENV_GET, "smbios.planar.maker", board_vendor, LEN);
	kenv(KENV_GET, "smbios.planar.product", board_name, LEN);
	kenv(KENV_GET, "smbios.planar.version", board_version, LEN);
	kenv(KENV_GET, "smbios.chassis.vendor", chassis_vendor, LEN);
	kenv(KENV_GET, "smbios.chassis.type", chassis_type, LEN);
	kenv(KENV_GET, "smbios.chassis.version", chassis_version, LEN);
#undef LEN

	if (strcmp(chassis_type, "Desktop") == 0)
		chassis_type_num = 0x3;
	else if (strcmp(chassis_type, "Portable") == 0)
		chassis_type_num = 0x8;
	else if (strcmp(chassis_type, "Laptop") == 0)
		chassis_type_num = 0x9;
	else if (strcmp(chassis_type, "Notebook") == 0)
		chassis_type_num = 0xA;
	else if (strcmp(chassis_type, "Tablet") == 0)
		chassis_type_num = 0x1E;
	else if (strcmp(chassis_type, "Convertible") == 0)
		chassis_type_num = 0x1F;
	else if (strcmp(chassis_type, "Detachable") == 0)
		chassis_type_num = 0x20;

	xasprintf(
		&modalias,
		"dmi:bvn%s:bvr%s:bd%s:svn%s:pn%s:pvr%s:rvn%s:rn%s:rvr%s:cvn%s:ct%d:cvr%s:",
		bios_vendor,
		bios_version,
		bios_date,
		sys_vendor,
		product_name,
		product_version,
		board_vendor,
		board_name,
		board_version,
		chassis_vendor,
		chassis_type_num,
		chassis_version);

	return modalias;
}
#endif

static inline char *
init_dmi(void)
{
	if (getenv("LIBINPUT_RUNNING_TEST_SUITE"))
		return safe_strdup("dmi:");

#if defined(__linux__)
	return init_dmi_linux();
#elif defined(__FreeBSD__)
	return init_dmi_freebsd();
#else
	return NULL;
#endif
}

/**
 * Return the dt compatible string
 */
static inline char *
init_dt(void)
{
	char compatible[1024];
	char *copy = NULL;
	const char *syspath = "/sys/firmware/devicetree/base/compatible";
	FILE *fp;

	if (getenv("LIBINPUT_RUNNING_TEST_SUITE"))
		return safe_strdup("");

	fp = fopen(syspath, "r");
	if (!fp)
		return NULL;

	/* devicetree/base/compatible has multiple null-terminated entries
	   but we only care about the first one here, so strdup is enough */
	if (fgets(compatible, sizeof(compatible), fp)) {
		copy = safe_strdup(compatible);
	}

	fclose(fp);

	return copy;
}

static inline struct section *
section_new(const char *path, const char *name)
{
	struct section *s = zalloc(sizeof(*s));

	char *path_dup = safe_strdup(path);
	xasprintf(&s->name, "%s (%s)", name, basename(path_dup));
	free(path_dup);
	list_init(&s->link);
	list_init(&s->properties);

	return s;
}

static inline void
section_destroy(struct section *s)
{
	struct property *p;

	free(s->name);
	free(s->match.name);
	free(s->match.uniq);
	free(s->match.dmi);
	free(s->match.dt);

	list_for_each_safe(p, &s->properties, link)
		property_cleanup(p);

	assert(list_empty(&s->properties));

	list_remove(&s->link);
	free(s);
}

static inline bool
parse_hex(const char *value, unsigned int *parsed)
{
	return strstartswith(value, "0x") && safe_atou_base(value, parsed, 16) &&
	       strspn(value, "0123456789xABCDEF") == strlen(value) && *parsed <= 0xFFFF;
}

static int
strv_parse_hex(const char *str, size_t index, void *data)
{
	unsigned int *product = data;

	return !parse_hex(str, &product[index]); /* 0 for success */
}

/**
 * Parse a MatchFooBar=banana line.
 *
 * @param section The section struct to be filled in
 * @param key The MatchFooBar part of the line
 * @param value The banana part of the line.
 *
 * @return true on success, false otherwise.
 */
static bool
parse_match(struct quirks_context *ctx,
	    struct section *s,
	    const char *key,
	    const char *value)
{
	int rc = false;

#define check_set_bit(s_, bit_) { \
		if ((s_)->match.bits & (bit_)) goto out; \
		(s_)->match.bits |= (bit_); \
	}

	assert(strlen(value) >= 1);

	if (streq(key, "MatchName")) {
		check_set_bit(s, M_NAME);
		s->match.name = safe_strdup(value);
	} else if (streq(key, "MatchUniq")) {
		check_set_bit(s, M_UNIQ);
		s->match.uniq = safe_strdup(value);
	} else if (streq(key, "MatchBus")) {
		check_set_bit(s, M_BUS);
		if (streq(value, "usb"))
			s->match.bus = BT_USB;
		else if (streq(value, "bluetooth"))
			s->match.bus = BT_BLUETOOTH;
		else if (streq(value, "ps2"))
			s->match.bus = BT_PS2;
		else if (streq(value, "rmi"))
			s->match.bus = BT_RMI;
		else if (streq(value, "i2c"))
			s->match.bus = BT_I2C;
		else if (streq(value, "spi"))
			s->match.bus = BT_SPI;
		else
			goto out;
	} else if (streq(key, "MatchVendor")) {
		unsigned int vendor;

		check_set_bit(s, M_VID);
		if (!parse_hex(value, &vendor))
			goto out;

		s->match.vendor = vendor;
	} else if (streq(key, "MatchProduct")) {
		unsigned int product[ARRAY_LENGTH(s->match.product)] = { 0 };
		const size_t max = ARRAY_LENGTH(s->match.product) - 1;

		size_t nelems = 0;
		char **strs = strv_from_string(value, ";", &nelems);
		int rc = strv_for_each_n((const char **)strs,
					 max,
					 strv_parse_hex,
					 product);
		strv_free(strs);
		if (rc != 0)
			goto out;

		check_set_bit(s, M_PID);
		memcpy(s->match.product, product, sizeof(product));
	} else if (streq(key, "MatchVersion")) {
		unsigned int version;

		check_set_bit(s, M_VERSION);
		if (!parse_hex(value, &version))
			goto out;

		s->match.version = version;
	} else if (streq(key, "MatchDMIModalias")) {
		check_set_bit(s, M_DMI);
		if (!strstartswith(value, "dmi:")) {
			qlog_parser(ctx,
				    "%s: MatchDMIModalias must start with 'dmi:'\n",
				    s->name);
			goto out;
		}
		s->match.dmi = safe_strdup(value);
	} else if (streq(key, "MatchUdevType")) {
		check_set_bit(s, M_UDEV_TYPE);
		if (streq(value, "touchpad"))
			s->match.udev_type = UDEV_TOUCHPAD;
		else if (streq(value, "mouse"))
			s->match.udev_type = UDEV_MOUSE;
		else if (streq(value, "pointingstick"))
			s->match.udev_type = UDEV_POINTINGSTICK;
		else if (streq(value, "keyboard"))
			s->match.udev_type = UDEV_KEYBOARD;
		else if (streq(value, "joystick"))
			s->match.udev_type = UDEV_JOYSTICK;
		else if (streq(value, "tablet"))
			s->match.udev_type = UDEV_TABLET;
		else if (streq(value, "tablet-pad"))
			s->match.udev_type = UDEV_TABLET_PAD;
		else
			goto out;
	} else if (streq(key, "MatchDeviceTree")) {
		check_set_bit(s, M_DT);
		s->match.dt = safe_strdup(value);
	} else {
		qlog_error(ctx, "Unknown match key '%s'\n", key);
		goto out;
	}

#undef check_set_bit
	s->has_match = true;
	rc = true;
out:
	return rc;
}

/**
 * Parse a ModelFooBar=1 line.
 *
 * @param section The section struct to be filled in
 * @param key The ModelFooBar part of the line
 * @param value The value after the =, must be 1 or 0.
 *
 * @return true on success, false otherwise.
 */
static bool
parse_model(struct quirks_context *ctx,
	    struct section *s,
	    const char *key,
	    const char *value)
{
	bool b;
	enum quirk q = QUIRK_MODEL_ALPS_SERIAL_TOUCHPAD;

	assert(strstartswith(key, "Model"));

	if (!parse_boolean_property(value, &b))
		return false;

	do {
		if (streq(key, quirk_get_name(q))) {
			struct property *p = property_new();
			p->id = q, p->type = PT_BOOL;
			p->value.b = b;
			list_append(&s->properties, &p->link);
			s->has_property = true;
			return true;
		}
	} while (++q < _QUIRK_LAST_MODEL_QUIRK_);

	qlog_error(ctx, "Unknown key %s in %s\n", key, s->name);

	return false;
}

/**
 * Parse a AttrFooBar=banana line.
 *
 * @param section The section struct to be filled in
 * @param key The AttrFooBar part of the line
 * @param value The banana part of the line.
 *
 * Value parsing depends on the attribute type.
 *
 * @return true on success, false otherwise.
 */
static inline bool
parse_attr(struct quirks_context *ctx,
	   struct section *s,
	   const char *key,
	   const char *value)
{
	struct property *p = property_new();
	bool rc = false;
	struct quirk_dimensions dim;
	struct quirk_range range;
	unsigned int v;
	bool b;
	double d;

	if (streq(key, quirk_get_name(QUIRK_ATTR_SIZE_HINT))) {
		p->id = QUIRK_ATTR_SIZE_HINT;
		if (!parse_dimension_property(value, &dim.x, &dim.y))
			goto out;
		p->type = PT_DIMENSION;
		p->value.dim = dim;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_TOUCH_SIZE_RANGE))) {
		p->id = QUIRK_ATTR_TOUCH_SIZE_RANGE;
		if (!parse_range_property(value, &range.upper, &range.lower))
			goto out;
		p->type = PT_RANGE;
		p->value.range = range;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_PALM_SIZE_THRESHOLD))) {
		p->id = QUIRK_ATTR_PALM_SIZE_THRESHOLD;
		if (!safe_atou(value, &v))
			goto out;
		p->type = PT_UINT;
		p->value.u = v;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_LID_SWITCH_RELIABILITY))) {
		p->id = QUIRK_ATTR_LID_SWITCH_RELIABILITY;
		if (!streq(value, "reliable") && !streq(value, "write_open") &&
		    !streq(value, "unreliable"))
			goto out;
		p->type = PT_STRING;
		p->value.s = safe_strdup(value);
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_KEYBOARD_INTEGRATION))) {
		p->id = QUIRK_ATTR_KEYBOARD_INTEGRATION;
		if (!streq(value, "internal") && !streq(value, "external"))
			goto out;
		p->type = PT_STRING;
		p->value.s = safe_strdup(value);
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_TRACKPOINT_INTEGRATION))) {
		p->id = QUIRK_ATTR_TRACKPOINT_INTEGRATION;
		if (!streq(value, "internal") && !streq(value, "external"))
			goto out;
		p->type = PT_STRING;
		p->value.s = safe_strdup(value);
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_TPKBCOMBO_LAYOUT))) {
		p->id = QUIRK_ATTR_TPKBCOMBO_LAYOUT;
		if (!streq(value, "below"))
			goto out;
		p->type = PT_STRING;
		p->value.s = safe_strdup(value);
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_PRESSURE_RANGE))) {
		p->id = QUIRK_ATTR_PRESSURE_RANGE;
		if (!parse_range_property(value, &range.upper, &range.lower))
			goto out;
		p->type = PT_RANGE;
		p->value.range = range;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_PALM_PRESSURE_THRESHOLD))) {
		p->id = QUIRK_ATTR_PALM_PRESSURE_THRESHOLD;
		if (!safe_atou(value, &v))
			goto out;
		p->type = PT_UINT;
		p->value.u = v;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_RESOLUTION_HINT))) {
		p->id = QUIRK_ATTR_RESOLUTION_HINT;
		if (!parse_dimension_property(value, &dim.x, &dim.y))
			goto out;
		p->type = PT_DIMENSION;
		p->value.dim = dim;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_TRACKPOINT_MULTIPLIER))) {
		p->id = QUIRK_ATTR_TRACKPOINT_MULTIPLIER;
		if (!safe_atod(value, &d))
			goto out;
		p->type = PT_DOUBLE;
		p->value.d = d;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_USE_VELOCITY_AVERAGING))) {
		p->id = QUIRK_ATTR_USE_VELOCITY_AVERAGING;
		if (!parse_boolean_property(value, &b))
			goto out;
		p->type = PT_BOOL;
		p->value.b = b;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_TABLET_SMOOTHING))) {
		p->id = QUIRK_ATTR_TABLET_SMOOTHING;
		if (!parse_boolean_property(value, &b))
			goto out;
		p->type = PT_BOOL;
		p->value.b = b;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_THUMB_PRESSURE_THRESHOLD))) {
		p->id = QUIRK_ATTR_THUMB_PRESSURE_THRESHOLD;
		if (!safe_atou(value, &v))
			goto out;
		p->type = PT_UINT;
		p->value.u = v;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_THUMB_SIZE_THRESHOLD))) {
		p->id = QUIRK_ATTR_THUMB_SIZE_THRESHOLD;
		if (!safe_atou(value, &v))
			goto out;
		p->type = PT_UINT;
		p->value.u = v;
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_MSC_TIMESTAMP))) {
		p->id = QUIRK_ATTR_MSC_TIMESTAMP;
		if (!streq(value, "watch"))
			goto out;
		p->type = PT_STRING;
		p->value.s = safe_strdup(value);
		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_EVENT_CODE))) {
		struct input_event events[32];
		size_t nevents = ARRAY_LENGTH(events);

		p->id = QUIRK_ATTR_EVENT_CODE;

		if (!parse_evcode_property(value, events, &nevents) || nevents == 0)
			goto out;

		for (size_t i = 0; i < nevents; i++) {
			p->value.tuples.tuples[i].first = events[i].type;
			p->value.tuples.tuples[i].second = events[i].code;
			p->value.tuples.tuples[i].third = events[i].value;
		}
		p->value.tuples.ntuples = nevents;
		p->type = PT_TUPLES;

		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_INPUT_PROP))) {
		struct input_prop props[INPUT_PROP_CNT];
		size_t nprops = ARRAY_LENGTH(props);

		p->id = QUIRK_ATTR_INPUT_PROP;

		if (!parse_input_prop_property(value, props, &nprops) || nprops == 0)
			goto out;

		for (size_t i = 0; i < nprops; i++) {
			p->value.tuples.tuples[i].first = props[i].prop;
			p->value.tuples.tuples[i].second = props[i].enabled;
		}

		p->value.tuples.ntuples = nprops;
		p->type = PT_TUPLES;

		rc = true;
	} else if (streq(key, quirk_get_name(QUIRK_ATTR_IS_VIRTUAL))) {
		p->id = QUIRK_ATTR_IS_VIRTUAL;
		if (!parse_boolean_property(value, &b))
			goto out;
		p->type = PT_BOOL;
		p->value.b = b;
		rc = true;
	} else {
		qlog_error(ctx, "Unknown key %s in %s\n", key, s->name);
	}
out:
	if (rc) {
		list_append(&s->properties, &p->link);
		s->has_property = true;
	} else {
		property_cleanup(p);
	}
	return rc;
}

/**
 * Parse a single line, expected to be in the format Key=value. Anything
 * else will be rejected with a failure.
 *
 * Our data files can only have Match, Model and Attr, so let's check for
 * those too.
 */
static bool
parse_value_line(struct quirks_context *ctx, struct section *s, const char *line)
{
	bool rc = false;

	size_t nelem;
	char **strv = strv_from_string(line, "=", &nelem);
	if (!strv || nelem != 2)
		goto out;

	const char *key = strv[0];
	const char *value = strv[1];
	if (strlen(key) == 0 || strlen(value) == 0)
		goto out;

	/* Whatever the value is, it's not supposed to be in quotes */
	if (value[0] == '"' || value[0] == '\'')
		goto out;

	if (strstartswith(key, "Match"))
		rc = parse_match(ctx, s, key, value);
	else if (strstartswith(key, "Model"))
		rc = parse_model(ctx, s, key, value);
	else if (strstartswith(key, "Attr"))
		rc = parse_attr(ctx, s, key, value);
	else
		qlog_error(ctx, "Unknown value prefix %s\n", line);
out:
	strv_free(strv);
	return rc;
}

static inline bool
parse_file(struct quirks_context *ctx, const char *path)
{
	enum state {
		STATE_SECTION,
		STATE_MATCH,
		STATE_MATCH_OR_VALUE,
		STATE_VALUE_OR_SECTION,
		STATE_ANY,
	};
	FILE *fp;
	char line[512];
	bool rc = false;
	enum state state = STATE_SECTION;
	struct section *section = NULL;
	int lineno = -1;

	qlog_debug(ctx, "%s\n", path);

	/* Not using open_restricted here, if we can't access
	 * our own data files, our installation is screwed up.
	 */
	fp = fopen(path, "r");
	if (!fp) {
		/* If the file doesn't exist that's fine. Only way this can
		 * happen is for the custom override file, all others are
		 * provided by scandir so they do exist. Short of races we
		 * don't care about. */
		if (errno == ENOENT)
			return true;

		qlog_error(ctx, "%s: failed to open file\n", path);
		goto out;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *comment;

		lineno++;

		comment = strstr(line, "#");
		if (comment) {
			/* comment points to # but we need to remove the
			 * preceding whitespaces too */
			comment--;
			while (comment >= line) {
				if (*comment != ' ' && *comment != '\t')
					break;
				comment--;
			}
			*(comment + 1) = '\0';
		} else { /* strip the trailing newline */
			comment = strstr(line, "\n");
			if (comment)
				*comment = '\0';
		}
		if (strlen(line) == 0)
			continue;

		/* We don't use quotes for strings, so we really don't want
		 * erroneous trailing whitespaces */
		switch (line[strlen(line) - 1]) {
		case ' ':
		case '\t':
			qlog_parser(ctx,
				    "%s:%d: Trailing whitespace '%s'\n",
				    path,
				    lineno,
				    line);
			goto out;
		}

		switch (line[0]) {
		case '\0':
		case '\n':
		case '#':
			break;
		/* white space not allowed */
		case ' ':
		case '\t':
			qlog_parser(ctx,
				    "%s:%d: Preceding whitespace '%s'\n",
				    path,
				    lineno,
				    line);
			goto out;
		/* section title */
		case '[':
			if (line[strlen(line) - 1] != ']') {
				qlog_parser(ctx,
					    "%s:%d: Closing ] missing '%s'\n",
					    path,
					    lineno,
					    line);
				goto out;
			}

			if (state != STATE_SECTION && state != STATE_VALUE_OR_SECTION) {
				qlog_parser(ctx,
					    "%s:%d: expected section before %s\n",
					    path,
					    lineno,
					    line);
				goto out;
			}
			if (section &&
			    (!section->has_match || !section->has_property)) {
				qlog_parser(ctx,
					    "%s:%d: previous section %s was empty\n",
					    path,
					    lineno,
					    section->name);
				goto out; /* Previous section was empty */
			}

			state = STATE_MATCH;
			section = section_new(path, line);
			list_append(&ctx->sections, &section->link);
			break;
		default:
			/* entries must start with A-Z */
			if (line[0] < 'A' || line[0] > 'Z') {
				qlog_parser(ctx,
					    "%s:%d: Unexpected line %s\n",
					    path,
					    lineno,
					    line);
				goto out;
			}
			switch (state) {
			case STATE_SECTION:
				qlog_parser(ctx,
					    "%s:%d: expected [Section], got %s\n",
					    path,
					    lineno,
					    line);
				goto out;
			case STATE_MATCH:
				if (!strstartswith(line, "Match")) {
					qlog_parser(
						ctx,
						"%s:%d: expected MatchFoo=bar, have %s\n",
						path,
						lineno,
						line);
					goto out;
				}
				state = STATE_MATCH_OR_VALUE;
				break;
			case STATE_MATCH_OR_VALUE:
				if (!strstartswith(line, "Match"))
					state = STATE_VALUE_OR_SECTION;
				break;
			case STATE_VALUE_OR_SECTION:
				if (strstartswith(line, "Match")) {
					qlog_parser(
						ctx,
						"%s:%d: expected value or [Section], have %s\n",
						path,
						lineno,
						line);
					goto out;
				}
				break;
			case STATE_ANY:
				break;
			}

			if (!parse_value_line(ctx, section, line)) {
				qlog_parser(ctx,
					    "%s:%d: failed to parse %s\n",
					    path,
					    lineno,
					    line);
				goto out;
			}
			break;
		}
	}

	if (!section) {
		qlog_parser(ctx, "%s: is an empty file\n", path);
		goto out;
	}

	if (!section->has_match || !section->has_property) {
		qlog_parser(ctx,
			    "%s:%d: previous section %s was empty\n",
			    path,
			    lineno,
			    section->name);
		goto out; /* Previous section was empty */
	}

	rc = true;
out:
	if (fp)
		fclose(fp);

	return rc;
}

static int
is_data_file(const struct dirent *dir)
{
	return strendswith(dir->d_name, ".quirks");
}

static inline bool
parse_files(struct quirks_context *ctx, const char *data_path)
{
	struct dirent **namelist;
	int ndev = -1;
	int idx = 0;

	ndev = scandir(data_path, &namelist, is_data_file, versionsort);
	if (ndev <= 0) {
		qlog_error(ctx, "%s: failed to find data files\n", data_path);
		return false;
	}

	for (idx = 0; idx < ndev; idx++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", data_path, namelist[idx]->d_name);

		if (!parse_file(ctx, path))
			break;
	}

	for (int i = 0; i < ndev; i++)
		free(namelist[i]);
	free(namelist);

	return idx == ndev;
}

struct quirks_context *
quirks_init_subsystem(const char *data_path,
		      const char *override_file,
		      libinput_log_handler log_handler,
		      struct libinput *libinput,
		      enum quirks_log_type log_type)
{
	_unref_(quirks_context) *ctx = zalloc(sizeof *ctx);

	assert(data_path);

	ctx->refcount = 1;
	ctx->log_handler = log_handler;
	ctx->log_type = log_type;
	ctx->libinput = libinput;
	list_init(&ctx->quirks);
	list_init(&ctx->sections);

	qlog_debug(ctx, "%s is data root\n", data_path);

	ctx->dmi = init_dmi();
	ctx->dt = init_dt();
	if (!ctx->dmi && !ctx->dt)
		return NULL;

	if (!parse_files(ctx, data_path))
		return NULL;

	if (override_file && !parse_file(ctx, override_file))
		return NULL;

	return steal(&ctx);
}

struct quirks_context *
quirks_context_ref(struct quirks_context *ctx)
{
	assert(ctx->refcount > 0);
	ctx->refcount++;

	return ctx;
}

struct quirks_context *
quirks_context_unref(struct quirks_context *ctx)
{
	struct section *s;

	if (!ctx)
		return NULL;

	assert(ctx->refcount >= 1);
	ctx->refcount--;

	if (ctx->refcount > 0)
		return NULL;

	/* Caller needs to clean up before calling this */
	assert(list_empty(&ctx->quirks));

	list_for_each_safe(s, &ctx->sections, link) {
		section_destroy(s);
	}

	free(ctx->dmi);
	free(ctx->dt);
	free(ctx);

	return NULL;
}

static struct quirks *
quirks_new(void)
{
	struct quirks *q;

	q = zalloc(sizeof *q);
	q->refcount = 1;
	q->nproperties = 0;
	list_init(&q->link);
	list_init(&q->floating_properties);

	return q;
}

struct quirks *
quirks_unref(struct quirks *q)
{
	if (!q)
		return NULL;

	/* We don't really refcount, but might
	 * as well have the API in place */
	assert(q->refcount == 1);

	for (size_t i = 0; i < q->nproperties; i++) {
		property_unref(q->properties[i]);
	}

	/* Floating properties are owned by our quirks context, need to be
	 * cleaned up here */
	struct property *p;
	list_for_each_safe(p, &q->floating_properties, link) {
		property_cleanup(p);
	}

	list_remove(&q->link);
	free(q->properties);
	free(q);

	return NULL;
}

/**
 * Searches for the udev property on this device and its parent devices.
 *
 * @return the value of the property or NULL
 */
static const char *
udev_prop(struct udev_device *device, const char *prop)
{
	struct udev_device *d = device;
	const char *value = NULL;

	do {
		value = udev_device_get_property_value(d, prop);
		d = udev_device_get_parent(d);
	} while (value == NULL && d != NULL);

	return value;
}

static inline void
match_fill_name(struct match *m, struct udev_device *device)
{
	const char *str = udev_prop(device, "NAME");
	size_t slen;

	if (!str)
		return;

	/* udev NAME is in quotes, strip them */
	if (str[0] == '"')
		str++;

	m->name = safe_strdup(str);
	slen = strlen(m->name);
	if (slen > 1 && m->name[slen - 1] == '"')
		m->name[slen - 1] = '\0';

	m->bits |= M_NAME;
}

static inline void
match_fill_uniq(struct match *m, struct udev_device *device)
{
	const char *str = udev_prop(device, "UNIQ");
	size_t slen;

	if (!str)
		return;

	/* udev uniq is in quotes, strip them */
	if (str[0] == '"')
		str++;

	m->uniq = safe_strdup(str);
	slen = safe_strlen(m->uniq);
	if (slen > 1 && m->uniq[slen - 1] == '"')
		m->uniq[slen - 1] = '\0';

	m->bits |= M_UNIQ;
}

static inline void
match_fill_bus_vid_pid(struct match *m, struct udev_device *device)
{
	const char *str;
	unsigned int product, vendor, bus, version;

	str = udev_prop(device, "PRODUCT");
	if (!str)
		return;

	/* ID_VENDOR_ID/ID_PRODUCT_ID/ID_BUS aren't filled in for virtual
	 * devices so we have to resort to PRODUCT  */
	if (sscanf(str, "%x/%x/%x/%x", &bus, &vendor, &product, &version) != 4)
		return;

	m->product[0] = product;
	m->product[1] = 0;
	m->vendor = vendor;
	m->version = version;
	m->bits |= M_PID | M_VID | M_VERSION;
	switch (bus) {
	case BUS_USB:
		m->bus = BT_USB;
		m->bits |= M_BUS;
		break;
	case BUS_BLUETOOTH:
		m->bus = BT_BLUETOOTH;
		m->bits |= M_BUS;
		break;
	case BUS_I8042:
		m->bus = BT_PS2;
		m->bits |= M_BUS;
		break;
	case BUS_RMI:
		m->bus = BT_RMI;
		m->bits |= M_BUS;
		break;
	case BUS_I2C:
		m->bus = BT_I2C;
		m->bits |= M_BUS;
		break;
	case BUS_SPI:
		m->bus = BT_SPI;
		m->bits |= M_BUS;
		break;
	default:
		break;
	}
}

static inline void
match_fill_udev_type(struct match *m, struct udev_device *device)
{
	struct ut_map {
		const char *prop;
		enum udev_type flag;
	} mappings[] = {
		{ "ID_INPUT_MOUSE", UDEV_MOUSE },
		{ "ID_INPUT_POINTINGSTICK", UDEV_POINTINGSTICK },
		{ "ID_INPUT_TOUCHPAD", UDEV_TOUCHPAD },
		{ "ID_INPUT_TABLET", UDEV_TABLET },
		{ "ID_INPUT_TABLET_PAD", UDEV_TABLET_PAD },
		{ "ID_INPUT_JOYSTICK", UDEV_JOYSTICK },
		{ "ID_INPUT_KEYBOARD", UDEV_KEYBOARD },
		{ "ID_INPUT_KEY", UDEV_KEYBOARD },
	};

	ARRAY_FOR_EACH(mappings, map) {
		if (udev_prop(device, map->prop))
			m->udev_type |= map->flag;
	}
	m->bits |= M_UDEV_TYPE;
}

static inline void
match_fill_dmi_dt(struct match *m, char *dmi, char *dt)
{
	if (dmi) {
		m->dmi = dmi;
		m->bits |= M_DMI;
	}

	if (dt) {
		m->dt = dt;
		m->bits |= M_DT;
	}
}

static struct match *
match_new(struct udev_device *device, char *dmi, char *dt)
{
	struct match *m = zalloc(sizeof *m);

	match_fill_name(m, device);
	match_fill_uniq(m, device);
	match_fill_bus_vid_pid(m, device);
	match_fill_dmi_dt(m, dmi, dt);
	match_fill_udev_type(m, device);
	return m;
}

static void
match_free(struct match *m)
{
	/* dmi and dt are global */
	free(m->name);
	free(m->uniq);
	free(m);
}

DEFINE_FREE_CLEANUP_FUNC(match);

static void
quirk_merge_event_codes(struct quirks_context *ctx,
			struct quirks *q,
			const struct property *property)
{
	for (size_t i = 0; i < q->nproperties; i++) {
		struct property *p = q->properties[i];

		if (p->id != property->id)
			continue;

		/* We have a duplicated property, merge in with ours */
		size_t offset = p->value.tuples.ntuples;
		size_t max = ARRAY_LENGTH(p->value.tuples.tuples);
		for (size_t j = 0; j < property->value.tuples.ntuples; j++) {
			if (offset + j >= max)
				break;
			p->value.tuples.tuples[offset + j] =
				property->value.tuples.tuples[j];
			p->value.tuples.ntuples++;
		}
		return;
	}

	/* First time we add AttrEventCode: create a new property.
	 * Unlike the other properties, this one isn't part of a section, it belongs
	 * to the quirks */
	struct property *newprop = property_new();
	newprop->id = property->id;
	newprop->type = property->type;
	newprop->value.tuples = property->value.tuples;
	/* Caller responsible for pre-allocating space */
	q->properties[q->nproperties++] = property_ref(newprop);
	list_append(&q->floating_properties, &newprop->link);
}

static void
quirk_apply_section(struct quirks_context *ctx,
		    struct quirks *q,
		    const struct section *s)
{
	struct property *p;
	size_t nprops = 0;
	void *tmp;

	list_for_each(p, &s->properties, link) {
		nprops++;
	}

	nprops += q->nproperties;
	tmp = realloc(q->properties, nprops * sizeof(p));
	if (!tmp)
		return;

	q->properties = tmp;
	list_for_each(p, &s->properties, link) {
		qlog_debug(ctx,
			   "property added: %s from %s\n",
			   quirk_get_name(p->id),
			   s->name);

		/* All quirks but AttrEventCode and AttrInputProp
		 * simply overwrite each other, so we can just append the
		 * matching property and, later when checking the quirk, pick
		 * the last one in the array.
		 *
		 * The event codes/input props are special because they're lists
		 * that may *partially* override each other, e.g. a section may
		 * enable BTN_LEFT and BTN_RIGHT but a later section may disable
		 * only BTN_RIGHT. This should result in BTN_LEFT force-enabled
		 * and BTN_RIGHT force-disabled.
		 *
		 * To hack around this, those are the only ones where only ever
		 * have one struct property in the list (not owned by a section)
		 * and we simply merge any extra sections onto that.
		 */
		if (p->id == QUIRK_ATTR_EVENT_CODE || p->id == QUIRK_ATTR_INPUT_PROP)
			quirk_merge_event_codes(ctx, q, p);
		else
			q->properties[q->nproperties++] = property_ref(p);
	}
}

static bool
quirk_match_section(struct quirks_context *ctx,
		    struct quirks *q,
		    struct section *s,
		    struct match *m,
		    struct udev_device *device)
{
	uint32_t matched_flags = 0x0;

	for (uint32_t flag = 0x1; flag <= M_LAST; flag <<= 1) {
		uint32_t prev_matched_flags = matched_flags;
		/* section doesn't have this bit set, continue */
		if ((s->match.bits & flag) == 0)
			continue;

		/* Couldn't fill in this bit for the match, so we
		 * do not match on it */
		if ((m->bits & flag) == 0) {
			qlog_debug(ctx,
				   "%s wants %s but we don't have that\n",
				   s->name,
				   matchflagname(flag));
			continue;
		}

		/* now check the actual matching bit */
		switch (flag) {
		case M_NAME:
			if (fnmatch(s->match.name, m->name, 0) == 0)
				matched_flags |= flag;
			break;
		case M_UNIQ:
			if (fnmatch(s->match.uniq, m->uniq, 0) == 0)
				matched_flags |= flag;
			break;
		case M_BUS:
			if (m->bus == s->match.bus)
				matched_flags |= flag;
			break;
		case M_VID:
			if (m->vendor == s->match.vendor)
				matched_flags |= flag;
			break;
		case M_PID:
			ARRAY_FOR_EACH(m->product, mi) {
				if (*mi == 0 || matched_flags & flag)
					break;

				ARRAY_FOR_EACH(s->match.product, si) {
					if (*si == 0)
						break;
					if (*mi == *si) {
						matched_flags |= flag;
						break;
					}
				}
			}
			break;
		case M_VERSION:
			if (m->version == s->match.version)
				matched_flags |= flag;
			break;
		case M_DMI:
			if (fnmatch(s->match.dmi, m->dmi, 0) == 0)
				matched_flags |= flag;
			break;
		case M_DT:
			if (fnmatch(s->match.dt, m->dt, 0) == 0)
				matched_flags |= flag;
			break;
		case M_UDEV_TYPE:
			if (s->match.udev_type & m->udev_type)
				matched_flags |= flag;
			break;
		default:
			abort();
		}

		if (prev_matched_flags != matched_flags) {
			qlog_debug(ctx,
				   "%s matches for %s\n",
				   s->name,
				   matchflagname(flag));
		}
	}

	if (s->match.bits == matched_flags) {
		qlog_debug(ctx, "%s is full match\n", s->name);
		quirk_apply_section(ctx, q, s);
	}

	return true;
}

struct quirks *
quirks_fetch_for_device(struct quirks_context *ctx, struct udev_device *udev_device)
{
	if (!ctx)
		return NULL;

	qlog_debug(ctx, "%s: fetching quirks\n", udev_device_get_devnode(udev_device));

	_unref_(quirks) *q = quirks_new();
	_free_(match) *m = match_new(udev_device, ctx->dmi, ctx->dt);

	struct section *s;
	list_for_each(s, &ctx->sections, link) {
		quirk_match_section(ctx, q, s, m, udev_device);
	}

	if (q->nproperties == 0) {
		return NULL;
	}

	list_insert(&ctx->quirks, &q->link);

	return steal(&q);
}

static inline struct property *
quirk_find_prop(struct quirks *q, enum quirk which)
{
	/* Run backwards to only handle the last one assigned */
	for (ssize_t i = q->nproperties - 1; i >= 0; i--) {
		struct property *p = q->properties[i];
		if (p->id == which)
			return p;
	}

	return NULL;
}

bool
quirks_has_quirk(struct quirks *q, enum quirk which)
{
	return quirk_find_prop(q, which) != NULL;
}

bool
quirks_get_int32(struct quirks *q, enum quirk which, int32_t *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_INT);
	*val = p->value.i;

	return true;
}

bool
quirks_get_uint32(struct quirks *q, enum quirk which, uint32_t *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_UINT);
	*val = p->value.u;

	return true;
}

bool
quirks_get_double(struct quirks *q, enum quirk which, double *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_DOUBLE);
	*val = p->value.d;

	return true;
}

bool
quirks_get_string(struct quirks *q, enum quirk which, char **val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_STRING);
	*val = p->value.s;

	return true;
}

bool
quirks_get_bool(struct quirks *q, enum quirk which, bool *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_BOOL);
	*val = p->value.b;

	return true;
}

bool
quirks_get_dimensions(struct quirks *q, enum quirk which, struct quirk_dimensions *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_DIMENSION);
	*val = p->value.dim;

	return true;
}

bool
quirks_get_range(struct quirks *q, enum quirk which, struct quirk_range *val)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_RANGE);
	*val = p->value.range;

	return true;
}

bool
quirks_get_tuples(struct quirks *q,
		  enum quirk which,
		  const struct quirk_tuples **tuples)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_TUPLES);
	*tuples = &p->value.tuples;

	return true;
}

bool
quirks_get_uint32_array(struct quirks *q,
			enum quirk which,
			const uint32_t **array,
			size_t *nelements)
{
	struct property *p;

	if (!q)
		return false;

	p = quirk_find_prop(q, which);
	if (!p)
		return false;

	assert(p->type == PT_UINT_ARRAY);
	*array = p->value.array.data.u;
	*nelements = p->value.array.nelements;

	return true;
}
