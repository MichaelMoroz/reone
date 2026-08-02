/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "skybaker.h"

#include "image.h"

#include <numeric>

#include "reone/graphics/dxtutil.h"
#include "reone/graphics/format/bwmreader.h"
#include "reone/graphics/format/mdlmdxreader.h"
#include "reone/graphics/format/tgareader.h"
#include "reone/graphics/format/tpcreader.h"
#include "reone/graphics/format/txireader.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/model.h"
#include "reone/graphics/modelnode.h"
#include "reone/graphics/statistic.h"
#include "reone/graphics/texture.h"
#include "reone/resource/format/gffreader.h"
#include "reone/resource/format/lytreader.h"
#include "reone/resource/id.h"
#include "reone/resource/parser/gff/ifo.h"
#include "reone/resource/resources.h"
#include "reone/system/fileutil.h"
#include "reone/system/stream/memoryinput.h"

namespace reone::skybake {

using namespace graphics;
using namespace resource;

namespace {

static constexpr int kBakerVersion = 3;
static constexpr double kContinuityReviewThreshold = 10.0;
static constexpr size_t kBvhLeafSize = 8;
static constexpr int kCoverageRayCount = 4096;
static constexpr double kGuessCoverageThreshold = 0.5;
static constexpr float kRayEpsilon = 1e-4f;
static constexpr float kAlphaTestThreshold = 0.5f;

struct MeshInfo {
    std::string nodeName;
    std::string texture;
    std::shared_ptr<Mesh> mesh;
    glm::mat4 transform {1.0f};
};

struct RoomInfo {
    std::string name;
    std::vector<MeshInfo> meshes;
    size_t faceCount {0};
    double coverage {0.0};
    bool hasSkyNamedTexture {false};
};

struct ModuleSurvey {
    std::string name;
    std::vector<RoomInfo> candidates;
    std::vector<std::string> guesses;
    std::vector<std::string> errors;
};

struct ConfigEntry {
    std::string module;
    std::string room;
    std::string sky;
};

struct RayTriangle {
    std::array<glm::vec3, 3> positions;
    std::array<glm::vec2, 3> uvs;
    std::string texture;
    glm::vec3 boundsMin {0.0f};
    glm::vec3 boundsMax {0.0f};
    glm::vec3 centroid {0.0f};
};

struct RayHit {
    size_t triangle {0};
    float distance {0.0f};
    glm::vec2 uv {0.0f};
};

struct TextureImage {
    int width {0};
    int height {0};
    std::vector<uint8_t> pixels;
    bool alphaTest {false};

    glm::u8vec4 pixel(int x, int y) const {
        const auto offset = 4ull * (static_cast<size_t>(y) * width + x);
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
    }
};

struct BakedSky {
    std::array<Image, 6> faces;
    std::array<double, 12> edgeMad {};
    std::array<std::string, 12> edgeNames;
    int size {0};
    size_t alphaPassThroughs {0};
};

class RayScene {
public:
    explicit RayScene(const RoomInfo &room) {
        glm::vec3 boundsMin(std::numeric_limits<float>::max());
        glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
        for (const auto &mesh : room.meshes) {
            for (const auto &face : mesh.mesh->faces()) {
                const auto local = mesh.mesh->faceVertexCoords(face);
                RayTriangle triangle;
                for (int i = 0; i < 3; ++i) {
                    triangle.positions[i] = glm::vec3(mesh.transform * glm::vec4(local[i], 1.0f));
                    triangle.uvs[i] = mesh.mesh->faceUV1(face, glm::vec3(i == 0, i == 1, i == 2));
                }
                if (glm::length2(glm::cross(triangle.positions[1] - triangle.positions[0],
                                            triangle.positions[2] - triangle.positions[0])) <= 1e-16f)
                    continue;
                triangle.texture = mesh.texture;
                triangle.boundsMin = glm::min(triangle.positions[0],
                                              glm::min(triangle.positions[1], triangle.positions[2]));
                triangle.boundsMax = glm::max(triangle.positions[0],
                                              glm::max(triangle.positions[1], triangle.positions[2]));
                triangle.centroid = (triangle.positions[0] + triangle.positions[1] + triangle.positions[2]) / 3.0f;
                boundsMin = glm::min(boundsMin, triangle.boundsMin);
                boundsMax = glm::max(boundsMax, triangle.boundsMax);
                _triangles.push_back(std::move(triangle));
            }
        }
        if (_triangles.empty())
            return;
        _center = 0.5f * (boundsMin + boundsMax);
        _indices.resize(_triangles.size());
        std::iota(_indices.begin(), _indices.end(), size_t {0});
        _nodes.reserve(2 * _triangles.size());
        build(0, _indices.size());
    }

    const glm::vec3 &center() const { return _center; }
    const std::vector<RayTriangle> &triangles() const { return _triangles; }

