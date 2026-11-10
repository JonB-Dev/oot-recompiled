// Exercise Plume's Metal BLAS path on a real Apple GPU without launching OoT.
#include "plume_metal.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct ConverterASHeader {
    uint64_t accelerationStructureID;
    uint64_t addressOfInstanceContributions;
    uint64_t padding[4] = {};
    MTL::DispatchThreadgroupsIndirectArguments indirectArgs = {};
};
static_assert(sizeof(ConverterASHeader) == 64);

struct DrawableStyleTexture : plume::ExtendedRenderTexture {
    MTL::Texture *native = nullptr;
    MTL::Texture *notAMetalTextureMember = nullptr;
    mutable unsigned nativeReads = 0;
    explicit DrawableStyleTexture(const plume::MetalTexture *source) {
        desc = source->desc;
        native = source->mtl;
    }
    MTL::Texture *getTexture() const override { ++nativeReads; return native; }
    std::unique_ptr<plume::RenderTextureView> createTextureView(
        const plume::RenderTextureViewDesc &) const override { return nullptr; }
    void setName(const std::string &) override {}
};

int main(int argc, char **argv) {
    plume::MetalInterface renderer;
    auto device = renderer.createDevice("");
    if (!device || !static_cast<plume::MetalDevice *>(device.get())->mtl->supportsRaytracing()) {
        std::fprintf(stderr, "Metal ray tracing device unavailable\n");
        return 1;
    }

    if (argc == 2 && std::strcmp(argv[1], "--command-lifetime-only") == 0) {
        auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        auto commands = queue->createCommandList();
        commands->begin();
        const auto *native = static_cast<plume::MetalCommandList *>(commands.get())->mtl;
        const bool retained = native->retainedReferences();
        const bool detailed = (native->errorOptions() & MTL::CommandBufferErrorOptionEncoderExecutionStatus) != 0;
        commands->end();
        if (!retained || !detailed) {
            std::fprintf(stderr, "Metal commands need retained resources and encoder error details\n");
            return 1;
        }
        std::printf("METAL_COMMAND_LIFETIME_OK (no GPU submission)\n");
        return 0;
    }

    // CPU encoding only: no command buffer is submitted to the GPU in this mode.
    // A drawable is not a MetalTexture; its native texture must use the virtual API.
    if (argc == 2 && std::strcmp(argv[1], "--texture-types-only") == 0) {
        auto texture = device->createTexture(plume::RenderTextureDesc::ColorTarget(
            2, 2, plume::RenderFormat::B8G8R8A8_UNORM));
        DrawableStyleTexture drawable(static_cast<const plume::MetalTexture *>(texture.get()));
        const plume::RenderDescriptorRange range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
        auto set = device->createDescriptorSet({&range, 1});
        set->setTexture(0, &drawable, plume::RenderTextureLayout::SHADER_READ, nullptr);
        auto *nativeSet = static_cast<plume::MetalDescriptorSet *>(set.get());
        if (nativeSet->resourceEntries[0].resource != drawable.native) {
            std::fprintf(stderr, "Drawable descriptor read the regular texture member\n");
            return 1;
        }
        const plume::RenderTexture *attachment = &drawable;
        auto framebuffer = device->createFramebuffer({&attachment, 1});
        auto *nativeFramebuffer = static_cast<plume::MetalFramebuffer *>(framebuffer.get());
        if (framebuffer->getWidth() != 2 || framebuffer->getHeight() != 2 ||
            nativeFramebuffer->colorAttachments[0].getTexture() != drawable.native) {
            std::fprintf(stderr, "Drawable framebuffer lost its texture or size\n");
            return 1;
        }
        auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        auto commands = queue->createCommandList();
        auto destination = device->createTexture(plume::RenderTextureDesc::ColorTarget(
            2, 2, plume::RenderFormat::B8G8R8A8_UNORM));
        drawable.nativeReads = 0;
        commands->begin();
        commands->copyTexture(destination.get(), &drawable);
        commands->end();
        if (drawable.nativeReads == 0) {
            std::fprintf(stderr, "Drawable copy bypassed its native texture interface\n");
            return 1;
        }
        std::printf("METAL_DRAWABLE_TYPES_OK (no GPU submission)\n");
        return 0;
    }

    constexpr std::array<float, 9> vertices = {-1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f};
    constexpr std::array<uint32_t, 3> indices = {0, 1, 2};
    auto vertexBuffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(vertices)));
    auto indexBuffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(indices)));
    std::memcpy(vertexBuffer->map(), vertices.data(), sizeof(vertices));
    vertexBuffer->unmap();
    std::memcpy(indexBuffer->map(), indices.data(), sizeof(indices));
    indexBuffer->unmap();

    plume::RenderBottomLevelASMesh mesh(indexBuffer->at(0), vertexBuffer->at(0),
        plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32B32_FLOAT, 3, 3,
        3 * sizeof(float), true);
    plume::RenderBottomLevelASBuildInfo info;
    device->setBottomLevelASBuildInfo(info, &mesh, 1, true, false);
    if (info.meshCount != 1 || info.primitiveCount != 1 ||
        info.accelerationStructureSize == 0 || info.scratchSize == 0 ||
        info.buildData.size() != sizeof(mesh)) {
        std::fprintf(stderr, "Metal BLAS sizing returned incomplete information\n");
        return 1;
    }

    auto backing = device->createBuffer(
        plume::RenderBufferDesc::AccelerationStructureBuffer(info.accelerationStructureSize));
    auto structure = device->createAccelerationStructure({
        plume::RenderAccelerationStructureType::BOTTOM_LEVEL, backing->at(0),
        info.accelerationStructureSize});
    const auto *metalBacking = static_cast<const plume::MetalBuffer *>(backing.get());
    if (!structure || !metalBacking->accelerationStructure || metalBacking->mtl->length() != 16) {
        std::fprintf(stderr, "Metal BLAS backing allocation failed or duplicated storage\n");
        return 1;
    }
    auto scratch = device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        info.scratchSize, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
    auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    // Regression: a capture destination is a readback buffer, not a texture.
    // The old Metal fallback passed it to copyFromTexture as a texture and crashed.
    constexpr uint32_t rowPitch = 256;
    auto captureUpload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(rowPitch * 2));
    auto captureReadback = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(rowPitch * 2));
    auto captureTexture = device->createTexture(plume::RenderTextureDesc::Texture2D(
        2, 2, 1, plume::RenderFormat::B8G8R8A8_UNORM));
    DrawableStyleTexture captureSource(static_cast<const plume::MetalTexture *>(captureTexture.get()));
    auto *uploadBytes = static_cast<uint8_t *>(captureUpload->map());
    constexpr uint8_t pixels[16] = {1, 2, 3, 255, 4, 5, 6, 255,
                                    7, 8, 9, 255, 10, 11, 12, 255};
    std::memcpy(uploadBytes, pixels, 8);
    std::memcpy(uploadBytes + rowPitch, pixels + 8, 8);
    captureUpload->unmap();
    auto copyCommands = queue->createCommandList();
    auto copyFence = device->createCommandFence();
    copyCommands->begin();
    copyCommands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(captureTexture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(captureUpload.get(),
            plume::RenderFormat::B8G8R8A8_UNORM, 2, 2, 1, rowPitch / 4));
    copyCommands->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(captureReadback.get(),
            plume::RenderFormat::B8G8R8A8_UNORM, 2, 2, 1, rowPitch / 4),
        plume::RenderTextureCopyLocation::Subresource(&captureSource));
    copyCommands->end();
    const plume::RenderCommandList *copyLists[] = {copyCommands.get()};
    queue->executeCommandLists(copyLists, 1, nullptr, 0, nullptr, 0, copyFence.get());
    queue->waitForCommandFence(copyFence.get());
    const auto *readbackBytes = static_cast<const uint8_t *>(captureReadback->map());
    const bool copyMatches = std::memcmp(readbackBytes, pixels, 8) == 0 &&
                             std::memcmp(readbackBytes + rowPitch, pixels + 8, 8) == 0;
    captureReadback->unmap();
    if (!copyMatches) {
        std::fprintf(stderr, "Metal texture-to-readback copy corrupted capture pixels\n");
        return 1;
    }
    std::printf("METAL_CAPTURE_READBACK_OK 2x2\n");
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    auto secondBacking = device->createBuffer(
        plume::RenderBufferDesc::AccelerationStructureBuffer(info.accelerationStructureSize));
    auto secondStructure = device->createAccelerationStructure({
        plume::RenderAccelerationStructureType::BOTTOM_LEVEL, secondBacking->at(0), info.accelerationStructureSize});
    auto secondScratch = device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        info.scratchSize, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
    commands->begin();
    commands->buildBottomLevelAS(structure.get(), scratch->at(0), info);
    auto *nativeList = static_cast<plume::MetalCommandList *>(commands.get());
    auto *firstEncoder = nativeList->activeAccelerationEncoder;
    commands->buildBottomLevelAS(secondStructure.get(), secondScratch->at(0), info);
    if (firstEncoder == nullptr || nativeList->activeAccelerationEncoder != firstEncoder) {
        std::fprintf(stderr, "Independent BLAS builds did not share an encoder\n");
        return 1;
    }
    commands->barriers(plume::RenderBarrierStage::COMPUTE,
        plume::RenderBufferBarrier(backing.get(), plume::RenderBufferAccess::READ));
    if (nativeList->activeAccelerationEncoder != nullptr) {
        std::fprintf(stderr, "An AS barrier did not finish the build batch\n");
        return 1;
    }
    commands->buildBottomLevelAS(secondStructure.get(), secondScratch->at(0), info);
    commands->end();
    if (nativeList->activeAccelerationEncoder != nullptr) {
        std::fprintf(stderr, "Command list end did not finish the final AS build batch\n");
        nativeList->activeAccelerationEncoder->endEncoding();
        nativeList->activeAccelerationEncoder->release();
        nativeList->activeAccelerationEncoder = nullptr;
        return 1;
    }
    auto *nativeCommands = static_cast<plume::MetalCommandList *>(commands.get())->mtl;
    nativeCommands->retain(); // The queue clears MetalCommandList::mtl when it commits.
    const plume::RenderCommandList *lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());
    if (nativeCommands->status() != MTL::CommandBufferStatusCompleted) {
        const NS::Error *error = nativeCommands->error();
        std::fprintf(stderr, "Metal BLAS build failed on GPU: %s\n",
            error ? error->localizedDescription()->utf8String() : "no Metal error");
        nativeCommands->release();
        return 1;
    }
    nativeCommands->release();

    plume::RenderTopLevelASInstance instance(backing->at(0), 17, 0xff, 1, false,
                                             plume::RenderAffineTransform());
    plume::RenderTopLevelASBuildInfo topInfo;
    device->setTopLevelASBuildInfo(topInfo, &instance, 1, true, false);
    MTL::AccelerationStructureUserIDInstanceDescriptor nativeInstance{};
    if (topInfo.instanceCount != 1 || topInfo.scratchSize == 0 ||
        topInfo.accelerationStructureSize == 0 ||
        topInfo.instancesBufferData.size() != sizeof(nativeInstance) ||
        topInfo.buildData.size() != sizeof(instance)) {
        std::fprintf(stderr, "Metal TLAS sizing returned incomplete information\n");
        return 1;
    }
    std::memcpy(&nativeInstance, topInfo.instancesBufferData.data(), sizeof(nativeInstance));
    if (nativeInstance.userID != 17 || nativeInstance.mask != 0xff ||
        nativeInstance.transformationMatrix[0][0] != 1.f ||
        nativeInstance.transformationMatrix[3][0] != 0.f) {
        std::fprintf(stderr, "Metal TLAS instance ID or transform was corrupted\n");
        return 1;
    }
    auto topBacking = device->createBuffer(
        plume::RenderBufferDesc::AccelerationStructureBuffer(topInfo.accelerationStructureSize));
    auto topStructure = device->createAccelerationStructure({
        plume::RenderAccelerationStructureType::TOP_LEVEL, topBacking->at(0),
        topInfo.accelerationStructureSize});
    auto topScratch = device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        topInfo.scratchSize, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
    auto instances = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        topInfo.instancesBufferData.size(), plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT));
    std::memcpy(instances->map(), topInfo.instancesBufferData.data(), topInfo.instancesBufferData.size());
    instances->unmap();
    auto topCommands = queue->createCommandList();
    auto topFence = device->createCommandFence();
    topCommands->begin();
    topCommands->buildTopLevelAS(topStructure.get(), topScratch->at(0), instances->at(0), topInfo);
    topCommands->end();
    auto *nativeTopCommands = static_cast<plume::MetalCommandList *>(topCommands.get())->mtl;
    nativeTopCommands->retain();
    const plume::RenderCommandList *topLists[] = {topCommands.get()};
    queue->executeCommandLists(topLists, 1, nullptr, 0, nullptr, 0, topFence.get());
    queue->waitForCommandFence(topFence.get());
    if (nativeTopCommands->status() != MTL::CommandBufferStatusCompleted) {
        const NS::Error *error = nativeTopCommands->error();
        std::fprintf(stderr, "Metal TLAS build failed on GPU: %s\n",
            error ? error->localizedDescription()->utf8String() : "no Metal error");
        nativeTopCommands->release();
        return 1;
    }
    nativeTopCommands->release();

    const plume::RenderDescriptorRange sceneRange(
        plume::RenderDescriptorRangeType::ACCELERATION_STRUCTURE, 0, 1);
    auto sceneSet = device->createDescriptorSet(plume::RenderDescriptorSetDesc(&sceneRange, 1));
    sceneSet->setAccelerationStructure(0, topStructure.get());
    auto *metalSet = static_cast<plume::MetalDescriptorSet *>(sceneSet.get());
    auto *nativeScene = static_cast<plume::MetalBuffer *>(topBacking.get())->accelerationStructure;
    const uint64_t sceneID = nativeScene->gpuResourceID()._impl;
    const auto *encodedSceneID = static_cast<const uint64_t *>(metalSet->argumentBuffer.mtl->contents());
    if (metalSet->resourceEntries[0].resource != nativeScene || *encodedSceneID != sceneID) {
        std::fprintf(stderr, "Metal TLAS descriptor did not bind the scene resource\n");
        return 1;
    }

    // Query through Plume's TLAS, proving that its instance record reaches the BLAS.
    static const char *shader = R"(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;
