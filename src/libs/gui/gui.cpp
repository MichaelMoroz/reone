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

#include "reone/gui/gui.h"

#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/gui/control/button.h"
#include "reone/gui/control/iconchain.h"
#include "reone/gui/control/imagebutton.h"
#include "reone/gui/control/label.h"
#include "reone/gui/control/listbox.h"
#include "reone/gui/control/panel.h"
#include "reone/gui/control/progressbar.h"
#include "reone/gui/control/scrollbar.h"
#include "reone/gui/control/slider.h"
#include "reone/gui/control/togglebutton.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/gff.h"
#include "reone/resource/parser/gff/gui.h"
#include "reone/resource/provider/gffs.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/resources.h"
#include "reone/system/exception/validation.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;

namespace reone {

namespace gui {

namespace {

constexpr glm::vec4 kBackgroundPlateWindow {
    504.0f / 2048.0f,
    250.0f / 1024.0f,
    1539.0f / 2048.0f,
    771.0f / 1024.0f};

} // namespace

GUI::Layout GUI::fitLayoutInArea(const glm::vec2 &areaOffset,
                                 const glm::vec2 &areaSize,
                                 const glm::ivec2 &layoutSize) {
    float factor = std::min(areaSize.x / layoutSize.x, areaSize.y / layoutSize.y);
    glm::vec2 size = glm::vec2(layoutSize) * factor;
    return {
        {static_cast<int>(areaOffset.x + (areaSize.x - size.x) / 2.0f),
         static_cast<int>(areaOffset.y + (areaSize.y - size.y) / 2.0f)},
        {factor, factor}};
}

GUI::Layout GUI::fitLayoutInBackgroundPlate(const glm::ivec2 &screenSize,
                                            const glm::ivec2 &layoutSize) {
    glm::vec2 areaOffset {
        screenSize.x * kBackgroundPlateWindow.x,
        screenSize.y * kBackgroundPlateWindow.y};
    glm::vec2 areaSize {
        screenSize.x * (kBackgroundPlateWindow.z - kBackgroundPlateWindow.x),
        screenSize.y * (kBackgroundPlateWindow.w - kBackgroundPlateWindow.y)};
    return fitLayoutInArea(areaOffset, areaSize, layoutSize);
}

void GUI::load(const Gff &gui) {
    auto guiParsed = resource::generated::parseGUI(gui);
    auto type = Control::getType(guiParsed);
    auto tag = Control::getTag(guiParsed);
    auto rootControl = newControl(type, tag);
    rootControl->load(guiParsed);

    _tagToControl.insert({tag, *rootControl});
    _rootControl = *rootControl;
    _controls.push_back(std::move(rootControl));

    for (auto &controlStruct : guiParsed.CONTROLS) {
        loadControl(controlStruct);
    }
    for (auto &[tag, children] : _controlTagToChildren) {
        auto maybeParent = _tagToControl.find(tag);
        if (maybeParent == _tagToControl.end()) {
            throw ValidationException("Parent control not found: " + tag);
        }
        auto &parent = maybeParent->second.get();
        for (auto &child : children) {
            parent.addChildToBack(child);
        }
    }

    applyLayout();
}

void GUI::stretchControl(Control &control) {
    float aspectX = _options.width / static_cast<float>(_resolutionX);
    float aspectY = _options.height / static_cast<float>(_resolutionY);
    control.stretch(aspectX, aspectY);
}

glm::vec2 GUI::scaledFactors() const {
    // The layout always fits the screen, never the background plate. The
    // in-game menu draws its tab strip and content as separate GUIs, and
    // keying the factor on only one background put them in different spaces.
    return screenScaledFactors();
}

glm::vec2 GUI::screenScaledFactors() const {
    // Full uniform fit preserves the authored 4:3 art without leaving the
    // three-quarters margin used by the earlier scaled mode.
    return fitLayoutInArea(
        glm::vec2(0.0f), glm::vec2(_options.width, _options.height), glm::ivec2(_resolutionX, _resolutionY)).factors;
}

void GUI::loadControl(const resource::generated::GUI_CONTROLS &gui) {
    auto type = Control::getType(gui);
    auto tag = Control::getTag(gui);
    auto parentTag = Control::getParent(gui);
    debug(str(boost::format("Loading control: type=%s, tag='%s', parent='%s'") % static_cast<int>(type) % tag % parentTag),
          LogChannel::GUI);

    auto control = newControl(type, tag);
    if (!control) {
        return;
    }
    control->load(gui);
    if (_hasDefaultHilightColor) {
        control->setHilightColor(_defaultHilightColor);
    }

    _tagToControl.insert({tag, *control});
    _controlTagToChildren[parentTag].push_back(*control);
    _controls.push_back(std::move(control));
}

void GUI::positionRelativeToCenter(Control &control) {
    // Anchored controls - HUD icons, portraits, the minimap - scale like
    // everything else, uniformly and aspect-preserved, while keeping their
    // authored screen-edge attachment: the inset from the anchored edge
    // scales with the same factor as the control itself. Before this they
    // kept their native 800x600-era pixel sizes on any screen.
    float s = screenScaledFactors().x;
    Control::Extent extent(control.authoredExtent());
    bool anchorRight = extent.left >= 0.5f * _resolutionX;
    bool anchorBottom = extent.top >= 0.5f * _resolutionY;
    int left = static_cast<int>(extent.left * s);
    int top = static_cast<int>(extent.top * s);
    if (anchorRight) {
        left = _options.width - static_cast<int>((_resolutionX - extent.left) * s);
    }
    if (anchorBottom) {
        top = _options.height - static_cast<int>((_resolutionY - extent.top) * s);
    }
    extent.left = left;
    extent.top = top;
    extent.width = static_cast<int>(extent.width * s);
    extent.height = static_cast<int>(extent.height * s);
    control.setScale(s * Control::kTextScaleFactor);
    control.setExtent(std::move(extent));
}

void GUI::applyLayout() {
    if (!_rootControl) {
        return;
    }

    _rootOffset = {0, 0};
    if (_scaling == ScalingMode::Center) {
        _rootOffset = {screenCenter().x - _resolutionX / 2, screenCenter().y - _resolutionY / 2};
    } else if (_scaling == ScalingMode::CenterHorizontal) {
        _rootOffset.x = screenCenter().x - _resolutionX / 2;
    } else if (_scaling == ScalingMode::Scaled) {
        auto factors = scaledFactors();
        _rootOffset.x = static_cast<int>((_options.width - _resolutionX * factors.x) / 2.0f);
        _rootOffset.y = static_cast<int>((_options.height - _resolutionY * factors.y) / 2.0f);
    }

    for (auto &control : _controls) {
        switch (controlScaling(*control)) {
        case ScalingMode::PositionRelativeToCenter:
            if (control.get() != &_rootControl->get()) {
                positionRelativeToCenter(*control);
            }
            break;
        case ScalingMode::Stretch:
            stretchControl(*control);
            break;
        case ScalingMode::Scaled: {
            auto factors = scaledFactors();
            control->stretch(factors.x, factors.y);
            break;
        }
        default:
            break;
        }

        auto sceneScaling = _sceneScalingByControlTag.find(control->tag());
        if (sceneScaling != _sceneScalingByControlTag.end() && sceneScaling->second == ScalingMode::Stretch) {
            control->setSceneExtent(Control::Extent(0, 0, _options.width, _options.height));
        } else {
            control->setSceneExtent(std::nullopt);
        }
    }

    const Control::Extent &rootExtent = _rootControl->get().extent();
    _controlOffset = _rootOffset + glm::ivec2(rootExtent.left, rootExtent.top);
}

GUI::ScalingMode GUI::controlScaling(const Control &control) const {
    auto scaling = _scalingByControlTag.find(control.tag());
    return scaling != _scalingByControlTag.end() ? scaling->second : _scaling;
}

glm::ivec2 GUI::renderOffset(const Control &control) const {
    switch (controlScaling(control)) {
    case ScalingMode::Stretch:
    case ScalingMode::PositionRelativeToCenter:
        return {0, 0};
    default:
        return &control == &_rootControl->get() ? _rootOffset : _controlOffset;
    }
}

void GUI::setBackground(std::shared_ptr<Texture> texture) {
    _background = std::move(texture);
    if (_scaling == ScalingMode::Scaled) {
        applyLayout();
    }
}

bool GUI::handle(const input::Event &event) {
    switch (event.type) {
    case input::EventType::KeyDown:
        return handleKeyDown(event.key.code);

    case input::EventType::KeyUp:
        return handleKeyUp(event.key.code);

    case input::EventType::MouseMotion: {
        glm::ivec2 ctrlCoords(event.motion.x - _controlOffset.x, event.motion.y - _controlOffset.y);
        updateSelection(ctrlCoords.x, ctrlCoords.y);
        if (_selection) {
            _selection->get().handleMouseMotion(ctrlCoords.x, ctrlCoords.y);
        }
        break;
    }
    case input::EventType::MouseButtonDown:
        if (event.button.button == input::MouseButton::Left) {
            _leftMouseDown = true;
        }
        break;
    case input::EventType::MouseButtonUp:
        if (_leftMouseDown && event.button.button == input::MouseButton::Left) {
            _leftMouseDown = false;
            glm::ivec2 ctrlCoords(event.button.x - _controlOffset.x, event.button.y - _controlOffset.y);
            auto control = findControlAt(
                ctrlCoords.x, ctrlCoords.y,
                [](const auto &control) { return control.isSelectable(); });
            if (control) {
                debug("Control clicked: " + control->get().tag(), LogChannel::GUI);
                onClick(control->get().tag());
                return control->get().handleClick(ctrlCoords.x, ctrlCoords.y, event.button.clicks);
            }
        }
        break;

    case input::EventType::MouseWheel:
        if (_selection && _selection->get().handleMouseWheel(event.wheel.x, event.wheel.y))
            return true;
        break;
    }

    return false;
}

bool GUI::handleKeyDown(input::KeyCode key) {
    return false;
}

bool GUI::handleKeyUp(input::KeyCode key) {
    return false;
}

void GUI::updateSelection(int x, int y) {
    auto control = findControlAt(
        x, y,
        [](const auto &control) { return control.isSelectable(); });
    if ((!_selection && !control) ||
        (_selection && control && _selection->get().id() == control->get().id())) {
        return;
    }
    if (_selection) {
        _selection->get().setSelected(false);
        onSelectionChanged(_selection->get().tag(), false);
    }
    _selection = control;
    if (control) {
        control->get().setSelected(true);
        onSelectionChanged(control->get().tag(), true);
    }
}

std::optional<std::reference_wrapper<Control>> GUI::findControlAt(int x, int y,
                                                                  const std::function<bool(const Control &)> &test) const {
    if (!_rootControl) {
        return std::nullopt;
    }
    std::stack<std::reference_wrapper<Control>> controls;
    controls.push(*_rootControl);
    while (!controls.empty()) {
        auto &control = controls.top().get();
        controls.pop();
        if (control.isVisible() && !control.isDisabled() &&
            control.extent().contains(x, y) &&
            test(control)) {
            return control;
        }
        for (auto &child : control.children()) {
            controls.push(child);
        }
    }
    return std::nullopt;
}

void GUI::update(float dt) {
    if (!_rootControl) {
        return;
    }
    _rootControl->get().update(dt);
}

void GUI::render() {
    _graphicsSvc.renderer2d.withBlendMode(BlendMode::Normal, [this]() {
        if (_background) {
            renderBackground();
        }
        if (!_rootControl) {
            return;
        }
        std::queue<std::pair<std::reference_wrapper<Control>, glm::ivec2>> controls;
        controls.push({*_rootControl, renderOffset(_rootControl->get())});
        while (!controls.empty()) {
            auto &[controlWrapper, offset] = controls.front();
            auto &control = controlWrapper.get();
            control.render({_options.width, _options.height}, offset);
            for (auto &child : control.children()) {
                controls.push({child, renderOffset(child)});
            }
            controls.pop();
        }
    });
}

void GUI::renderOffscreen() {
    if (!_rootControl) {
        return;
    }
    std::queue<std::reference_wrapper<Control>> controls;
    controls.push(*_rootControl);
    while (!controls.empty()) {
        controls.front().get().renderOffscreen();
        for (auto &child : controls.front().get().children()) {
            controls.push(child);
        }
        controls.pop();
    }
}

void GUI::renderBackground() {
    // The background plate is a surround with a framed window. At a different
    // aspect ratio no scaling lines that window up with the fitted layout, so
    // tint it black: it still covers the screen and has no edge to misalign.
    _graphicsSvc.renderer2d.drawImage(
        *_background,
        {0, 0},
        {_options.width, _options.height},
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

void GUI::clearSelection() {
    if (_selection) {
        _selection->get().setSelected(false);
        onSelectionChanged(_selection->get().tag(), false);
        _selection.reset();
    }
}

std::shared_ptr<Control> GUI::findControl(const std::string &tag) const {
    for (auto &control : _controls) {
        if (control->tag() == tag) {
            return control;
        }
    }
    return nullptr;
}

std::unique_ptr<Control> GUI::newControl(
    ControlType type,
    std::string tag) {
    std::unique_ptr<Control> control;
    switch (type) {
    case ControlType::Panel:
        control = std::make_unique<Panel>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::Label:
        control = std::make_unique<Label>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::ImageButton:
        control = std::make_unique<ImageButton>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::Button:
        control = std::make_unique<Button>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::ToggleButton:
        control = std::make_unique<ToggleButton>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::Slider:
        control = std::make_unique<Slider>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::ScrollBar:
        control = std::make_unique<ScrollBar>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::ProgressBar:
        control = std::make_unique<ProgressBar>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::ListBox:
        control = std::make_unique<ListBox>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    case ControlType::IconChain:
        control = std::make_unique<IconChain>(*this, _sceneGraphs, _graphicsSvc, _resourceSvc);
        break;
    default:
        debug("Unsupported control type: " + std::to_string(static_cast<int>(type)), LogChannel::GUI);
        return nullptr;
    }

    control->setTag(tag);

    return control;
}

void GUI::addControlToFront(std::shared_ptr<Control> control) {
    _rootControl->get().addChildToFront(*control);
    _tagToControl.insert({control->tag(), *control});
    _controls.push_back(std::move(control));
}

void GUI::addControlToBack(std::shared_ptr<Control> control) {
    _rootControl->get().addChildToBack(*control);
    _tagToControl.insert({control->tag(), *control});
    _controls.push_back(std::move(control));
}

} // namespace gui

} // namespace reone
