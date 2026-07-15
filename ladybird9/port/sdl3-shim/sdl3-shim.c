/* sdl3-shim — SDL3 for Ladybird on 9front, gamepad-only, permanently empty.
 *
 * Ladybird uses SDL3 solely for the Gamepad API (Services/WebContent/main.cpp,
 * Libraries/LibWeb/Gamepad/, Libraries/LibWeb/Page/EventHandler.cpp). 9front
 * has no game controller stack, so this shim behaves exactly like real SDL3
 * built with only the "dummy" joystick backend: init succeeds, zero gamepads
 * are ever enumerated, no gamepad events are ever posted, and every query
 * returns the documented not-found/default value.
 *
 * Compiled against the vendored official SDL3 3.2.24 headers (vendor/sdl3/),
 * so every definition here is signature-checked against the real API.
 *
 * ponytail: error state is one static pointer, not per-thread like real SDL;
 * upgrade to thread-local if anything but WebContent's main thread ever asks.
 */

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_events.h>

#include <stdlib.h>

static const char *sdl9_error = "";

/* --- init / error ------------------------------------------------------- */

bool SDL_Init(SDL_InitFlags flags)
{
    (void)flags; /* gamepad subsystem with the dummy backend always inits */
    return true;
}

void SDL_Quit(void)
{
}

const char *SDL_GetError(void)
{
    return sdl9_error;
}

/* --- memory --------------------------------------------------------------
 * Callers SDL_free() what SDL_GetGamepads() returns; we allocate with
 * calloc, so SDL_free is libc free (real SDL3 defaults to libc too). */

void SDL_free(void *mem)
{
    free(mem);
}

/* --- events: none, ever --------------------------------------------------- */

bool SDL_PollEvent(SDL_Event *event)
{
    (void)event;
    return false;
}

bool SDL_WaitEvent(SDL_Event *event)
{
    (void)event;
    sdl9_error = "No events available";
    return false; /* would block forever; real SDL returns false on error */
}

bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS)
{
    (void)event;
    (void)timeoutMS;
    return false; /* timed out, no event */
}

/* --- gamepad enumeration: zero devices ----------------------------------- */

SDL_JoystickID *SDL_GetGamepads(int *count)
{
    /* Real contract: 0-terminated array the caller SDL_free()s, count out. */
    if (count)
        *count = 0;
    return (SDL_JoystickID *)calloc(1, sizeof(SDL_JoystickID));
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id)
{
    (void)instance_id;
    sdl9_error = "Joystick not found"; /* no such instance id exists */
    return NULL;
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
    (void)gamepad;
}

const char *SDL_GetGamepadNameForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    sdl9_error = "Joystick not found";
    return NULL;
}

bool SDL_IsJoystickVirtual(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return false;
}

/* --- per-gamepad queries: only reachable with a NULL/invalid gamepad ------ */

SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    sdl9_error = "Invalid gamepad";
    return 0;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    (void)props;
    (void)name;
    return default_value; /* property never set -> documented default */
}

bool SDL_GamepadHasAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    (void)gamepad;
    (void)axis;
    return false;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    (void)gamepad;
    (void)axis;
    return 0;
}

bool SDL_GamepadHasButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad;
    (void)button;
    return false;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad;
    (void)button;
    return false;
}

Sint16 SDL_GetJoystickAxis(SDL_Joystick *joystick, int axis)
{
    (void)joystick;
    (void)axis;
    return 0;
}

/* --- haptics: unsupported, like any pad with no rumble hardware ----------- */

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    (void)gamepad;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    (void)duration_ms;
    sdl9_error = "Rumble not supported";
    return false;
}

bool SDL_RumbleGamepadTriggers(SDL_Gamepad *gamepad, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms)
{
    (void)gamepad;
    (void)left_rumble;
    (void)right_rumble;
    (void)duration_ms;
    sdl9_error = "Rumble not supported";
    return false;
}
