/* sdl3_smoke — gate for the gamepad-only SDL3 shim: init must succeed,
 * enumeration must be empty (0-terminated, count 0), no events. */
#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int count = -1;
    SDL_JoystickID *pads = SDL_GetGamepads(&count);
    if (!pads || count != 0 || pads[0] != 0) {
        printf("FAIL SDL_GetGamepads: pads=%p count=%d\n", (void *)pads, count);
        return 1;
    }
    SDL_free(pads);
    SDL_Event ev;
    if (SDL_PollEvent(&ev)) {
        printf("FAIL SDL_PollEvent returned an event\n");
        return 1;
    }
    if (SDL_OpenGamepad(1) != NULL) {
        printf("FAIL SDL_OpenGamepad returned a gamepad\n");
        return 1;
    }
    printf("sdl3 shim smoke: OK (%s)\n", SDL_GetError());
    return 0;
}
