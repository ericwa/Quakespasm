/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

static qboolean	textmode;

static cvar_t in_debugkeys = {"in_debugkeys", "0", CVAR_NONE};

#ifdef __APPLE__
/* Mouse acceleration needs to be disabled on OS X */
#define MACOS_X_ACCELERATION_HACK
#endif

#ifdef MACOS_X_ACCELERATION_HACK
#include <IOKit/IOTypes.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/event_status_driver.h>
#endif

/* analog axis ease math functions */
#define sine(x)      ((0.5f) * ( (1) - (cosf( (x) * M_PI )) ))
#define quadratic(x) ((x) * (x))
#define cubic(x)     ((x) * (x) * (x))
#define quartic(x)   ((x) * (x) * (x) * (x))
#define quintic(x)   ((x) * (x) * (x) * (x) * (x))

/* dual axis utility macro */
#define dualfunc(d,f) {        \
    d.left.x  = d.left.x < 0 ? -f( (float)-d.left.x ) : f( (float)d.left.x );  \
    d.left.y  = d.left.y < 0 ? -f( (float)-d.left.y ) : f( (float)d.left.y );  \
    d.right.x  = d.right.x < 0 ? -f( (float)-d.right.x ) : f( (float)d.right.x );  \
    d.right.y  = d.right.y < 0 ? -f( (float)-d.right.y ) : f( (float)d.right.y );  }

typedef struct
{
	float x;
	float y;
} joyAxis_t;

typedef struct
{
	joyAxis_t	_oldleft;
	joyAxis_t	_oldright;
	joyAxis_t	left;		/* TODO: assumed move, rename */
	joyAxis_t	right;		/* TODO: assumed look, rename? */
} dualAxis_t;

/* -1 is mentioned as the invalid instance ID in SDL_joystick.h */
static SDL_JoystickID joy_active_instaceid = -1;
static SDL_GameController *joy_active_controller = NULL;

static int buttonremap[] =
{
	K_MOUSE1,
	K_MOUSE3,	/* right button		*/
	K_MOUSE2,	/* middle button	*/
#if !defined(USE_SDL2)	/* mousewheel up/down not counted as buttons in SDL2 */
	K_MWHEELUP,
	K_MWHEELDOWN,
#endif
	K_MOUSE4,
	K_MOUSE5
};

static int joyremap[] =
{
	K_JOY1,
	K_JOY2,
	K_JOY3,
	K_JOY4,
	K_AUX1,
	K_AUX2,
	K_AUX3,
	K_AUX4,
	K_AUX5,
	K_AUX6,
	K_AUX7,
	K_AUX8,
	K_AUX9,
	K_AUX10,
	K_AUX11,
	K_AUX12,
	K_AUX13,
	K_AUX14,
	K_AUX15,
	K_AUX16,
	K_AUX17,
	K_AUX18,
	K_AUX19,
	K_AUX20,
	K_AUX21,
	K_AUX22,
	K_AUX23,
	K_AUX24,
	K_AUX25,
	K_AUX26,
	K_AUX27,
	K_AUX28
};

static qboolean	no_mouse = false;
static dualAxis_t _rawDualAxis = {0};

/* total accumulated mouse movement since last frame */
static int	total_dx, total_dy = 0;

/* deadzones from XInput documentation */
cvar_t	joy_deadzone_l = { "joy_deadzone_l", "7849", CVAR_NONE };
cvar_t	joy_deadzone_r = { "joy_deadzone_r", "8689", CVAR_NONE };
cvar_t	joy_deadzone_trigger = { "joy_deadzone_trigger", "30", CVAR_NONE };

/* joystick variables */
cvar_t	joy_sensitivity = { "joy_sensitivity", "10000", CVAR_NONE };
cvar_t	joy_filter = { "joy_filter", "0", CVAR_NONE };
//cvar_t	joy_deadzone = { "joy_deadzone", "0.125", CVAR_NONE };
cvar_t	joy_function = { "joy_function", "2", CVAR_NONE };
cvar_t	joy_axismove_x = { "joy_axismove_x", "0", CVAR_NONE };
cvar_t	joy_axismove_y = { "joy_axismove_y", "1", CVAR_NONE };
cvar_t	joy_axislook_x = { "joy_axislook_x", "3", CVAR_NONE };
cvar_t	joy_axislook_y = { "joy_axislook_y", "4", CVAR_NONE };
cvar_t	joy_axis_debug = { "joy_axis_debug", "0", CVAR_NONE };

/* joystick support functions */

static float Sint16ToPlusMinusOne (const Sint16 input)
{
	return (float)input / 32768.0f;
}

