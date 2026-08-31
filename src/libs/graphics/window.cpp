/*
 * Copyright (c) 2020-2023 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "reone/graphics/window.h"

#include "SDL3/SDL.h"

#include "reone/system/checkutil.h"
#include "reone/system/threadutil.h"

namespace reone {

namespace graphics {

void Window::init() {
    checkThat(!_inited, "Must not be initialized");
    checkMainThread();

    int flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (_options.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (_options.headless) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    // A fullscreen window is a borderless window covering the desktop: the
    // display's resolution replaces the authored one and the scale is ignored.
    if (_options.fullscreenWindow) {
        flags |= SDL_WINDOW_BORDERLESS;
        if (const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
            _options.width = mode->w;
            _options.height = mode->h;
        }
    }
    const int windowWidth =
        _options.fullscreenWindow ? _options.width : _options.width * _options.winScale / 100;
    const int windowHeight =
        _options.fullscreenWindow ? _options.height : _options.height * _options.winScale / 100;
    _window = SDL_CreateWindow("reone", windowWidth, windowHeight, flags);
    if (!_window) {
        throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
    }
    if (_options.fullscreenWindow) {
        SDL_SetWindowPosition(_window, 0, 0);
    }
    _windowID = SDL_GetWindowID(_window);
    _inited = true;
}

void Window::deinit() {
    if (!_inited) {
        return;
    }
    SDL_DestroyWindow(_window);
    _inited = false;
}

bool Window::isAssociatedWith(const SDL_Event &event) const {
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID == _windowID;
    }

    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        return event.key.windowID == _windowID;
    case SDL_EVENT_KEY_UP:
        return event.key.windowID == _windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID == _windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return event.button.windowID == _windowID;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID == _windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID == _windowID;
    default:
        return false;
    }
}

bool Window::handle(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (handleKeyDownEvent(event.key)) {
            return true;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        _inFocus = true;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        _inFocus = false;
        return true;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        _closeRequested = true;
        return true;
    default:
        break;
    }
    return false;
}

bool Window::handleKeyDownEvent(const SDL_KeyboardEvent &event) {
    switch (event.scancode) {
    case SDL_SCANCODE_C:
        if (event.mod & SDL_KMOD_CTRL) {
            _closeRequested = true;
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void Window::swap() {
}

void Window::setRelativeMouseMode(bool isRelative) {
    SDL_SetWindowRelativeMouseMode(_window, isRelative);
}

void Window::resize(int width, int height) {
    if (_options.fullscreenWindow) {
        if (const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
            _options.width = mode->w;
            _options.height = mode->h;
        }
        SDL_SetWindowBordered(_window, false);
        SDL_SetWindowSize(_window, _options.width, _options.height);
        SDL_SetWindowPosition(_window, 0, 0);
        return;
    }
    SDL_SetWindowBordered(_window, true);
    SDL_SetWindowSize(_window, width * _options.winScale / 100,
                      height * _options.winScale / 100);
}

void Window::setVsync(bool enabled) {
}

} // namespace graphics

} // namespace reone
