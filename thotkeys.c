#include "config.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>

static int VERBOSE = 0;

#define debug(...) do { \
	if (VERBOSE) \
		fprintf(stderr, "debug: " __VA_ARGS__); \
} while (0)

#define warn(...) do { \
	fprintf(stderr, "warning: " __VA_ARGS__); \
} while (0)

#define fatal(...) do { \
	fprintf(stderr, "fatal: " __VA_ARGS__); \
	exit(1); \
} while (0)

static inline void *xrealloc(void *o, size_t size)
{
	void *p = realloc(o, size);
	if (!p)
		fatal("realloc failed\n");
	return p;
}

static inline void *xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (!p)
		fatal("calloc failed\n");
	return p;
}

struct hotkey_map {
	char keys[256];
	char buttons[256];
};

struct hotkey_config {
	const char **keystrs;
	size_t numkeystrs;
	const char **buttonstrs;
	size_t numbuttonstrs;
	const char *on_press;

	struct hotkey_map keymap;
	struct hotkey_map checkmap;
	bool activated;
	pid_t pid;
};

static Display *get_display(void)
{
	Display *display = XOpenDisplay(NULL);
	if (!display)
		fatal("XOpenDisplay() failed\n");
	return display;
}

static XIDeviceInfo *get_device_info(Display *display, const char *name)
{
	bool use_id = true;
	long id;

	errno = 0;
	char *endp;
	id = strtol(name, &endp, 10);
	if (errno == ERANGE || id == 0 && endp != name + strlen(name))
		use_id = false;

	int num_devices;
	XIDeviceInfo *devices = XIQueryDevice(display, XIAllDevices, &num_devices);

	XIDeviceInfo *found = NULL;
	for (int i = 0; i < num_devices; i++) {
		XIDeviceInfo *device = &devices[i];

		if (device->use != XISlaveKeyboard)
			continue;
		if (!strcmp(device->name, name) || use_id && (long)device->deviceid == id) {
			if (found)
				fatal("more than one keyboard found with the " \
				      "name '%s'\n", name);
			found = device;
		}
	}
	return found;
}

static void prepare_monitor(Display *display, const char *device_name)
{
	XIDeviceInfo *info = NULL;
	if (device_name) {
		info = get_device_info(display, device_name);
		if (!info)
			fatal("unable to find device '%s'\n", device_name);
	}

	XIEventMask mask;
	mask.deviceid = info ? info->deviceid : XIAllMasterDevices;
	mask.mask_len = XIMaskLen(XI_LASTEVENT);
	mask.mask = xcalloc((size_t)mask.mask_len, 1);
	XISetMask(mask.mask, XI_RawKeyPress);
	XISetMask(mask.mask, XI_RawKeyRelease);
	XISetMask(mask.mask, XI_RawButtonPress);
	XISetMask(mask.mask, XI_RawButtonRelease);

	if (XISelectEvents(display,  DefaultRootWindow(display), &mask, 1))
		fatal("XISelectEvents() failed\n");
	XSync(display, False);
	free(mask.mask);
}

static const XIRawEvent *process_event(Display *display, int *evtype)
{
	static XEvent ev;
	XGenericEventCookie *cookie = &ev.xcookie;

	static int xi_opcode;
	if (!xi_opcode) {
		int event, error;
		if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
			fatal("X Input extension not available\n");
	}

	while (1) {
		XNextEvent(display, &ev);
		if (!XGetEventData(display, cookie) ||
		    cookie->type != GenericEvent ||
		    cookie->extension != xi_opcode)
			continue;

		switch (cookie->evtype) {
		case XI_RawKeyPress:
		case XI_RawKeyRelease:
		case XI_RawButtonPress:
		case XI_RawButtonRelease:
			*evtype = cookie->evtype;
			return cookie->data;
		}
	}
}