/*
// adapted in part from:
// http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html
*/
static joyAxis_t ApplyJoyDeadzone(joyAxis_t axis, float deadzone)
{
	joyAxis_t result = {0};
	float magnitude = sqrtf( (axis.x * axis.x) + (axis.y * axis.y) );

	if ( magnitude < deadzone ) {
		result.x = result.y = 0.0f;
	} else {
		joyAxis_t normalized;
		float gradient;

		if ( magnitude > 1.0f ) {
			magnitude = 1.0f;
		}

		normalized.x = axis.x / magnitude;
		normalized.y = axis.y / magnitude;
		gradient = ( (magnitude - deadzone) / (1.0f - deadzone) );
		result.x = normalized.x * gradient;
		result.y = normalized.y * gradient;
	}

	return result;
}

static int IN_FilterMouseEvents (const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_MOUSEMOTION:
	// case SDL_MOUSEBUTTONDOWN:
	// case SDL_MOUSEBUTTONUP:
		return 0;
	}

	return 1;
}

#if defined(USE_SDL2)
static int IN_SDL2_FilterMouseEvents (void *userdata, SDL_Event *event)
{
	return IN_FilterMouseEvents (event);
}
#endif

static void IN_BeginIgnoringMouseEvents()
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter = NULL;
	void *currentUserdata = NULL;
	SDL_GetEventFilter(&currentFilter, &currentUserdata);

	if (currentFilter != IN_SDL2_FilterMouseEvents)
		SDL_SetEventFilter(IN_SDL2_FilterMouseEvents, NULL);
#else
	if (SDL_GetEventFilter() != IN_FilterMouseEvents)
		SDL_SetEventFilter(IN_FilterMouseEvents);
#endif
}

static void IN_EndIgnoringMouseEvents()
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter;
	void *currentUserdata;
	if (SDL_GetEventFilter(&currentFilter, &currentUserdata) == SDL_TRUE)
		SDL_SetEventFilter(NULL, NULL);
#else
	if (SDL_GetEventFilter() != NULL)
		SDL_SetEventFilter(NULL);
#endif
}

#ifdef MACOS_X_ACCELERATION_HACK
static cvar_t in_disablemacosxmouseaccel = {"in_disablemacosxmouseaccel", "1", CVAR_ARCHIVE};
static double originalMouseSpeed = -1.0;

static io_connect_t IN_GetIOHandle(void)
{
	io_connect_t iohandle = MACH_PORT_NULL;
	io_service_t iohidsystem = MACH_PORT_NULL;
	mach_port_t masterport;
	kern_return_t status;

	status = IOMasterPort(MACH_PORT_NULL, &masterport);
	if (status != KERN_SUCCESS)
		return 0;

	iohidsystem = IORegistryEntryFromPath(masterport, kIOServicePlane ":/IOResources/IOHIDSystem");
	if (!iohidsystem)
		return 0;

	status = IOServiceOpen(iohidsystem, mach_task_self(), kIOHIDParamConnectType, &iohandle);
	IOObjectRelease(iohidsystem);

	return iohandle;
}

static void IN_DisableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDGetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), &originalMouseSpeed) == kIOReturnSuccess)
		{
			if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), -1.0) != kIOReturnSuccess)
			{
				Cvar_Set("in_disablemacosxmouseaccel", "0");
				Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
			}
		}
		else
		{
			Cvar_Set("in_disablemacosxmouseaccel", "0");
			Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDGetAccelerationWithKey).\n");
		}
		IOServiceClose(mouseDev);
	}
	else
	{
		Cvar_Set("in_disablemacosxmouseaccel", "0");
		Con_Printf("WARNING: Could not disable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
}

static void IN_ReenableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), originalMouseSpeed) != kIOReturnSuccess)
			Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
		IOServiceClose(mouseDev);
	}
	else
	{
		Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
	originalMouseSpeed = -1;
}
#endif /* MACOS_X_ACCELERATION_HACK */


void IN_Activate (void)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	/* Save the status of mouse acceleration */
	if (originalMouseSpeed == -1 && in_disablemacosxmouseaccel.value)
		IN_DisableOSXMouseAccel();
#endif

#if defined(USE_SDL2)
	if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0)
	{
		Con_Printf("WARNING: SDL_SetRelativeMouseMode(SDL_TRUE) failed.\n");
	}
#else
	if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
	{
		SDL_WM_GrabInput(SDL_GRAB_ON);
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
			Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_ON) failed.\n");
	}

	if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
	{
		SDL_ShowCursor(SDL_DISABLE);
		if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
			Con_Printf("WARNING: SDL_ShowCursor(SDL_DISABLE) failed.\n");
	}
#endif

	IN_EndIgnoringMouseEvents();

	total_dx = 0;
	total_dy = 0;
}

void IN_Deactivate (qboolean free_cursor)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	if (originalMouseSpeed != -1)
		IN_ReenableOSXMouseAccel();
#endif

	if (free_cursor)
	{
#if defined(USE_SDL2)
		SDL_SetRelativeMouseMode(SDL_FALSE);
#else
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
		{
			SDL_WM_GrabInput(SDL_GRAB_OFF);
			if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
				Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_OFF) failed.\n");
		}

		if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
		{
			SDL_ShowCursor(SDL_ENABLE);
			if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
				Con_Printf("WARNING: SDL_ShowCursor(SDL_ENABLE) failed.\n");
		}
