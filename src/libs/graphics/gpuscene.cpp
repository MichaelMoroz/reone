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
#include "reone/graphics/gpuscene.h"

#include "reone/system/profiler.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "reone/graphics/mesh.h"
#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

uint32_t grownCapacity(uint32_t current, uint32_t required, uint32_t minimum) {
    if (current != 0 && required <= current)
        return current;
    uint64_t capacity = std::max(current, minimum);
    while (capacity < required)
        capacity *= 2;
    if (capacity > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Merged scene capacity exceeds uint32 range");
    return static_cast<uint32_t>(capacity);
}

void releaseBuffer(std::unique_ptr<IBuffer> &buffer) {
    if (buffer)
        buffer->deinit();
    buffer.reset();
}

} // namespace

struct GpuScene::Frame {
    std::unique_ptr<IBuffer> scene;
    std::unique_ptr<IBuffer> geometry;
    std::unique_ptr<IBuffer> proceduralQuads;
    std::unique_ptr<IBuffer> grassRanges;
    std::unique_ptr<IBuffer> materials;
    uint32_t sceneObjectCapacity {0};
    uint32_t boneCapacity {0};
    uint32_t danglyPositionCapacity {0};
    uint32_t vertexCapacity {0};
    uint32_t triangleCapacity {0};
    uint32_t proceduralQuadCapacity {0};
    uint32_t grassRangeCapacity {0};
    std::vector<PrimitiveIdRange> primitiveIds;

    void deinit() {
        releaseBuffer(scene);
        releaseBuffer(geometry);
        releaseBuffer(proceduralQuads);
        releaseBuffer(grassRanges);
        releaseBuffer(materials);
    }
};

GpuScene::GpuScene() = default;

GpuScene::~GpuScene() {
    deinit();
}

GpuScene::PrimitiveId GpuScene::PrimitiveIdView::operator[](uint32_t index) const {
    for (uint32_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
        const auto &range = ranges[rangeIndex];
        if (index < range.firstTriangle || index - range.firstTriangle >= range.triangleCount)
            continue;
        auto id = range.first;
        id.localPrimitive += index - range.firstTriangle;
        return id;
    }
    throw std::out_of_range("Frame-local primitive address is not published");
}

void GpuScene::init(IGpuSceneContext &context) {
    if (_inited)
        return;
    _context = &context;
    _mergePipeline = _context->makeGpuSceneMergePipeline();
    for (auto &frame : _frames)
        frame = std::make_unique<Frame>();
    _inited = true;
}

void GpuScene::deinit() {
    if (!_inited)
        return;
    for (auto &frame : _frames) {
        if (frame)
            frame->deinit();
    }
    _frames = {};
    clearSourceGeometry();
    releaseBuffer(_grassFaces);
    _grassFaceGeneration = 0;
    _mergePipeline.reset();
    _context = nullptr;
    _inited = false;
}

void GpuScene::clearSourceGeometry() {
    _sourceGeometry.clear();
    _sourceVertexData.clear();
    _sourceIndexData.clear();
    releaseBuffer(_sourceVertices);
    releaseBuffer(_sourceIndices);
    for (auto &buffer : _retiredSourceBuffers)
        releaseBuffer(buffer);
    _retiredSourceBuffers.clear();
    _sourceVertexCapacity = 0;
    _sourceIndexCapacity = 0;
}

