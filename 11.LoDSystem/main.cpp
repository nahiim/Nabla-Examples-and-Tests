// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#define _NBL_STATIC_LIB_
#include <nabla.h>
#include "nbl/scene/CLevelOfDetailLibrary.h"
#include "nbl/scene/ICullingLoDSelectionSystem.h"

#include "../common/Camera.hpp"
#include "../common/CommonAPI.h"
#include "nbl/ext/ScreenShot/ScreenShot.h"

using namespace nbl;
using namespace core;
using namespace system;
using namespace asset;

using lod_library_t = scene::CLevelOfDetailLibrary<>;

struct LoDLibraryData
{
    core::vector<uint32_t> drawCallOffsetsIn20ByteStrides;
    core::vector<uint32_t> drawCountOffsets;
    core::vector<asset::DrawElementsIndirectCommand_t> drawCallData;
    core::vector<uint32_t> drawCountData;
    core::vector<uint32_t> lodInfoDstUvec4s;
    core::vector<uint32_t> lodTableDstUvec4s;
    scene::ILevelOfDetailLibrary::InfoContainerAdaptor<lod_library_t::LoDInfo> lodInfoData;
    scene::ILevelOfDetailLibrary::InfoContainerAdaptor<scene::ILevelOfDetailLibrary::LoDTableInfo> lodTableData;
};
enum E_GEOM_TYPE
{
    EGT_CUBE,
    EGT_SPHERE,
    EGT_CONE
};
template<E_GEOM_TYPE geom, uint32_t LoDLevels>
void addLoDTable(
    IAssetManager* assetManager, core::smart_refctd_ptr<ICPUDescriptorSetLayout>&& cpuPerViewDSLayout,
    const core::smart_refctd_ptr<ICPUSpecializedShader>* shaders,
    nbl::video::IGPUObjectFromAssetConverter& cpu2gpu,
    nbl::video::IGPUObjectFromAssetConverter::SParams& cpu2gpuParams,
    LoDLibraryData& lodLibraryData,
    video::CDrawIndirectAllocator<>* drawIndirectAllocator,
    lod_library_t* lodLibrary,
    core::vector<video::CSubpassKiln::DrawcallInfo>& drawcallInfos,
    const SBufferRange<video::IGPUBuffer>& perInstanceRedirectAttribs,
    const core::smart_refctd_ptr<video::IGPURenderpass>& renderpass,
    const core::smart_refctd_ptr<video::IGPUDescriptorSet>& perViewDS
)
{
    constexpr auto perInstanceRedirectAttrID = 15u;
    auto* const geometryCreator = assetManager->getGeometryCreator();
    auto* const meshManipulator = assetManager->getMeshManipulator();

    core::smart_refctd_ptr<ICPURenderpassIndependentPipeline> cpupipeline;
    core::smart_refctd_ptr<ICPUMeshBuffer> cpumeshes[LoDLevels];
    for (uint32_t poly=4u,lod=0u; lod<LoDLevels; lod++)
    {
        IGeometryCreator::return_type geomData;
        switch (geom)
        {
            case EGT_CUBE:
                geomData = geometryCreator->createCubeMesh(core::vector3df(2.f));
                break;
            case EGT_SPHERE:
                geomData = geometryCreator->createSphereMesh(2.f,poly,poly,meshManipulator);
                break;
            case EGT_CONE:
                geomData = geometryCreator->createConeMesh(2.f,2.f,poly,0x0u,0x0u,0.f,meshManipulator);
                break;
            default:
                assert(false);
                break;
        }
        // we'll stick instance data refs in the last attribute binding
        assert((geomData.inputParams.enabledBindingFlags>>perInstanceRedirectAttrID)==0u);

        geomData.inputParams.enabledAttribFlags |= 0x1u<<perInstanceRedirectAttrID;
        geomData.inputParams.enabledBindingFlags |= 0x1u<<perInstanceRedirectAttrID;
        geomData.inputParams.attributes[perInstanceRedirectAttrID].binding = perInstanceRedirectAttrID;
        geomData.inputParams.attributes[perInstanceRedirectAttrID].relativeOffset = 0u;
        geomData.inputParams.attributes[perInstanceRedirectAttrID].format = asset::EF_R32G32_UINT;
        geomData.inputParams.bindings[perInstanceRedirectAttrID].inputRate = asset::EVIR_PER_INSTANCE;
        geomData.inputParams.bindings[perInstanceRedirectAttrID].stride = asset::getTexelOrBlockBytesize(asset::EF_R32G32_UINT);

        if (!cpupipeline)
        {
            auto pipelinelayout = core::make_smart_refctd_ptr<ICPUPipelineLayout>(nullptr,nullptr,nullptr,std::move(cpuPerViewDSLayout));
            cpupipeline = core::make_smart_refctd_ptr<ICPURenderpassIndependentPipeline>(
                std::move(pipelinelayout),&shaders->get(),&shaders->get()+2u,
                geomData.inputParams,SBlendParams{},geomData.assemblyParams,SRasterizationParams{}
            );
        }
        cpumeshes[lod] = core::make_smart_refctd_ptr<ICPUMeshBuffer>();
        cpumeshes[lod]->setPipeline(core::smart_refctd_ptr(cpupipeline));
        cpumeshes[lod]->setIndexType(geomData.indexType);
        cpumeshes[lod]->setIndexCount(geomData.indexCount);
        cpumeshes[lod]->setIndexBufferBinding(std::move(geomData.indexBuffer));
        for (auto j=0u; j<ICPUMeshBuffer::MAX_ATTR_BUF_BINDING_COUNT; j++)
            cpumeshes[lod]->setVertexBufferBinding(asset::SBufferBinding(geomData.bindings[j]),j);

        poly <<= 1u;
    }
    auto gpumeshes = cpu2gpu.getGPUObjectsFromAssets(cpumeshes,cpumeshes+LoDLevels,cpu2gpuParams);

    core::smart_refctd_ptr<video::IGPUGraphicsPipeline> pipeline;
    {
        video::IGPUGraphicsPipeline::SCreationParams params;
        params.renderpass = renderpass;
        params.renderpassIndependent = core::smart_refctd_ptr_dynamic_cast<video::IGPURenderpassIndependentPipeline>(assetManager->findGPUObject(cpupipeline.get()));
        params.subpassIx = 0u;
        pipeline = cpu2gpuParams.device->createGPUGraphicsPipeline(nullptr,std::move(params));
    }

    auto drawcallInfosOutIx = drawcallInfos.size();
    drawcallInfos.resize(drawcallInfos.size()+gpumeshes->size());
	core::aabbox3df aabb(FLT_MAX,FLT_MAX,FLT_MAX,-FLT_MAX,-FLT_MAX,-FLT_MAX);
    for (auto lod=0u; lod<gpumeshes->size(); lod++)
    {
        auto gpumb = gpumeshes->operator[](lod);

        auto& di = drawcallInfos[drawcallInfosOutIx++];
        memcpy(di.pushConstantData,gpumb->getPushConstantsDataPtr(),video::IGPUMeshBuffer::MAX_PUSH_CONSTANT_BYTESIZE);
        di.pipeline = pipeline;
        std::fill_n(di.descriptorSets,video::IGPUPipelineLayout::DESCRIPTOR_SET_COUNT,nullptr);
        di.descriptorSets[1] = perViewDS;
        di.indexType = gpumb->getIndexType();
        std::copy_n(gpumb->getVertexBufferBindings(),perInstanceRedirectAttrID,di.vertexBufferBindings);
        di.vertexBufferBindings[perInstanceRedirectAttrID] = {perInstanceRedirectAttribs.offset,perInstanceRedirectAttribs.buffer};
        di.indexBufferBinding = gpumb->getIndexBufferBinding().buffer;
        di.drawCommandStride = sizeof(asset::DrawElementsIndirectCommand_t);
        di.drawCountOffset = video::IDrawIndirectAllocator::invalid_draw_count_ix;
        di.drawCallOffset = video::IDrawIndirectAllocator::invalid_draw_range_begin;
        di.drawMaxCount = 0u;

        video::IDrawIndirectAllocator::Allocation mdiAlloc;
        mdiAlloc.count = 1u;
        mdiAlloc.multiDrawCommandRangeByteOffsets = &di.drawCallOffset;
        mdiAlloc.multiDrawCommandMaxCounts = &di.drawMaxCount;
        mdiAlloc.multiDrawCommandCountOffsets = &di.drawCountOffset;
        mdiAlloc.setAllCommandStructSizesConstant(di.drawCommandStride);

        constexpr auto indicesPerBatch = 1023u;
        const auto indexCount = gpumb->getIndexCount();
        const auto batchCount = (indexCount-1u)/indicesPerBatch+1u;
        //
        const auto& mbAABB = cpumeshes[lod]->getBoundingBox();
        aabb.addInternalBox(mbAABB);
        auto& lodInfo = lodLibraryData.lodInfoData.emplace_back(batchCount);
        lodInfo = lod_library_t::LoDInfo(batchCount,{3.f*exp2f(lod<<1)},mbAABB);
        //
        size_t indexSize;
        switch (gpumb->getIndexType())
        {
            case EIT_16BIT:
                indexSize = sizeof(uint16_t);
                break;
            case EIT_32BIT:
                indexSize = sizeof(uint32_t);
                break;
            default:
                assert(false);
                break;
        }
        for (auto i=0u; i<indexCount; i+=indicesPerBatch)
        {
            const auto& drawCallData = lodLibraryData.drawCallData.emplace_back(
                core::min(indexCount-i,indicesPerBatch),
                0u,
                gpumb->getIndexBufferBinding().offset/indexSize+i,
                0u,
                0xdeadbeefu // set to garbage to test the prefix sum
            );
            di.drawMaxCount++;
            /* TODO
            const aabb = IMeshManipulator::calculateBoundingBox(
                cpumeshes[lod].get(),nullptr,drawCallData.count,
                reinterpret_cast<const uint8_t*>(cpumeshes[lod]->getIndices())+i*indexSize
            );
            */
        }
        lodLibraryData.drawCountData.emplace_back(di.drawMaxCount);
        
        const bool success = drawIndirectAllocator->allocateMultiDraws(mdiAlloc);
        assert(success);
        lodLibraryData.drawCountOffsets.emplace_back(di.drawCountOffset);
        di.drawCountOffset *= sizeof(uint32_t);
        for (auto i=0u; i<di.drawMaxCount; i++)
        {
            lodLibraryData.drawCallOffsetsIn20ByteStrides.emplace_back(di.drawCallOffset/di.drawCommandStride+i);
            lodInfo.drawcallInfos[i].drawcallDWORDOffset = (di.drawCallOffset+i*di.drawCommandStride)/sizeof(uint32_t);
        }
    }
    auto& lodTable = lodLibraryData.lodTableData.emplace_back(LoDLevels);
    lodTable = scene::ILevelOfDetailLibrary::LoDTableInfo(LoDLevels,aabb);
    std::fill_n(lodTable.leveInfoUvec4Offsets,LoDLevels,scene::ILevelOfDetailLibrary::invalid);
    {
        lod_library_t::Allocation::LevelInfoAllocation lodLevelAllocations[1] =
        {
            lodTable.leveInfoUvec4Offsets,
            lodLibraryData.drawCountData.data()+lodLibraryData.drawCountData.size()-LoDLevels
        };
        uint32_t lodTableOffsets[1u] = {scene::ILevelOfDetailLibrary::invalid};
        const uint32_t lodLevelCounts[1u] = {LoDLevels};
        //
        lod_library_t::Allocation alloc = {};
        {
            alloc.count = 1u;
            alloc.tableUvec4Offsets = lodTableOffsets;
            alloc.levelCounts = lodLevelCounts;
            alloc.levelAllocations = lodLevelAllocations;
        }
        const bool success = lodLibrary->allocateLoDs(alloc);
        assert(success);
        for (auto i=0u; i<scene::ILevelOfDetailLibrary::LoDTableInfo::getSizeInUvec4(LoDLevels); i++)
            lodLibraryData.lodTableDstUvec4s.push_back(lodTableOffsets[0]+i);
        for (auto lod=0u; lod<LoDLevels; lod++)
        {
            const auto drawcallCount = lodLevelAllocations[0].drawcallCounts[lod];
            const auto offset = lodLevelAllocations[0].levelUvec4Offsets[lod];
            for (auto i=0u; i<lod_library_t::LoDInfo::getSizeInUvec4(drawcallCount); i++)
                lodLibraryData.lodInfoDstUvec4s.push_back(offset+i);
        }
    }
    cpu2gpuParams.waitForCreationToComplete();
}