#endif
	}

	/* discard all mouse events when input is deactivated */
	IN_BeginIgnoringMouseEvents();
}

void IN_Init (void)
{
	textmode = Key_TextEntry();

#if !defined(USE_SDL2)
	SDL_EnableUNICODE (textmode);
	if (SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL) == -1)
		Con_Printf("Warning: SDL_EnableKeyRepeat() failed.\n");
#else
	if (textmode)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
#endif
	if (safemode || COM_CheckParm("-nomouse"))
	{
		no_mouse = true;
		/* discard all mouse events when input is deactivated */
		IN_BeginIgnoringMouseEvents();
	}

	// BEGIN jeremiah sypult
	Cvar_RegisterVariable( &joy_sensitivity );
	Cvar_RegisterVariable( &joy_filter );
	Cvar_RegisterVariable( &joy_deadzone_l );
	Cvar_RegisterVariable( &joy_deadzone_r );
	Cvar_RegisterVariable( &joy_deadzone_trigger );
	Cvar_RegisterVariable( &joy_function );
	Cvar_RegisterVariable( &joy_axismove_x );
	Cvar_RegisterVariable( &joy_axismove_y );
	Cvar_RegisterVariable( &joy_axislook_x );
	Cvar_RegisterVariable( &joy_axislook_y );
	Cvar_RegisterVariable( &joy_axis_debug );

	if ( SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER ) == -1 ) {
		Con_Printf( "WARNING: Could not initialize SDL Game Controller\n" );
	} else {
		int i;
		
		for ( i = 0; i < SDL_NumJoysticks(); i++ )
		{
			if ( SDL_IsGameController(i) )
			{
				SDL_GameController *gamecontroller;
				gamecontroller = SDL_GameControllerOpen(i);
				if (gamecontroller)
				{
					Con_Printf("Opened controller %s\n", SDL_GameControllerNameForIndex(i));

					joy_active_instaceid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamecontroller));
					joy_active_controller = gamecontroller;
					break;
				}
			}
		}
	}
	
	if ( SDL_InitSubSystem( SDL_INIT_JOYSTICK ) == -1 ) {
		Con_Printf( "WARNING: Could not initialize SDL Joystick\n" );
	} else {
		int i;
		SDL_JoystickEventState( SDL_ENABLE );

		for ( i = 0; i < SDL_NumJoysticks(); i++ ) {
			//if ( ! SDL_JoystickOpened( i ) ) {
				SDL_Joystick* controller = SDL_JoystickOpen( i );

				if ( controller ) {
					Con_Printf( "%s\n     axes: %d\n  buttons: %d\n    balls: %d\n     hats: %d\n",
							    SDL_JoystickName( i ),
							    SDL_JoystickNumAxes( controller ),
							    SDL_JoystickNumButtons( controller ),
							    SDL_JoystickNumBalls( controller ),
							    SDL_JoystickNumHats( controller ) );
				}
			//}
		}
	}
	// END jeremiah sypult

#ifdef MACOS_X_ACCELERATION_HACK
	Cvar_RegisterVariable(&in_disablemacosxmouseaccel);
#endif
	Cvar_RegisterVariable(&in_debugkeys);

	IN_Activate();
}

void IN_Shutdown (void)
{
	IN_Deactivate(true);
}

void IN_Commands (void)
{
/* TODO: implement this for joystick support */
}

extern cvar_t cl_maxpitch; /* johnfitz -- variable pitch clamping */
extern cvar_t cl_minpitch; /* johnfitz -- variable pitch clamping */


void IN_MouseMove(int dx, int dy)
{
	total_dx += dx;
	total_dy += dy;
}

void IN_JoyHatEvent(Uint8 hat, Uint8 value)
{
	// map hat to K_AUX29 - K_AUX32
	// value flags: 1 = up, 2 = right, 4 = down, 8 = left
	static Uint8 _oldValue = 0;
	int i;

	for (i=0; i<4; i++)
	{
		if ( (value & (1<<i)) && !(_oldValue & (1<<i)) )
		{
			// hat enabled
			Key_Event(K_AUX29+i, true);
		}
		
		if ( !(value & (1<<i)) && (_oldValue & (1<<i)) )
		{
			// hat disabled
			Key_Event(K_AUX29+i, false);
		}
	}

	_oldValue = value;
}