    std::optional<RayHit> nearestHit(const glm::vec3 &origin, const glm::vec3 &direction,
                                     float minDistance = kRayEpsilon) const {
        if (_nodes.empty())
            return std::nullopt;
        std::optional<RayHit> result;
        float nearest = std::numeric_limits<float>::max();
        std::vector<size_t> stack {0};
        while (!stack.empty()) {
            const auto nodeIndex = stack.back();
            stack.pop_back();
            const auto &node = _nodes[nodeIndex];
            if (!intersects(node.boundsMin, node.boundsMax, origin, direction, minDistance, nearest))
                continue;
            if (node.count != 0) {
                for (size_t i = node.begin; i < node.begin + node.count; ++i) {
                    const size_t triangleIndex = _indices[i];
                    float distance, u, v;
                    if (!intersectTriangle(_triangles[triangleIndex], origin, direction,
                                           minDistance, nearest, distance, u, v))
                        continue;
                    nearest = distance;
                    const auto &triangle = _triangles[triangleIndex];
                    result = RayHit {triangleIndex, distance,
                                     triangle.uvs[0] * (1.0f - u - v) + triangle.uvs[1] * u + triangle.uvs[2] * v};
                }
            } else {
                stack.push_back(node.left);
                stack.push_back(node.right);
            }
        }
        return result;
    }

private:
    struct Node {
        glm::vec3 boundsMin {0.0f};
        glm::vec3 boundsMax {0.0f};
        size_t begin {0};
        size_t count {0};
        size_t left {0};
        size_t right {0};
    };

    std::vector<RayTriangle> _triangles;
    std::vector<size_t> _indices;
    std::vector<Node> _nodes;
    glm::vec3 _center {0.0f};

    size_t build(size_t begin, size_t end) {
        Node node;
        node.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        node.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        glm::vec3 centroidMin(std::numeric_limits<float>::max());
        glm::vec3 centroidMax(std::numeric_limits<float>::lowest());
        for (size_t i = begin; i < end; ++i) {
            const auto &triangle = _triangles[_indices[i]];
            node.boundsMin = glm::min(node.boundsMin, triangle.boundsMin);
            node.boundsMax = glm::max(node.boundsMax, triangle.boundsMax);
            centroidMin = glm::min(centroidMin, triangle.centroid);
            centroidMax = glm::max(centroidMax, triangle.centroid);
        }
        const size_t index = _nodes.size();
        _nodes.push_back(node);
        if (end - begin <= kBvhLeafSize) {
            _nodes[index].begin = begin;
            _nodes[index].count = end - begin;
            return index;
        }
        const glm::vec3 extent = centroidMax - centroidMin;
        int axis = extent.y > extent.x ? 1 : 0;
        if (extent.z > extent[axis])
            axis = 2;
        const size_t middle = begin + (end - begin) / 2;
        std::nth_element(_indices.begin() + begin, _indices.begin() + middle, _indices.begin() + end,
                         [&](size_t a, size_t b) { return _triangles[a].centroid[axis] < _triangles[b].centroid[axis]; });
        const size_t left = build(begin, middle);
        const size_t right = build(middle, end);
        _nodes[index].left = left;
        _nodes[index].right = right;
        return index;
    }

    static bool intersects(const glm::vec3 &boundsMin, const glm::vec3 &boundsMax,
                           const glm::vec3 &origin, const glm::vec3 &direction,
                           float minDistance, float maxDistance) {
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(direction[axis]) <= 1e-12f) {
                if (origin[axis] < boundsMin[axis] || origin[axis] > boundsMax[axis])
                    return false;
                continue;
            }
            float nearDistance = (boundsMin[axis] - origin[axis]) / direction[axis];
            float farDistance = (boundsMax[axis] - origin[axis]) / direction[axis];
            if (nearDistance > farDistance)
                std::swap(nearDistance, farDistance);
            minDistance = std::max(minDistance, nearDistance);
            maxDistance = std::min(maxDistance, farDistance);
            if (minDistance > maxDistance)
                return false;
        }
        return true;
    }

    static bool intersectTriangle(const RayTriangle &triangle, const glm::vec3 &origin,
                                  const glm::vec3 &direction, float minDistance, float maxDistance,
                                  float &distance, float &u, float &v) {
        const glm::vec3 edge1 = triangle.positions[1] - triangle.positions[0];
        const glm::vec3 edge2 = triangle.positions[2] - triangle.positions[0];
        const glm::vec3 p = glm::cross(direction, edge2);
        const float determinant = glm::dot(edge1, p);
        // Deliberately do not backface-cull: every backdrop is observed from inside.
        if (std::abs(determinant) <= 1e-8f)
            return false;
        const float inverse = 1.0f / determinant;
        const glm::vec3 t = origin - triangle.positions[0];
        u = glm::dot(t, p) * inverse;
        if (u < -1e-6f || u > 1.0f + 1e-6f)
            return false;
        const glm::vec3 q = glm::cross(t, edge1);
        v = glm::dot(direction, q) * inverse;
        if (v < -1e-6f || u + v > 1.0f + 1e-6f)
            return false;
        distance = glm::dot(edge2, q) * inverse;
        return distance >= minDistance && distance < maxDistance;
    }
};

double coverageFraction(const RayScene &scene) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kGoldenRatio = 1.6180339887498948482f;
    size_t hits = 0;
    for (int i = 0; i < kCoverageRayCount; ++i) {
        const float z = 1.0f - 2.0f * (i + 0.5f) / kCoverageRayCount;
        const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float azimuth = 2.0f * kPi * i / kGoldenRatio;
        const glm::vec3 direction(radius * std::cos(azimuth), radius * std::sin(azimuth), z);
        hits += scene.nearestHit(scene.center(), direction).has_value();
    }
    return static_cast<double>(hits) / kCoverageRayCount;
}

