/*
HOST_INPUT.C

Controllers: the Switch's (Joy-Con pairs, the handheld's attached Joy-Con,
Pro Controllers) as the guest's SDL3 gamepads (port/linux/src/xinput_sdl.c,
sdl_platform.c), through libnx's pad and HID.

Players 1 to 4 are the Switch's controllers 1 to 4; player 1 is also the
handheld's Joy-Con. The buttons are given by position, as an Xbox
controller's (the game's), as SDL gives them: the bottom face button (B on
a Switch controller) is SDL's south (the Xbox's A), the right one (A) east,
the left one (Y) west and the top one (X) north. ZL and ZR are the
triggers, L and R the shoulders, - and + back and start.

A controller that connects or disconnects is announced as SDL's
SDL_EVENT_GAMEPAD_ADDED or _REMOVED (host_sdl_poll_event), on which the
game opens it. Rumble is HD Rumble at two fixed frequencies.

Holding - and + together for a second opens the system's controller
screen (the controller support applet), where players can connect
controllers and take a Joy-Con each, held sideways: such a Joy-Con is a
player of its own, its SL and SR the triggers (fire and grenade), as it has
no ZL or ZR within reach. The game does not see - and + while both are
held.

There are no touch controls yet: the Android touch functions report none.
*/

#include "host.h"

#include <string.h>

#define PORT_COUNT 4
/* the pads are read at most this often (nanoseconds) */
#define UPDATE_INTERVAL 2000000ULL
/* how long - and + are held to open the controller screen (nanoseconds) */
#define CONTROLLER_SCREEN_HOLD 1000000000ULL
#define SCREEN_BUTTONS (HidNpadButton_Minus | HidNpadButton_Plus)

/* SDL3's values (SDL_events.h, SDL_gamepad.h) */
#define SDL_EVENT_GAMEPAD_ADDED 0x653
#define SDL_EVENT_GAMEPAD_REMOVED 0x654
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO 7
#define SDL_GAMEPAD_BUTTON_COUNT 15
#define SDL_GAMEPAD_AXIS_COUNT 6

/* the guest's SDL_GamepadDeviceEvent, in its 128-byte SDL_Event */
struct guest_gamepad_event
{
	uint32_t type;
	uint32_t reserved;
	uint64_t timestamp;
	uint32_t which;
};

struct pad
{
	PadState state;
	HidNpadIdType id;
	/* as last read */
	int connected;
	int announced;
	uint64_t buttons;
	HidAnalogStickState sticks[2];
	/* rumble */
	HidVibrationDeviceHandle vibration[2];
	int vibration_count;
	uint32_t vibration_style;
	uint64_t rumble_until;
	/* since when - and + are held together (0: they are not) */
	uint64_t screen_held_since;
	int screen_opened;
};

/* SDL's buttons, in their order, as HidNpadButton masks */
static const uint64_t button_masks[SDL_GAMEPAD_BUTTON_COUNT] =
{
	HidNpadButton_B,      /* south */
	HidNpadButton_A,      /* east */
	HidNpadButton_Y,      /* west */
	HidNpadButton_X,      /* north */
	HidNpadButton_Minus,  /* back */
	0,                    /* guide: HOME is the system's */
	HidNpadButton_Plus,   /* start */
	HidNpadButton_StickL,
	HidNpadButton_StickR,
	HidNpadButton_L,
	HidNpadButton_R,
	HidNpadButton_Up,
	HidNpadButton_Down,
	HidNpadButton_Left,
	HidNpadButton_Right,
};

static struct pad pads[PORT_COUNT];
static Mutex input_lock;
static uint64_t last_update;
static int input_ready;
static volatile int controller_screen_wanted;

static void input_start(void)
{
	int index;

	if (input_ready)
		return;
	padConfigureInput(PORT_COUNT, HidNpadStyleSet_NpadStandard);
	/* (a Joy-Con taken on its own is held sideways) */
	hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
	for (index = 0; index < PORT_COUNT; index++)
	{
		pads[index].id = (HidNpadIdType)(HidNpadIdType_No1 + index);
		if (index == 0)
			padInitialize(&pads[index].state, HidNpadIdType_No1, HidNpadIdType_Handheld);
		else
			padInitialize(&pads[index].state, pads[index].id);
	}
	input_ready = 1;
}

/* the guest's handle for a port: its SDL joystick id, 1 to 4 */
static struct pad *pad_of(uint32_t handle)
{
	return handle >= 1 && handle <= PORT_COUNT ? &pads[handle - 1] : NULL;
}