void IN_JoyAxisMove(Uint8 axis, Sint16 value)
{
#if 0
	float axisValue = Sint16ToPlusMinusOne( value );
	Uint8 axisMap[] = {
		(Uint8)joy_axismove_x.value,
		(Uint8)joy_axismove_y.value,
		(Uint8)joy_axislook_x.value,
		(Uint8)joy_axislook_y.value
	};

	// map the incoming axis to the cvars defining which axis index controls movement.
	if ( axisMap[0] == axis ) {
		_rawDualAxis.left.x = axisValue;
	} else if ( axisMap[1] == axis ) {
		_rawDualAxis.left.y = axisValue;
	} else if ( axisMap[2] == axis ) {
		_rawDualAxis.right.x = axisValue;
	} else if ( axisMap[3] == axis ) {
		_rawDualAxis.right.y = axisValue;
	}

	if ( joy_axis_debug.value ) {
		Sint16 deadzone = joy_deadzone.value * 32767.6f;
		if ( value < -deadzone || value > deadzone ) {
			Con_Printf( "joy axis %i, value %i\n", axis, value );
		}
	}
	
	if ( axis == 2)
		Key_Event(K_SPACE, value >= 0);
	
	if ( axis == 5)
		Key_Event(K_CTRL, value >= 0);
#endif
}

static int IN_KeyForControllerButton(SDL_GameControllerButton button)
{
	switch (button)
	{
		case SDL_CONTROLLER_BUTTON_A: return K_X360_A;
		case SDL_CONTROLLER_BUTTON_B: return K_X360_B;
		case SDL_CONTROLLER_BUTTON_X: return K_X360_X;
		case SDL_CONTROLLER_BUTTON_Y: return K_X360_Y;
		case SDL_CONTROLLER_BUTTON_BACK: return K_X360_BACK;
		//case SDL_CONTROLLER_BUTTON_GUIDE: return K_X360_GUIDE;
		case SDL_CONTROLLER_BUTTON_START: return K_X360_START;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return K_X360_LEFT_THUMB;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return K_X360_RIGHT_THUMB;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_X360_LEFT_SHOULDER;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_X360_RIGHT_SHOULDER;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_X360_DPAD_UP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_X360_DPAD_DOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_X360_DPAD_LEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_X360_DPAD_RIGHT;
		default: return 0;
	}
}

// adapted from DP
static int IN_EmulatedKeyForControllerKey(int button)
{
	switch (button)
	{
		case K_X360_DPAD_UP: return K_UPARROW;
		case K_X360_DPAD_DOWN: return K_DOWNARROW;
		case K_X360_DPAD_LEFT: return K_LEFTARROW;
		case K_X360_DPAD_RIGHT: return K_RIGHTARROW;
		case K_X360_START: return K_ESCAPE;
		case K_X360_BACK: return K_ESCAPE;
		case K_X360_A: return K_ENTER;
		case K_X360_B: return K_ESCAPE;
		default: return 0;
	}
}

void IN_ControllerButton(SDL_JoystickID instanceid, SDL_GameControllerButton button, qboolean down)
{
	int key, emulatedkey;
	
	if (instanceid != joy_active_instaceid)
		return;
	
	key = IN_KeyForControllerButton(button);

	if (key)
		Key_Event(key, down);

	// also send emulated keyboard key
	emulatedkey = IN_EmulatedKeyForControllerKey(key);
	if (emulatedkey)
		Key_Event(emulatedkey, down);
}

#define DOWN_THRESHOLD 0.5
#define UP_THRESHOLD 0.25

void IN_ControllerAxisButton(float oldval, float newval, int negativekey, int positivekey)
{
	if (!(oldval < -DOWN_THRESHOLD) && (newval < -DOWN_THRESHOLD))
		Key_Event(negativekey, true);
	else if (!(oldval > -UP_THRESHOLD) && (newval > -UP_THRESHOLD))
		Key_Event(negativekey, false);
	
	if (!(oldval > DOWN_THRESHOLD) && (newval > DOWN_THRESHOLD))
		Key_Event(positivekey, true);
	else if (!(oldval < UP_THRESHOLD) && (newval < UP_THRESHOLD))
		Key_Event(positivekey, false);
}

