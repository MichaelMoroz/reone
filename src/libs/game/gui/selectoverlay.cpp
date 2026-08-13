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

#include "reone/game/gui/selectoverlay.h"

#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/font.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/provider/fonts.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/resources.h"

#include "reone/game/action/attackobject.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/action/useskill.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/party.h"

using namespace reone::graphics;
using namespace reone::resource;

namespace reone {

namespace game {

static constexpr int kOffsetToReticle = 8;
static constexpr int kTitleBarWidth = 250;
static constexpr int kTitleBarPadding = 6;
static constexpr int kHealthBarHeight = 6;
static constexpr float kObjectTitleScale = 2.0f / 3.0f;
static constexpr int kNumActionSlots = 3;
static constexpr int kActionBarMargin = 3;
static constexpr int kActionBarPadding = 3;
static constexpr int kActionWidth = 35;
static constexpr int kActionHeight = 59;
static constexpr int kActionArrowHeight = (kActionHeight - kActionWidth) / 2;

static void cycleActionSlot(ActionSlot &slot, bool previous) {
    if (slot.actions.empty())
        return;

    if (previous) {
        if (slot.indexSelected == 0) {
            slot.indexSelected = static_cast<uint32_t>(slot.actions.size() - 1);
        } else {
            --slot.indexSelected;
        }
    } else if (++slot.indexSelected == slot.actions.size()) {
        slot.indexSelected = 0;
    }
}

SelectionOverlay::SelectionOverlay(
    Game &game,
    ServicesView &services) :
    _game(game),
    _services(services) {
    _actionSlots.resize(kNumActionSlots);
}

void SelectionOverlay::init() {
    _font = _services.resource.fonts.get("dialogfont16x16");
    _friendlyReticle = _services.resource.textures.get("friendlyreticle", TextureUsage::GUI);
    _friendlyReticle2 = _services.resource.textures.get("friendlyreticle2", TextureUsage::GUI);
    _hostileReticle = _services.resource.textures.get("hostilereticle", TextureUsage::GUI);
    _hostileReticle2 = _services.resource.textures.get("hostilereticle2", TextureUsage::GUI);
    _friendlyScroll = _services.resource.textures.get("lbl_miscroll_f", TextureUsage::GUI);
    _hostileScroll = _services.resource.textures.get("lbl_miscroll_h", TextureUsage::GUI);
    _hilightedScroll = _services.resource.textures.get("lbl_miscroll_hi", TextureUsage::GUI);
    _actionArrow = _services.resource.textures.get("lbl_miarr_1", TextureUsage::GUI);
    _hilightedActionArrow = _services.resource.textures.get("lbl_miarr_2", TextureUsage::GUI);
    _reticleHeight = _friendlyReticle2->height();
}

bool SelectionOverlay::handle(const input::Event &event) {
    switch (event.type) {
    case input::EventType::MouseMotion:
        return handleMouseMotion(event.motion);
    case input::EventType::MouseButtonDown:
        return handleMouseButtonDown(event.button);
    case input::EventType::MouseWheel:
        return handleMouseWheel(event.wheel);
    default:
        return false;
    }
}

bool SelectionOverlay::handleMouseMotion(const input::MouseMotionEvent &event) {
    _selectedActionSlot = -1;
    _hilightedActionBand = ActionBand::None;

    if (!_selectedObject)
        return false;

    for (int i = 0; i < kNumActionSlots; ++i) {
        float x, y;
        getActionScreenCoords(i, x, y);
        float scale = layoutScale();
        if (event.x >= x && event.y >= y && event.x < x + kActionWidth * scale && event.y < y + kActionHeight * scale) {
            _selectedActionSlot = i;
            float actionY = event.y - y;
            if (actionY < kActionArrowHeight * scale) {
                _hilightedActionBand = ActionBand::Previous;
            } else if (actionY >= (kActionArrowHeight + kActionWidth) * scale) {
                _hilightedActionBand = ActionBand::Next;
            } else {
                _hilightedActionBand = ActionBand::Icon;
            }
            return true;
        }
    }

    return false;
}

bool SelectionOverlay::handleMouseButtonDown(const input::MouseButtonEvent &event) {
    if (event.button != input::MouseButton::Left)
        return false;
    if (_selectedActionSlot == -1 || _selectedActionSlot >= _actionSlots.size())
        return false;

    ActionSlot &slot = _actionSlots[_selectedActionSlot];

    float frameX, frameY;
    getActionScreenCoords(_selectedActionSlot, frameX, frameY);

    float scale = layoutScale();
    if (event.x < frameX || event.y < frameY ||
        event.x >= frameX + kActionWidth * scale || event.y >= frameY + kActionHeight * scale)
        return false;

    float actionY = event.y - frameY;
    if (actionY < kActionArrowHeight * scale) {
        cycleActionSlot(slot, true);
        return true;
    }
    if (actionY >= (kActionArrowHeight + kActionWidth) * scale) {
        cycleActionSlot(slot, false);
        return true;
    }

    std::shared_ptr<Creature> leader(_game.party().getLeader());
    if (!leader)
        return false;

    std::shared_ptr<Area> area(_game.module()->area());
    auto selectedObject = area->selectedObject();
    if (!selectedObject)
        return false;

    if (slot.indexSelected >= slot.actions.size())
        return false;

    const ContextAction &ctxAction = slot.actions[slot.indexSelected];
    std::shared_ptr<Action> action;
    switch (ctxAction.type) {
    case ActionType::AttackObject:
        action = _game.newAction<AttackObjectAction>(selectedObject);
        break;
    case ActionType::UseFeat:
        action = _game.newAction<UseFeatAction>(ctxAction.feat, selectedObject);
        break;
    case ActionType::UseSkill:
        action = _game.newAction<UseSkillAction>(ctxAction.skill, selectedObject);
        break;
    case ActionType::CastSpellAtObject: {
        std::optional<std::shared_ptr<Item>> item =
            ctxAction.item ? std::optional(ctxAction.item) : std::nullopt;

        action = _game.newAction<CastSpellAtObjectAction>(
            ctxAction.spell, selectedObject, item);
        break;
    }
    default:
        break;
    }
    if (action) {
        action->setUserAction(true);
        leader->addAction(std::move(action));
    }

    return true;
}

bool SelectionOverlay::handleMouseWheel(const input::MouseWheelEvent &event) {
    if (_selectedActionSlot == -1 || _selectedActionSlot >= _actionSlots.size())
        return false;

    ActionSlot &slot = _actionSlots[_selectedActionSlot];
    if (slot.actions.empty())
        return false;

    cycleActionSlot(slot, event.y > 0);
    return true;
}

void SelectionOverlay::update() {
    // TODO: update on selection change only

    _hilightedObject.reset();
    _hilightedHostile = false;

    _selectedObject.reset();
    _selectedHostile = false;

    std::shared_ptr<Module> module(_game.module());
    std::shared_ptr<Area> area(module->area());

    auto camera = _game.getActiveCamera();
    glm::mat4 projection(camera->cameraSceneNode()->camera()->projection());
    glm::mat4 view(camera->cameraSceneNode()->camera()->view());

    auto hilightedObject = area->hilightedObject();
    if (hilightedObject) {
        _hilightedScreenCoords = area->getSelectableScreenCoords(hilightedObject, projection, view);

        if (_hilightedScreenCoords.z < 1.0f) {
            _hilightedObject = hilightedObject;

            auto hilightedCreature = std::dynamic_pointer_cast<Creature>(hilightedObject);
            if (hilightedCreature) {
                _hilightedHostile = module->isHostileToPartyLeader(*hilightedCreature);
            }
        }
    }

    auto selectedObject = area->selectedObject();
    if (selectedObject) {
        _selectedScreenCoords = area->getSelectableScreenCoords(selectedObject, projection, view);

        if (_selectedScreenCoords.z < 1.0f) {
            _selectedObject = selectedObject;

            for (int i = 0; i < kNumActionSlots; ++i) {
                _actionSlots[i].actions.clear();
            }
            std::vector<ContextAction> actions(module->getContextActions(selectedObject));
            _hasActions = !actions.empty();
            if (_hasActions) {
                for (auto &action : actions) {
                    switch (action.type) {
                    case ActionType::AttackObject:
                    case ActionType::UseFeat:
                        _actionSlots[0].actions.push_back(action);
                        break;
                    case ActionType::UseSkill:
                        _actionSlots[1].actions.push_back(action);
                        break;
                    case ActionType::CastSpellAtObject: {
                        if (action.item) {
                            _actionSlots[2].actions.push_back(action);
                        } else {
                            _actionSlots[1].actions.push_back(action);
                        }
                    }
                    default:
                        break;
                    }
                }
            }
            for (int i = 0; i < kNumActionSlots; ++i) {
                if (_actionSlots[i].indexSelected >= _actionSlots[i].actions.size()) {
                    _actionSlots[i].indexSelected = 0;
                }
            }

            auto selectedCreature = std::dynamic_pointer_cast<Creature>(selectedObject);
            if (selectedCreature) {
                _selectedHostile = module->isHostileToPartyLeader(*selectedCreature);
            }
        }
    }
}

void SelectionOverlay::render() {
    _services.graphics.renderer2d.withBlendMode(BlendMode::Normal, [this]() {
        if (_hilightedObject) {
            renderReticle(_hilightedHostile ? _hostileReticle : _friendlyReticle, _hilightedScreenCoords);
        }
        if (_selectedObject) {
            renderReticle(_selectedHostile ? _hostileReticle2 : _friendlyReticle2, _selectedScreenCoords);
            renderActionBar();
            renderTitleBar();
            renderHealthBar();
        }
    });
}

void SelectionOverlay::renderReticle(std::shared_ptr<Texture> texture, const glm::vec3 &screenCoords) {
    const GraphicsOptions &opts = _game.options().graphics;
    float scale = layoutScale();
    float width = texture->width() * scale;
    float height = texture->height() * scale;

    glm::vec2 position(
        (opts.width * screenCoords.x) - width / 2,
        (opts.height * (1.0f - screenCoords.y)) - height / 2);
    _services.graphics.renderer2d.drawImage(*texture, position, {width, height});
}

void SelectionOverlay::renderTitleBar() {
    if (_selectedObject->name().empty())
        return;

    const GraphicsOptions &opts = _game.options().graphics;
    float scale = layoutScale();
    float titleScale = scale * kObjectTitleScale;
    float fontScale = titleScale * opts.guiTextScale * (_game.isTSL() ? 1.0f : kK1CombatTextScale);
    float barWidth = kTitleBarWidth * titleScale;
    float barHeight = _font->height() * fontScale + kTitleBarPadding * titleScale;
    float reticleHeight = _reticleHeight * scale;
    float offsetToReticle = kOffsetToReticle * scale;
    float healthBarHeight = kHealthBarHeight * titleScale;
    float actionHeight = kActionHeight * scale;
    float actionMargin = kActionBarMargin * scale;
    {
        float x = opts.width * _selectedScreenCoords.x - barWidth / 2;
        float y = opts.height * (1.0f - _selectedScreenCoords.y) - reticleHeight / 2.0f - barHeight - offsetToReticle - healthBarHeight - scale;

        if (_hasActions) {
            y -= actionHeight + 2 * actionMargin;
        }
        _services.graphics.renderer2d.drawRect(
            {x, y},
            {barWidth, barHeight},
            glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
    }
    {
        float x = opts.width * _selectedScreenCoords.x;
        float y = opts.height * (1.0f - _selectedScreenCoords.y) - (reticleHeight + barHeight) / 2 - offsetToReticle - healthBarHeight - scale;
        if (_hasActions) {
            y -= actionHeight + 2 * actionMargin;
        }
        glm::vec3 position(x, y, 0.0f);
        _font->render(_selectedObject->name(), position, getColorFromSelectedObject(), TextGravity::CenterCenter, fontScale);
    }
}

void SelectionOverlay::renderHealthBar() {
    const GraphicsOptions &opts = _game.options().graphics;
    float scale = layoutScale();
    float titleScale = scale * kObjectTitleScale;
    float barWidth = kTitleBarWidth * titleScale;
    float healthBarHeight = kHealthBarHeight * titleScale;
    float x = opts.width * _selectedScreenCoords.x - barWidth / 2;
    float y = opts.height * (1.0f - _selectedScreenCoords.y) - _reticleHeight * scale / 2.0f - healthBarHeight - kOffsetToReticle * scale;
    float w = glm::clamp(_selectedObject->currentHitPoints() / static_cast<float>(_selectedObject->hitPoints()), 0.0f, 1.0f) * barWidth;

    if (_hasActions) {
        y -= (kActionHeight + 2 * kActionBarMargin) * scale;
    }
    _services.graphics.renderer2d.drawRect(
        {x, y},
        {w, healthBarHeight},
        glm::vec4(getColorFromSelectedObject(), 1.0f));
}

void SelectionOverlay::renderActionBar() {
    if (!_hasActions)
        return;

    for (int i = 0; i < kNumActionSlots; ++i) {
        renderActionFrame(i);
        renderActionArrows(i);
        renderActionIcon(i);
    }
}

void SelectionOverlay::renderActionFrame(int index) {
    std::shared_ptr<Texture> frameTexture;
    if (index == _selectedActionSlot) {
        frameTexture = _hilightedScroll;
    } else if (_selectedHostile) {
        frameTexture = _hostileScroll;
    } else {
        frameTexture = _friendlyScroll;
    }
    float frameX, frameY;
    getActionScreenCoords(index, frameX, frameY);
    float scale = layoutScale();

    _services.graphics.renderer2d.drawImage(
        *frameTexture,
        {frameX, frameY},
        {kActionWidth * scale, kActionHeight * scale});
}

void SelectionOverlay::renderActionArrows(int index) {
    if (_actionSlots[index].actions.size() < 2)
        return;

    renderActionArrow(index, true);
    renderActionArrow(index, false);
}

void SelectionOverlay::renderActionArrow(int index, bool previous) {
    bool hilighted = index == _selectedActionSlot &&
                     _hilightedActionBand == (previous ? ActionBand::Previous : ActionBand::Next);
    auto texture = hilighted ? _hilightedActionArrow : _actionArrow;

    float frameX, frameY;
    getActionScreenCoords(index, frameX, frameY);
    float scale = layoutScale();

    // The "next" arrow is the "previous" one turned around.
    auto uv = previous ? glm::mat3x4(1.0f)
                       : glm::mat3x4(
                             glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f),
                             glm::vec4(0.0f, -1.0f, 0.0f, 0.0f),
                             glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
    _services.graphics.renderer2d.drawImage(
        *texture,
        {frameX, previous ? frameY : frameY + (kActionArrowHeight + kActionWidth) * scale},
        {kActionWidth * scale, kActionArrowHeight * scale},
        glm::vec4(1.0f),
        uv);
}

bool SelectionOverlay::getActionScreenCoords(int index, float &x, float &y) const {
    if (!_selectedObject)
        return false;

    const GraphicsOptions &opts = _game.options().graphics;
    float scale = layoutScale();
    x = opts.width * _selectedScreenCoords.x + ((static_cast<float>(index - 1) - 0.5f) * kActionWidth + (index - 1) * kActionBarMargin) * scale;
    y = opts.height * (1.0f - _selectedScreenCoords.y) - (_reticleHeight / 2.0f + kActionHeight + kOffsetToReticle + kActionBarMargin) * scale;

    return true;
}

void SelectionOverlay::renderActionIcon(int index) {
    const ActionSlot &slot = _actionSlots[index];
    if (slot.indexSelected >= slot.actions.size())
        return;

    const ContextAction &action = slot.actions[slot.indexSelected];

    float frameX, frameY;
    getActionScreenCoords(index, frameX, frameY);

    const GraphicsOptions &opts = _game.options().graphics;
    float scale = layoutScale();
    float y = opts.height * (1.0f - _selectedScreenCoords.y) - ((_reticleHeight + kActionHeight + kActionWidth) / 2.0f + kOffsetToReticle + kActionBarMargin) * scale;

    glm::mat4 transform(1.0f);
    transform = glm::translate(transform, glm::vec3(frameX, y, 0.0f));
    transform = glm::scale(transform, glm::vec3(kActionWidth * scale, kActionWidth * scale, 1.0f));

    renderContextActionIcon(action, transform, _services);
}

float SelectionOverlay::layoutScale() const {
    const auto &opts = _game.options().graphics;
    return std::min(opts.width / 800.0f, opts.height / 600.0f) * opts.guiScale;
}

glm::vec3 SelectionOverlay::getColorFromSelectedObject() const {
    static glm::vec3 red(1.0f, 0.0f, 0.0f);

    auto guiColorBase = _game.isTSL() ? kTSLGUIColorBase : kGUIColorBase;

    return (_selectedObject && _selectedHostile) ? red : guiColorBase;
}

} // namespace game

} // namespace reone
