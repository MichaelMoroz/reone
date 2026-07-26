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

#pragma once

namespace reone {

namespace graphics {

class Texture;

/**
 * Owns the frame: the target everything is drawn into, and how a finished frame
 * reaches the screen.
 *
 * This exists so that callers do not have to know what a frame is made of. In
 * OpenGL it is the default framebuffer and a window buffer swap; in Vulkan it
 * will be an acquired swapchain image, a recorded command buffer and a queue
 * present. Those differ enough that every call site which currently binds a
 * shader and draws a fullscreen quad would otherwise need a second version.
 *
 * A frame is strictly beginFrame, drawing, endFrame. Nothing may be drawn
 * outside that span.
 */
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void init() = 0;
    virtual void deinit() = 0;

    /**
     * Acquire and clear a frame covering the whole of @p extent, in pixels.
     * Everything drawn until endFrame targets it.
     */
    virtual void beginFrame(glm::ivec2 extent) = 0;

    /**
     * Composite an offscreen scene render over the frame, filling it. This is
     * the hand-off from a render pipeline, which produces a texture, to the
     * backend, which decides how that texture becomes visible.
     */
    virtual void drawSceneOutput(Texture &output) = 0;

    /**
     * Read the frame back as drawn so far, for screenshots and comparisons.
     *
     * Must be called before endFrame: once a frame is presented, the contents
     * of its target are undefined. Expect this to stall the pipeline.
     */
    virtual std::shared_ptr<Texture> captureFrame() = 0;

    /**
     * Finish the frame and present it, if this renderer owns presentation. A
     * renderer drawing into a target owned by someone else - the toolkit's
     * canvas, say - ends the frame without presenting.
     */
    virtual void endFrame() = 0;
};

} // namespace graphics

} // namespace reone
