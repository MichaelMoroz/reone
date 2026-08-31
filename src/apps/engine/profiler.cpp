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

#include "profiler.h"

#include "reone/graphics/di/services.h"
#include "reone/system/checkutil.h"
#include "reone/system/clock.h"
#include "reone/system/di/services.h"
#include "reone/system/stringbuilder.h"

using namespace reone::game;
using namespace reone::graphics;

namespace reone {

static constexpr int kNumTimedFrames = 100;
static constexpr float kFrameTimesScale = 2.0f;

void Profiler::init() {
    checkThat(!_inited, "Must not be initialized");
    _inited = true;
}

void Profiler::deinit() {
    if (!_inited) {
        return;
    }
    _inited = false;
}

bool Profiler::handle(const input::Event &event) {
    if (event.type != input::EventType::KeyDown) {
        return false;
    }
    bool enabled = _enabled.load(std::memory_order::memory_order_acquire);
    if (event.key.code == input::KeyCode::F5) {
        _enabled.store(!enabled, std::memory_order::memory_order_release);
        return true;
    }
    if (!enabled) {
        return false;
    }
    switch (event.key.code) {
    case input::KeyCode::Key1:
        _fpsTarget = 30.0f;
        return true;
    case input::KeyCode::Key2:
        _fpsTarget = 60.0f;
        return true;
    case input::KeyCode::Key3:
        _fpsTarget = 120.0f;
        return true;
    case input::KeyCode::Key4:
        _fpsTarget = 240.0f;
        return true;
    default:
        return false;
    }
}

void Profiler::update(float dt) {
    if (!_enabled.load(std::memory_order::memory_order_acquire)) {
        return;
    }
}

void Profiler::reserveThread(std::string name, std::vector<glm::vec3> colors) {
    if (_nameToTimedThread.count(name) > 0) {
        return;
    }
    checkLessOrEqual("timed thread count", _numTimedThreads, kMaxTimedThreads);
    _timedThreads[_numTimedThreads].name = std::move(name);
    _timedThreads[_numTimedThreads].colors = std::move(colors);
    auto &reserved = _timedThreads[_numTimedThreads];
    _nameToTimedThread.insert({reserved.name, reserved});
    ++_numTimedThreads;
}

void Profiler::measure(const std::string &threadName,
                       int timeIndex,
                       const std::function<void()> &block) {
    uint64_t before = _systemSvc.clock.micros();
    block();
    uint64_t after = _systemSvc.clock.micros();
    checkThat(0 <= timeIndex && timeIndex < 4, "timeIndex must be between 0 and 3");
    checkThat(_nameToTimedThread.count(threadName) > 0, "Timed thread must be reserved");
    auto &thread = _nameToTimedThread.at(threadName).get();
    std::lock_guard<std::mutex> lock {thread.mutex};
    auto &times = thread.times[timeIndex];
    if (times.size() == kNumTimedFrames) {
        times.pop_front();
    }
    times.push_back((after - before) / 1e6f);
    thread.sums[timeIndex] += (after - before) / 1e6;
    ++thread.counts[timeIndex];
}

void Profiler::resetAccumulation(const std::string &threadName) {
    auto found = _nameToTimedThread.find(threadName);
    if (found == _nameToTimedThread.end()) {
        return;
    }
    auto &thread = found->second.get();
    std::lock_guard<std::mutex> lock {thread.mutex};
    thread.sums = {};
    thread.counts = {};
}

std::array<std::pair<double, uint64_t>, 4> Profiler::accumulation(const std::string &threadName) const {
    std::array<std::pair<double, uint64_t>, 4> result {};
    auto found = _nameToTimedThread.find(threadName);
    if (found == _nameToTimedThread.end()) {
        return result;
    }
    const auto &thread = found->second.get();
    std::lock_guard<std::mutex> lock {thread.mutex};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = {thread.sums[i], thread.counts[i]};
    }
    return result;
}

std::array<std::vector<float>, 4> Profiler::frameTimes(const std::string &threadName) const {
    std::array<std::vector<float>, 4> result;
    auto found = _nameToTimedThread.find(threadName);
    if (found == _nameToTimedThread.end()) {
        return result;
    }
    const auto &thread = found->second.get();
    std::lock_guard<std::mutex> lock {thread.mutex};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i].assign(thread.times[i].begin(), thread.times[i].end());
    }
    return result;
}

} // namespace reone