std::string lower(std::string value) {
    boost::to_lower(value);
    return value;
}

std::string trim(std::string value) {
    boost::trim(value);
    return value;
}

bool isSkyTexture(std::string_view texture) {
    auto name = lower(std::string(texture));
    return name.find("sky") != std::string::npos ||
           name.find("stars") != std::string::npos ||
           name.find("nebula") != std::string::npos;
}

std::optional<std::filesystem::path> find(const std::filesystem::path &dir, const std::string &name) {
    return findFileIgnoreCase(dir, name);
}

std::vector<std::string> moduleNames(const std::filesystem::path &gameDir) {
    auto modulesDir = find(gameDir, "modules");
    if (!modulesDir)
        throw std::runtime_error("Modules directory not found: " + gameDir.string());
    std::set<std::string> result;
    for (const auto &entry : std::filesystem::directory_iterator(*modulesDir)) {
        if (!entry.is_regular_file())
            continue;
        auto filename = lower(entry.path().filename().string());
        if (!boost::ends_with(filename, ".rim") || boost::ends_with(filename, "_s.rim"))
            continue;
        result.insert(filename.substr(0, filename.size() - 4));
    }
    return {result.begin(), result.end()};
}

class GameResources {
public:
    explicit GameResources(const RunOptions &options) :
        _gameDir(options.gameDir) {
        auto key = find(_gameDir, "chitin.key");
        if (!key)
            throw std::runtime_error("chitin.key not found: " + _gameDir.string());
        _resources.addKEY(*key);

        if (auto texturePacks = find(_gameDir, "texturepacks")) {
            if (auto gui = find(*texturePacks, "swpc_tex_gui.erf"))
                _resources.addERF(*gui);
            if (auto tpa = find(*texturePacks, "swpc_tex_tpa.erf"))
                _resources.addERF(*tpa);
        }
        if (auto patch = find(_gameDir, "patch.erf"))
            _resources.addERF(*patch);
        if (auto overrideDir = find(_gameDir, "override"))
            _resources.addFolder(*overrideDir);
    }

    Resources &loadModule(const std::string &name) {
        _resources.clearLocal();
        auto modulesDir = find(_gameDir, "modules");
        if (!modulesDir)
            throw std::runtime_error("Modules directory not found");
        addRim(*modulesDir, name);
        addRim(*modulesDir, name + "_s");
        addErf(*modulesDir, name);
        addErf(*modulesDir, name + "_loc");
        if (auto lips = find(_gameDir, "lips"))
            addErf(*lips, name + "_loc");
        addErf(*modulesDir, name + "_dlg");
        return _resources;
    }

private:
    std::filesystem::path _gameDir;
    Resources _resources;

    void addRim(const std::filesystem::path &dir, const std::string &name) {
        if (auto path = find(dir, name + ".rim"))
            _resources.addRIM(*path, ContainerKind::Local);
    }

    void addErf(const std::filesystem::path &dir, const std::string &name) {
        if (auto path = find(dir, name + ".mod"))
            _resources.addERF(*path, ContainerKind::Local);
        if (auto path = find(dir, name + ".erf"))
            _resources.addERF(*path, ContainerKind::Local);
    }
};

std::set<std::string> areaNames(Resources &resources) {
    std::set<std::string> names;
    auto ifoData = resources.find(ResourceId("module", ResType::Ifo));
    if (!ifoData)
        throw std::runtime_error("module.ifo not found");
    MemoryInputStream input(ifoData->data);
    GffReader reader(input);
    reader.load();
    auto ifo = generated::parseIFO(*reader.root());
    for (const auto &area : ifo.Mod_Area_list) {
        if (!area.Area_Name.empty())
            names.insert(lower(area.Area_Name));
    }
    if (names.empty() && !ifo.Mod_Entry_Area.empty())
        names.insert(lower(ifo.Mod_Entry_Area));
    return names;
}

bool hasWalkmesh(Resources &resources, const std::string &room) {
    auto data = resources.find(ResourceId(room, ResType::Wok));
    if (!data)
        return false;
    MemoryInputStream input(data->data);
    BwmReader reader(input);
    reader.load();
    return static_cast<bool>(reader.walkmesh());
}

std::shared_ptr<Model> loadModel(Resources &resources, const std::string &name) {
    auto mdlData = resources.find(ResourceId(name, ResType::Mdl));
    auto mdxData = resources.find(ResourceId(name, ResType::Mdx));
    if (!mdlData || !mdxData)
        throw std::runtime_error("MDL/MDX not found for room '" + name + "'");
    MemoryInputStream mdl(mdlData->data);
    MemoryInputStream mdx(mdxData->data);
    Statistic statistic;
    MdlMdxReader reader(mdl, mdx, statistic);
    reader.load();
    return reader.model();
}