static void command_help(void)
{
	fprintf(stderr, "%s\n", PACKAGE_STRING);
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  thotkeys --help\n");
	fprintf(stderr, "    Show this message.\n");
	fprintf(stderr, "  thotkeys --monitor\n");
	fprintf(stderr, "    Print key and button events to stdout.\n");
	fprintf(stderr, "  thotkeys --hotkey [--key <keysym>] [--button <num>] --on-press <on-press>\n");
	fprintf(stderr, "    Register a hotkey. See also 'Hotkey options' section.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --device <device>\n");
	fprintf(stderr, "    Monitor events from the specified device only.\n");
	fprintf(stderr, "    <device> may be either the device name or the number. Check 'xinput list'.\n");
	fprintf(stderr, "    [TODO: Support for mouse and multiple keyboard devices]\n");
	fprintf(stderr, "  --verbose\n");
	fprintf(stderr, "    Enable debugging output.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Hotkey options:\n");
	fprintf(stderr, "  --key <keysym>\n");
	fprintf(stderr, "    Specify a key. Use --monitor to see the appropriate keysym string.\n");
	fprintf(stderr, "  --button <num>\n");
	fprintf(stderr, "    Specify a button by the button number.\n");
	fprintf(stderr, "  --on-press <on-press>\n");
	fprintf(stderr, "    Execute <on-press> on '/bin/sh -c' when all specified keys and buttons\n");
	fprintf(stderr, "    are pressed at the same time.\n");
	fprintf(stderr, "    SIGTERM will be sent to the process when the condition is no longer met.\n");
	exit(0);
}

static void command_monitor(const char *device_name)
{
	Display *display = get_display();
	prepare_monitor(display, device_name);

	struct hotkey_map keymap = { 0 };
	while (1) {
		int evtype;
		const XIRawEvent *data = process_event(display, &evtype);
		bool pressed;
		char comment[256];

		switch (evtype) {
		case XI_RawKeyPress:
		case XI_RawKeyRelease:
			if (data->detail > 255)
				fatal("unexpected keycode %d\n", data->detail);
			pressed = evtype == XI_RawKeyPress;
			keymap.keys[data->detail] = pressed;

			KeySym basekeysym = XkbKeycodeToKeysym(display, (KeyCode)data->detail, 0, 0);
			snprintf(comment, sizeof(comment), "# %s key %s",
				 pressed ? "pressed" : "released",
				 XKeysymToString(basekeysym));
			break;
		case XI_RawButtonPress:
		case XI_RawButtonRelease:
			if (data->detail > 255)
				fatal("unexpected button number %d\n", data->detail);
			pressed = evtype == XI_RawButtonPress;
			keymap.buttons[data->detail] = pressed;

			snprintf(comment, sizeof(comment), "# %s button %d",
				 pressed ? "pressed" : "released",
				 data->detail);
			break;
		default:
			fatal("unreachable\n");
		}

		for (int i = 0; i < 256; i++) {
			if (keymap.keys[i]) {
				KeySym keysym = XkbKeycodeToKeysym(display, (KeyCode)i, 0, 0);
				printf("--key %s ", XKeysymToString(keysym));
			}
		}
		for (int i = 0; i < 256; i++) {
			if (keymap.buttons[i])
				printf("--button %d ", i);
		}
		printf("%s\n", comment);
	}
}

static void command_hotkeys(const char *device_name, struct hotkey_config *hotkeys,
			    size_t numhotkeys)
{
	Display *display = get_display();
	prepare_monitor(display, device_name);

	for (size_t i = 0; i < numhotkeys; i++) {
		struct hotkey_config *c = hotkeys + i;
		memset(&c->keymap, 0, sizeof(c->keymap));
		memset(&c->checkmap, 0, sizeof(c->checkmap));
		c->activated = false;
		c->pid = -1;

		for (size_t j = 0; j < c->numkeystrs; j++) {
			const char *str = c->keystrs[j];
			KeySym keysym = XStringToKeysym(str);
			if (keysym == NoSymbol)
				fatal("--key %s could not be recognized\n", str);
			KeyCode keycode = XKeysymToKeycode(display, keysym);
			if (keycode == 0)
				fatal("--key %s could not be converted into keycode\n", str);
			c->checkmap.keys[keycode] = 1;
		}
		for (size_t j = 0; j < c->numbuttonstrs; j++) {
			const char *str = c->buttonstrs[j];
			long num = strtol(str, NULL, 10);
			if (num < 1 || num > 255)
				fatal("--button %s could not be recognized\n", str);
			c->checkmap.buttons[num] = 1;
		}
	}

	while (1) {
		// Reap child processes
		pid_t pid;
		int status;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
			debug("reaped child process %d\n", pid);
			for (size_t i = 0; i < numhotkeys; i++) {
				struct hotkey_config *c = hotkeys + i;
				if (c->pid == pid) {
					c->pid = -1;
					break;
				}
			}
		}

		int evtype;
		const XIRawEvent *data = process_event(display, &evtype);
		bool pressed;
		ptrdiff_t offset;

		switch (evtype) {
		case XI_RawKeyPress:
		case XI_RawKeyRelease:
			if (data->detail > 255)
				fatal("unexpected keycode %d\n", data->detail);
			pressed = evtype == XI_RawKeyPress;
			offset = offsetof(struct hotkey_map, keys) + (size_t)data->detail;
			break;
		case XI_RawButtonPress:
		case XI_RawButtonRelease:
			if (data->detail > 255)
				fatal("unexpected button number %d\n", data->detail);
			pressed = evtype == XI_RawButtonPress;
			offset = offsetof(struct hotkey_map, buttons) + (size_t)data->detail;
			break;
		default:
			fatal("unreachable\n");
		}

		for (size_t i = 0; i < numhotkeys; i++) {
			struct hotkey_config *c = hotkeys + i;
			if (!*((char *)&c->checkmap + offset))
				continue;

			*((char *)&c->keymap + offset) = pressed;
			bool matched = !memcmp(&c->checkmap, &c->keymap, sizeof(c->checkmap));

			if (!c->activated && matched) {
				if (c->pid != -1)
					warn("program '%s' is still running with pid %d\n",
					     c->on_press, c->pid);
				debug("spawning process %s\n", c->on_press);
				if (!(c->pid = fork())) {
					execl("/bin/sh", "sh", "-c", c->on_press, NULL);
					exit(0);
				}
			}
			else if (c->activated && !matched) {
				if (c->pid != -1) {
					debug("sending SIGTERM to process %d\n", c->pid);
					kill(c->pid, SIGTERM);
				}
			}
			c->activated = matched;
		}
	}
}

