#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput.h>

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

static Display *get_display(void)
{
	Display *display = XOpenDisplay(NULL);
	if (!display)
		fatal("XOpenDisplay() failed\n");
	return display;
}

static XDeviceInfo *get_device_info(Display *display, const char *name)
{
	bool use_id = true;
	long id;

	errno = 0;
	char *endp;
	id = strtol(name, &endp, 10);
	if (errno == ERANGE || id == 0 && endp != name + strlen(name))
		use_id = false;

	int num_devices;
	XDeviceInfo *devices = XListInputDevices(display, &num_devices);

	XDeviceInfo *found = NULL;
	for (int i = 0; i < num_devices; i++) {
		XDeviceInfo *device = &devices[i];

		if (device->use != IsXExtensionKeyboard)
			continue;
		if (!strcmp(device->name, name) || use_id && (long)device->id == id) {
			if (found)
				fatal("more than one keyboard found with the " \
				      "name '%s'\n", name);
			found = device;
		}
	}
	return found;
}

static XID key_press_type, key_release_type;
static bool is_key_press_event(const XDeviceKeyEvent *ev)
{
	return ev->type == (int)key_press_type;
}

static void register_events(Display *display, XDevice *device)
{
	int screen = DefaultScreen(display);
	Window root_win = RootWindow(display, screen);

	XEventClass event_list[2];
	DeviceKeyPress(device, key_press_type, event_list[0]);
	DeviceKeyRelease(device, key_release_type, event_list[1]);
	if (XSelectExtensionEvent(display, root_win, event_list, 2))
		fatal("XSelectExtensionEvent() failed\n");
}

static void prepare_monitor(Display *display, const char *device_name)
{
	XDeviceInfo *info = get_device_info(display, device_name);
	if (!info)
		fatal("unable to find device '%s'\n", device_name);

	XDevice *device = XOpenDevice(display, info->id);
	if (!device)
		fatal("unable to open device '%s'\n", device_name);

	register_events(display, device);
}


static XEvent process_event_ev;
static const XDeviceKeyEvent *process_event(Display *display)
{
redo:
	XNextEvent(display, &process_event_ev);

	if (process_event_ev.type == (int)key_press_type)
		return (XDeviceKeyEvent *)&process_event_ev;
	if (process_event_ev.type == (int)key_release_type) {
		// Retriggered events
		if (XEventsQueued(display, QueuedAfterReading)) {
			XEvent nev;
			XPeekEvent(display, &nev);

			if (nev.type == (int)key_press_type &&
			    nev.xkey.time == process_event_ev.xkey.time &&
			    nev.xkey.keycode == process_event_ev.xkey.keycode) {
				// Consume the following KeyPress
				XNextEvent(display, &nev);
				goto redo;
			}
		}
		return (XDeviceKeyEvent *)&process_event_ev;
	}

	debug("ignoring event %d\n", process_event_ev.type);
	goto redo;
}

static void command_help(void)
{
	fprintf(stderr, "%s\n", PACKAGE_STRING);
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  thotkeys --help\n");
	fprintf(stderr, "    Show this message\n");
	fprintf(stderr, "  thotkeys --device <device> --monitor\n");
	fprintf(stderr, "    Print key press and release events to stdout\n");
	fprintf(stderr, "  thotkeys --device <device> --hotkey --key <key> " \
		"[--key <key>...] --on-press <on-press> [--hotkey ...]\n");
	fprintf(stderr, "    Run <on-press> when <key> is pressed down. " \
		"SIGTERM will be sent to the process when the hotkey is\n" \
		"    released. --hotkey and the following options may be repeated.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --verbose\n");
	fprintf(stderr, "    Enable debugging output\n");
	exit(0);
}

static void command_monitor(const char *device_name)
{
	Display *display = get_display();
	prepare_monitor(display, device_name);

	char keymap[256] = { 0 };
	while (1) {
		const XDeviceKeyEvent *ev = process_event(display);
		bool pressed = is_key_press_event(ev);

		if (ev->keycode > 255)
			fatal("unexpected keycode %d\n", (int)ev->keycode);
		keymap[ev->keycode] = pressed;

		for (int i = 0; i < 256; i++) {
			if (keymap[i]) {
				KeySym keysym = XkbKeycodeToKeysym(display, (KeyCode)i, 0, 0);
				printf("--key %s ", XKeysymToString(keysym));
			}
		}
		KeySym basekeysym = XkbKeycodeToKeysym(display, (KeyCode)ev->keycode, 0, 0);
		printf("# %s %s\n",
		       pressed ? "pressed" : "released",
		       XKeysymToString(basekeysym));
	}
}

struct hotkey_config {
	const char **keystrs;
	size_t numkeystrs;
	const char *on_press;

	char keymap[256];
	char checkmap[256];
	bool activated;
	pid_t pid;
};

static void command_hotkeys(const char *device_name, struct hotkey_config *hotkeys,
			    size_t numhotkeys)
{
	Display *display = get_display();
	prepare_monitor(display, device_name);

	for (size_t i = 0; i < numhotkeys; i++) {
		struct hotkey_config *c = hotkeys + i;
		memset(c->keymap, 0, sizeof(c->keymap));
		memset(c->checkmap, 0, sizeof(c->checkmap));
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
			c->checkmap[keycode] = 1;
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

		const XDeviceKeyEvent *ev = process_event(display);
		bool pressed = is_key_press_event(ev);

		if (ev->keycode > 255)
			fatal("unexpected keycode %d\n", (int)ev->keycode);

		for (size_t i = 0; i < numhotkeys; i++) {
			struct hotkey_config *c = hotkeys + i;
			if (!c->checkmap[ev->keycode])
				continue;

			c->keymap[ev->keycode] = pressed;
			bool matched = !memcmp(c->checkmap, c->keymap, sizeof(c->checkmap));

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
	size_t numhotkeys = 0, numkeys = 0;
	struct hotkey_config *hotkeys = NULL;
	const char **keys = NULL, *on_press = NULL;

	while (1) {
		static struct option long_options[] = {
			{ "verbose",  no_argument,       0, 'V' },
			{ "version",  no_argument,       0, 'H' },
			{ "help",     no_argument,       0, 'H' },
			{ "monitor",  no_argument,       0, 'M' },
			{ "hotkey",   no_argument,       0, 'K' },

			{ "device",   required_argument, 0, 'd' },
			{ "key",      required_argument, 0, 'k' },
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
				if (!keys || !on_press)
					fatal("--key and --on-press options are required\n");
				hotkeys = xrealloc(hotkeys, sizeof(*hotkeys) * (numhotkeys + 1));
				hotkeys[numhotkeys++] = (struct hotkey_config) {
					.keystrs = keys,
						.numkeystrs = numkeys,
						.on_press = on_press,
				};
				keys = NULL;
				numkeys = 0;
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
		case 'p':
			on_press = optarg; break;
		case '?':
			exit(1);
		default:
			fatal("[BUG] unknown option '%c'\n", c);
		}
	}
	if (do_hotkeys) {
		if (!keys || !on_press)
			fatal("--key and --on-press options are required\n");
		hotkeys = xrealloc(hotkeys, sizeof(*hotkeys) * (numhotkeys + 1));
		hotkeys[numhotkeys++] = (struct hotkey_config) {
			.keystrs = keys,
				.numkeystrs = numkeys,
				.on_press = on_press,
		};
	}
	if (do_monitor || do_hotkeys) {
		if (!device_name)
			fatal("--device option is required\n");
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