std::vector<MeshInfo> collectMeshes(const Model &model) {
    std::vector<MeshInfo> result;
    std::stack<std::shared_ptr<ModelNode>> nodes;
    nodes.push(model.rootNode());
    while (!nodes.empty()) {
        auto node = std::move(nodes.top());
        nodes.pop();
        for (const auto &child : node->children())
            nodes.push(child);
        auto triangleMesh = node->mesh();
        if (!triangleMesh || !triangleMesh->mesh || !triangleMesh->render)
            continue;
        result.push_back({node->name(), lower(triangleMesh->diffuseMap), triangleMesh->mesh,
                          node->absoluteTransform()});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return std::tie(a.nodeName, a.texture) < std::tie(b.nodeName, b.texture);
    });
    return result;
}

RoomInfo loadRoom(Resources &resources, const std::string &name) {
    auto model = loadModel(resources, name);
    RoomInfo room;
    room.name = name;
    room.meshes = collectMeshes(*model);
    for (const auto &mesh : room.meshes) {
        room.faceCount += mesh.mesh->faces().size();
        room.hasSkyNamedTexture = room.hasSkyNamedTexture || isSkyTexture(mesh.texture);
    }
    return room;
}

ModuleSurvey surveyModule(GameResources &game, const std::string &moduleName) {
    ModuleSurvey survey;
    survey.name = moduleName;
    auto &resources = game.loadModule(moduleName);
    std::set<std::string> seenRooms;
    for (const auto &layoutName : areaNames(resources)) {
        try {
            auto lytData = resources.find(ResourceId(layoutName, ResType::Lyt));
            if (!lytData)
                continue;
            MemoryInputStream input(lytData->data);
            LytReader reader;
            reader.load(input);
            for (const auto &layoutRoom : reader.layout().rooms) {
                if (layoutRoom.name.empty() || layoutRoom.name.find('*') != std::string::npos)
                    continue;
                if (!seenRooms.insert(layoutRoom.name).second)
                    continue;
                if (hasWalkmesh(resources, layoutRoom.name))
                    continue;
                try {
                    auto room = loadRoom(resources, layoutRoom.name);
                    RayScene scene(room);
                    room.coverage = coverageFraction(scene);
                    // Neither signal is sufficient alone, and each one's false
                    // positives are a different shape. Coverage alone admits
                    // architecture: 104pere is a Peragus interior - walls,
                    // railings, floor - at 0.73. Sky-named textures alone admit
                    // set dressing: 003eboq carries them but subtends 2.8% of
                    // the sphere, because it is scenery seen through the Ebon
                    // Hawk's windows rather than a shell around the camera.
                    // Every candidate is still reported with both numbers, so
                    // curation can see the near misses this rejects.
                    if (room.coverage >= kGuessCoverageThreshold && room.hasSkyNamedTexture)
                        survey.guesses.push_back(room.name);
                    survey.candidates.push_back(std::move(room));
                } catch (const std::exception &e) {
                    survey.errors.push_back(layoutRoom.name + ": " + e.what());
                }
            }
        } catch (const std::exception &e) {
            survey.errors.push_back(layoutName + ".lyt: " + e.what());
        }
    }
    std::sort(survey.guesses.begin(), survey.guesses.end(), [&](const auto &a, const auto &b) {
        const auto coverage = [&](const auto &name) {
            return std::find_if(survey.candidates.begin(), survey.candidates.end(),
                                [&](const auto &room) { return room.name == name; })
                ->coverage;
        };
        return std::make_tuple(coverage(b), a) < std::make_tuple(coverage(a), b);
    });
    return survey;
}

void printSurvey(const ModuleSurvey &survey) {
    for (const auto &room : survey.candidates) {
        std::cout << "candidate module=" << survey.name << " room=" << room.name
                  << " meshes=" << room.meshes.size() << " faces=" << room.faceCount
                  << " coverage=" << std::fixed << std::setprecision(4) << room.coverage
                  << " sky_named_textures=" << (room.hasSkyNamedTexture ? "yes" : "no") << '\n';
        for (const auto &mesh : room.meshes)
            std::cout << "  mesh=" << mesh.nodeName << " faces=" << mesh.mesh->faces().size()
                      << " texture=" << (mesh.texture.empty() ? "none" : mesh.texture) << '\n';
    }
    for (const auto &error : survey.errors)
        std::cerr << "review module=" << survey.name << " error=" << error << '\n';
}

