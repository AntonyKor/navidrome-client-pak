#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * Modal on-screen keyboard. Edits buf in place (append/backspace at the end).
 *
 * Controls: D-pad move, A type key, B backspace, X space, Y cycle
 * abc / ABC / symbols, START done, SELECT cancel.
 *
 * masked: show the text as dots (passwords); the last typed character is
 * shown briefly so the user can check it.
 *
 * Returns true if the user confirmed (buf holds the new text), false if
 * cancelled (buf restored to its original contents).
 */
bool UIKeyboard_run(SDL_Surface *screen, const char *title,
                    char *buf, int buf_size, bool masked);

#endif /* UI_KEYBOARD_H */
