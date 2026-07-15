/*
 * ladybird9 plan9: SDL3 virtual-joystick entry points the sysroot sdl3-shim
 * doesn't yet carry (LibWeb/Internals/InternalGamepad.cpp, the test-infra
 * virtual gamepad, links them). Semantics match real SDL3's dummy joystick
 * backend with no virtual-device support: attach fails cleanly, so no
 * virtual joystick ever exists and the remaining calls are unreachable.
 * Durable home: fold into port/sdl3-shim/sdl3-shim.c on the next sysroot
 * regeneration (this object is compiled into _out/build and appended to
 * SDL3::SDL3 by the PLAN9 arm in check_for_dependencies.cmake).
 */
#include <SDL3/SDL.h>

/* Written for parity with the shim's error-text convention; SDL_GetError in
 * the shim reports its own message, so this is informational only. */
static const char *sdl9vj_error __attribute__((unused)) = "";

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc)
{
    (void)desc;
    sdl9vj_error = "Virtual joysticks are not supported on Plan 9";
    return 0; /* 0 = invalid id = failure */
}

bool SDL_DetachVirtualJoystick(SDL_JoystickID instance_id)
{
    (void)instance_id;
    sdl9vj_error = "Joystick not found";
    return false;
}

SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID instance_id)
{
    (void)instance_id;
    sdl9vj_error = "Joystick not found";
    return NULL;
}

void SDL_CloseJoystick(SDL_Joystick *joystick)
{
    (void)joystick;
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value)
{
    (void)joystick;
    (void)axis;
    (void)value;
    sdl9vj_error = "Joystick is not virtual";
    return false;
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick *joystick, int button, bool down)
{
    (void)joystick;
    (void)button;
    (void)down;
    sdl9vj_error = "Joystick is not virtual";
    return false;
}
