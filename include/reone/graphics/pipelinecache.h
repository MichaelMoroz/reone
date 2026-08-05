/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <string>
#include <vector>

#include "rhi.h"
#include "types.h"

namespace reone {

namespace graphics {

/** Pipeline state selected by the 2D and image-based-lighting clients. */
struct PipelineKey {
    std::string module;
    std::string vertexEntry;
    std::string fragmentEntry;
    std::vector<Format> colorFormats;
    Format depthFormat {Format::D32Sfloat};
    uint32_t viewMask {0};
    BlendMode blend {BlendMode::None};
    bool depthTest {false};
    bool depthWrite {false};
    bool depthBias {false};
    float depthBiasConstantFactor {0.0f};
    float depthBiasSlopeFactor {0.0f};
    FaceCullMode cull {FaceCullMode::None};
};

struct PipelineBinding {
    Pipeline pipeline;
    PipelineLayout layout;
};

/** Pipeline-cache operation used by the 2D and image-based-lighting clients. */
class IPipelineCache {
public:
    virtual ~IPipelineCache() = default;

    virtual PipelineBinding get(const PipelineKey &key) = 0;
};

} // namespace graphics

} // namespace reone