void writeSurveyConfig(const RunOptions &options, const std::vector<ModuleSurvey> &surveys) {
    if (auto parent = options.outPath.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
    std::ofstream out(options.outPath);
    if (!out)
        throw std::runtime_error("Unable to create config: " + options.outPath.string());
    out << "# First-pass sky survey for "
        << (options.gameId == GameID::KotOR ? "k1" : "k2")
        << ". Every classification must be reviewed by a human.\n\n";
    for (const auto &survey : surveys) {
        out << '[' << survey.name << "]\n";
        if (survey.guesses.empty()) {
            out << "sky = none # review\n";
        } else {
            const auto &room = survey.guesses.front();
            out << "room = " << room << " # review\n";
            out << "sky = " << room << " # review";
            if (survey.guesses.size() > 1)
                out << ": candidates=" << boost::join(survey.guesses, ",");
            out << '\n';
        }
        for (const auto &room : survey.candidates) {
            out << "# review candidate: room=" << room.name
                << " coverage=" << std::fixed << std::setprecision(4) << room.coverage
                << " sky_named_textures=" << (room.hasSkyNamedTexture ? "yes" : "no") << "\n";
        }
        if (!survey.errors.empty())
            out << "# review: " << survey.errors.size() << " candidate room(s) could not be read\n";
        out << '\n';
    }
}

std::vector<ConfigEntry> readConfig(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Unable to open config: " + path.string());
    std::vector<ConfigEntry> entries;
    ConfigEntry *current = nullptr;
    std::string line;
    while (std::getline(input, line)) {
        if (auto comment = line.find('#'); comment != std::string::npos)
            line.resize(comment);
        line = trim(std::move(line));
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']') {
            entries.push_back({lower(trim(line.substr(1, line.size() - 2))), {}, {}});
            current = &entries.back();
            continue;
        }
        if (!current)
            throw std::runtime_error("Config key before first section");
        auto equals = line.find('=');
        if (equals == std::string::npos)
            throw std::runtime_error("Invalid config line: " + line);
        auto key = lower(trim(line.substr(0, equals)));
        auto value = lower(trim(line.substr(equals + 1)));
        if (key == "room")
            current->room = value;
        else if (key == "sky")
            current->sky = value;
    }
    for (const auto &entry : entries) {
        if (entry.sky.empty())
            throw std::runtime_error("Missing sky key in [" + entry.module + "]");
        if (entry.sky != "none" && entry.room.empty())
            throw std::runtime_error("Missing room key in [" + entry.module + "]");
    }
    return entries;
}

