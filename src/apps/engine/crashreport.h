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

#pragma once

namespace reone {

/**
 * Write a symbolized stack into the log when the process dies of a hardware
 * fault, instead of vanishing.
 *
 * An access violation leaves nothing behind today: the process exits
 * 0xC0000005, the log stops mid-sentence wherever the last flush happened, and
 * what faulted is a guess. That guess has been wrong repeatedly - a mode-switch
 * crash was attributed in turn to resource release, to a registry
 * classification and to a duplicated rule in a UI widget, each plausible, each
 * costing a build and a round trip, because nothing said which function was on
 * the stack.
 *
 * Installed once from main and never removed. Does nothing on platforms without
 * an implementation.
 */
void installCrashReporter();

} // namespace reone