#include <random>

int main()
{
	constexpr uint32_t WIN_W = 1280;
	constexpr uint32_t WIN_H = 720;
    constexpr uint32_t FBO_COUNT = 1u;
	constexpr uint32_t FRAMES_IN_FLIGHT = 5u;
	static_assert(FRAMES_IN_FLIGHT>FBO_COUNT);

	auto initOutput = CommonAPI::Init<WIN_W, WIN_H, FBO_COUNT>(video::EAT_OPENGL, "Level of Detail System", asset::EF_D32_SFLOAT);
    auto window = std::move(initOutput.window);
    auto gl = std::move(initOutput.apiConnection);
    auto surface = std::move(initOutput.surface);
    auto gpuPhysicalDevice = std::move(initOutput.physicalDevice);
    auto logicalDevice = std::move(initOutput.logicalDevice);
    auto queues = std::move(initOutput.queues);
    auto swapchain = std::move(initOutput.swapchain);
    auto renderpass = std::move(initOutput.renderpass);
    auto fbos = std::move(initOutput.fbo);
    auto commandPool = std::move(initOutput.commandPool);
    auto assetManager = std::move(initOutput.assetManager);
    auto logger = std::move(initOutput.logger);
    auto inputSystem = std::move(initOutput.inputSystem);
    auto system = std::move(initOutput.system);
    auto windowCallback = std::move(initOutput.windowCb);
    auto cpu2gpuParams = std::move(initOutput.cpu2gpuParams);
    auto utilities = std::move(initOutput.utilities);


    // lod table entries
    constexpr auto MaxDrawables = 5u;
    // all the lod infos from all lod entries
    constexpr auto MaxTotalLoDs = 8u*MaxDrawables;
    // how many contiguous ranges of drawcalls with explicit draw counts
    constexpr auto MaxMDIs = 16u;
    // how many drawcalls (meshlets)
    constexpr auto MaxDrawCalls = 2048u;
    // how many instances
    constexpr auto MaxInstanceCount = 8u;
    // maximum visible instances of a drawcalll
    constexpr auto MaxTotalDrawcallInstances = 2048u;
    
    // Drawcall Allocator
    core::smart_refctd_ptr<video::CDrawIndirectAllocator<>> drawIndirectAllocator;
    {
        video::IDrawIndirectAllocator::ImplicitBufferCreationParameters drawAllocatorParams;
        drawAllocatorParams.device = logicalDevice.get();
        drawAllocatorParams.maxDrawCommandStride = sizeof(asset::DrawElementsIndirectCommand_t);
        drawAllocatorParams.drawCommandCapacity = MaxDrawCalls;
        drawAllocatorParams.drawCountCapacity = MaxMDIs;
        drawIndirectAllocator = video::CDrawIndirectAllocator<>::create(std::move(drawAllocatorParams));
    }

    // LoD Library
    auto lodLibrary = lod_library_t::create({logicalDevice.get(),MaxDrawables,MaxTotalLoDs,MaxDrawCalls});

    // Culling System
    using culling_system_t = scene::ICullingLoDSelectionSystem;
    core::smart_refctd_ptr<culling_system_t> cullingSystem;
    culling_system_t::Params cullingParams;
    core::smart_refctd_ptr<video::IDescriptorPool> cullingDSPool;
    {
        constexpr auto LayoutCount = 4u;
        core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> layouts[LayoutCount] =
        {
            scene::ILevelOfDetailLibrary::createDescriptorSetLayout(logicalDevice.get()),
            culling_system_t::createInputDescriptorSetLayout(logicalDevice.get()),
            culling_system_t::createOutputDescriptorSetLayout(logicalDevice.get(),true),
            [&]() -> core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>
            {
                // TODO: figure out what should be here
                constexpr auto BindingCount = 2u;
                video::IGPUDescriptorSetLayout::SBinding bindings[BindingCount];
                for (auto i=0u; i<BindingCount; i++)
                {
                    bindings[i].binding = i;
                    bindings[i].type = asset::EDT_STORAGE_BUFFER;
                    bindings[i].count = 1u;
                    bindings[i].stageFlags = asset::ISpecializedShader::ESS_COMPUTE;
                    bindings[i].samplers = nullptr;
                }
                return logicalDevice->createGPUDescriptorSetLayout(bindings,bindings+BindingCount);
            }()
        };
        cullingDSPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE,&layouts->get(),&layouts->get()+LayoutCount);
        
		const asset::SPushConstantRange range = {asset::ISpecializedShader::ESS_COMPUTE,0u,sizeof(core::matrix4SIMD)+sizeof(uint32_t)};
        cullingSystem = core::make_smart_refctd_ptr<culling_system_t>(
            utilities.get(),&range,&range+1u,core::smart_refctd_ptr(layouts[3]),std::filesystem::current_path(),
            "\n#include \"../per_view_per_instance_struct.glsl\"\n#include \"../cull_overrides.glsl\"\n"
        );

        cullingParams.indirectDispatchParams = {0ull,culling_system_t::createDispatchIndirectBuffer(utilities.get(),queues[decltype(initOutput)::EQT_TRANSFER_UP])};
        {
            // set up the instance list
            core::vector<culling_system_t::InstanceToCull> instanceList; instanceList.reserve(MaxInstanceCount);
            for (auto i=0u; i<7u; i++)
            {
                auto& instance = instanceList.emplace_back();
                instance.instanceGUID = i; // TODO: do this better
                instance.lodTableUvec4Offset = 0u; // TODO: should be `lodTableDstUvec4s[0]`
            }
            cullingParams.directInstanceCount = instanceList.size();

            video::IGPUBuffer::SCreationParams params;
            params.usage = asset::IBuffer::EUF_STORAGE_BUFFER_BIT;
            cullingParams.instanceList = {0ull,~0ull,utilities->createFilledDeviceLocalGPUBufferOnDedMem(queues[decltype(initOutput)::EQT_TRANSFER_UP],sizeof(culling_system_t::InstanceToCull)*MaxInstanceCount,instanceList.data())};
        }
        cullingParams.scratchBufferRanges = culling_system_t::createScratchBuffer(logicalDevice.get(),MaxInstanceCount,MaxTotalDrawcallInstances);
        cullingParams.drawCalls = drawIndirectAllocator->getDrawCommandMemoryBlock();
        cullingParams.perViewPerInstance = {0ull,~0ull,culling_system_t::createPerViewPerInstanceDataBuffer<core::matrix4SIMD>(logicalDevice.get(),MaxTotalDrawcallInstances)}; // TODO: perView type
        cullingParams.perInstanceRedirectAttribs = {0ul,~0ull,culling_system_t::createInstanceRedirectBuffer(logicalDevice.get(),MaxTotalDrawcallInstances)};
        const auto drawCountsBlock = drawIndirectAllocator->getDrawCountMemoryBlock();
        if (drawCountsBlock)
            cullingParams.drawCounts = *drawCountsBlock;

        cullingParams.lodLibraryDS = core::smart_refctd_ptr<video::IGPUDescriptorSet>(lodLibrary->getDescriptorSet());
        cullingParams.transientOutputDS = culling_system_t::createOutputDescriptorSet(
            logicalDevice.get(),cullingDSPool.get(),std::move(layouts[2]),
            cullingParams.drawCalls,
            cullingParams.perViewPerInstance,
            cullingParams.perInstanceRedirectAttribs,
            cullingParams.drawCounts
        );
        cullingParams.customDS = logicalDevice->createGPUDescriptorSet(cullingDSPool.get(),std::move(layouts[3]));

        cullingParams.indirectDispatchParams.buffer->setObjectDebugName("CullingIndirect");
        cullingParams.drawCalls.buffer->setObjectDebugName("DrawCallPool");
        cullingParams.perInstanceRedirectAttribs.buffer->setObjectDebugName("PerInstanceInputAttribs");
        if (cullingParams.drawCounts.buffer)
            cullingParams.drawCounts.buffer->setObjectDebugName("DrawCountPool");
    }


    core::smart_refctd_ptr<video::IGPUFence> gpuTransferFence;
    core::smart_refctd_ptr<video::IGPUFence> gpuComputeFence;
    nbl::video::IGPUObjectFromAssetConverter cpu2gpu;
    {
        cpu2gpuParams.perQueue[nbl::video::IGPUObjectFromAssetConverter::EQU_TRANSFER].fence = &gpuTransferFence;
        cpu2gpuParams.perQueue[nbl::video::IGPUObjectFromAssetConverter::EQU_COMPUTE].fence = &gpuComputeFence;
    }

    core::smart_refctd_ptr<ICPUSpecializedShader> shaders[2];
    {
        IAssetLoader::SAssetLoadParams lp;
        lp.workingDirectory = std::filesystem::current_path();
        lp.logger = logger.get();
        auto vertexShaderBundle = assetManager->getAsset("../mesh.vert", lp);
        auto fragShaderBundle = assetManager->getAsset("../mesh.frag", lp);
        shaders[0] = IAsset::castDown<ICPUSpecializedShader>(*vertexShaderBundle.getContents().begin());
        shaders[1] = IAsset::castDown<ICPUSpecializedShader>(*fragShaderBundle.getContents().begin());
    }
    





    // TODO: refactor
    core::smart_refctd_ptr<video::IGPUBuffer> perViewPerInstanceDataScratch;
    {
        video::IGPUBuffer::SCreationParams params;
        params.usage = core::bitflag(asset::IBuffer::EUF_STORAGE_BUFFER_BIT);
        perViewPerInstanceDataScratch = logicalDevice->createDeviceLocalGPUBufferOnDedMem(params,sizeof(core::matrix4SIMD)*MaxInstanceCount);
        auto mreqs = logicalDevice->getDeviceLocalGPUMemoryReqs();
        mreqs.vulkanReqs.size = sizeof(core::matrix4SIMD)*MaxInstanceCount;
        perViewPerInstanceDataScratch = logicalDevice->createGPUBufferOnDedMem(params,mreqs,true);
    }


    core::smart_refctd_ptr<video::IGPUDescriptorSet> perViewDS;
    core::smart_refctd_ptr<ICPUDescriptorSetLayout> cpuPerViewDSLayout;
    {
        constexpr auto BindingCount = 1;
        ICPUDescriptorSetLayout::SBinding cpuBindings[BindingCount];
        for (auto i=0; i<BindingCount; i++)
        {
            cpuBindings[i].binding = i;
            cpuBindings[i].count = 1u;
            cpuBindings[i].stageFlags = ISpecializedShader::ESS_VERTEX;
            cpuBindings[i].samplers = nullptr;
        }
        cpuBindings[0].type = EDT_STORAGE_BUFFER;
        cpuPerViewDSLayout = core::make_smart_refctd_ptr<ICPUDescriptorSetLayout>(cpuBindings,cpuBindings+BindingCount);

        auto bindings = reinterpret_cast<video::IGPUDescriptorSetLayout::SBinding*>(cpuBindings);
        auto perViewDSLayout = logicalDevice->createGPUDescriptorSetLayout(bindings,bindings+BindingCount);
        auto dsPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE,&perViewDSLayout.get(),&perViewDSLayout.get()+1u);
        perViewDS = logicalDevice->createGPUDescriptorSet(dsPool.get(),std::move(perViewDSLayout));
        {
            video::IGPUDescriptorSet::SWriteDescriptorSet writes[BindingCount];
            video::IGPUDescriptorSet::SDescriptorInfo infos[BindingCount];
            for (auto i=0; i<BindingCount; i++)
            {
                writes[i].dstSet = perViewDS.get();
                writes[i].binding = i;
                writes[i].arrayElement = 0u;
                writes[i].count = 1u;
                writes[i].info = infos+i;
            }
            writes[0].descriptorType = EDT_STORAGE_BUFFER;
            infos[0].desc = perViewPerInstanceDataScratch;
            infos[0].buffer = {0u,video::IGPUDescriptorSet::SDescriptorInfo::SBufferInfo::WholeBuffer};
            logicalDevice->updateDescriptorSets(BindingCount,writes,0u,nullptr);
        }
    }

    std::random_device rd;
    std::mt19937 randGen(rd());
    //
    core::smart_refctd_ptr<video::IGPUCommandBuffer> bakedCommandBuffer;
    {
        video::CSubpassKiln kiln;
        {
            LoDLibraryData lodLibraryData;
            // create all the LoDs of drawables
            {
                auto* qnc = assetManager->getMeshManipulator()->getQuantNormalCache();
                //loading cache from file
                const system::path cachePath = std::filesystem::current_path() / "../../tmp/normalCache101010.sse";
                if (!qnc->loadCacheFromFile<asset::EF_A2B10G10R10_SNORM_PACK32>(system.get(), cachePath))
                    logger->log("%s", ILogger::ELL_ERROR, "Failed to load cache.");

                addLoDTable<EGT_SPHERE,7>(
                    assetManager.get(),core::smart_refctd_ptr(cpuPerViewDSLayout),shaders,cpu2gpu,cpu2gpuParams,
                    lodLibraryData,drawIndirectAllocator.get(),lodLibrary.get(),kiln.getDrawcallMetadataVector(),
                    cullingParams.perInstanceRedirectAttribs,renderpass,perViewDS
                );

                //! cache results -- speeds up mesh generation on second run
                qnc->saveCacheToFile<asset::EF_A2B10G10R10_SNORM_PACK32>(system.get(), cachePath);
            }
            cullingParams.drawcallCount = lodLibraryData.drawCallData.size();
            // do the transfer of drawcall and LoD data
            {
                constexpr auto MaxTransfers = 4u;
                video::CPropertyPoolHandler::TransferRequest requests[MaxTransfers];
                for (auto i=0u; i<MaxTransfers; i++)
                {
                    requests[i].flags = video::CPropertyPoolHandler::TransferRequest::EF_NONE;
                    requests[i].srcAddresses = nullptr; // iota 0,1,2,3,4,etc.
                    requests[i].device2device = false;
                }
                requests[0].memblock = drawIndirectAllocator->getDrawCommandMemoryBlock();
                requests[0].elementSize = sizeof(asset::DrawElementsIndirectCommand_t);
                requests[0].elementCount = cullingParams.drawcallCount;
                requests[0].dstAddresses = lodLibraryData.drawCallOffsetsIn20ByteStrides.data();
                requests[0].source = lodLibraryData.drawCallData.data();
                requests[1].memblock = lodLibrary->getLoDInfoBinding();
                requests[1].elementSize = sizeof(scene::ILevelOfDetailLibrary::AlignBase);
                requests[1].elementCount = lodLibraryData.lodInfoDstUvec4s.size();
                requests[1].dstAddresses = lodLibraryData.lodInfoDstUvec4s.data();
                requests[1].source = lodLibraryData.lodInfoData.data();
                requests[2].memblock = lodLibrary->getLodTableInfoBinding();
                requests[2].elementSize = sizeof(scene::ILevelOfDetailLibrary::AlignBase);
                requests[2].elementCount = lodLibraryData.lodTableDstUvec4s.size();
                requests[2].dstAddresses = lodLibraryData.lodTableDstUvec4s.data();
                requests[2].source = lodLibraryData.lodTableData.data();
                auto requestCount = 3u;
                if (drawIndirectAllocator->getDrawCountMemoryBlock())
                {
                    requests[requestCount].memblock = *drawIndirectAllocator->getDrawCountMemoryBlock();
                    requests[requestCount].elementSize = sizeof(uint32_t);
                    requests[requestCount].elementCount = lodLibraryData.drawCountOffsets.size();
                    requests[requestCount].dstAddresses = lodLibraryData.drawCountOffsets.data();
                    requests[requestCount].source = lodLibraryData.drawCountData.data();
                    requestCount++;
                }

                core::smart_refctd_ptr<video::IGPUCommandBuffer> tferCmdBuf;
                logicalDevice->createCommandBuffers(commandPool.get(),video::IGPUCommandBuffer::EL_PRIMARY,1u,&tferCmdBuf);
                auto fence = logicalDevice->createFence(video::IGPUFence::ECF_UNSIGNALED);
                tferCmdBuf->begin(0u); // TODO some one time submit bit or something
                utilities->getDefaultPropertyPoolHandler()->transferProperties(utilities->getDefaultUpStreamingBuffer(),nullptr,tferCmdBuf.get(),fence.get(),requests,requests+requestCount,logger.get());
                tferCmdBuf->end();
                {
                    video::IGPUQueue::SSubmitInfo submit = {}; // intializes all semaphore stuff to 0 and nullptr
                    submit.commandBufferCount = 1u;
                    submit.commandBuffers = &tferCmdBuf.get();
                    queues[decltype(initOutput)::EQT_TRANSFER_UP]->submit(1u,&submit,fence.get());
                }
                logicalDevice->blockForFences(1u,&fence.get());
            }
            // set up the remaining descriptor sets of the culling system
            {
                auto& drawCallOffsetsInDWORDs = lodLibraryData.drawCallOffsetsIn20ByteStrides;
                for (auto i=0u; i<cullingParams.drawcallCount; i++)
                    drawCallOffsetsInDWORDs[i] = lodLibraryData.drawCallOffsetsIn20ByteStrides[i]*sizeof(asset::DrawElementsIndirectCommand_t)/sizeof(uint32_t);
                cullingParams.transientInputDS = culling_system_t::createInputDescriptorSet(
                    logicalDevice.get(),cullingDSPool.get(),
                    culling_system_t::createInputDescriptorSetLayout(logicalDevice.get()),
                    cullingParams.indirectDispatchParams,
                    cullingParams.instanceList,
                    cullingParams.scratchBufferRanges,
                    {0ull,~0ull,utilities->createFilledDeviceLocalGPUBufferOnDedMem(queues[decltype(initOutput)::EQT_TRANSFER_UP],cullingParams.drawcallCount*sizeof(uint32_t),drawCallOffsetsInDWORDs.data())},
                    {0ull,~0ull,utilities->createFilledDeviceLocalGPUBufferOnDedMem(queues[decltype(initOutput)::EQT_TRANSFER_UP],lodLibraryData.drawCountOffsets.size()*sizeof(uint32_t),lodLibraryData.drawCountOffsets.data())}
                );
            }
        }
        // prerecord the secondary cmdbuffer
        {
            logicalDevice->createCommandBuffers(commandPool.get(),video::IGPUCommandBuffer::EL_SECONDARY,1u,&bakedCommandBuffer);
            bakedCommandBuffer->begin(video::IGPUCommandBuffer::EU_RENDER_PASS_CONTINUE_BIT|video::IGPUCommandBuffer::EU_SIMULTANEOUS_USE_BIT);
            // TODO: handle teh offsets
            kiln.bake(bakedCommandBuffer.get(),renderpass.get(),0u,drawIndirectAllocator->getDrawCommandMemoryBlock().buffer.get(),drawIndirectAllocator->getDrawCountMemoryBlock()->buffer.get());
            bakedCommandBuffer->end();
        }
    }


    CommonAPI::InputSystem::ChannelReader<IMouseEventChannel> mouse;
    CommonAPI::InputSystem::ChannelReader<IKeyboardEventChannel> keyboard;

    core::vectorSIMDf cameraPosition(0, 5, -10);
    matrix4SIMD projectionMatrix = matrix4SIMD::buildProjectionMatrixPerspectiveFovLH(core::radians(60), float(WIN_W) / WIN_H, 2.f, 4000.f);
    Camera camera = Camera(cameraPosition, core::vectorSIMDf(0, 0, 0), projectionMatrix, 2.f, 1.f);
    
    video::CDumbPresentationOracle oracle;
    oracle.reportBeginFrameRecord();

    core::smart_refctd_ptr<video::IGPUCommandBuffer> commandBuffers[FRAMES_IN_FLIGHT];
    logicalDevice->createCommandBuffers(commandPool.get(),video::IGPUCommandBuffer::EL_PRIMARY,FRAMES_IN_FLIGHT,commandBuffers);

    core::smart_refctd_ptr<video::IGPUFence> frameComplete[FRAMES_IN_FLIGHT] = { nullptr };
    core::smart_refctd_ptr<video::IGPUSemaphore> imageAcquire[FRAMES_IN_FLIGHT] = { nullptr };
    core::smart_refctd_ptr<video::IGPUSemaphore> renderFinished[FRAMES_IN_FLIGHT] = { nullptr };
    for (uint32_t i=0u; i<FRAMES_IN_FLIGHT; i++)
    {
        imageAcquire[i] = logicalDevice->createSemaphore();
        renderFinished[i] = logicalDevice->createSemaphore();
    }

    uint32_t acquiredNextFBO = {};
    auto resourceIx = -1;
	while(windowCallback->isWindowOpen())
	{
        ++resourceIx;
        if (resourceIx >= FRAMES_IN_FLIGHT)
            resourceIx = 0;
        
        auto& commandBuffer = commandBuffers[resourceIx];
        auto& fence = frameComplete[resourceIx];
        if (fence)
            logicalDevice->blockForFences(1u,&fence.get());
        else
            fence = logicalDevice->createFence(static_cast<video::IGPUFence::E_CREATE_FLAGS>(0));

        //
        commandBuffer->reset(nbl::video::IGPUCommandBuffer::ERF_RELEASE_RESOURCES_BIT);
        commandBuffer->begin(0);

        // late latch input
        const auto nextPresentationTimestamp = oracle.acquireNextImage(swapchain.get(),imageAcquire[resourceIx].get(),nullptr,&acquiredNextFBO);

        // input
        {
            inputSystem->getDefaultMouse(&mouse);
            inputSystem->getDefaultKeyboard(&keyboard);

            camera.beginInputProcessing(nextPresentationTimestamp);
            mouse.consumeEvents([&](const IMouseEventChannel::range_t& events) -> void { camera.mouseProcess(events); }, logger.get());
            keyboard.consumeEvents([&](const IKeyboardEventChannel::range_t& events) -> void { camera.keyboardProcess(events); }, logger.get());
            camera.endInputProcessing(nextPresentationTimestamp);
        }

        // update camera, TODO: redo
        {
            const auto& viewProjectionMatrix = camera.getConcatenatedMatrix();
            std::array<core::matrix4SIMD,8u> data;
            for (auto i=0; i<data.size(); i++)
            {
                data[i].setTranslation(core::vectorSIMDf(0.f,i,0.f)*6.f);
                data[i] = core::concatenateBFollowedByA(viewProjectionMatrix,data[i]);
            }
            commandBuffer->updateBuffer(perViewPerInstanceDataScratch.get(),0ull,perViewPerInstanceDataScratch->getSize(),data.data());

            const auto* layout = cullingSystem->getInstanceCullAndLoDSelectLayout();
            commandBuffer->pushConstants(layout,asset::ISpecializedShader::ESS_COMPUTE,0u,sizeof(core::matrix4SIMD),camera.getConcatenatedMatrix().pointer());
            commandBuffer->pushConstants(layout,asset::ISpecializedShader::ESS_COMPUTE,sizeof(core::matrix4SIMD),sizeof(uint32_t),&cullingParams.directInstanceCount);
            cullingParams.cmdbuf = commandBuffer.get();
            cullingSystem->processInstancesAndFillIndirectDraws(cullingParams);
        }
        
        // renderpass
        {
            asset::SViewport viewport;
            viewport.minDepth = 1.f;
            viewport.maxDepth = 0.f;
            viewport.x = 0u;
            viewport.y = 0u;
            viewport.width = WIN_W;
            viewport.height = WIN_H;
            commandBuffer->setViewport(0u,1u,&viewport);

            nbl::video::IGPUCommandBuffer::SRenderpassBeginInfo beginInfo;
            {
                VkRect2D area;
                area.offset = { 0,0 };
                area.extent = { WIN_W, WIN_H };
                asset::SClearValue clear[2] = {};
                clear[0].color.float32[0] = 1.f;
                clear[0].color.float32[1] = 1.f;
                clear[0].color.float32[2] = 1.f;
                clear[0].color.float32[3] = 1.f;
                clear[1].depthStencil.depth = 0.f;

                beginInfo.clearValueCount = 2u;
                beginInfo.framebuffer = fbos[acquiredNextFBO];
                beginInfo.renderpass = renderpass;
                beginInfo.renderArea = area;
                beginInfo.clearValues = clear;
            }

            commandBuffer->beginRenderPass(&beginInfo,nbl::asset::ESC_INLINE);
            commandBuffer->executeCommands(1u,&bakedCommandBuffer.get());
            commandBuffer->endRenderPass();

            commandBuffer->end();
        }

        CommonAPI::Submit(logicalDevice.get(), swapchain.get(), commandBuffer.get(), queues[decltype(initOutput)::EQT_GRAPHICS], imageAcquire[resourceIx].get(), renderFinished[resourceIx].get(), fence.get());
        CommonAPI::Present(logicalDevice.get(), swapchain.get(), queues[decltype(initOutput)::EQT_GRAPHICS], renderFinished[resourceIx].get(), acquiredNextFBO);
    }

    lodLibrary->clear();
    drawIndirectAllocator->clear();

    const auto& fboCreationParams = fbos[acquiredNextFBO]->getCreationParameters();
    auto gpuSourceImageView = fboCreationParams.attachments[0];

    bool status = ext::ScreenShot::createScreenShot(logicalDevice.get(), queues[decltype(initOutput)::EQT_TRANSFER_DOWN], renderFinished[resourceIx].get(), gpuSourceImageView.get(), assetManager.get(), "ScreenShot.png");
    assert(status);

    return 0;
}

