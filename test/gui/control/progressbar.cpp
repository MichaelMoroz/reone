/*
 * Copyright (c) 2026 The reone project contributors
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

#include <gtest/gtest.h>

#include "../../fixtures/graphics.h"
#include "../../fixtures/gui.h"
#include "../../fixtures/resource.h"
#include "../../fixtures/scene.h"
#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/texture.h"
#include "reone/gui/control/progressbar.h"

using namespace reone;
using namespace reone::graphics;
using namespace reone::gui;
using namespace reone::resource;
using namespace reone::scene;
using namespace testing;

namespace {

/** Records the rect of every image a control draws, in draw order. */
class RecordingRenderer2d : public NiceMock<Mock2DRenderer> {
public:
    struct ImageDraw {
        glm::ivec2 position;
        glm::ivec2 size;
    };

    RecordingRenderer2d() {
        ON_CALL(*this,
                drawImage(_, An<const glm::vec2 &>(), An<const glm::vec2 &>(), _, _, _))
            .WillByDefault(Invoke([this](Texture &,
                                         const glm::vec2 &position,
                                         const glm::vec2 &size,
                                         const glm::vec4 &,
                                         const glm::mat3x4 &,
                                         ImageAlphaMode) {
                imageDraws.push_back({glm::ivec2(position), glm::ivec2(size)});
            }));
        ON_CALL(*this, withBlendMode(_, _))
            .WillByDefault(Invoke([](BlendMode, const std::function<void()> &block) { block(); }));
        ON_CALL(*this, withScissor(_, _))
            .WillByDefault(Invoke([](const glm::ivec4 &, const std::function<void()> &block) { block(); }));
    }

    std::vector<ImageDraw> imageDraws;
};

class ProgressBarTest : public Test {
protected:
    void SetUp() override {
        _graphicsModule.init();
        _resourceModule.init();
        _sceneModule.init();

        _fill = std::make_shared<Texture>(
            "fill",
            TextureType::TwoDim,
            Texture::Properties {});
        EXPECT_CALL(_resourceModule.textures(), get("fill", TextureUsage::GUI))
            .WillOnce(Return(_fill));
    }

    std::unique_ptr<ProgressBar> newProgressBar(Control::Extent extent, bool startFromLeft) {
        resource::generated::GUI_CONTROLS control;
        control.CONTROLTYPE = static_cast<int>(ControlType::ProgressBar);
        control.EXTENT.LEFT = extent.left;
        control.EXTENT.TOP = extent.top;
        control.EXTENT.WIDTH = extent.width;
        control.EXTENT.HEIGHT = extent.height;
        control.STARTFROMLEFT = startFromLeft;
        control.PROGRESS.emplace();
        control.PROGRESS->FILL = "fill";

        auto bar = std::make_unique<ProgressBar>(
            _gui,
            _sceneModule.graphs(),
            _graphicsModule.services(),
            _resourceModule.services());
        bar->load(control, false);
        return bar;
    }

    NiceMock<MockGUI> _gui;
    TestGraphicsModule _graphicsModule;
    TestResourceModule _resourceModule;
    TestSceneModule _sceneModule;
    std::shared_ptr<Texture> _fill;
};

TEST_F(ProgressBarTest, vertical_fill_keeps_bottom_edge_and_grows_monotonically) {
    auto bar = newProgressBar({10, 100, 7, 19}, true);
    RecordingRenderer2d renderer2d;
    int previousHeight = 0;

    for (int value = 0; value <= 100; ++value) {
        renderer2d.imageDraws.clear();
        bar->setValue(value);
        bar->render({640, 480}, {3, 0}, renderer2d);

        if (value == 0) {
            EXPECT_TRUE(renderer2d.imageDraws.empty());
            continue;
        }

        ASSERT_EQ(renderer2d.imageDraws.size(), 1u) << "value = " << value;
        const auto &draw = renderer2d.imageDraws.front();
        EXPECT_EQ(draw.position.y + draw.size.y, 119) << "value = " << value;
        EXPECT_GE(draw.size.y, previousHeight) << "value = " << value;
        previousHeight = draw.size.y;
    }
}

TEST_F(ProgressBarTest, right_anchored_horizontal_fill_keeps_right_edge_and_grows_monotonically) {
    auto bar = newProgressBar({100, 10, 19, 7}, false);
    RecordingRenderer2d renderer2d;
    int previousWidth = 0;

    for (int value = 0; value <= 100; ++value) {
        renderer2d.imageDraws.clear();
        bar->setValue(value);
        bar->render({640, 480}, {5, 2}, renderer2d);

        if (value == 0) {
            EXPECT_TRUE(renderer2d.imageDraws.empty());
            continue;
        }

        ASSERT_EQ(renderer2d.imageDraws.size(), 1u) << "value = " << value;
        const auto &draw = renderer2d.imageDraws.front();
        EXPECT_EQ(draw.position.x + draw.size.x, 124) << "value = " << value;
        EXPECT_GE(draw.size.x, previousWidth) << "value = " << value;
        previousWidth = draw.size.x;
    }
}

} // namespace