int main(int argc, char **argv)
{
	const char *device_name = NULL;
	bool do_help = false, do_monitor = false, do_hotkeys = false;
	size_t numhotkeys = 0, numkeys = 0, numbuttons = 0;
	struct hotkey_config *hotkeys = NULL;
	const char **keys = NULL, **buttons = NULL, *on_press = NULL;

	while (1) {
		static struct option long_options[] = {
			{ "verbose",  no_argument,       0, 'V' },
			{ "version",  no_argument,       0, 'H' },
			{ "help",     no_argument,       0, 'H' },
			{ "monitor",  no_argument,       0, 'M' },
			{ "hotkey",   no_argument,       0, 'K' },

			{ "device",   required_argument, 0, 'd' },
			{ "key",      required_argument, 0, 'k' },
			{ "button",   required_argument, 0, 'b' },
			{ "on-press", required_argument, 0, 'p' },
			{ 0 }
		};

		int c = getopt_long(argc, argv, "", long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'V':
			VERBOSE = 1;
			break;
		case 'H':
			do_help = true;
			break;
		case 'M':
			do_monitor = true;
			break;
		case 'K':
			if (do_hotkeys) {
				if ((!keys && !buttons) || !on_press)
					fatal("--key and --on-press options are required\n");
				hotkeys = xrealloc(hotkeys, sizeof(*hotkeys) * (numhotkeys + 1));
				hotkeys[numhotkeys++] = (struct hotkey_config) {
					.keystrs = keys,
					.numkeystrs = numkeys,
					.buttonstrs = buttons,
					.numbuttonstrs = numbuttons,
					.on_press = on_press,
				};
				keys = NULL;
				numkeys = 0;
				numbuttons = 0;
				on_press = NULL;
			}
			do_hotkeys = true;
			break;
		case 'd':
			device_name = optarg; break;
		case 'k':
			keys = xrealloc(keys, sizeof(*keys) * (numkeys + 1));
			keys[numkeys++] = optarg;
			break;
		case 'b':
			buttons = xrealloc(buttons, sizeof(*buttons) * (numbuttons + 1));
			buttons[numbuttons++] = optarg;
			break;
		case 'p':
			on_press = optarg; break;
		case '?':
			exit(1);
		default:
			fatal("[BUG] unknown option '%c'\n", c);
		}
	}
	if (do_hotkeys) {
		if ((!keys && !buttons) || !on_press)
			fatal("--key and --on-press options are required\n");
		hotkeys = xrealloc(hotkeys, sizeof(*hotkeys) * (numhotkeys + 1));
		hotkeys[numhotkeys++] = (struct hotkey_config) {
			.keystrs = keys,
			.numkeystrs = numkeys,
			.buttonstrs = buttons,
			.numbuttonstrs = numbuttons,
			.on_press = on_press,
		};
	}
	if (optind != argc)
		fatal("unknown argument %s\n", argv[optind]);

	if (do_help)
		command_help();
	if (do_monitor)
		command_monitor(device_name);
	if (do_hotkeys)
		command_hotkeys(device_name, hotkeys, numhotkeys);
}