/* the vibration devices of the pad's current style (two Joy-Con, or one
controller) */
static void rumble_prepare(struct pad *pad)
{
	uint32_t style = padGetStyleSet(&pad->state);
	HidNpadIdType id = pad->id;

	if (style == pad->vibration_style)
		return;
	pad->vibration_style = style;
	pad->vibration_count = 0;
	if (style & HidNpadStyleTag_NpadHandheld)
	{
		id = HidNpadIdType_Handheld;
		style = HidNpadStyleTag_NpadHandheld;
	}
	else if (style & HidNpadStyleTag_NpadJoyDual)
	{
		style = HidNpadStyleTag_NpadJoyDual;
	}
	else if (style & HidNpadStyleTag_NpadFullKey)
	{
		style = HidNpadStyleTag_NpadFullKey;
	}
	else
	{
		return;
	}
	pad->vibration_count = style == HidNpadStyleTag_NpadFullKey ? 1 : 2;
	if (R_FAILED(hidInitializeVibrationDevices(pad->vibration, pad->vibration_count, id, style)))
		pad->vibration_count = 0;
}

static void rumble_send(struct pad *pad, float low, float high)
{
	HidVibrationValue values[2];
	int index;

	rumble_prepare(pad);
	if (!pad->vibration_count)
		return;
	for (index = 0; index < 2; index++)
	{
		values[index].amp_low = low;
		values[index].freq_low = 160.0f;
		values[index].amp_high = high;
		values[index].freq_high = 320.0f;
	}
	hidSendVibrationValues(pad->vibration, values, pad->vibration_count);
}

/* reads the pads (at most every UPDATE_INTERVAL); input_lock is held */
static void pads_update(void)
{
	uint64_t now = host_now();
	int index;

	if (now - last_update < UPDATE_INTERVAL)
		return;
	last_update = now;
	for (index = 0; index < PORT_COUNT; index++)
	{
		struct pad *pad = &pads[index];

		padUpdate(&pad->state);
		pad->connected = padIsConnected(&pad->state);
		pad->buttons = pad->connected ? padGetButtons(&pad->state) : 0;
		/* a single Joy-Con's SL and SR as the triggers */
		if (padGetStyleSet(&pad->state) & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))
		{
			if (pad->buttons & HidNpadButton_AnySR)
				pad->buttons |= HidNpadButton_ZR;
			if (pad->buttons & HidNpadButton_AnySL)
				pad->buttons |= HidNpadButton_ZL;
		}
		/* - and + held together: the controller screen, once a hold */
		if ((pad->buttons & SCREEN_BUTTONS) == SCREEN_BUTTONS)
		{
			if (!pad->screen_held_since)
				pad->screen_held_since = now;
			else if (!pad->screen_opened && now - pad->screen_held_since >= CONTROLLER_SCREEN_HOLD)
			{
				pad->screen_opened = 1;
				controller_screen_wanted = 1;
			}
			pad->buttons &= ~(uint64_t)SCREEN_BUTTONS;
		}
		else
		{
			pad->screen_held_since = 0;
			pad->screen_opened = 0;
		}
		pad->sticks[0] = padGetStickPos(&pad->state, 0);
		pad->sticks[1] = padGetStickPos(&pad->state, 1);
		if (pad->rumble_until && now >= pad->rumble_until)
		{
			pad->rumble_until = 0;
			rumble_send(pad, 0.0f, 0.0f);
		}
	}
}

/* the system's controller screen (the game waits meanwhile): up to four
players, single Joy-Cons allowed */
static void controller_screen(void)
{
	HidLaControllerSupportArg arg;
	HidLaControllerSupportResultInfo result;
	Result rc;

	hidLaCreateControllerSupportArg(&arg);
	arg.hdr.player_count_min = 0;
	arg.hdr.player_count_max = PORT_COUNT;
	arg.hdr.enable_take_over_connection = 1;
	arg.hdr.enable_permit_joy_dual = 1;
	memset(&result, 0, sizeof(result));
	host_logf(HOST_LOG_INFO, "controllers: the system's screen");
	rc = hidLaShowControllerSupport(&result, &arg);
	if (R_FAILED(rc))
		host_logf(HOST_LOG_WARN, "controllers: the screen could not open: 0x%x", rc);
	else
		host_logf(HOST_LOG_INFO, "controllers: %d players", result.player_count);
}

/* ---------- the guest's SDL events and gamepads */

/* the next event: a controller connected or gone, else none (0) */
int host_input_poll_event(void *event)
{
	struct guest_gamepad_event *gamepad_event = event;
	int index;

	if (controller_screen_wanted)
	{
		controller_screen_wanted = 0;
		controller_screen();
	}
	mutexLock(&input_lock);
	input_start();
	pads_update();
	for (index = 0; index < PORT_COUNT; index++)
	{
		struct pad *pad = &pads[index];

		if (pad->connected != pad->announced)
		{
			pad->announced = pad->connected;
			memset(event, 0, 128);
			gamepad_event->type = pad->connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
			gamepad_event->timestamp = host_now();
			gamepad_event->which = (uint32_t)index + 1;
			mutexUnlock(&input_lock);
			host_logf(HOST_LOG_INFO, "controller %d %s", index + 1, pad->connected ? "connected" : "disconnected");
			return 1;
		}
	}
	mutexUnlock(&input_lock);
	return 0;
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;

	mutexLock(&input_lock);
	input_start();
	pads_update();
	for (index = 0; index < PORT_COUNT && count < capacity; index++)
	{
		if (pads[index].announced)
			ids[count++] = (uint32_t)index + 1;
	}
	mutexUnlock(&input_lock);
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	struct pad *pad = pad_of(id);

	return pad && pad->announced ? id : 0;
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return host_sdl_open_gamepad(id);
}

