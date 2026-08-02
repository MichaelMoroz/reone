/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "image.h"
#include "skybaker.h"

using namespace reone::skybake;

namespace {

enum class CubeFace {
    PositiveX,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ,
};

struct FaceInfo {
    CubeFace face;
    const char *filename;
    const char *label;
    glm::vec3 normal;
    glm::vec3 tangentU;
    glm::vec3 tangentV;
    glm::u8vec3 debugColor;
};

const std::array<FaceInfo, 6> kFaces {{
    {CubeFace::PositiveX, "px.png", "+X", {1, 0, 0}, {0, 0, -1}, {0, -1, 0}, {255, 0, 0}},
    {CubeFace::NegativeX, "nx.png", "-X", {-1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 255, 255}},
    {CubeFace::PositiveY, "py.png", "+Y", {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 255, 0}},
    {CubeFace::NegativeY, "ny.png", "-Y", {0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {255, 0, 255}},
    {CubeFace::PositiveZ, "pz.png", "+Z", {0, 0, 1}, {1, 0, 0}, {0, -1, 0}, {0, 0, 255}},
    {CubeFace::NegativeZ, "nz.png", "-Z", {0, 0, -1}, {-1, 0, 0}, {0, -1, 0}, {255, 255, 0}},
}};

const FaceInfo &faceForDirection(const glm::vec3 &direction) {
    const glm::vec3 absolute = glm::abs(direction);
    if (absolute.x >= absolute.y && absolute.x >= absolute.z)
        return kFaces[direction.x >= 0 ? 0 : 1];
    if (absolute.y >= absolute.z)
        return kFaces[direction.y >= 0 ? 2 : 3];
    return kFaces[direction.z >= 0 ? 4 : 5];
}

Image makeEquirectangularDebug(int width, int height) {
    Image image(width, height);
    constexpr float kPi = 3.14159265358979323846f;
    for (int y = 0; y < height; ++y) {
        const float latitude = (0.5f - (y + 0.5f) / height) * kPi;
        for (int x = 0; x < width; ++x) {
            const float longitude = ((x + 0.5f) / width * 2.0f - 1.0f) * kPi;
            const glm::vec3 worldDirection {
                std::cos(latitude) * std::cos(longitude),
                std::cos(latitude) * std::sin(longitude),
                std::sin(latitude),
            };
            image.setPixel(x, y, faceForDirection(worldDirection).debugColor);
        }
    }
    return image;
}

void writeSkyIni(const std::filesystem::path &outDir, int faceSize) {
    std::ofstream out(outDir / "sky.ini");
    out << "[sky]\n"
        << "format = png\n"
        << "size = " << faceSize << "\n";
    for (const auto &face : kFaces)
        out << face.label << " = " << face.filename << "\n";
}

void debugFaces(const std::filesystem::path &outDir, int faceSize) {
    std::filesystem::create_directories(outDir);
    for (const auto &face : kFaces)
        writePng(outDir / face.filename, Image(faceSize, faceSize, face.debugColor));
    writePng(outDir / "equirectangular.png", makeEquirectangularDebug(2 * faceSize, faceSize));
    writeSkyIni(outDir, faceSize);

    std::cout << "World direction -> cubemap face mapping (world direction is sampled directly):\n";
    for (const auto &face : kFaces)
        std::cout << "  world " << face.label << " -> " << face.filename << '\n';
    std::cout << "KOTOR +Z (up) therefore maps to pz.png; cubemap +Y is world +Y, not world up.\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        namespace po = boost::program_options;
        po::options_description options("skybake options");
        options.add_options()("help,h", "show help")("debug-faces", "write the six-colour cubemap orientation fixture")("survey", "survey all modules and draft modules.ini")("bake", "bake skies named by modules.ini")("game", po::value<std::string>(), "game installation directory")("id", po::value<std::string>(), "game id: k1 or k2")("config", po::value<std::string>(), "input modules.ini")("out", po::value<std::string>(), "output path")("size", po::value<int>()->default_value(512), "face size in pixels (0: largest source texture)")("force", "regenerate even when outputs are up to date");

        po::variables_map args;
        po::store(po::parse_command_line(argc, argv, options), args);
        po::notify(args);
        if (args.count("help") || argc == 1) {
            std::cout << options << '\n';
            return 0;
        }
        const int modes = static_cast<int>(args.count("debug-faces")) +
                          static_cast<int>(args.count("survey")) +
                          static_cast<int>(args.count("bake"));
        if (modes != 1)
            throw std::invalid_argument("Exactly one mode is required: --debug-faces, --survey, or --bake");
        if (!args.count("out"))
            throw std::invalid_argument("--out is required");
        const int size = args["size"].as<int>();
        if (size < 0)
            throw std::invalid_argument("--size must not be negative");
        if (args.count("debug-faces")) {
            debugFaces(args["out"].as<std::string>(), size == 0 ? 512 : size);
            return 0;
        }
        if (!args.count("game") || !args.count("id"))
            throw std::invalid_argument("--game and --id are required");
        const auto id = boost::to_lower_copy(args["id"].as<std::string>());
        if (id != "k1" && id != "k2")
            throw std::invalid_argument("--id must be k1 or k2");
        RunOptions runOptions;
        runOptions.gameId = id == "k1" ? reone::resource::GameID::KotOR : reone::resource::GameID::TSL;
        runOptions.gameDir = args["game"].as<std::string>();
        runOptions.outPath = args["out"].as<std::string>();
        runOptions.faceSize = size;
        runOptions.force = args.count("force") != 0;
        if (!std::filesystem::is_directory(runOptions.gameDir))
            throw std::invalid_argument("Game directory not found: " + runOptions.gameDir.string());
        if (args.count("survey")) {
            runSurvey(runOptions);
        } else {
            if (!args.count("config"))
                throw std::invalid_argument("--config is required for --bake");
            runOptions.configPath = args["config"].as<std::string>();
            runBake(runOptions);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "skybake: " << e.what() << '\n';
        return 1;
    }
}
