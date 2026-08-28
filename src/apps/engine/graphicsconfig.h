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

#pragma once

#include <string>

#include "reone/graphics/options.h"

namespace reone {

/**
 * Write the graphics options into reone.cfg, leaving foreign lines alone.
 *
 * Lives here rather than in the settings window because the console saves
 * through it too, and a commands file is the only way to exercise a save
 * without a person pressing a button.
 *
 * Returns false and fills \p error on an I/O failure.
 */
bool saveGraphicsOptions(const graphics::GraphicsOptions &options, std::string &error);

} // namespace reone