void IN_ControllerAxis(SDL_JoystickID instanceid, SDL_GameControllerAxis axis, Sint16 value)
{
	float axisValue = Sint16ToPlusMinusOne( value );
	const float triggerThreshold = Sint16ToPlusMinusOne(joy_deadzone_trigger.value);
	
	if (instanceid != joy_active_instaceid)
		return;
	
	switch (axis)
	{
		// TODO: swap move/look cvar
		case SDL_CONTROLLER_AXIS_LEFTX:
			IN_ControllerAxisButton(_rawDualAxis.left.x, axisValue, K_X360_LEFT_THUMB_LEFT, K_X360_LEFT_THUMB_RIGHT);
			_rawDualAxis.left.x = axisValue;
			break;
		case SDL_CONTROLLER_AXIS_LEFTY:
			IN_ControllerAxisButton(_rawDualAxis.left.y, axisValue, K_X360_LEFT_THUMB_DOWN, K_X360_LEFT_THUMB_UP);
			_rawDualAxis.left.y = axisValue;
			break;
		case SDL_CONTROLLER_AXIS_RIGHTX:
			IN_ControllerAxisButton(_rawDualAxis.right.x, axisValue, K_X360_RIGHT_THUMB_LEFT, K_X360_RIGHT_THUMB_RIGHT);
			_rawDualAxis.right.x = axisValue;
			break;
		case SDL_CONTROLLER_AXIS_RIGHTY:
			IN_ControllerAxisButton(_rawDualAxis.right.y, axisValue, K_X360_RIGHT_THUMB_DOWN, K_X360_RIGHT_THUMB_UP);
			_rawDualAxis.right.y = axisValue;
			break;

		case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
		{
			static qboolean ltrigdown = false;
			if (axisValue > triggerThreshold && !ltrigdown)
			{
				Key_Event(K_X360_LEFT_TRIGGER, true);
				ltrigdown = true;
			}
			if (axisValue <= triggerThreshold && ltrigdown)
			{
				Key_Event(K_X360_LEFT_TRIGGER, false);
				ltrigdown = false;
			}
			break;
		}


		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
		{
			static qboolean ltrigdown = false;
			if (axisValue > triggerThreshold && !ltrigdown)
			{
				Key_Event(K_X360_RIGHT_TRIGGER, true);
				ltrigdown = true;
			}
			if (axisValue <= triggerThreshold && ltrigdown)
			{
				Key_Event(K_X360_RIGHT_TRIGGER, false);
				ltrigdown = false;
			}
			break;
		}
		default:
			return; // ignore
	}
}

void IN_ControllerAdded(SDL_GameControllerButton button, qboolean down)
{
	
}

void IN_Move (usercmd_t *cmd)
{
	int		dmx, dmy;

	// jeremiah sypult -- BEGIN joystick
	//
	dualAxis_t moveDualAxis = {0};

	if ( joy_filter.value ) {
		moveDualAxis.left.x = ( _rawDualAxis.left.x + _rawDualAxis._oldleft.x ) * 0.5;
		moveDualAxis.left.y = ( _rawDualAxis.left.y + _rawDualAxis._oldleft.y ) * 0.5;
		moveDualAxis.right.x = ( _rawDualAxis.right.x + _rawDualAxis._oldright.x ) * 0.5;
		moveDualAxis.right.y = ( _rawDualAxis.right.y + _rawDualAxis._oldright.y ) * 0.5;
	} else {
		moveDualAxis.left = _rawDualAxis.left;
		moveDualAxis.right = _rawDualAxis.right;
	}

	_rawDualAxis._oldleft = _rawDualAxis.left;
	_rawDualAxis._oldright = _rawDualAxis.right;
	
	moveDualAxis.left = ApplyJoyDeadzone( moveDualAxis.left, Sint16ToPlusMinusOne(joy_deadzone_l.value) );
	moveDualAxis.right = ApplyJoyDeadzone( moveDualAxis.right, Sint16ToPlusMinusOne(joy_deadzone_r.value) );
	
	switch ( (int)joy_function.value ) {
		default:
		case 0: break;
		case 1: dualfunc( moveDualAxis, sine );      break;
		case 2: dualfunc( moveDualAxis, quadratic ); break;
		case 3: dualfunc( moveDualAxis, cubic );     break;
		case 4: dualfunc( moveDualAxis, quartic );   break;
		case 5: dualfunc( moveDualAxis, quintic );   break;
	}

	// movements are not scaled by sensitivity
	if ( moveDualAxis.left.x != 0.0f ) {
		cmd->sidemove += (cl_sidespeed.value * moveDualAxis.left.x);
	}
	if ( moveDualAxis.left.y != 0.0f ) {
		cmd->forwardmove -= (cl_forwardspeed.value * moveDualAxis.left.y);
	}

	//
	// adjust for speed key
	//
	if (cl_forwardspeed.value > 200 && cl_movespeedkey.value)
		cmd->forwardmove /= cl_movespeedkey.value;
	if ((cl_forwardspeed.value > 200) ^ (in_speed.state & 1))
	{
		cmd->forwardmove *= cl_movespeedkey.value;
		cmd->sidemove *= cl_movespeedkey.value;
		cmd->upmove *= cl_movespeedkey.value;
	}

	// add the joy look axis to mouse look
	// ericw -- multiply by host_frametime (seconds/frame) to convert units/second to units/frame
	float joy_dx = (moveDualAxis.right.x * joy_sensitivity.value * host_frametime);
	float joy_dy = (moveDualAxis.right.y * joy_sensitivity.value * host_frametime);
	//
	// jeremiah sypult -- ENDjoystick

	dmx = (total_dx * sensitivity.value) + joy_dx;
	dmy = (total_dy * sensitivity.value) + joy_dy;

	total_dx = 0;
	total_dy = 0;

	if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * dmx;
	else
		cl.viewangles[YAW] -= m_yaw.value * dmx;

	if (in_mlook.state & 1)
	{
		if (dmx || dmy)
			V_StopPitchDrift ();
	}

	if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * dmy;
		/* johnfitz -- variable pitch clamping */
		if (cl.viewangles[PITCH] > cl_maxpitch.value)
			cl.viewangles[PITCH] = cl_maxpitch.value;
		if (cl.viewangles[PITCH] < cl_minpitch.value)
			cl.viewangles[PITCH] = cl_minpitch.value;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * dmy;
		else
			cmd->forwardmove -= m_forward.value * dmy;
	}
}