void host_sdl_close_gamepad(uint32_t gamepad)
{
	(void)gamepad;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	(void)gamepad;
	return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	struct pad *pad = pad_of(gamepad);
	int pressed;

	if (!pad || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT || !button_masks[button])
		return 0;
	mutexLock(&input_lock);
	pads_update();
	pressed = (pad->buttons & button_masks[button]) != 0;
	mutexUnlock(&input_lock);
	return pressed;
}

static int axis_value(int value)
{
	return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}

/* SDL's axes: sticks -32768 to 32767, down and right positive; triggers 0
to 32767 (the Switch's are buttons) */
int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	struct pad *pad = pad_of(gamepad);
	int value = 0;

	if (!pad || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
		return 0;
	mutexLock(&input_lock);
	pads_update();
	switch (axis)
	{
	case 0: value = pad->sticks[0].x; break;
	case 1: value = -pad->sticks[0].y; break;
	case 2: value = pad->sticks[1].x; break;
	case 3: value = -pad->sticks[1].y; break;
	case 4: value = (pad->buttons & HidNpadButton_ZL) ? 32767 : 0; break;
	case 5: value = (pad->buttons & HidNpadButton_ZR) ? 32767 : 0; break;
	}
	mutexUnlock(&input_lock);
	return axis_value(value);
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	struct pad *pad = pad_of(gamepad);

	if (!pad)
		return 0;
	mutexLock(&input_lock);
	rumble_send(pad, (float)(low & 0xffff) / 65535.0f, (float)(high & 0xffff) / 65535.0f);
	pad->rumble_until = (low || high) ? host_now() + (uint64_t)milliseconds * 1000000ULL : 0;
	mutexUnlock(&input_lock);
	return 1;
}

/* ---------- the keyboard and the clipboard: none */

void host_sdl_scancode_name(int scancode, char *buffer, uint32_t size)
{
	(void)scancode;
	if (size)
		buffer[0] = 0;
}

int host_sdl_scancode_from_name(const char *name)
{
	(void)name;
	return 0;
}

int host_sdl_set_clipboard_text(const char *text)
{
	(void)text;
	return 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	if (size)
		buffer[0] = 0;
}

/* ---------- the Android touch controls: none */

void host_gesture_insets(int *insets)
{
	memset(insets, 0, 4 * sizeof(int));
}

void host_touch_read(int32_t *state)
{
	memset(state, 0, 7 * sizeof(int32_t));
}

void host_touch_look_read(float *delta)
{
	memset(delta, 0, 4 * sizeof(float));
}

void host_touch_rumble(unsigned int low, unsigned int high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	(void)scene;
}

void host_touch_bindings(const int32_t *controls)
{
	(void)controls;
}

const struct host_import host_input_imports[] =
{
	{"host_sdl_get_gamepads", (void *)host_sdl_get_gamepads},
	{"host_sdl_open_gamepad", (void *)host_sdl_open_gamepad},
	{"host_sdl_gamepad_from_id", (void *)host_sdl_gamepad_from_id},
	{"host_sdl_close_gamepad", (void *)host_sdl_close_gamepad},
	{"host_sdl_gamepad_type", (void *)host_sdl_gamepad_type},
	{"host_sdl_gamepad_button", (void *)host_sdl_gamepad_button},
	{"host_sdl_gamepad_axis", (void *)host_sdl_gamepad_axis},
	{"host_sdl_rumble_gamepad", (void *)host_sdl_rumble_gamepad},
	{"host_sdl_scancode_name", (void *)host_sdl_scancode_name},
	{"host_sdl_scancode_from_name", (void *)host_sdl_scancode_from_name},
	{"host_sdl_set_clipboard_text", (void *)host_sdl_set_clipboard_text},
	{"host_sdl_get_clipboard_text", (void *)host_sdl_get_clipboard_text},
	{"host_gesture_insets", (void *)host_gesture_insets},
	{"host_touch_read", (void *)host_touch_read},
	{"host_touch_look_read", (void *)host_touch_look_read},
	{"host_touch_rumble", (void *)host_touch_rumble},
	{"host_touch_scene", (void *)host_touch_scene},
	{"host_touch_bindings", (void *)host_touch_bindings},
	{NULL, NULL}
};