void GpuScene::ensureMergeBuffers(Frame &frame, uint32_t objectCount,
                                        uint32_t boneCount, uint32_t vertexCount,
                                        uint32_t triangleCount, uint32_t proceduralQuadCount,
                                        uint32_t danglyPositionCount,
                                        uint32_t grassRangeCount) {
    constexpr uint32_t kInitialObjectCapacity = 64, kInitialBoneCapacity = 256;
    constexpr uint32_t kInitialVertexCapacity = 4096, kInitialTriangleCapacity = 4096;
    const auto objectCapacity =
        grownCapacity(frame.sceneObjectCapacity, objectCount, kInitialObjectCapacity);
    const auto boneCapacity = grownCapacity(frame.boneCapacity, boneCount, kInitialBoneCapacity);
    const auto danglyPositionCapacity =
        grownCapacity(frame.danglyPositionCapacity, danglyPositionCount, 256);
    if (!frame.scene || objectCapacity != frame.sceneObjectCapacity ||
        boneCapacity != frame.boneCapacity ||
        danglyPositionCapacity != frame.danglyPositionCapacity) {
        frame.sceneObjectCapacity = objectCapacity;
        frame.boneCapacity = boneCapacity;
        frame.danglyPositionCapacity = danglyPositionCapacity;
        releaseBuffer(frame.scene);
        frame.scene = _context->makeBuffer();
        frame.scene->initHostVisibleStorage(
            static_cast<uint64_t>(objectCapacity) * sizeof(SceneObject) +
                static_cast<uint64_t>(boneCapacity) * sizeof(Matrix3x4) +
                static_cast<uint64_t>(danglyPositionCapacity) * sizeof(glm::vec4));
    }
    const auto vertexCapacity =
        grownCapacity(frame.vertexCapacity, vertexCount, kInitialVertexCapacity);
    const auto triangleCapacity =
        grownCapacity(frame.triangleCapacity, triangleCount, kInitialTriangleCapacity);
    if (!frame.geometry || vertexCapacity != frame.vertexCapacity ||
        triangleCapacity != frame.triangleCapacity) {
        frame.vertexCapacity = vertexCapacity;
        frame.triangleCapacity = triangleCapacity;
        const uint64_t vertexBytes =
            static_cast<uint64_t>(vertexCapacity) * sizeof(MergedVertex);
        const uint64_t indexBytes =
            static_cast<uint64_t>(triangleCapacity) * 3 * sizeof(uint32_t);
        const uint64_t materialIdBytes =
            static_cast<uint64_t>(triangleCapacity) * sizeof(uint32_t);
        releaseBuffer(frame.geometry);
        frame.geometry = _context->makeBuffer();
        // INDEX_BUFFER is for the raster consumer added in F2. Vertices need no
        // new usage because raster pulls them programmably - a
        // StructuredBuffer<MergedVertex> indexed by SV_VertexID - rather than
        // going through vertex input, so there is one declaration of the vertex
        // layout instead of two and no format plumbing in the pipeline key.
        // Indices still bind as indices, which keeps the post-transform cache.
        frame.geometry->initMergedGeometry(vertexBytes + indexBytes + materialIdBytes);
    }
    const auto proceduralQuadCapacity =
        grownCapacity(frame.proceduralQuadCapacity, proceduralQuadCount, 64);
    if (!frame.proceduralQuads || proceduralQuadCapacity != frame.proceduralQuadCapacity) {
        frame.proceduralQuadCapacity = proceduralQuadCapacity;
        releaseBuffer(frame.proceduralQuads);
        frame.proceduralQuads = _context->makeBuffer();
        frame.proceduralQuads->initHostVisibleStorage(
            static_cast<uint64_t>(proceduralQuadCapacity) * sizeof(ProceduralQuad));
    }
    const auto grassRangeCapacity =
        grownCapacity(frame.grassRangeCapacity, grassRangeCount, 64);
    if (!frame.grassRanges || grassRangeCapacity != frame.grassRangeCapacity) {
        frame.grassRangeCapacity = grassRangeCapacity;
        releaseBuffer(frame.grassRanges);
        frame.grassRanges = _context->makeBuffer();
        frame.grassRanges->initHostVisibleStorage(
            static_cast<uint64_t>(grassRangeCapacity) * sizeof(GrassRange));
    }
}