void IN_ClearStates (void)
{
}

void IN_UpdateInputMode (void)
{
	qboolean want_textmode = Key_TextEntry();
	if (textmode != want_textmode)
	{
		textmode = want_textmode;
#if !defined(USE_SDL2)
		SDL_EnableUNICODE(textmode);
#else
		if (textmode)
			SDL_StartTextInput();
		else
			SDL_StopTextInput();
#endif
	}
}

#if !defined(USE_SDL2)
static inline int IN_SDL_KeysymToQuakeKey(SDLKey sym)
{
	if (sym > SDLK_SPACE && sym < SDLK_DELETE)
		return sym;

	switch (sym)
	{
	case SDLK_TAB: return K_TAB;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_SPACE: return K_SPACE;

	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP: return K_UPARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;

	case SDLK_LALT: return K_ALT;
	case SDLK_RALT: return K_ALT;
	case SDLK_LCTRL: return K_CTRL;
	case SDLK_RCTRL: return K_CTRL;
	case SDLK_LSHIFT: return K_SHIFT;
	case SDLK_RSHIFT: return K_SHIFT;

	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_INSERT: return K_INS;
	case SDLK_DELETE: return K_DEL;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;

	case SDLK_NUMLOCK: return K_KP_NUMLOCK;
	case SDLK_KP_DIVIDE: return K_KP_SLASH;
	case SDLK_KP_MULTIPLY: return K_KP_STAR;
	case SDLK_KP_MINUS:return K_KP_MINUS;
	case SDLK_KP7: return K_KP_HOME;
	case SDLK_KP8: return K_KP_UPARROW;
	case SDLK_KP9: return K_KP_PGUP;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_KP4: return K_KP_LEFTARROW;
	case SDLK_KP5: return K_KP_5;
	case SDLK_KP6: return K_KP_RIGHTARROW;
	case SDLK_KP1: return K_KP_END;
	case SDLK_KP2: return K_KP_DOWNARROW;
	case SDLK_KP3: return K_KP_PGDN;
	case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_KP0: return K_KP_INS;
	case SDLK_KP_PERIOD: return K_KP_DEL;

	case SDLK_LMETA: return K_COMMAND;
	case SDLK_RMETA: return K_COMMAND;

	case SDLK_BREAK: return K_PAUSE;
	case SDLK_PAUSE: return K_PAUSE;

	case SDLK_WORLD_18: return '~'; // the '²' key

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static inline int IN_SDL2_ScancodeToQuakeKey(SDL_Scancode scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_TAB: return K_TAB;
	case SDL_SCANCODE_RETURN: return K_ENTER;
	case SDL_SCANCODE_RETURN2: return K_ENTER;
	case SDL_SCANCODE_ESCAPE: return K_ESCAPE;
	case SDL_SCANCODE_SPACE: return K_SPACE;

	case SDL_SCANCODE_A: return 'a';
	case SDL_SCANCODE_B: return 'b';
	case SDL_SCANCODE_C: return 'c';
	case SDL_SCANCODE_D: return 'd';
	case SDL_SCANCODE_E: return 'e';
	case SDL_SCANCODE_F: return 'f';
	case SDL_SCANCODE_G: return 'g';
	case SDL_SCANCODE_H: return 'h';
	case SDL_SCANCODE_I: return 'i';
	case SDL_SCANCODE_J: return 'j';
	case SDL_SCANCODE_K: return 'k';
	case SDL_SCANCODE_L: return 'l';
	case SDL_SCANCODE_M: return 'm';
	case SDL_SCANCODE_N: return 'n';
	case SDL_SCANCODE_O: return 'o';
	case SDL_SCANCODE_P: return 'p';
	case SDL_SCANCODE_Q: return 'q';
	case SDL_SCANCODE_R: return 'r';
	case SDL_SCANCODE_S: return 's';
	case SDL_SCANCODE_T: return 't';
	case SDL_SCANCODE_U: return 'u';
	case SDL_SCANCODE_V: return 'v';
	case SDL_SCANCODE_W: return 'w';
	case SDL_SCANCODE_X: return 'x';
	case SDL_SCANCODE_Y: return 'y';
	case SDL_SCANCODE_Z: return 'z';

	case SDL_SCANCODE_1: return '1';
	case SDL_SCANCODE_2: return '2';
	case SDL_SCANCODE_3: return '3';
	case SDL_SCANCODE_4: return '4';
	case SDL_SCANCODE_5: return '5';
	case SDL_SCANCODE_6: return '6';
	case SDL_SCANCODE_7: return '7';
	case SDL_SCANCODE_8: return '8';
	case SDL_SCANCODE_9: return '9';
	case SDL_SCANCODE_0: return '0';

	case SDL_SCANCODE_MINUS: return '-';
	case SDL_SCANCODE_EQUALS: return '=';
	case SDL_SCANCODE_LEFTBRACKET: return '[';
	case SDL_SCANCODE_RIGHTBRACKET: return ']';
	case SDL_SCANCODE_BACKSLASH: return '\\';
	case SDL_SCANCODE_NONUSHASH: return '#';
	case SDL_SCANCODE_SEMICOLON: return ';';
	case SDL_SCANCODE_APOSTROPHE: return '\'';
	case SDL_SCANCODE_GRAVE: return '`';
	case SDL_SCANCODE_COMMA: return ',';
	case SDL_SCANCODE_PERIOD: return '.';
	case SDL_SCANCODE_SLASH: return '/';
	case SDL_SCANCODE_NONUSBACKSLASH: return '\\';

	case SDL_SCANCODE_BACKSPACE: return K_BACKSPACE;
	case SDL_SCANCODE_UP: return K_UPARROW;
	case SDL_SCANCODE_DOWN: return K_DOWNARROW;
	case SDL_SCANCODE_LEFT: return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT: return K_RIGHTARROW;

	case SDL_SCANCODE_LALT: return K_ALT;
	case SDL_SCANCODE_RALT: return K_ALT;
	case SDL_SCANCODE_LCTRL: return K_CTRL;
	case SDL_SCANCODE_RCTRL: return K_CTRL;
	case SDL_SCANCODE_LSHIFT: return K_SHIFT;
	case SDL_SCANCODE_RSHIFT: return K_SHIFT;

	case SDL_SCANCODE_F1: return K_F1;
	case SDL_SCANCODE_F2: return K_F2;
	case SDL_SCANCODE_F3: return K_F3;
	case SDL_SCANCODE_F4: return K_F4;
	case SDL_SCANCODE_F5: return K_F5;
	case SDL_SCANCODE_F6: return K_F6;
	case SDL_SCANCODE_F7: return K_F7;
	case SDL_SCANCODE_F8: return K_F8;
	case SDL_SCANCODE_F9: return K_F9;
	case SDL_SCANCODE_F10: return K_F10;
	case SDL_SCANCODE_F11: return K_F11;
	case SDL_SCANCODE_F12: return K_F12;
	case SDL_SCANCODE_INSERT: return K_INS;
	case SDL_SCANCODE_DELETE: return K_DEL;
	case SDL_SCANCODE_PAGEDOWN: return K_PGDN;
	case SDL_SCANCODE_PAGEUP: return K_PGUP;
	case SDL_SCANCODE_HOME: return K_HOME;
	case SDL_SCANCODE_END: return K_END;

	case SDL_SCANCODE_NUMLOCKCLEAR: return K_KP_NUMLOCK;
	case SDL_SCANCODE_KP_DIVIDE: return K_KP_SLASH;
	case SDL_SCANCODE_KP_MULTIPLY: return K_KP_STAR;
	case SDL_SCANCODE_KP_MINUS: return K_KP_MINUS;
	case SDL_SCANCODE_KP_7: return K_KP_HOME;
	case SDL_SCANCODE_KP_8: return K_KP_UPARROW;
	case SDL_SCANCODE_KP_9: return K_KP_PGUP;
	case SDL_SCANCODE_KP_PLUS: return K_KP_PLUS;
	case SDL_SCANCODE_KP_4: return K_KP_LEFTARROW;
	case SDL_SCANCODE_KP_5: return K_KP_5;
	case SDL_SCANCODE_KP_6: return K_KP_RIGHTARROW;
	case SDL_SCANCODE_KP_1: return K_KP_END;
	case SDL_SCANCODE_KP_2: return K_KP_DOWNARROW;
	case SDL_SCANCODE_KP_3: return K_KP_PGDN;
	case SDL_SCANCODE_KP_ENTER: return K_KP_ENTER;
	case SDL_SCANCODE_KP_0: return K_KP_INS;
	case SDL_SCANCODE_KP_PERIOD: return K_KP_DEL;

	case SDL_SCANCODE_LGUI: return K_COMMAND;
	case SDL_SCANCODE_RGUI: return K_COMMAND;

	case SDL_SCANCODE_PAUSE: return K_PAUSE;

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static void IN_DebugTextEvent(SDL_Event *event)
{
	Con_Printf ("SDL_TEXTINPUT '%s'\n", event->text.text);
}
#endif

static void IN_DebugKeyEvent(SDL_Event *event)
{
	const char *eventtype = (event->key.state == SDL_PRESSED) ? "SDL_KEYDOWN" : "SDL_KEYUP";
#if defined(USE_SDL2)
	Con_Printf ("%s scancode: '%s' keycode: '%s'\n",
		eventtype,
		SDL_GetScancodeName(event->key.keysym.scancode),
		SDL_GetKeyName(event->key.keysym.sym));
#else
	Con_Printf ("%s sym: '%s' unicode: %04x\n",
		eventtype,
		SDL_GetKeyName(event->key.keysym.sym),
		(int)event->key.keysym.unicode);
#endif
}

void IN_SendKeyEvents (void)
{
	SDL_Event event;
	int key;
	qboolean down;

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
#if defined(USE_SDL2)
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				S_UnblockSound();
			else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				S_BlockSound();
			break;
#else
		case SDL_ACTIVEEVENT:
			if (event.active.state & (SDL_APPINPUTFOCUS|SDL_APPACTIVE))
			{
				if (event.active.gain)
					S_UnblockSound();
				else	S_BlockSound();
			}
			break;
#endif
#if defined(USE_SDL2)
		case SDL_TEXTINPUT:
			if (in_debugkeys.value)
				IN_DebugTextEvent(&event);

		// SDL2: We use SDL_TEXTINPUT for typing in the console / chat.
		// SDL2 uses the local keyboard layout and handles modifiers
		// (shift for uppercase, etc.) for us.
			{
				unsigned char *ch;
				for (ch = (unsigned char *)event.text.text; *ch; ch++)
					if ((*ch & ~0x7F) == 0)
						Char_Event (*ch);
			}
			break;
#endif
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			down = (event.key.state == SDL_PRESSED);

			if (in_debugkeys.value)
				IN_DebugKeyEvent(&event);

#if defined(USE_SDL2)
		// SDL2: we interpret the keyboard as the US layout, so keybindings
		// are based on key position, not the label on the key cap.
			key = IN_SDL2_ScancodeToQuakeKey(event.key.keysym.scancode);
#else
			key = IN_SDL_KeysymToQuakeKey(event.key.keysym.sym);
#endif

			Key_Event (key, down);

#if !defined(USE_SDL2)
			if (down && (event.key.keysym.unicode & ~0x7F) == 0)
				Char_Event (event.key.keysym.unicode);
#endif
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (event.button.button < 1 ||
			    event.button.button > sizeof(buttonremap) / sizeof(buttonremap[0]))
			{
				Con_Printf ("Ignored event for mouse button %d\n",
							event.button.button);
				break;
			}
			Key_Event(buttonremap[event.button.button - 1], event.button.state == SDL_PRESSED);
			break;

#if defined(USE_SDL2)
		case SDL_MOUSEWHEEL:
			if (event.wheel.y > 0)
			{
				Key_Event(K_MWHEELUP, true);
				Key_Event(K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0)
			{
				Key_Event(K_MWHEELDOWN, true);
				Key_Event(K_MWHEELDOWN, false);
			}
			break;
#endif

		case SDL_MOUSEMOTION:
			IN_MouseMove(event.motion.xrel, event.motion.yrel);
			break;

#if 0
		case SDL_JOYHATMOTION:
			// TODO: VERIFY hat support, handle multiple hats?
			IN_JoyHatEvent(event.jhat.hat, event.jhat.value);
			break;

		case SDL_JOYBALLMOTION:
			// TODO: VERIFY joyball support, assignment other than mouse?
			IN_MouseMove(event.jball.xrel, event.jball.yrel);
			break;

		case SDL_JOYAXISMOTION:
			IN_JoyAxisMove(event.jaxis.axis, event.jaxis.value);
			break;

		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (event.jbutton.button > sizeof(joyremap) / sizeof(joyremap[0]))
			{
				Con_Printf ("Ignored event for joy button %d\n",
							event.button.button);
				break;
			}
			Key_Event(joyremap[event.jbutton.button], event.jbutton.state == SDL_PRESSED);
			break;
#endif

#if defined(USE_SDL2)
		case SDL_CONTROLLERAXISMOTION:
			IN_ControllerAxis(event.caxis.which, event.caxis.axis, event.caxis.value);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			IN_ControllerButton(event.cbutton.which, event.cbutton.button, event.cbutton.state == SDL_PRESSED);
			break;
		case SDL_CONTROLLERDEVICEADDED:
			if (joy_active_instaceid == -1)
			{
				joy_active_controller = SDL_GameControllerOpen(event.cdevice.which);
				if (joy_active_controller == NULL)
					Con_DPrintf("Couldn't open game controller\n");
				else
				{
					SDL_Joystick *joy;
					joy = SDL_GameControllerGetJoystick(joy_active_controller);
					joy_active_instaceid = SDL_JoystickInstanceID(joy);
				}
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEADDED\n");
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			if (joy_active_instaceid != -1 && event.cdevice.which == joy_active_instaceid)
			{
				SDL_GameControllerClose(joy_active_controller);
				joy_active_controller = NULL;
				joy_active_instaceid = -1;
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEREMOVED\n");
			break;
		case SDL_CONTROLLERDEVICEREMAPPED:
			Con_DPrintf("Unimplemented SDL_CONTROLLERDEVICEREMAPPED\n");
			break;
#endif
				
		case SDL_QUIT:
			CL_Disconnect ();
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}