std::pair<glm::vec3, glm::vec3> cubeFaceTangents(int face) {
    switch (static_cast<CubeMapFace>(face)) {
    case CubeMapFace::PositiveX:
        return {{0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}};
    case CubeMapFace::NegativeX:
        return {{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}};
    case CubeMapFace::PositiveY:
        return {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    case CubeMapFace::NegativeY:
        return {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
    case CubeMapFace::PositiveZ:
        return {{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
    case CubeMapFace::NegativeZ:
        return {{-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
    }
    throw std::logic_error("invalid cube face");
}

TextureImage imageFromTexture(const Texture &texture) {
    if (texture.layers().empty() || !texture.layers().front().pixels)
        throw std::runtime_error("Texture has no pixels: " + texture.name());
    TextureImage image;
    image.width = texture.width();
    image.height = texture.height();
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4, 255);
    image.alphaTest = texture.features().blending == Texture::Blending::PunchThrough;
    const auto *source = reinterpret_cast<const uint8_t *>(texture.layers().front().pixels->data());
    const size_t count = static_cast<size_t>(texture.width()) * texture.height();
    if (texture.pixelFormat() == PixelFormat::DXT1 || texture.pixelFormat() == PixelFormat::DXT5) {
        std::vector<uint32_t> decoded(count);
        if (texture.pixelFormat() == PixelFormat::DXT1)
            decompressDXT1(texture.width(), texture.height(), source, decoded.data());
        else
            decompressDXT5(texture.width(), texture.height(), source, decoded.data());
        for (size_t i = 0; i < count; ++i) {
            const uint32_t value = decoded[i];
            image.pixels[4 * i] = static_cast<uint8_t>(value >> 24);
            image.pixels[4 * i + 1] = static_cast<uint8_t>(value >> 16);
            image.pixels[4 * i + 2] = static_cast<uint8_t>(value >> 8);
            image.pixels[4 * i + 3] = static_cast<uint8_t>(value);
        }
        return image;
    }
    for (size_t i = 0; i < count; ++i) {
        switch (texture.pixelFormat()) {
        case PixelFormat::R8:
            image.pixels[4 * i] = image.pixels[4 * i + 1] = image.pixels[4 * i + 2] = source[i];
            break;
        case PixelFormat::RGB8:
        case PixelFormat::RGBA8:
            image.pixels[4 * i] = source[i * (texture.pixelFormat() == PixelFormat::RGB8 ? 3 : 4)];
            image.pixels[4 * i + 1] = source[i * (texture.pixelFormat() == PixelFormat::RGB8 ? 3 : 4) + 1];
            image.pixels[4 * i + 2] = source[i * (texture.pixelFormat() == PixelFormat::RGB8 ? 3 : 4) + 2];
            if (texture.pixelFormat() == PixelFormat::RGBA8)
                image.pixels[4 * i + 3] = source[4 * i + 3];
            break;
        case PixelFormat::BGR8:
        case PixelFormat::BGRA8: {
            const int stride = texture.pixelFormat() == PixelFormat::BGR8 ? 3 : 4;
            image.pixels[4 * i] = source[i * stride + 2];
            image.pixels[4 * i + 1] = source[i * stride + 1];
            image.pixels[4 * i + 2] = source[i * stride];
            if (texture.pixelFormat() == PixelFormat::BGRA8)
                image.pixels[4 * i + 3] = source[4 * i + 3];
            break;
        }
        default:
            throw std::runtime_error("Unsupported texture pixel format: " + std::to_string(static_cast<int>(texture.pixelFormat())));
        }
    }
    return image;
}

TextureImage loadTexture(Resources &resources, const std::string &name) {
    if (auto data = resources.find(ResourceId(name, ResType::Tpc))) {
        MemoryInputStream input(data->data);
        TpcReader reader(input, name, TextureUsage::EnvironmentMap);
        reader.load();
        return imageFromTexture(*reader.texture());
    }
    if (auto data = resources.find(ResourceId(name, ResType::Tga))) {
        MemoryInputStream input(data->data);
        TgaReader reader(input, name, TextureUsage::EnvironmentMap);
        reader.load();
        if (auto txiData = resources.find(ResourceId(name, ResType::Txi))) {
            MemoryInputStream txiInput(txiData->data);
            TxiReader txiReader;
            txiReader.load(txiInput);
            reader.texture()->setFeatures(txiReader.features());
        }
        return imageFromTexture(*reader.texture());
    }
    throw std::runtime_error("TPC/TGA texture not found: " + name);
}

glm::vec4 sampleRepeat(const TextureImage &image, glm::vec2 uv) {
    uv -= glm::floor(uv);
    const float x = uv.x * image.width - 0.5f;
    const float y = uv.y * image.height - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = x - std::floor(x);
    const float ty = y - std::floor(y);
    auto wrap = [](int value, int size) { return (value % size + size) % size; };
    const glm::vec4 p00(image.pixel(wrap(x0, image.width), wrap(y0, image.height)));
    const glm::vec4 p10(image.pixel(wrap(x0 + 1, image.width), wrap(y0, image.height)));
    const glm::vec4 p01(image.pixel(wrap(x0, image.width), wrap(y0 + 1, image.height)));
    const glm::vec4 p11(image.pixel(wrap(x0 + 1, image.width), wrap(y0 + 1, image.height)));
    return glm::clamp(glm::mix(glm::mix(p00, p10, tx), glm::mix(p01, p11, tx), ty) / 255.0f,
                      glm::vec4(0.0f), glm::vec4(1.0f));
}

glm::vec3 faceDirection(int face, float u, float v) {
    static const std::array<glm::vec3, 6> normals {{
        {1, 0, 0},
        {-1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
        {0, 0, 1},
        {0, 0, -1},
    }};
    const auto [tangentU, tangentV] = cubeFaceTangents(face);
    return glm::normalize(normals[face] + (2.0f * u - 1.0f) * tangentU + (2.0f * v - 1.0f) * tangentV);
}

int dominantFace(const glm::vec3 &direction) {
    const glm::vec3 absolute = glm::abs(direction);
    if (absolute.x >= absolute.y && absolute.x >= absolute.z)
        return direction.x >= 0 ? 0 : 1;
    if (absolute.y >= absolute.z)
        return direction.y >= 0 ? 2 : 3;
    return direction.z >= 0 ? 4 : 5;
}

glm::u8vec3 sampleCube(const std::array<Image, 6> &faces, const glm::vec3 &direction) {
    const int face = dominantFace(direction);
    const auto [uAxis, vAxis] = cubeFaceTangents(face);
    static const std::array<glm::vec3, 6> normals {{
        {1, 0, 0},
        {-1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
        {0, 0, 1},
        {0, 0, -1},
    }};
    const float major = glm::dot(direction, normals[face]);
    const float u = 0.5f * (glm::dot(direction, uAxis) / major + 1.0f);
    const float v = 0.5f * (glm::dot(direction, vAxis) / major + 1.0f);
    const auto &image = faces[face];
    const int x = glm::clamp(static_cast<int>(u * image.width), 0, image.width - 1);
    const int y = glm::clamp(static_cast<int>(v * image.height), 0, image.height - 1);
    return image.pixel(x, y);
}

void computeEdgeMetrics(BakedSky &baked) {
    static const std::array<glm::vec3, 6> normals {{
        {1, 0, 0},
        {-1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
        {0, 0, 1},
        {0, 0, -1},
    }};
    size_t edgeIndex = 0;
    for (int a = 0; a < 6; ++a) {
        for (int b = a + 1; b < 6; ++b) {
            if (std::abs(glm::dot(normals[a], normals[b])) > 0.5f)
                continue;
            double sum = 0.0;
            glm::vec3 tangent = glm::normalize(glm::cross(normals[a], normals[b]));
            for (int i = 0; i < baked.size; ++i) {
                const float q = 2.0f * (i + 0.5f) / baked.size - 1.0f;
                const float inner = 1.0f - 1.0f / baked.size;
                const glm::vec3 da = glm::normalize(normals[a] + inner * normals[b] + q * tangent);
                const glm::vec3 db = glm::normalize(normals[b] + inner * normals[a] + q * tangent);
                const glm::vec3 pa(sampleCube(baked.faces, da));
                const glm::vec3 pb(sampleCube(baked.faces, db));
                sum += std::abs(pa.r - pb.r) + std::abs(pa.g - pb.g) + std::abs(pa.b - pb.b);
            }
            baked.edgeMad[edgeIndex] = sum / (3.0 * baked.size);
            static const std::array<const char *, 6> labels {{"+X", "-X", "+Y", "-Y", "+Z", "-Z"}};
            baked.edgeNames[edgeIndex] = std::string(labels[a]) + "/" + labels[b];
            ++edgeIndex;
        }
    }
}

BakedSky bakeSky(Resources &resources, const RayScene &scene, int requestedSize) {
    std::unordered_map<std::string, TextureImage> textures;
    int size = requestedSize;
    for (const auto &triangle : scene.triangles()) {
        if (triangle.texture.empty() || triangle.texture == "null")
            continue;
        if (textures.find(triangle.texture) != textures.end())
            continue;
        auto texture = loadTexture(resources, triangle.texture);
        if (size == 0)
            size = std::max(size, std::max(texture.width, texture.height));
        textures.emplace(triangle.texture, std::move(texture));
    }
    if (size <= 0)
        throw std::runtime_error("Unable to determine output face size");

    BakedSky baked;
    baked.size = size;
    for (auto &face : baked.faces)
        face = Image(size, size, {0, 0, 0});
    for (int outputFace = 0; outputFace < 6; ++outputFace) {
        auto &output = baked.faces[outputFace];
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const glm::vec3 worldDirection = faceDirection(outputFace, (x + 0.5f) / size, (y + 0.5f) / size);
                float minDistance = kRayEpsilon;
                while (auto hit = scene.nearestHit(scene.center(), worldDirection, minDistance)) {
                    const auto &triangle = scene.triangles()[hit->triangle];
                    auto texture = textures.find(triangle.texture);
                    if (texture == textures.end())
                        break;
                    const glm::vec4 sample = sampleRepeat(texture->second, hit->uv);
                    if (texture->second.alphaTest && sample.a < kAlphaTestThreshold) {
                        minDistance = hit->distance + std::max(kRayEpsilon, hit->distance * 1e-5f);
                        ++baked.alphaPassThroughs;
                        continue;
                    }
                    output.setPixel(x, y, glm::u8vec3(glm::clamp(glm::vec3(sample) * 255.0f + 0.5f, glm::vec3(0.0f), glm::vec3(255.0f))));
                    break;
                }
            }
        }
    }
    computeEdgeMetrics(baked);
    return baked;
}

Image equirectangular(const BakedSky &baked) {
    Image image(2 * baked.size, baked.size);
    constexpr float kPi = 3.14159265358979323846f;
    for (int y = 0; y < image.height; ++y) {
        const float latitude = (0.5f - (y + 0.5f) / image.height) * kPi;
        for (int x = 0; x < image.width; ++x) {
            const float longitude = ((x + 0.5f) / image.width * 2.0f - 1.0f) * kPi;
            const glm::vec3 direction {
                std::cos(latitude) * std::cos(longitude),
                std::cos(latitude) * std::sin(longitude),
                std::sin(latitude),
            };
            image.setPixel(x, y, sampleCube(baked.faces, direction));
        }
    }
    return image;
}

std::filesystem::file_time_type newestInput(const RunOptions &options, const std::string &module) {
    auto newest = std::filesystem::last_write_time(options.configPath);
    const auto update = [&](const std::filesystem::path &path, auto &self) -> void {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            newest = std::max(newest, std::filesystem::last_write_time(path, ec));
        } else if (std::filesystem::is_directory(path, ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(path, ec))
                self(entry.path(), self);
        }
    };
    if (auto key = find(options.gameDir, "chitin.key"))
        update(*key, update);
    if (auto data = find(options.gameDir, "data"))
        update(*data, update);
    if (auto packs = find(options.gameDir, "texturepacks"))
        update(*packs, update);
    if (auto overrideDir = find(options.gameDir, "override"))
        update(*overrideDir, update);
    if (auto modules = find(options.gameDir, "modules")) {
        for (const auto &suffix : {".rim", "_s.rim", ".mod", ".erf", "_loc.erf", "_dlg.erf"}) {
            if (auto path = find(*modules, module + suffix))
                update(*path, update);
        }
    }
    return newest;
}

bool upToDate(const RunOptions &options, const ConfigEntry &entry, const std::filesystem::path &skyDir) {
    if (options.force)
        return false;
    static const std::array<const char *, 8> outputs {{
        "px.png",
        "nx.png",
        "py.png",
        "ny.png",
        "pz.png",
        "nz.png",
        "equirectangular.png",
        "sky.ini",
    }};
    auto newest = newestInput(options, entry.module);
    for (const auto *name : outputs) {
        auto path = skyDir / name;
        if (!std::filesystem::exists(path) || std::filesystem::last_write_time(path) < newest)
            return false;
    }
    std::ifstream ini(skyDir / "sky.ini");
    std::string line;
    bool versionMatches = false;
    bool sizeMatches = options.faceSize == 0;
    while (std::getline(ini, line)) {
        line = trim(std::move(line));
        if (line == "baker_version = " + std::to_string(kBakerVersion))
            versionMatches = true;
        if (options.faceSize > 0 && line == "size = " + std::to_string(options.faceSize))
            sizeMatches = true;
    }
    if (!versionMatches || !sizeMatches)
        return false;
    return true;
}

void writeBaked(const ConfigEntry &entry, const RayScene &scene, const BakedSky &baked,
                const std::filesystem::path &outDir) {
    static const std::array<const char *, 6> names {{"px.png", "nx.png", "py.png", "ny.png", "pz.png", "nz.png"}};
    std::filesystem::create_directories(outDir);
    for (int face = 0; face < 6; ++face)
        writePng(outDir / names[face], baked.faces[face]);
    writePng(outDir / "equirectangular.png", equirectangular(baked));
    std::ofstream ini(outDir / "sky.ini");
    ini << "[sky]\n"
        << "baker_version = " << kBakerVersion << "\n"
        << "module = " << entry.module << "\n"
        << "room = " << entry.room << "\n"
        << "renderer = cpu_bvh_raycast\n"
        << "format = png\n"
        << "size = " << baked.size << "\n"
        << "+X = px.png\n-X = nx.png\n+Y = py.png\n-Y = ny.png\n+Z = pz.png\n-Z = nz.png\n"
        << "# Cube-edge differences are authored art seams, not an orientation diagnostic.\n";
    for (size_t i = 0; i < baked.edgeMad.size(); ++i)
        ini << "edge_" << baked.edgeNames[i] << " = " << std::fixed << std::setprecision(4) << baked.edgeMad[i] << "\n";
    ini << "\n[raycast]\n"
        << "triangles = " << scene.triangles().size() << "\n"
        << "origin = " << scene.center().x << ',' << scene.center().y << ',' << scene.center().z << "\n"
        << "alpha_pass_throughs = " << baked.alphaPassThroughs << "\n";
}

} // namespace

void runSurvey(const RunOptions &options) {
    GameResources game(options);
    const auto modules = moduleNames(options.gameDir);
    std::vector<ModuleSurvey> surveys;
    surveys.reserve(modules.size());
    size_t skies = 0;
    size_t errors = 0;
    for (size_t i = 0; i < modules.size(); ++i) {
        std::cout << "survey " << (i + 1) << '/' << modules.size() << " module=" << modules[i] << '\n';
        auto survey = surveyModule(game, modules[i]);
        printSurvey(survey);
        skies += !survey.guesses.empty();
        errors += survey.errors.size();
        surveys.push_back(std::move(survey));
    }
    writeSurveyConfig(options, surveys);
    std::cout << "SURVEY_TOTAL game=" << (options.gameId == GameID::KotOR ? "k1" : "k2")
              << " modules=" << surveys.size() << " sky=" << skies
              << " none=" << (surveys.size() - skies) << " review=" << surveys.size()
              << " read_errors=" << errors << '\n';
}

void runBake(const RunOptions &options) {
    GameResources game(options);
    const auto entries = readConfig(options.configPath);
    std::unordered_map<std::string, std::pair<std::string, std::string>> skySources;
    size_t bakedCount = 0;
    size_t skipped = 0;
    size_t failed = 0;
    size_t continuityReview = 0;
    for (const auto &entry : entries) {
        if (entry.sky == "none")
            continue;
        auto inserted = skySources.emplace(entry.sky, std::make_pair(entry.module, entry.room));
        if (!inserted.second) {
            if (inserted.first->second.second != entry.room)
                throw std::runtime_error("Sky name '" + entry.sky + "' maps to multiple rooms");
            continue;
        }
        const auto skyDir = options.outPath / "override" /
                            (options.gameId == GameID::KotOR ? "k1" : "k2") /
                            "sky" / entry.sky;
        if (upToDate(options, entry, skyDir)) {
            std::cout << "up-to-date module=" << entry.module << " sky=" << entry.sky << '\n';
            ++skipped;
            continue;
        }
        try {
            std::cout << "bake module=" << entry.module << " room=" << entry.room << " sky=" << entry.sky << '\n';
            auto &resources = game.loadModule(entry.module);
            auto room = loadRoom(resources, entry.room);
            RayScene scene(room);
            auto baked = bakeSky(resources, scene, options.faceSize);
            writeBaked(entry, scene, baked, skyDir);
            std::cout << "  raycast_triangles=" << scene.triangles().size()
                      << " alpha_pass_throughs=" << baked.alphaPassThroughs << '\n';
            for (size_t i = 0; i < baked.edgeMad.size(); ++i)
                std::cout << "  art_edge=" << baked.edgeNames[i] << " mean_abs="
                          << std::fixed << std::setprecision(4) << baked.edgeMad[i] << '\n';
            const double worstArtEdge = *std::max_element(baked.edgeMad.begin(), baked.edgeMad.end());
            if (worstArtEdge > kContinuityReviewThreshold) {
                std::cerr << "review art_seam sky=" << entry.sky << " max_edge_mean_abs="
                          << std::fixed << std::setprecision(4) << worstArtEdge << '\n';
                ++continuityReview;
            }
            ++bakedCount;
        } catch (const std::exception &e) {
            std::cerr << "review bake_failed module=" << entry.module << " room=" << entry.room
                      << " sky=" << entry.sky << " error=" << e.what() << '\n';
            ++failed;
        }
    }
    std::cout << "BAKE_TOTAL baked=" << bakedCount << " up_to_date=" << skipped << " failed=" << failed
              << " continuity_review=" << continuityReview << '\n';
    if (failed)
        throw std::runtime_error(std::to_string(failed) + " sky bake(s) require review");
}

} // namespace reone::skybake