struct SceneArgs { instance_acceleration_structure scene [[id(0)]]; };
kernel void probe(constant SceneArgs &args [[buffer(0)]],
                  device float *result [[buffer(1)]]) {
    ray r;
    r.origin = float3(0.0, 0.0, 1.0);
    r.direction = float3(0.0, 0.0, -1.0);
    r.min_distance = 0.0;
    r.max_distance = 10.0;
    intersector<triangle_data, instancing> query;
    result[0] = query.intersect(r, args.scene).distance;
}
)";
    auto *metalDevice = static_cast<plume::MetalDevice *>(device.get())->mtl;
    NS::Error *shaderError = nullptr;
    auto *library = metalDevice->newLibrary(NS::String::string(shader, NS::UTF8StringEncoding),
                                            nullptr, &shaderError);
    if (!library) {
        std::fprintf(stderr, "Metal TLAS query shader failed: %s\n",
            shaderError ? shaderError->localizedDescription()->utf8String() : "no Metal error");
        return 1;
    }
    auto *function = library->newFunction(NS::String::string("probe", NS::UTF8StringEncoding));
    auto *pipeline = metalDevice->newComputePipelineState(function, &shaderError);
    if (!pipeline) {
        std::fprintf(stderr, "Metal TLAS query pipeline failed: %s\n",
            shaderError ? shaderError->localizedDescription()->utf8String() : "no Metal error");
        return 1;
    }
    auto *result = metalDevice->newBuffer(sizeof(float), MTL::ResourceStorageModeShared);
    auto *rawQueue = static_cast<plume::MetalCommandQueue *>(queue.get())->mtl;
    auto *queryCommands = rawQueue->commandBuffer();
    auto *encoder = queryCommands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(metalSet->argumentBuffer.mtl, metalSet->argumentBuffer.offset, 0);
    encoder->useResource(nativeScene, MTL::ResourceUsageRead);
    encoder->setBuffer(result, 0, 1);
    encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    queryCommands->commit();
    queryCommands->waitUntilCompleted();
    if (queryCommands->status() != MTL::CommandBufferStatusCompleted ||
        std::fabs(*static_cast<float *>(result->contents()) - 1.f) > 0.001f) {
        std::fprintf(stderr, "Metal TLAS query did not hit the test triangle\n");
        return 1;
    }
    result->release();
    pipeline->release();
    function->release();
    library->release();

    sceneSet->setAccelerationStructure(0, nullptr);
    if (metalSet->resourceEntries[0].resource != nullptr || *encodedSceneID != 0) {
        std::fprintf(stderr, "Metal TLAS descriptor did not clear the scene resource\n");
        return 1;
    }

    plume::RenderTopLevelASBuildInfo emptyInfo;
    device->setTopLevelASBuildInfo(emptyInfo, nullptr, 0, true, false);
    if (emptyInfo.instanceCount != 0 || emptyInfo.accelerationStructureSize == 0 ||
        emptyInfo.scratchSize == 0 || emptyInfo.instancesBufferData.empty()) {
        std::fprintf(stderr, "Metal empty TLAS sizing failed\n");
        return 1;
    }
    auto emptyBacking = device->createBuffer(
        plume::RenderBufferDesc::AccelerationStructureBuffer(emptyInfo.accelerationStructureSize));
    auto emptyStructure = device->createAccelerationStructure({
        plume::RenderAccelerationStructureType::TOP_LEVEL, emptyBacking->at(0),
        emptyInfo.accelerationStructureSize});
    auto emptyScratch = device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        emptyInfo.scratchSize, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
    auto emptyInstances = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        emptyInfo.instancesBufferData.size(), plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT));
    auto emptyCommands = queue->createCommandList();
    auto emptyFence = device->createCommandFence();
    emptyCommands->begin();
    emptyCommands->buildTopLevelAS(emptyStructure.get(), emptyScratch->at(0),
                                   emptyInstances->at(0), emptyInfo);
    emptyCommands->end();
    auto *nativeEmptyCommands = static_cast<plume::MetalCommandList *>(emptyCommands.get())->mtl;
    nativeEmptyCommands->retain();
    const plume::RenderCommandList *emptyLists[] = {emptyCommands.get()};
    queue->executeCommandLists(emptyLists, 1, nullptr, 0, nullptr, 0, emptyFence.get());
    queue->waitForCommandFence(emptyFence.get());
    if (nativeEmptyCommands->status() != MTL::CommandBufferStatusCompleted) {
        const NS::Error *error = nativeEmptyCommands->error();
        std::fprintf(stderr, "Metal empty TLAS build failed on GPU: %s\n",
            error ? error->localizedDescription()->utf8String() : "no Metal error");
        nativeEmptyCommands->release();
        return 1;
    }
    nativeEmptyCommands->release();

    // The converter's 24-byte table retains register slots, even for unsorted
    // ranges. Raster's eight-byte argument buffer is encoded separately.
    const plume::RenderDescriptorRange converterRanges[] = {
        {plume::RenderDescriptorRangeType::TEXTURE, 9, 1},
        {plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 4, 1},
        {plume::RenderDescriptorRangeType::SAMPLER, 7, 1},
    };
    auto converterSet = device->createDescriptorSet({converterRanges, 3});
    auto *metalConverterSet = static_cast<plume::MetalDescriptorSet *>(converterSet.get());
    auto dataBuffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(32));
    auto texture = device->createTexture(plume::RenderTextureDesc::Texture2D(
        1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    auto sampler = device->createSampler(plume::RenderSamplerDesc());
    converterSet->setTexture(0, texture.get(), plume::RenderTextureLayout::SHADER_READ, nullptr);
    converterSet->setBuffer(1, dataBuffer.get(), 16, nullptr, nullptr);
    converterSet->setSampler(2, sampler.get());
    const auto &entries = metalConverterSet->converterEntries;
    if (entries.size() != 10 ||
        entries[4].gpuVA != static_cast<plume::MetalBuffer *>(dataBuffer.get())->mtl->gpuAddress() ||
        entries[4].metadata != 16 ||
        entries[9].textureViewID != static_cast<plume::MetalTexture *>(texture.get())->mtl->gpuResourceID()._impl ||
        entries[7].gpuVA != static_cast<plume::MetalSampler *>(sampler.get())->state->gpuResourceID()._impl ||
        entries[0].gpuVA != 0 || entries[8].textureViewID != 0) {
        std::fprintf(stderr, "Metal converter descriptor table encoded wrong resources or slots\n");
        return 1;
    }
    converterSet->setBuffer(1, nullptr, 0, nullptr, nullptr);
    converterSet->setTexture(0, nullptr, plume::RenderTextureLayout::SHADER_READ, nullptr);
    converterSet->setSampler(2, nullptr);
    if (entries[4].gpuVA != 0 || entries[4].metadata != 0 ||
        entries[9].textureViewID != 0 || entries[7].gpuVA != 0) {
        std::fprintf(stderr, "Metal converter descriptor table did not clear resources\n");
        return 1;
    }
    const plume::RenderSampler *immutableSampler[] = {sampler.get()};
    const plume::RenderDescriptorRange immutableRange(
        plume::RenderDescriptorRangeType::SAMPLER, 7, 1, immutableSampler);
    auto immutableSet = device->createDescriptorSet({&immutableRange, 1});
    if (static_cast<plume::MetalDescriptorSet *>(immutableSet.get())->converterEntries[7].gpuVA !=
        static_cast<plume::MetalSampler *>(sampler.get())->state->gpuResourceID()._impl) {
        std::fprintf(stderr, "Metal converter table omitted the immutable sampler\n");
        return 1;
    }

    if (argc >= 2) {
        // Run Apple's converted HLSL probe through Plume's live descriptor encoding.
        const plume::RenderDescriptorRange convertedRanges[] = {
            {plume::RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER, 0, 1},
            {plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 1, 1},
        };
        auto convertedSet = device->createDescriptorSet({convertedRanges, 2});
        auto convertedInput = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(4));
        auto convertedOutput = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(4));
        *static_cast<uint32_t *>(convertedInput->map()) = 35;
        convertedInput->unmap();
        *static_cast<uint32_t *>(convertedOutput->map()) = 0;
        convertedOutput->unmap();
        convertedSet->setBuffer(0, convertedOutput.get(), 4, nullptr, nullptr);
        convertedSet->setBuffer(1, convertedInput.get(), 4, nullptr, nullptr);
        const auto &convertedEntries = static_cast<plume::MetalDescriptorSet *>(convertedSet.get())->converterEntries;
        auto *table = metalDevice->newBuffer(convertedEntries.size() * sizeof(convertedEntries[0]),
                                              MTL::ResourceStorageModeShared);
        std::memcpy(table->contents(), convertedEntries.data(), table->length());
        auto *root = metalDevice->newBuffer(sizeof(uint64_t), MTL::ResourceStorageModeShared);
        *static_cast<uint64_t *>(root->contents()) = table->gpuAddress();
        NS::Error *convertedError = nullptr;
        auto *convertedLibrary = metalDevice->newLibrary(
            NS::URL::fileURLWithPath(NS::String::string(argv[1], NS::UTF8StringEncoding)),
            &convertedError);
        if (convertedLibrary == nullptr) {
            std::fprintf(stderr, "Converted probe library failed to load: %s\n",
                convertedError ? convertedError->localizedDescription()->utf8String() : "no Metal error");
            return 1;
        }
        auto *convertedFunction = convertedLibrary->newFunction(NS::String::string("CSMain", NS::UTF8StringEncoding));
        auto *convertedPipeline = metalDevice->newComputePipelineState(convertedFunction, &convertedError);
        if (convertedPipeline == nullptr) {
            std::fprintf(stderr, "Converted probe pipeline failed: %s\n",
                convertedError ? convertedError->localizedDescription()->utf8String() : "no Metal error");
            return 1;
        }
        auto *convertedCommands = rawQueue->commandBuffer();
        auto *convertedEncoder = convertedCommands->computeCommandEncoder();
        convertedEncoder->setComputePipelineState(convertedPipeline);
        convertedEncoder->setBuffer(root, 0, 2);
        convertedEncoder->useResource(table, MTL::ResourceUsageRead);
        convertedEncoder->useResource(static_cast<plume::MetalBuffer *>(convertedInput.get())->mtl,
                                      MTL::ResourceUsageRead);
        convertedEncoder->useResource(static_cast<plume::MetalBuffer *>(convertedOutput.get())->mtl,
                                      MTL::ResourceUsageWrite);
        convertedEncoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        convertedEncoder->endEncoding();
        convertedCommands->commit();
        convertedCommands->waitUntilCompleted();
        if (convertedCommands->status() != MTL::CommandBufferStatusCompleted ||
            *static_cast<uint32_t *>(convertedOutput->map()) != 42) {
            std::fprintf(stderr, "Plume's converted HLSL descriptor probe did not return 42\n");
            return 1;
        }
        convertedOutput->unmap();
        convertedPipeline->release();
        convertedFunction->release();
        convertedLibrary->release();
        root->release();
        table->release();
    }

    if (argc >= 3) {
        const plume::RenderDescriptorRange rayRanges[] = {
            {plume::RenderDescriptorRangeType::ACCELERATION_STRUCTURE, 0, 1},
            {plume::RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER, 1, 1},
        };
        auto raySet = device->createDescriptorSet({rayRanges, 2});
        auto rayOutput = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(4));
        raySet->setAccelerationStructure(0, topStructure.get());
        raySet->setBuffer(1, rayOutput.get(), 4, nullptr, nullptr);
        const auto &rayEntries = static_cast<plume::MetalDescriptorSet *>(raySet.get())->converterEntries;
        auto *contributions = metalDevice->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
        *static_cast<uint32_t *>(contributions->contents()) = 1;
        auto *headerBuffer = metalDevice->newBuffer(sizeof(ConverterASHeader), MTL::ResourceStorageModeShared);
        auto *header = static_cast<ConverterASHeader *>(headerBuffer->contents());
        *header = {};
        header->accelerationStructureID = nativeScene->gpuResourceID()._impl;
        header->addressOfInstanceContributions = contributions->gpuAddress();
        auto *rayTable = metalDevice->newBuffer(rayEntries.size() * sizeof(rayEntries[0]),
                                                MTL::ResourceStorageModeShared);
        std::memcpy(rayTable->contents(), rayEntries.data(), rayTable->length());
        static_cast<plume::MetalConverterDescriptorEntry *>(rayTable->contents())[0].gpuVA =
            headerBuffer->gpuAddress();
        auto *rayRoot = metalDevice->newBuffer(sizeof(uint64_t), MTL::ResourceStorageModeShared);
        *static_cast<uint64_t *>(rayRoot->contents()) = rayTable->gpuAddress();
        NS::Error *rayError = nullptr;
        auto *rayLibrary = metalDevice->newLibrary(
            NS::URL::fileURLWithPath(NS::String::string(argv[2], NS::UTF8StringEncoding)), &rayError);
        if (rayLibrary == nullptr) {
            std::fprintf(stderr, "Converted ray query library failed: %s\n",
                rayError ? rayError->localizedDescription()->utf8String() : "no Metal error");
            return 1;
        }
        auto *rayFunction = rayLibrary->newFunction(NS::String::string("CSMain", NS::UTF8StringEncoding));
        auto *rayPipeline = metalDevice->newComputePipelineState(rayFunction, &rayError);
        if (rayPipeline == nullptr) {
            std::fprintf(stderr, "Converted ray query pipeline failed: %s\n",
                rayError ? rayError->localizedDescription()->utf8String() : "no Metal error");
            return 1;
        }
        auto *rayCommands = rawQueue->commandBuffer();
        auto *rayEncoder = rayCommands->computeCommandEncoder();
        rayEncoder->setComputePipelineState(rayPipeline);
        rayEncoder->setBuffer(rayRoot, 0, 2);
        rayEncoder->useResource(rayTable, MTL::ResourceUsageRead);
        rayEncoder->useResource(headerBuffer, MTL::ResourceUsageRead);
        rayEncoder->useResource(contributions, MTL::ResourceUsageRead);
        rayEncoder->useResource(nativeScene, MTL::ResourceUsageRead);
        rayEncoder->useResource(static_cast<plume::MetalBuffer *>(rayOutput.get())->mtl,
                                MTL::ResourceUsageWrite);
        rayEncoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        rayEncoder->endEncoding();
        rayCommands->commit();
        rayCommands->waitUntilCompleted();
        const uint32_t distanceBits = *static_cast<uint32_t *>(rayOutput->map());
        rayOutput->unmap();
        float rayDistance = 0.f;
        std::memcpy(&rayDistance, &distanceBits, sizeof(rayDistance));
        if (rayCommands->status() != MTL::CommandBufferStatusCompleted ||
            std::fabs(rayDistance - 1.f) > 0.001f) {
            std::fprintf(stderr, "Converted ray query missed Plume's TLAS: status=%ld distance=%f\n",
                static_cast<long>(rayCommands->status()), rayDistance);
            return 1;
        }
        std::printf("METAL_CONVERTER_RAY_QUERY_OK distance=%f\n", rayDistance);
        rayPipeline->release();
        rayFunction->release();
        rayLibrary->release();
        rayRoot->release();
        rayTable->release();
        headerBuffer->release();
        contributions->release();
    }
    if (argc == 4) {
        if (!device->getCapabilities().raytracing) {
            std::fprintf(stderr, "Explicit Metal ray tracing opt-in was not advertised\n");
            return 1;
        }
        const uint32_t dxilMarker = 0;
        auto dxilLibrary = device->createShader(&dxilMarker, sizeof(dxilMarker), "",
            plume::RenderShaderFormat::DXIL);
        if (!dxilLibrary) {
            std::fprintf(stderr, "Plume refused the DXIL ray library placeholder\n");
            return 1;
        }
        dxilLibrary->setName("Test ray library");
        setenv("RT64_METAL_RT_SHADER_DIR", argv[3], 1);
        auto layout = device->createPipelineLayout(plume::RenderPipelineLayoutDesc());
        const plume::RenderRaytracingPipelineLibrarySymbol symbols[] = {
            {"PrimaryRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"DirectRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"IndirectRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"ReflectionRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"RefractionRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"OcclusionRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"LightsRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"ScatterRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN},
            {"SurfaceMiss", plume::RenderRaytracingPipelineLibrarySymbolType::MISS},
            {"ShadowMiss", plume::RenderRaytracingPipelineLibrarySymbolType::MISS},
            {"SurfaceClosestHit", plume::RenderRaytracingPipelineLibrarySymbolType::CLOSEST_HIT},
            {"SurfaceAnyHit", plume::RenderRaytracingPipelineLibrarySymbolType::ANY_HIT},
            {"ShadowAnyHit", plume::RenderRaytracingPipelineLibrarySymbolType::ANY_HIT},
        };
        const plume::RenderRaytracingPipelineHitGroup groups[] = {
            {"SurfaceHitGroup", "SurfaceClosestHit", "SurfaceAnyHit"},
            {"ShadowHitGroup", nullptr, "ShadowAnyHit"},
        };
        const plume::RenderRaytracingPipelineLibrary rayLibrary(nullptr, symbols,
            uint32_t(std::size(symbols)));
        plume::RenderRaytracingPipelineDesc rayDesc;
        rayDesc.libraries = &rayLibrary;
        rayDesc.librariesCount = 1;
        rayDesc.hitGroups = groups;
        rayDesc.hitGroupsCount = uint32_t(std::size(groups));
        rayDesc.pipelineLayout = layout.get();
        const std::string missingDirectory = std::string(argv[3]) + "/missing";
        setenv("RT64_METAL_RT_SHADER_DIR", missingDirectory.c_str(), 1);
        if (device->createRaytracingPipeline(rayDesc, nullptr)) {
            std::fprintf(stderr, "Plume accepted a ray pipeline with missing Metal libraries\n");
            return 1;
        }
        setenv("RT64_METAL_RT_SHADER_DIR", argv[3], 1);
        auto rayPipeline = device->createRaytracingPipeline(rayDesc, nullptr);
        if (!rayPipeline || !rayPipeline->isValid() ||
            rayPipeline->getProgram("PrimaryRayGen").programIndex != 0 ||
            rayPipeline->getProgram("SurfaceHitGroup").programIndex != std::size(symbols)) {
            std::fprintf(stderr, "Plume failed to build the converted OoT ray pipeline\n");
            return 1;
        }
        const auto *nativeRayPipeline = static_cast<const plume::MetalRaytracingPipeline *>(rayPipeline.get());
        if (nativeRayPipeline->rayGenerations.size() != 8) {
            std::fprintf(stderr, "Plume linked the wrong number of ray generation passes\n");
            return 1;
        }
        for (const auto &state : nativeRayPipeline->rayGenerations) {
            if (!state.pipeline || !state.visibleTable || !state.intersectionTable) {
                std::fprintf(stderr, "Plume omitted a linked ray function table\n");
                return 1;
            }
        }
        const char *rayNames[] = {"PrimaryRayGen", "DirectRayGen", "IndirectRayGen",
            "ReflectionRayGen", "RefractionRayGen", "OcclusionRayGen", "LightsRayGen", "ScatterRayGen"};
        plume::RenderPipelineProgram rayPrograms[8];
        for (uint32_t i = 0; i < 8; i++) rayPrograms[i] = rayPipeline->getProgram(rayNames[i]);
        const plume::RenderPipelineProgram missPrograms[] = {
            rayPipeline->getProgram("SurfaceMiss"), rayPipeline->getProgram("ShadowMiss")};
        const plume::RenderPipelineProgram hitPrograms[] = {
            rayPipeline->getProgram("SurfaceHitGroup"), rayPipeline->getProgram("ShadowHitGroup")};
        plume::RenderShaderBindingTableInfo bindingTable;
        device->setShaderBindingTableInfo(bindingTable,
            {plume::RenderShaderBindingGroup(rayPrograms, 8),
             plume::RenderShaderBindingGroup(missPrograms, 2),
             plume::RenderShaderBindingGroup(hitPrograms, 2)},
            rayPipeline.get(), nullptr, 0);
        auto readIdentifier = [&](uint64_t offset) {
            plume::MetalIRShaderIdentifier identifier;
            std::memcpy(&identifier, bindingTable.tableBufferData.data() + offset, sizeof(identifier));
            return identifier;
        };
        const auto miss = readIdentifier(bindingTable.groups.miss.offset);
        const auto surface = readIdentifier(bindingTable.groups.hitGroup.offset);
        const auto shadow = readIdentifier(bindingTable.groups.hitGroup.offset + bindingTable.groups.hitGroup.stride);
        if (bindingTable.groups.rayGen.size != 8 * 64 ||
            bindingTable.groups.miss.offset != 8 * 64 ||
            bindingTable.groups.hitGroup.offset != 10 * 64 ||
            bindingTable.tableBufferData.size() != 12 * 64 ||
            miss.shaderHandle != 1 || surface.shaderHandle != 3 ||
            surface.intersectionShaderHandle != 4 || shadow.shaderHandle != 0 ||
            shadow.intersectionShaderHandle != 5) {
            std::fprintf(stderr, "Plume encoded incorrect Metal ray shader identifiers\n");
            return 1;
        }
        std::printf("METAL_PLUME_RAY_PIPELINE_OK raygen=%zu\n", nativeRayPipeline->rayGenerations.size());

        // A converted ray generation shader must see Plume's copied root table,
        // TLAS header, and instance contributions during a real GPU dispatch.
        const plume::RenderRaytracingPipelineLibrarySymbol probeSymbol =
            {"TestRayGen", plume::RenderRaytracingPipelineLibrarySymbolType::RAYGEN};
        const plume::RenderRaytracingPipelineLibrary probeLibrary(nullptr, &probeSymbol, 1);
        plume::RenderRaytracingPipelineDesc probeDesc;
        probeDesc.libraries = &probeLibrary;
        probeDesc.librariesCount = 1;
        probeDesc.pipelineLayout = layout.get();
        auto probePipeline = device->createRaytracingPipeline(probeDesc, nullptr);
        if (!probePipeline || !probePipeline->isValid()) {
            std::fprintf(stderr, "Plume failed to link the ray dispatch regression shader\n");
            return 1;
        }
        const plume::RenderDescriptorRange probeRanges[] = {
            {plume::RenderDescriptorRangeType::ACCELERATION_STRUCTURE, 0, 1},
            {plume::RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER, 1, 1},
        };
        auto probeSet = device->createDescriptorSet({probeRanges, 2});
        auto probeOutput = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(uint32_t)));
        *static_cast<uint32_t *>(probeOutput->map()) = 0;
        probeOutput->unmap();
        probeSet->setAccelerationStructure(0, topStructure.get());
        probeSet->setBuffer(1, probeOutput.get(), sizeof(uint32_t), nullptr, nullptr);
        const plume::RenderPipelineProgram probeProgram = probePipeline->getProgram("TestRayGen");
        plume::RenderShaderBindingTableInfo probeTable;
        device->setShaderBindingTableInfo(probeTable,
            {plume::RenderShaderBindingGroup(&probeProgram, 1), {}, {}},
            probePipeline.get(), nullptr, 0);
        auto probeSbt = device->createBuffer(
            plume::RenderBufferDesc::UploadBuffer(probeTable.tableBufferData.size()));
        std::memcpy(probeSbt->map(), probeTable.tableBufferData.data(), probeTable.tableBufferData.size());
        probeSbt->unmap();
        auto probeCommands = queue->createCommandList();
        auto probeFence = device->createCommandFence();
        probeCommands->begin();
        probeCommands->setPipeline(probePipeline.get());
        probeCommands->setRaytracingPipelineLayout(layout.get());
        for (uint32_t i = 0; i < 4; i++) probeCommands->setRaytracingDescriptorSet(probeSet.get(), i);
        probeCommands->traceRays(1, 1, 1, probeSbt->at(0), probeTable.groups);
        probeCommands->end();
        auto switchedOutput = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(uint32_t)));
        *static_cast<uint32_t *>(switchedOutput->map()) = 0;
        switchedOutput->unmap();
        probeSet->setBuffer(1, switchedOutput.get(), sizeof(uint32_t), nullptr, nullptr);
        probeSet->setAccelerationStructure(0, nullptr);
        auto *nativeProbeCommands = static_cast<plume::MetalCommandList *>(probeCommands.get())->mtl;
        nativeProbeCommands->retain();
        const plume::RenderCommandList *probeLists[] = {probeCommands.get()};
        queue->executeCommandLists(probeLists, 1, nullptr, 0, nullptr, 0, probeFence.get());
        queue->waitForCommandFence(probeFence.get());
        const uint32_t resultBits = *static_cast<uint32_t *>(probeOutput->map());
        probeOutput->unmap();
        const uint32_t switchedBits = *static_cast<uint32_t *>(switchedOutput->map());
        switchedOutput->unmap();
        float resultDistance = 0.f;
        std::memcpy(&resultDistance, &resultBits, sizeof(resultDistance));
        if (nativeProbeCommands->status() != MTL::CommandBufferStatusCompleted ||
            std::fabs(resultDistance - 1.f) > 0.001f || switchedBits != 0) {
            const NS::Error *error = nativeProbeCommands->error();
            std::fprintf(stderr, "Plume ray dispatch failed: status=%ld distance=%f error=%s\n",
                static_cast<long>(nativeProbeCommands->status()), resultDistance,
                error ? error->localizedDescription()->utf8String() : "no Metal error");
            nativeProbeCommands->release();
            return 1;
        }
        nativeProbeCommands->release();
        std::printf("METAL_PLUME_RAY_DISPATCH_OK distance=%f\n", resultDistance);
    }
    std::printf("METAL_AS_OK blas_size=%llu tlas_size=%llu\n",
        static_cast<unsigned long long>(info.accelerationStructureSize),
        static_cast<unsigned long long>(topInfo.accelerationStructureSize));
    return 0;
}