const GpuScene::SourceGeometry &GpuScene::appendSourceGeometry(const Mesh &mesh) {
    const auto [it, inserted] = _sourceGeometry.emplace(&mesh, SourceGeometry {});
    const auto &vertices = mesh.vertexData();
    const auto &faces = mesh.faces();
    if (!inserted) {
        if (it->second.vertexDataCount != vertices.size() ||
            it->second.indexCount != faces.size() * 3) {
            warn("GpuScene source cache reused a Mesh address with different geometry",
                 LogChannel::Graphics);
        }
        return it->second;
    }
    if (vertices.size() > std::numeric_limits<uint32_t>::max() - _sourceVertexData.size() ||
        faces.size() > (std::numeric_limits<uint32_t>::max() - _sourceIndexData.size()) / 3)
        throw std::runtime_error("Source geometry pool exceeds shader index range");
    auto &location = it->second;
    location.vertexOffset = static_cast<uint32_t>(_sourceVertexData.size());
    location.indexOffset = static_cast<uint32_t>(_sourceIndexData.size());
    location.vertexDataCount = static_cast<uint32_t>(vertices.size());
    location.indexCount = static_cast<uint32_t>(faces.size() * 3);
    _sourceVertexData.insert(_sourceVertexData.end(), vertices.begin(), vertices.end());
    for (const auto &face : faces) {
        _sourceIndexData.push_back(face.vertices[0]);
        _sourceIndexData.push_back(face.vertices[1]);
        _sourceIndexData.push_back(face.vertices[2]);
    }
    const auto vertexCapacity = grownCapacity(
        _sourceVertexCapacity, static_cast<uint32_t>(_sourceVertexData.size()), 256 * 1024);
    const auto indexCapacity = grownCapacity(
        _sourceIndexCapacity, static_cast<uint32_t>(_sourceIndexData.size()), 256 * 1024);
    if (!_sourceVertices || !_sourceIndices || vertexCapacity != _sourceVertexCapacity ||
        indexCapacity != _sourceIndexCapacity) {
        auto vertexBuffer = _context->makeBuffer();
        auto indexBuffer = _context->makeBuffer();
        vertexBuffer->initDeviceStorage(static_cast<uint64_t>(vertexCapacity) * sizeof(float), nullptr);
        indexBuffer->initDeviceStorage(static_cast<uint64_t>(indexCapacity) * sizeof(uint32_t), nullptr);
        vertexBuffer->uploadDeviceStorage(
            0, static_cast<uint64_t>(_sourceVertexData.size()) * sizeof(float),
            _sourceVertexData.data());
        indexBuffer->uploadDeviceStorage(
            0, static_cast<uint64_t>(_sourceIndexData.size()) * sizeof(uint32_t),
            _sourceIndexData.data());
        if (_sourceVertices)
            _retiredSourceBuffers.push_back(std::move(_sourceVertices));
        if (_sourceIndices)
            _retiredSourceBuffers.push_back(std::move(_sourceIndices));
        _sourceVertices = std::move(vertexBuffer);
        _sourceIndices = std::move(indexBuffer);
        _sourceVertexCapacity = vertexCapacity;
        _sourceIndexCapacity = indexCapacity;
    } else {
        _sourceVertices->uploadDeviceStorage(
            static_cast<uint64_t>(location.vertexOffset) * sizeof(float),
            static_cast<uint64_t>(vertices.size()) * sizeof(float), vertices.data());
        _sourceIndices->uploadDeviceStorage(
            static_cast<uint64_t>(location.indexOffset) * sizeof(uint32_t),
            static_cast<uint64_t>(faces.size()) * 3 * sizeof(uint32_t),
            _sourceIndexData.data() + location.indexOffset);
    }
    return location;
}

