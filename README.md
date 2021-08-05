thotkeys
========

A "transparent" hotkey implementation for X11 - thotkeys will not consume any
keyboard events and they will be passed down to the focused window intact.

Typically, window managers implement a hotkey feature with GrabKey. A big
problem with the approach is that activating a hotkey grabs the entire keyboard
until the hotkey is released. This makes it impossible to accomplish a common
request: "doing something while a hotkey is held down, while allowing
applications to process other key input events".

thotkeys instead uses the X Input Device Extension to monitor key events
directly from an input device, but not grab it.


Installation
------------

	$ autoreconf -i
	$ ./configure
	$ make
	$ make install


Usage
-----

Monitor events:

	# Use xorg-xinput to find out the device name/id
	$ xinput list
	[...]
	    ↳ Topre REALFORCE 87 US                   	id=11	[slave  keyboard (3)]
	[...]

	$ ./thotkeys --device 'Topre REALFORCE 87 US' --monitor
	# released Return
	--key Control_L # pressed Control_L
	--key Control_L --key c # pressed c
	^C

Register hotkeys:

	$ ./thotkeys --device 'Topre REALFORCE 87 US' \
		--hotkey --key Control_L --key m \
		--on-press 'while :; do echo Ctrl+M is pressed; sleep 0.1; done'

	$ # Registering multiple hotkeys
	$ ./thotkeys --device 'Topre REALFORCE 87 US' \
		--hotkey --key Control_L --key m --on-press \
			'while :; do echo Ctrl+M is pressed; sleep 0.1; done' \
		--hotkey --key F11 --on-press \
			'while :; do echo F11 is pressed; sleep 0.1; done'

The program passed to --on-press is executed on a shell. The process will
receive SIGTERM once the hotkey is released.


Limitations
-----------

 - The hotkey is always global and it is currently not possible to enable or
   disable hotkeys conditionally.

 - The current KeyCode <-> KeySym conversion is probably erroneous. How does it
   behave with a different keyboard layout, or when multiple keyboards are
   connected to the computer?


License
-------

thotkeys is licensed under the MIT license. See also COPYING.
