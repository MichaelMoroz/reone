/*
 * Copyright (c) 2020-2026 The reone project contributors
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

/*
 * Deliberately NOT named drawdebug.cpp.
 *
 * Upstream owns that path and fills it with an immediate-mode renderer built
 * on the GL context, shader registry and render-pass types this branch
 * deleted. Keeping our implementation under a name upstream does not use means
 * the two never occupy the same file: upstream's drawdebug.cpp simply stays
 * deleted here, which is a modify/delete conflict that resolves with one
 * `git rm` instead of a content merge every time it changes.
 */

#include "reone/scene/drawdebug.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace reone {

namespace {

/**
 * What a scope owns, in the one representation the overlay can draw.
 *
 * Triangles and points are decomposed to lines when they are recorded rather
 * than when they are drawn, because the overlay has exactly three primitives
 * and this keeps the drawing side free of shape-specific cases. A point
 * becomes three axis crosses, which reads at any camera angle where a single
 * quad would vanish edge-on.
 */
struct Element {
    enum class Kind {
        Line,
        Box,
        Text,
    };

    Kind kind {Kind::Line};
    glm::vec3 a {0.0f};
    glm::vec3 b {0.0f};
    glm::vec4 color {1.0f};
    float width {1.0f};
    std::string text;
    std::string scene;
    /** Seconds remaining, or a negative value for "until clear()". */
    float lifetime {-1.0f};
};

/**
 * The scope stacks and the elements they own.
 *
 * Scoped by id exactly as upstream specifies: pushId opens a bucket, every
 * record lands in the innermost open one, and clear() empties that bucket
 * alone. That is what lets the pathfinder call clear() on its own funnel each
 * time it recomputes without disturbing the face graph drawn beside it.
 */
struct State {
    std::vector<uint64_t> idStack;
    std::vector<float> lifetimeStack;
    std::vector<std::string> sceneStack;
    std::map<uint64_t, std::vector<Element>> buckets;

    uint64_t currentId() const {
        return idStack.empty() ? 0ull : idStack.back();
    }

    float currentLifetime() const {
        return lifetimeStack.empty() ? -1.0f : lifetimeStack.back();
    }

    const std::string &currentScene() const {
        static const std::string empty;
        return sceneStack.empty() ? empty : sceneStack.back();
    }

    void record(Element element) {
        element.scene = currentScene();
        element.lifetime = currentLifetime();
        buckets[currentId()].push_back(std::move(element));
    }
};

/**
 * Process-wide, matching upstream's own storage class.
 *
 * The callers are free functions with no context argument - that is the API -
 * so the state has nowhere else to live. Single-threaded by the same
 * assumption the rest of the frame makes.
 */
State &state() {
    static State s;
    return s;
}

glm::vec4 unpackRgba(uint32_t rgba) {
    return glm::vec4 {
        static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgba & 0xFFu) / 255.0f};
}

} // namespace