GpuScene::View GpuScene::update(ICommandBuffer &commandBuffer, GpuSceneUpload &upload) {
    R_PROFILE_ZONE("GpuScene::update");
    const auto resourceGeneration = _context->resourceGeneration();
    if (resourceGeneration != _sourceResourceGeneration) {
        clearSourceGeometry();
        _sourceResourceGeneration = resourceGeneration;
        if (++_sceneScope == 0)
            ++_sceneScope;
    }
    if (++_revision == 0)
        ++_revision;

    View empty;
    empty.sceneScope = _sceneScope;
    empty.revision = _revision;
    if (upload.objects.empty())
        return empty;

    auto &frame = *_frames[_context->frameIndex()];
    if (!_grassFaces || _grassFaceGeneration != upload.grassFaceGeneration) {
        auto grassFaces = _context->makeBuffer();
        const GrassFace emptyFace {};
        const uint64_t faceCount = std::max<size_t>(1, upload.grassFaces.size());
        grassFaces->initDeviceStorage(
            faceCount * sizeof(GrassFace),
            upload.grassFaces.empty() ? static_cast<const void *>(&emptyFace)
                                      : static_cast<const void *>(upload.grassFaces.data()));
        if (_grassFaces)
            _retiredSourceBuffers.push_back(std::move(_grassFaces));
        _grassFaces = std::move(grassFaces);
        _grassFaceGeneration = upload.grassFaceGeneration;
    }
    uint64_t vertexCount = 0;
    uint64_t opaqueTriangleCount = 0;
    uint64_t nonOpaqueTriangleCount = 0;
    for (auto &input : upload.objects) {
        auto &object = input.data;
        if (input.sourceMesh) {
            _context->prepareMesh(*input.sourceMesh);
            const auto &source = appendSourceGeometry(*input.sourceMesh);
            const auto &layout = input.sourceMesh->vertexLayout();
            if (layout.stride % sizeof(float) != 0 ||
                layout.offPosition % static_cast<int>(sizeof(float)) != 0 ||
                (layout.offNormals >= 0 && layout.offNormals % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV1 >= 0 && layout.offUV1 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV2 >= 0 && layout.offUV2 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offTanSpace >= 0 && layout.offTanSpace % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneIndices >= 0 && layout.offBoneIndices % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneWeights >= 0 && layout.offBoneWeights % static_cast<int>(sizeof(float)) != 0))
            throw std::runtime_error("Source vertex attributes must be float-aligned");
            object.srcVertexOffset = source.vertexOffset;
            object.srcIndexOffset = source.indexOffset;
            object.srcVertexStride = static_cast<uint32_t>(layout.stride);
            object.offPosition = layout.offPosition;
            object.offNormals = layout.offNormals;
            object.offUV1 = layout.offUV1;
            object.offUV2 = layout.offUV2;
            object.offTanSpace = layout.offTanSpace;
            object.offBoneIndices = layout.offBoneIndices;
            object.offBoneWeights = layout.offBoneWeights;
            object.vertexCount = static_cast<uint32_t>(input.sourceMesh->vertexCount());
            object.triangleCount = static_cast<uint32_t>(input.sourceMesh->faces().size());
        }
        if (vertexCount + object.vertexCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Merged scene exceeds shader index range");
        auto &triangleBase = object.geometryIndex == 0 ? opaqueTriangleCount : nonOpaqueTriangleCount;
        if (triangleBase + object.triangleCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Merged scene exceeds shader index range");
        object.dstVertexBase = static_cast<uint32_t>(vertexCount);
        object.dstTriangleBase = static_cast<uint32_t>(triangleBase);
        vertexCount += object.vertexCount;
        triangleBase += object.triangleCount;
    }
    const uint64_t triangleCount = opaqueTriangleCount + nonOpaqueTriangleCount;
    if (vertexCount == 0 || triangleCount == 0)
        return empty;
    if (upload.objects.size() > std::numeric_limits<uint32_t>::max() ||
        upload.bones.size() > std::numeric_limits<uint32_t>::max() ||
        upload.danglyPositions.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Merged scene exceeds shader index range");

    uint64_t sceneObjectBytes = 0;
    uint64_t sceneBoneBytes = 0;
    uint64_t danglyPositionBytes = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    {
        R_PROFILE_ZONE("GpuScene::staging upload");
        ensureMergeBuffers(frame, static_cast<uint32_t>(upload.objects.size()),
                           static_cast<uint32_t>(upload.bones.size()),
                           static_cast<uint32_t>(vertexCount), static_cast<uint32_t>(triangleCount),
                           static_cast<uint32_t>(upload.proceduralQuads.size()),
                           static_cast<uint32_t>(upload.danglyPositions.size()),
                           static_cast<uint32_t>(upload.grassRanges.size()));
        sceneObjectBytes =
            static_cast<uint64_t>(frame.sceneObjectCapacity) * sizeof(SceneObject);
        sceneBoneBytes =
            static_cast<uint64_t>(frame.boneCapacity) * sizeof(Matrix3x4);
        danglyPositionBytes =
            static_cast<uint64_t>(frame.danglyPositionCapacity) * sizeof(glm::vec4);
        vertexBytes =
            static_cast<uint64_t>(frame.vertexCapacity) * sizeof(MergedVertex);
        indexBytes =
            static_cast<uint64_t>(frame.triangleCapacity) * 3 * sizeof(uint32_t);
        auto *sceneObjects = static_cast<SceneObject *>(frame.scene->mapped());
        for (size_t i = 0; i < upload.objects.size(); ++i)
            sceneObjects[i] = upload.objects[i].data;
        if (!upload.bones.empty())
            std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes,
                        upload.bones.data(), upload.bones.size() * sizeof(Matrix3x4));
        if (!upload.danglyPositions.empty())
            std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes + sceneBoneBytes,
                        upload.danglyPositions.data(), upload.danglyPositions.size() * sizeof(glm::vec4));
        if (!upload.proceduralQuads.empty())
            std::memcpy(frame.proceduralQuads->mapped(), upload.proceduralQuads.data(),
                        upload.proceduralQuads.size() * sizeof(ProceduralQuad));
        if (!upload.grassRanges.empty())
            std::memcpy(frame.grassRanges->mapped(), upload.grassRanges.data(),
                        upload.grassRanges.size() * sizeof(GrassRange));

        releaseBuffer(frame.materials);
        frame.materials = _context->makeBuffer();
        frame.materials->initHostVisibleStorage(
            static_cast<uint64_t>(upload.materials.size()) * sizeof(InstanceMaterial));
        std::memcpy(frame.materials->mapped(), upload.materials.data(),
                    upload.materials.size() * sizeof(InstanceMaterial));
    }

    {
        R_PROFILE_ZONE("GpuScene::command recording");
        const auto *sourceVertices =
            _sourceVertices ? _sourceVertices.get() : frame.proceduralQuads.get();
        const auto *sourceIndices =
            _sourceIndices ? _sourceIndices.get() : frame.proceduralQuads.get();
        std::array<BufferView, 11> buffers {{{frame.scene.get(), 0, sceneObjectBytes},
                                              {frame.scene.get(), sceneObjectBytes, sceneBoneBytes},
                                              {frame.geometry.get(), 0, vertexBytes},
                                              {frame.geometry.get(), vertexBytes, indexBytes},
                                              {frame.geometry.get(), vertexBytes + indexBytes,
                                               static_cast<uint64_t>(frame.triangleCapacity) * sizeof(uint32_t)},
                                              {sourceVertices, 0, sourceVertices->size()},
                                              {sourceIndices, 0, sourceIndices->size()},
                                              {frame.proceduralQuads.get(), 0,
                                               frame.proceduralQuads->size()},
                                              {frame.scene.get(), sceneObjectBytes + sceneBoneBytes,
                                               danglyPositionBytes},
                                              {_grassFaces.get(), 0, _grassFaces->size()},
                                              {frame.grassRanges.get(), 0,
                                               frame.grassRanges->size()}}};
        commandBuffer.makeGpuSceneSourcesAvailable(*sourceVertices, *sourceIndices);
        _mergePipeline->merge(commandBuffer, {buffers.data(), static_cast<uint32_t>(buffers.size()),
                                               static_cast<uint32_t>(_context->frameIndex()),
                                               static_cast<uint32_t>(upload.objects.size()),
                                               upload.opaqueObjectCount,
                                               static_cast<uint32_t>(vertexCount),
                                               static_cast<uint32_t>(triangleCount),
                                               static_cast<uint32_t>(opaqueTriangleCount),
                                               upload.cameraPosition});
        commandBuffer.publishMergedScene();
    }

    const uint64_t writtenVertexBytes =
        static_cast<uint64_t>(vertexCount) * sizeof(MergedVertex);
    const uint64_t writtenIndexBytes =
        static_cast<uint64_t>(triangleCount) * 3 * sizeof(uint32_t);
    const uint64_t writtenMaterialIdBytes =
        static_cast<uint64_t>(triangleCount) * sizeof(uint32_t);
    frame.primitiveIds.resize(upload.objects.size());
    for (size_t objectIndex = 0; objectIndex < upload.objects.size(); ++objectIndex) {
        const auto &input = upload.objects[objectIndex];
        const auto &object = input.data;
        const uint32_t first =
            (object.geometryIndex == 0 ? 0 : static_cast<uint32_t>(opaqueTriangleCount)) +
            object.dstTriangleBase;
        frame.primitiveIds[objectIndex] =
            {first, object.triangleCount,
             {_sceneScope, input.objectIndex, input.objectGeneration, 0}};
    }
    View view;
    view.sceneScope = _sceneScope;
    view.revision = _revision;
    view.vertices = {frame.geometry.get(), 0, writtenVertexBytes};
    view.indices = {frame.geometry.get(), vertexBytes, writtenIndexBytes};
    view.materialIds =
        {frame.geometry.get(), vertexBytes + indexBytes, writtenMaterialIdBytes};
    view.materials = {frame.materials.get(), 0, frame.materials->size()};
    view.objectCount = static_cast<uint32_t>(upload.objects.size());
    view.opaqueObjectCount = upload.opaqueObjectCount;
    view.vertexCount = static_cast<uint32_t>(vertexCount);
    view.opaqueTriangleCount = static_cast<uint32_t>(opaqueTriangleCount);
    view.triangleCount = static_cast<uint32_t>(triangleCount);
    view.primitiveIds =
        {frame.primitiveIds.data(), static_cast<uint32_t>(frame.primitiveIds.size())};
    view.regions = {{GpuSceneResidencyClass::Dynamic, _revision, 0,
                     static_cast<uint32_t>(vertexCount), 0,
                     static_cast<uint32_t>(triangleCount)}};
    return view;
}

} // namespace reone::graphics
