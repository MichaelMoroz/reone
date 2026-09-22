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

#include "reone/resource/provider/textures.h"



#include "reone/graphics/format/curreader.h"
#include "reone/graphics/format/tgareader.h"
#include "reone/graphics/format/tpcreader.h"
#include "reone/graphics/format/txireader.h"
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/types.h"
#include "reone/resource/resources.h"
#include "reone/system/logutil.h"
#include "reone/system/stream/memoryinput.h"
#include "reone/system/threadutil.h"

using namespace reone::graphics;

namespace reone {

namespace resource {

void Textures::init() {
}

void Textures::clear() {
    _cache.clear();
}

std::shared_ptr<Texture> Textures::get(const std::string &resRef, TextureUsage usage) {
    if (resRef.empty()) {
        return nullptr;
    }
    const auto lcResRef = boost::to_lower_copy(resRef);
    return _cache.getOrAdd({lcResRef, usage},
                           [this, &lcResRef, usage]() { return doGet(lcResRef, usage); });
}

std::shared_ptr<Texture> Textures::doGet(const std::string &resRef, TextureUsage usage) {
    std::shared_ptr<Texture> texture;
    std::optional<Texture::Features> features;

    auto txiRes = _resources.find(ResourceId(resRef, ResType::Txi));
    if (txiRes) {
        auto txi = MemoryInputStream(txiRes->data);
        auto txiReader = TxiReader();
        txiReader.load(txi);
        features = txiReader.features();
    }

    auto tgaRes = _resources.find(ResourceId(resRef, ResType::Tga));
    if (tgaRes) {
        auto tga = MemoryInputStream(tgaRes->data);
        auto tgaReader = TgaReader(tga, resRef, usage);
        tgaReader.load();
        texture = tgaReader.texture();
        if (texture && features) {
            texture->setFeatures(*features);
        }
    }

    if (!texture) {
        auto tpcRes = _resources.find(ResourceId(resRef, ResType::Tpc));
        if (tpcRes) {
            auto tpc = MemoryInputStream(tpcRes->data);
            auto tpcReader = TpcReader(tpc, resRef, usage);
            tpcReader.load();
            texture = tpcReader.texture();
            if (texture) {
                if (features) {
                    texture->setFeatures(*features);
                } else {
                    features = texture->features();
                }
            }
        }
    }

    if (texture) {
        // The material schema samples grayscale bump maps through a
        // Sampler2DArray even when there is only one frame. Keep that view
        // contract true at the resource boundary; otherwise the material id
        // addresses an unpopulated array-descriptor slot.
        if (usage == TextureUsage::BumpMap && texture->isGrayscale() &&
            texture->is2D()) {
            if (features &&
                features->procedureType != Texture::ProcedureType::Invalid &&
                (features->numX > 1 || features->numY > 1)) {
                convertGridTextureToArray(*texture, features->numX, features->numY);
            } else {
                texture->setType(TextureType::TwoDimArray);
            }
        }
        // An animation arrives from the reader as one layer per frame, which
        // is what the file holds. Only the bumpmap slot can index a layer, so
        // every other slot gets those frames laid out as a sheet instead, which
        // the UV transform then windows a frame at a time.
        //
        // This used to run the other way round - the reader handed over one
        // flat image and this sliced it into an array, whatever slot it was
        // headed for. The image it sliced was frame 0's mip chain and the other
        // frames read as one picture, so the slices were quarters of that; and
        // for a diffuse the array then sat in a bindless table its material id
        // never addressed, which is why the Taris force field drew nothing at
        // all here and one frozen frame in the OpenGL build.
        if (features &&
            features->procedureType != Texture::ProcedureType::Invalid &&
            (features->numX > 1 || features->numY > 1)) {
            if (usage != TextureUsage::BumpMap) {
                convertArrayTextureToGrid(*texture, features->numX, features->numY);
            }
            debug("Texture '" + resRef + "': " + std::to_string(features->numX) + "x" +
                      std::to_string(features->numY) + " frame grid, " +
                      (usage == TextureUsage::BumpMap ? "kept as layers" : "laid out as a sheet"),
                  LogChannel::Graphics);
        }
        float anisotropy = std::max(1.0f, exp2f(_options.anisotropicFiltering));
        texture->setAnisotropy(anisotropy);
        texture->init();
    } else {
        warn("Texture not found: " + resRef, LogChannel::Graphics);
    }

    return texture;
}

} // namespace resource

} // namespace reone