namespace drawdebug {

void pushId(uint64_t id) {
    state().idStack.push_back(id);
}

void pushId(const void *id) {
    pushId(reinterpret_cast<uint64_t>(id));
}

void popId() {
    auto &s = state();
    if (!s.idStack.empty()) {
        s.idStack.pop_back();
    }
}

void pushLifetime(float lifetime) {
    state().lifetimeStack.push_back(lifetime);
}

void popLifetime() {
    auto &s = state();
    if (!s.lifetimeStack.empty()) {
        s.lifetimeStack.pop_back();
    }
}

void pushScene(std::string sceneName) {
    state().sceneStack.push_back(std::move(sceneName));
}

void popScene() {
    auto &s = state();
    if (!s.sceneStack.empty()) {
        s.sceneStack.pop_back();
    }
}

void clear() {
    auto &s = state();
    auto bucket = s.buckets.find(s.currentId());
    if (bucket != s.buckets.end()) {
        bucket->second.clear();
    }
}

void line(glm::vec3 start, glm::vec3 end, uint32_t colorRgba, float thickness) {
    Element element;
    element.kind = Element::Kind::Line;
    element.a = start;
    element.b = end;
    element.color = unpackRgba(colorRgba);
    // Thickness arrives in world units by upstream's convention and the
    // overlay expands in pixels, so this is a nominal half-width rather than a
    // conversion. A projection-correct width would make a distant path
    // invisible, which is the opposite of what a debug line is for.
    element.width = std::max(0.5f, thickness * 20.0f);
    state().record(std::move(element));
}

void triangle(glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, uint32_t colorRgba) {
    // Outlined, not filled: the overlay has no filled primitive, and an
    // outline is what a debug triangle is read for anyway.
    line(v0, v1, colorRgba, 0.05f);
    line(v1, v2, colorRgba, 0.05f);
    line(v2, v0, colorRgba, 0.05f);
}

void text(const std::string &str, glm::vec3 position, uint32_t colorRgba, float scale) {
    Element element;
    element.kind = Element::Kind::Text;
    element.a = position;
    element.color = unpackRgba(colorRgba);
    element.width = scale;
    element.text = str;
    state().record(std::move(element));
}

void point(glm::vec3 position, uint32_t colorRgba, float scale) {
    const float r = 0.1f * scale;
    line(position - glm::vec3 {r, 0.0f, 0.0f}, position + glm::vec3 {r, 0.0f, 0.0f}, colorRgba, 0.05f);
    line(position - glm::vec3 {0.0f, r, 0.0f}, position + glm::vec3 {0.0f, r, 0.0f}, colorRgba, 0.05f);
    line(position - glm::vec3 {0.0f, 0.0f, r}, position + glm::vec3 {0.0f, 0.0f, r}, colorRgba, 0.05f);
}

void box(glm::vec3 min, glm::vec3 max, uint32_t colorRgba) {
    Element element;
    element.kind = Element::Kind::Box;
    element.a = min;
    element.b = max;
    element.color = unpackRgba(colorRgba);
    state().record(std::move(element));
}

} // namespace drawdebug

void updateDrawDebug(float dt) {
    auto &s = state();
    for (auto &[id, elements] : s.buckets) {
        elements.erase(
            std::remove_if(elements.begin(), elements.end(),
                           [dt](Element &element) {
                               if (element.lifetime < 0.0f) {
                                   // Lives until its scope is cleared.
                                   return false;
                               }
                               element.lifetime -= dt;
                               return element.lifetime <= 0.0f;
                           }),
            elements.end());
    }
}

void collectDrawDebug(std::string_view sceneName,
                      std::vector<graphics::DebugOverlayShape> &shapes,
                      std::vector<graphics::DebugOverlayLine> &lines,
                      std::vector<graphics::DebugOverlayLabel> &labels) {
    auto &s = state();
    for (const auto &[id, elements] : s.buckets) {
        for (const auto &element : elements) {
            // An element recorded outside any pushScene belongs to whichever
            // scene is collecting; only an explicitly scoped one is filtered.
            if (!element.scene.empty() && element.scene != sceneName) {
                continue;
            }
            switch (element.kind) {
            case Element::Kind::Line: {
                graphics::DebugOverlayLine line;
                line.start = glm::vec4 {element.a, 1.0f};
                line.end = glm::vec4 {element.b, 1.0f};
                line.color = element.color;
                line.halfWidth = element.width;
                lines.push_back(line);
                break;
            }
            case Element::Kind::Box: {
                graphics::DebugOverlayShape shape;
                // Corner i selects max over min per axis by bits x=1, y=2,
                // z=4 - the overlay's order, which is NOT AABB::corners().
                for (int i = 0; i < 8; ++i) {
                    shape.corners[i] = glm::vec4 {
                        (i & 1) ? element.b.x : element.a.x,
                        (i & 2) ? element.b.y : element.a.y,
                        (i & 4) ? element.b.z : element.a.z,
                        1.0f};
                }
                shape.color = element.color;
                shapes.push_back(shape);
                break;
            }
            case Element::Kind::Text: {
                graphics::DebugOverlayLabel label;
                label.position = element.a;
                label.text = element.text;
                label.color = element.color;
                labels.push_back(std::move(label));
                break;
            }
            }
        }
    }
}

} // namespace reone