#if 0
int main2()
{

    refctd_dynamic_array<ModelData_t>* dummy0 = nullptr;
    refctd_dynamic_array<DrawData_t>* dummy1;
    
    auto instanceData = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<ModelData_t>>(kInstanceCount);
    auto mbuff = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<core::smart_refctd_ptr<video::IGPUMeshBuffer> > >(kInstanceCount);
    
    //
    SBufferBinding<video::IGPUBuffer> globalVertexBindings[SVertexInputParams::MAX_ATTR_BUF_BINDING_COUNT];
    core::smart_refctd_ptr<video::IGPUBuffer> globalIndexBuffer,perDrawDataSSBO,indirectDrawSSBO,perInstanceDataSSBO;

    
    core::smart_refctd_ptr<video::IGPURenderpassIndependentPipeline> gpuDrawIndirectPipeline;
	{
        DrawElementsIndirectCommand_t indirectDrawData[kInstanceCount];

        {
            size_t vertexSize = 0;
            std::vector<uint8_t> vertexData;
            std::vector<uint32_t> indexData;

            std::uniform_int_distribution<uint32_t> dist(16, 4*1024);
            for (size_t i=0; i<kInstanceCount; i++)
            {
                float poly = sqrtf(dist(mt))+0.5f;

                //
                indirectDrawData[i].count = sphereData.indexCount;
                indirectDrawData[i].instanceCount = 1;
                indirectDrawData[i].firstIndex = indexData.size();
                indirectDrawData[i].baseVertex = vertexData.size()/vertexSize;
                indirectDrawData[i].baseInstance = 0;

                //
                auto vdata = reinterpret_cast<const uint8_t*>(databuf->buffer->getPointer());
                vertexData.insert(vertexData.end(),vdata,vdata+vdatasize);

                auto idata = reinterpret_cast<const uint32_t*>(sphereData.indexBuffer.buffer->getPointer());
                indexData.insert(indexData.end(),idata,idata+sphereData.indexCount);
            }
            indirectDrawSSBO = driver->createFilledDeviceLocalGPUBufferOnDedMem(sizeof(indirectDrawData), indirectDrawData);
            
            globalIndexBuffer = driver->createFilledDeviceLocalGPUBufferOnDedMem(indexData.size()*sizeof(uint32_t),indexData.data());
            indexData.clear();

            globalVertexBindings[0] = { 0u,driver->createFilledDeviceLocalGPUBufferOnDedMem(vertexData.size(),vertexData.data()) };
            vertexData.clear();
        }
        
        //
        gpuDrawIndirectPipeline = driver->getGPUObjectsFromAssets(&cpuDrawIndirectPipeline.get(),&cpuDrawIndirectPipeline.get()+1)->operator[](0);

        std::uniform_real_distribution<float> dist3D(0.f,400.f);
        for (size_t i=0; i<kInstanceCount; i++)
        {
            auto& meshbuffer = mbuff->operator[](i);
            meshbuffer = core::make_smart_refctd_ptr<video::IGPUMeshBuffer>(
                core::smart_refctd_ptr(gpuDrawDirectPipeline),
                nullptr,
                globalVertexBindings,
                SBufferBinding<video::IGPUBuffer>{indirectDrawData[i].firstIndex*sizeof(uint32_t),core::smart_refctd_ptr(globalIndexBuffer)}
            );

            meshbuffer->setBaseVertex(indirectDrawData[i].baseVertex);
            meshbuffer->setIndexCount(indirectDrawData[i].count);
            meshbuffer->setIndexType(asset::EIT_32BIT);

            auto& instance = instanceData->operator[](i);
            meshbuffer->setBoundingBox({instance.bbox[0].getAsVector3df(),instance.bbox[1].getAsVector3df()});

            {
                float scale = dist3D(mt)*0.0025f+1.f;
                instance.worldMatrix.setScale(core::vectorSIMDf(scale,scale,scale));
            }
            instance.worldMatrix.setTranslation(core::vectorSIMDf(dist3D(mt),dist3D(mt),dist3D(mt)));
            instance.worldMatrix.getSub3x3InverseTranspose(instance.normalMatrix);
        }

        perInstanceDataSSBO = driver->createFilledDeviceLocalGPUBufferOnDedMem(instanceData->bytesize(),instanceData->data());
	}
    
	auto perDrawData = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<DrawData_t>>(kInstanceCount);
	perDrawDataSSBO = driver->createDeviceLocalGPUBufferOnDedMem(perDrawData->bytesize());
    
    // TODO: get rid of the `const_cast`s
    auto drawIndirectLayout = const_cast<video::IGPUPipelineLayout*>(gpuDrawIndirectPipeline->getLayout());
    auto cullLayout = const_cast<video::IGPUPipelineLayout*>(gpuCullPipeline->getLayout());
    auto drawDirectDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(drawDirectLayout->getDescriptorSetLayout(1));
    auto drawIndirectDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(drawIndirectLayout->getDescriptorSetLayout(1));
    auto cullDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(cullLayout->getDescriptorSetLayout(1));
    auto drawDirectDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(drawDirectDescriptorLayout));
    auto drawIndirectDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(drawIndirectDescriptorLayout));
    auto cullDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(cullDescriptorLayout));
    {
        constexpr auto BindingCount = 3u;
        video::IGPUDescriptorSet::SWriteDescriptorSet writes[BindingCount];
        video::IGPUDescriptorSet::SDescriptorInfo infos[BindingCount];
        for (auto i=0; i<BindingCount; i++)
        {
            writes[i].binding = i;
            writes[i].arrayElement = 0u;
            writes[i].count = 1u;
            writes[i].descriptorType = asset::EDT_STORAGE_BUFFER;
            writes[i].info = infos+i;
        }
        infos[0].desc = perDrawDataSSBO;
        infos[0].buffer = { 0u,perDrawDataSSBO->getSize() };
        infos[1].desc = indirectDrawSSBO;
        infos[1].buffer = { 0u,indirectDrawSSBO->getSize() };
        infos[2].desc = perInstanceDataSSBO;
        infos[2].buffer = { 0u,perInstanceDataSSBO->getSize() };

        writes[0].dstSet = drawDirectDescriptorSet.get();
        driver->updateDescriptorSets(1u,writes,0u,nullptr);

        writes[0].dstSet = drawIndirectDescriptorSet.get();
        driver->updateDescriptorSets(1u,writes,0u,nullptr);

        writes[0].dstSet = cullDescriptorSet.get();
        writes[1].dstSet = cullDescriptorSet.get();
        writes[2].dstSet = cullDescriptorSet.get();
        driver->updateDescriptorSets(BindingCount,writes,0u,nullptr);
    }
    





        
        {
            CullShaderData_t pc;
            pc.viewProjMatrix = camera->getConcatenatedMatrix();
            pc.viewInverseTransposeMatrix = normalMatrix;
            pc.maxDrawCount = kInstanceCount;
            pc.cull = doCulling ? 1u:0u;

            driver->bindComputePipeline(gpuCullPipeline.get());
            driver->bindDescriptorSets(video::EPBP_COMPUTE, gpuCullPipeline->getLayout(), 1u, 1u, &cullDescriptorSet.get(), nullptr);
            driver->pushConstants(gpuCullPipeline->getLayout(), asset::ICPUSpecializedShader::ESS_COMPUTE, 0u, sizeof(CullShaderData_t), &pc);
            driver->dispatch((kInstanceCount+kCullWorkgroupSize-1)/kCullWorkgroupSize,1u,1u);
            video::COpenGLExtensionHandler::extGlMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_COMMAND_BARRIER_BIT);

            driver->bindGraphicsPipeline(gpuDrawIndirectPipeline.get());
            driver->bindDescriptorSets(video::EPBP_GRAPHICS, gpuDrawIndirectPipeline->getLayout(), 1u, 1u, &drawIndirectDescriptorSet.get(), nullptr);
            driver->drawIndexedIndirect(globalVertexBindings,asset::EPT_TRIANGLE_LIST,asset::EIT_32BIT, globalIndexBuffer.get(),indirectDrawSSBO.get(),0,kInstanceCount,sizeof(DrawElementsIndirectCommand_t));
        }

#endif