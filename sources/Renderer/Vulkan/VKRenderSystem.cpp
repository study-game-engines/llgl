/*
 * VKRenderSystem.cpp
 *
 * This file is part of the "LLGL" project (Copyright (c) 2015-2019 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include <LLGL/Platform/Platform.h>
#include "VKRenderSystem.h"
#include "Ext/VKExtensionLoader.h"
#include "Ext/VKExtensions.h"
#include "Memory/VKDeviceMemory.h"
#include "../RenderSystemUtils.h"
#include "../TextureUtils.h"
#include "../CheckedCast.h"
#include "../../Core/Helper.h"
#include "../../Core/Vendor.h"
#include "VKCore.h"
#include "VKTypes.h"
#include "VKInitializers.h"
#include "RenderState/VKPredicateQueryHeap.h"
#include "RenderState/VKComputePSO.h"
#include <LLGL/Log.h>
#include <LLGL/ImageFlags.h>
#include <limits>


namespace LLGL
{


/* ----- Internal functions ----- */

static VkResult CreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT* createInfo, const VkAllocationCallbacks* allocator, VkDebugReportCallbackEXT* callback)
{
    auto func = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT"));
    if (func != nullptr)
        return func(instance, createInfo, allocator, callback);
    else
        return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void DestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks* allocator)
{
    auto func = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT"));
    if (func != nullptr)
        func(instance, callback, allocator);
}

static VkBufferUsageFlags GetStagingVkBufferUsageFlags(long cpuAccessFlags)
{
    if ((cpuAccessFlags & CPUAccessFlags::Write) != 0)
        return VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    else
        return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
}


/* ----- Common ----- */

VKRenderSystem::VKRenderSystem(const RenderSystemDescriptor& renderSystemDesc) :
    instance_              { vkDestroyInstance                        },
    debugReportCallback_   { instance_, DestroyDebugReportCallbackEXT },
    defaultPipelineLayout_ { device_, vkDestroyPipelineLayout         }
{
    /* Extract optional renderer configuartion */
    auto rendererConfigVK = GetRendererConfiguration<RendererConfigurationVulkan>(renderSystemDesc);

    #ifdef LLGL_DEBUG
    debugLayerEnabled_ = true;
    #endif

    /* Create Vulkan instance and device objects */
    CreateInstance(rendererConfigVK);
    PickPhysicalDevice();
    CreateLogicalDevice();

    /* Create default resources */
    CreateDefaultPipelineLayout();

    /* Create device memory manager */
    deviceMemoryMngr_ = MakeUnique<VKDeviceMemoryManager>(
        device_,
        physicalDevice_.GetMemoryProperties(),
        (rendererConfigVK != nullptr ? rendererConfigVK->minDeviceMemoryAllocationSize : 1024*1024),
        (rendererConfigVK != nullptr ? rendererConfigVK->reduceDeviceMemoryFragmentation : false)
    );
}

VKRenderSystem::~VKRenderSystem()
{
    device_.WaitIdle();
}

/* ----- Swap-chain ----- */

SwapChain* VKRenderSystem::CreateSwapChain(const SwapChainDescriptor& swapChainDesc, const std::shared_ptr<Surface>& surface)
{
    return TakeOwnership(
        swapChains_,
        MakeUnique<VKSwapChain>(instance_, physicalDevice_, device_, *deviceMemoryMngr_, swapChainDesc, surface)
    );
}

void VKRenderSystem::Release(SwapChain& swapChain)
{
    RemoveFromUniqueSet(swapChains_, &swapChain);
}

/* ----- Command queues ----- */

CommandQueue* VKRenderSystem::GetCommandQueue()
{
    return commandQueue_.get();
}

/* ----- Command buffers ----- */

CommandBuffer* VKRenderSystem::CreateCommandBuffer(const CommandBufferDescriptor& commandBufferDesc)
{
    return TakeOwnership(
        commandBuffers_,
        MakeUnique<VKCommandBuffer>(physicalDevice_, device_, device_.GetVkQueue(), device_.GetQueueFamilyIndices(), commandBufferDesc)
    );
}

void VKRenderSystem::Release(CommandBuffer& commandBuffer)
{
    RemoveFromUniqueSet(commandBuffers_, &commandBuffer);
}

/* ----- Buffers ------ */

Buffer* VKRenderSystem::CreateBuffer(const BufferDescriptor& bufferDesc, const void* initialData)
{
    AssertCreateBuffer(bufferDesc, static_cast<uint64_t>(std::numeric_limits<VkDeviceSize>::max()));

    /* Create staging buffer */
    VkBufferCreateInfo stagingCreateInfo;
    BuildVkBufferCreateInfo(
        stagingCreateInfo,
        static_cast<VkDeviceSize>(bufferDesc.size),
        GetStagingVkBufferUsageFlags(bufferDesc.cpuAccessFlags)
    );

    auto stagingBuffer = CreateStagingBufferAndInitialize(stagingCreateInfo, initialData, bufferDesc.size);

    /* Create primary buffer object */
    auto buffer = TakeOwnership(buffers_, MakeUnique<VKBuffer>(device_, bufferDesc));

    /* Allocate device memory */
    auto memoryRegion = deviceMemoryMngr_->Allocate(
        buffer->GetDeviceBuffer().GetRequirements(),
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    buffer->BindMemoryRegion(device_, memoryRegion);

    /* Copy staging buffer into hardware buffer */
    device_.CopyBuffer(stagingBuffer.GetVkBuffer(), buffer->GetVkBuffer(), static_cast<VkDeviceSize>(bufferDesc.size));

    if (bufferDesc.cpuAccessFlags != 0 || (bufferDesc.miscFlags & MiscFlags::DynamicUsage) != 0)
    {
        /* Store ownership of staging buffer */
        buffer->TakeStagingBuffer(std::move(stagingBuffer));
    }
    else
    {
        /* Release staging buffer */
        stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);
    }

    return buffer;
}

BufferArray* VKRenderSystem::CreateBufferArray(std::uint32_t numBuffers, Buffer* const * bufferArray)
{
    AssertCreateBufferArray(numBuffers, bufferArray);
    return TakeOwnership(bufferArrays_, MakeUnique<VKBufferArray>(numBuffers, bufferArray));
}

void VKRenderSystem::Release(Buffer& buffer)
{
    /* Release device memory regions for primary buffer and internal staging buffer, then release buffer object */
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);
    bufferVK.GetDeviceBuffer().ReleaseMemoryRegion(*deviceMemoryMngr_);
    bufferVK.GetStagingDeviceBuffer().ReleaseMemoryRegion(*deviceMemoryMngr_);
    RemoveFromUniqueSet(buffers_, &buffer);
}

void VKRenderSystem::Release(BufferArray& bufferArray)
{
    RemoveFromUniqueSet(bufferArrays_, &bufferArray);
}

void VKRenderSystem::WriteBuffer(Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t dataSize)
{
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);

    if (bufferVK.GetStagingVkBuffer() != VK_NULL_HANDLE)
    {
        /* Copy input data to staging buffer memory */
        device_.WriteBuffer(bufferVK.GetStagingDeviceBuffer(), data, dataSize, offset);

        /* Copy staging buffer into hardware buffer */
        device_.CopyBuffer(bufferVK.GetStagingVkBuffer(), bufferVK.GetVkBuffer(), dataSize, offset, offset);
    }
    else
    {
        /* Create staging buffer */
        VkBufferCreateInfo stagingCreateInfo;
        BuildVkBufferCreateInfo(
            stagingCreateInfo,
            dataSize,
            (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        );

        auto stagingBuffer = CreateStagingBufferAndInitialize(stagingCreateInfo, data, dataSize);

        /* Copy staging buffer into hardware buffer */
        device_.CopyBuffer(stagingBuffer.GetVkBuffer(), bufferVK.GetVkBuffer(), dataSize, 0, offset);

        /* Release device memory region of staging buffer */
        stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);
    }
}

void VKRenderSystem::ReadBuffer(Buffer& buffer, std::uint64_t offset, void* data, std::uint64_t dataSize)
{
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);

    if (bufferVK.GetStagingVkBuffer() != VK_NULL_HANDLE)
    {
        /* Copy hardware buffer into staging buffer */
        device_.CopyBuffer(bufferVK.GetVkBuffer(), bufferVK.GetStagingVkBuffer(), dataSize, offset, offset);

        /* Copy staging buffer memory to output data */
        device_.ReadBuffer(bufferVK.GetStagingDeviceBuffer(), data, dataSize, offset);
    }
    else
    {
        /* Create staging buffer */
        VkBufferCreateInfo stagingCreateInfo;
        BuildVkBufferCreateInfo(
            stagingCreateInfo,
            dataSize,
            (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        );

        auto stagingBuffer = CreateStagingBuffer(stagingCreateInfo);

        /* Copy hardware buffer into staging buffer */
        device_.CopyBuffer(bufferVK.GetVkBuffer(), stagingBuffer.GetVkBuffer(), dataSize, offset, 0);

        /* Copy staging buffer memory to output data */
        device_.ReadBuffer(stagingBuffer, data, dataSize, 0);

        /* Release device memory region of staging buffer */
        stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);
    }
}

void* VKRenderSystem::MapBuffer(Buffer& buffer, const CPUAccess access)
{
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);
    return bufferVK.Map(device_, access, 0, bufferVK.GetSize());
}

void* VKRenderSystem::MapBuffer(Buffer& buffer, const CPUAccess access, std::uint64_t offset, std::uint64_t length)
{
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);
    return bufferVK.Map(device_, access, static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(length));
}

void VKRenderSystem::UnmapBuffer(Buffer& buffer)
{
    auto& bufferVK = LLGL_CAST(VKBuffer&, buffer);
    bufferVK.Unmap(device_);
}

/* ----- Textures ----- */

Texture* VKRenderSystem::CreateTexture(const TextureDescriptor& textureDesc, const SrcImageDescriptor* imageDesc)
{
    /* Determine size of image for staging buffer */
    const auto imageSize        = NumMipTexels(textureDesc, 0);
    const auto initialDataSize  = static_cast<VkDeviceSize>(GetMemoryFootprint(textureDesc.format, imageSize));

    /* Set up initial image data */
    const void* initialData = nullptr;
    ByteBuffer intermediateData;

    if (imageDesc)
    {
        /* Check if image data must be converted */
        const auto& formatAttribs = GetFormatAttribs(textureDesc.format);
        if (formatAttribs.bitSize > 0 && (formatAttribs.flags & FormatFlags::IsCompressed) == 0)
        {
            /* Convert image format (will be null if no conversion is necessary) */
            intermediateData = ConvertImageBuffer(*imageDesc, formatAttribs.format, formatAttribs.dataType, Constants::maxThreadCount);
        }

        if (intermediateData)
        {
            /*
            Validate that source image data was large enough so conversion is valid,
            then use temporary image buffer as source for initial data
            */
            const auto srcImageDataSize = imageSize * ImageFormatSize(imageDesc->format) * DataTypeSize(imageDesc->dataType);
            AssertImageDataSize(imageDesc->dataSize, static_cast<std::size_t>(srcImageDataSize));
            initialData = intermediateData.get();
        }
        else
        {
            /*
            Validate that image data is large enough,
            then use input data as source for initial data
            */
            AssertImageDataSize(imageDesc->dataSize, static_cast<std::size_t>(initialDataSize));
            initialData = imageDesc->data;
        }
    }
    else if ((textureDesc.miscFlags & MiscFlags::NoInitialData) == 0)
    {
        /* Allocate default image data */
        const auto& formatAttribs = GetFormatAttribs(textureDesc.format);
        if (formatAttribs.bitSize > 0 && (formatAttribs.flags & FormatFlags::IsCompressed) == 0)
        {
            const ColorRGBAd fillColor{ textureDesc.clearValue.color.Cast<double>() };
            intermediateData = GenerateImageBuffer(formatAttribs.format, formatAttribs.dataType, imageSize, fillColor);
        }
        else
            intermediateData = AllocateByteBuffer(static_cast<std::size_t>(initialDataSize), UninitializeTag{});

        initialData = intermediateData.get();
    }

    /* Create staging buffer */
    VkBufferCreateInfo stagingCreateInfo;
    BuildVkBufferCreateInfo(
        stagingCreateInfo,
        initialDataSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    );

    auto stagingBuffer = CreateStagingBufferAndInitialize(stagingCreateInfo, initialData, initialDataSize);

    /* Create device texture */
    auto textureVK  = MakeUnique<VKTexture>(device_, *deviceMemoryMngr_, textureDesc);

    /* Copy staging buffer into hardware texture, then transfer image into sampling-ready state */
    auto cmdBuffer = device_.AllocCommandBuffer();
    {
        const TextureSubresource subresource{ 0, textureVK->GetNumArrayLayers(), 0, textureVK->GetNumMipLevels() };

        device_.TransitionImageLayout(
            cmdBuffer,
            textureVK->GetVkImage(),
            textureVK->GetVkFormat(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            subresource
        );

        device_.CopyBufferToImage(
            cmdBuffer,
            stagingBuffer.GetVkBuffer(),
            textureVK->GetVkImage(),
            textureVK->GetVkFormat(),
            VkOffset3D{ 0, 0, 0 },
            textureVK->GetVkExtent(),
            subresource
        );

        device_.TransitionImageLayout(
            cmdBuffer,
            textureVK->GetVkImage(),
            textureVK->GetVkFormat(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource
        );

        /* Generate MIP-maps if enabled */
        if (imageDesc != nullptr && MustGenerateMipsOnCreate(textureDesc))
        {
            device_.GenerateMips(
                cmdBuffer,
                textureVK->GetVkImage(),
                textureVK->GetVkFormat(),
                textureVK->GetVkExtent(),
                subresource
            );
        }
    }
    device_.FlushCommandBuffer(cmdBuffer);

    /* Release staging buffer */
    stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);

    /* Create image view for texture */
    textureVK->CreateInternalImageView(device_);

    return TakeOwnership(textures_, std::move(textureVK));
}

void VKRenderSystem::Release(Texture& texture)
{
    /* Release device memory region, then release texture object */
    auto& textureVK = LLGL_CAST(VKTexture&, texture);
    deviceMemoryMngr_->Release(textureVK.GetMemoryRegion());
    RemoveFromUniqueSet(textures_, &texture);
}

void VKRenderSystem::WriteTexture(Texture& texture, const TextureRegion& textureRegion, const SrcImageDescriptor& imageDesc)
{
    auto& textureVK = LLGL_CAST(VKTexture&, texture);

    /* Determine size of image for staging buffer */
    const auto& offset          = textureRegion.offset;
    const auto& extent          = textureRegion.extent;
    const auto& subresource     = textureRegion.subresource;
    const auto  format          = VKTypes::Unmap(textureVK.GetVkFormat());

    auto        image           = textureVK.GetVkImage();
    const auto  imageSize       = extent.width * extent.height * extent.depth;
    const void* imageData       = nullptr;
    const auto  imageDataSize   = static_cast<VkDeviceSize>(GetMemoryFootprint(format, imageSize));

    /* Check if image data must be converted */
    ByteBuffer intermediateData;

    const auto& formatAttribs = GetFormatAttribs(format);
    if (formatAttribs.bitSize > 0 && (formatAttribs.flags & FormatFlags::IsCompressed) == 0)
    {
        /* Convert image format (will be null if no conversion is necessary) */
        intermediateData = ConvertImageBuffer(imageDesc, formatAttribs.format, formatAttribs.dataType, Constants::maxThreadCount);
    }

    if (intermediateData)
    {
        /*
        Validate that source image data was large enough so conversion is valid,
        then use temporary image buffer as source for initial data
        */
        const auto srcImageDataSize = imageSize * ImageFormatSize(imageDesc.format) * DataTypeSize(imageDesc.dataType);
        AssertImageDataSize(imageDesc.dataSize, static_cast<std::size_t>(srcImageDataSize));
        imageData = intermediateData.get();
    }
    else
    {
        /*
        Validate that image data is large enough,
        then use input data as source for initial data
        */
        AssertImageDataSize(imageDesc.dataSize, static_cast<std::size_t>(imageDataSize));
        imageData = imageDesc.data;
    }

    /* Create staging buffer */
    VkBufferCreateInfo stagingCreateInfo;
    BuildVkBufferCreateInfo(
        stagingCreateInfo,
        imageDataSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT  // <-- TODO: support read/write mapping //GetStagingVkBufferUsageFlags(bufferDesc.cpuAccessFlags)
    );

    auto stagingBuffer = CreateStagingBufferAndInitialize(stagingCreateInfo, imageData, imageDataSize);

    /* Copy staging buffer into hardware texture, then transfer image into sampling-ready state */
    auto cmdBuffer = device_.AllocCommandBuffer();
    {
        device_.TransitionImageLayout(
            cmdBuffer,
            image,
            textureVK.GetVkFormat(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            subresource
        );

        device_.CopyBufferToImage(
            cmdBuffer,
            stagingBuffer.GetVkBuffer(),
            image,
            textureVK.GetVkFormat(),
            VkOffset3D{ offset.x, offset.y, offset.z },
            VkExtent3D{ extent.width, extent.height, extent.depth },
            subresource
        );

        device_.TransitionImageLayout(
            cmdBuffer,
            image,
            textureVK.GetVkFormat(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource
        );
    }
    device_.FlushCommandBuffer(cmdBuffer);

    /* Release staging buffer */
    stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);
}

void VKRenderSystem::ReadTexture(Texture& texture, const TextureRegion& textureRegion, const DstImageDescriptor& imageDesc)
{
    auto& textureVK = LLGL_CAST(VKTexture&, texture);

    /* Determine size of image for staging buffer */
    const auto& offset = textureRegion.offset;
    const auto& extent = textureRegion.extent;
    const auto  format = VKTypes::Unmap(textureVK.GetVkFormat());

    auto        image           = textureVK.GetVkImage();
    const auto  imageSize       = extent.width * extent.height * extent.depth;
    const auto  imageDataSize   = static_cast<VkDeviceSize>(GetMemoryFootprint(format, imageSize));

    /* Create staging buffer */
    VkBufferCreateInfo stagingCreateInfo;
    BuildVkBufferCreateInfo(stagingCreateInfo, imageDataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto stagingBuffer = CreateStagingBuffer(stagingCreateInfo);

    /* Copy staging buffer into hardware texture, then transfer image into sampling-ready state */
    auto cmdBuffer = device_.AllocCommandBuffer();
    {
        device_.TransitionImageLayout(
            cmdBuffer,
            image,
            textureVK.GetVkFormat(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            textureRegion.subresource
        );

        device_.CopyImageToBuffer(
            cmdBuffer,
            image,
            stagingBuffer.GetVkBuffer(),
            textureVK.GetVkFormat(),
            VkOffset3D{ offset.x, offset.y, offset.z },
            VkExtent3D{ extent.width, extent.height, extent.depth },
            textureRegion.subresource
        );

        device_.TransitionImageLayout(
            cmdBuffer,
            image,
            textureVK.GetVkFormat(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            textureRegion.subresource
        );
    }
    device_.FlushCommandBuffer(cmdBuffer);

    /* Map staging buffer to CPU memory space */
    if (auto region = stagingBuffer.GetMemoryRegion())
    {
        /* Map buffer memory to host memory */
        auto deviceMemory = region->GetParentChunk();
        if (auto memory = deviceMemory->Map(device_, region->GetOffset(), imageDataSize))
        {
            /* Copy data to buffer object */
            CopyTextureImageData(imageDesc, extent, format, memory);
            deviceMemory->Unmap(device_);
        }
    }

    /* Release staging buffer */
    stagingBuffer.ReleaseMemoryRegion(*deviceMemoryMngr_);
}

/* ----- Sampler States ---- */

Sampler* VKRenderSystem::CreateSampler(const SamplerDescriptor& samplerDesc)
{
    return TakeOwnership(samplers_, MakeUnique<VKSampler>(device_, samplerDesc));
}

void VKRenderSystem::Release(Sampler& sampler)
{
    RemoveFromUniqueSet(samplers_, &sampler);
}

/* ----- Resource Heaps ----- */

ResourceHeap* VKRenderSystem::CreateResourceHeap(const ResourceHeapDescriptor& resourceHeapDesc, const ArrayView<ResourceViewDescriptor>& initialResourceViews)
{
    return TakeOwnership(
        resourceHeaps_,
        MakeUnique<VKResourceHeap>(device_, resourceHeapDesc, initialResourceViews)
    );
}

void VKRenderSystem::Release(ResourceHeap& resourceHeap)
{
    RemoveFromUniqueSet(resourceHeaps_, &resourceHeap);
}

std::uint32_t VKRenderSystem::WriteResourceHeap(ResourceHeap& resourceHeap, std::uint32_t firstDescriptor, const ArrayView<ResourceViewDescriptor>& resourceViews)
{
    auto& resourceHeapVK = LLGL_CAST(VKResourceHeap&, resourceHeap);
    return resourceHeapVK.UpdateDescriptors(device_, firstDescriptor, resourceViews);
}

/* ----- Render Passes ----- */

RenderPass* VKRenderSystem::CreateRenderPass(const RenderPassDescriptor& renderPassDesc)
{
    return TakeOwnership(renderPasses_, MakeUnique<VKRenderPass>(device_, renderPassDesc));
}

void VKRenderSystem::Release(RenderPass& renderPass)
{
    RemoveFromUniqueSet(renderPasses_, &renderPass);
}

/* ----- Render Targets ----- */

RenderTarget* VKRenderSystem::CreateRenderTarget(const RenderTargetDescriptor& renderTargetDesc)
{
    AssertCreateRenderTarget(renderTargetDesc);
    return TakeOwnership(renderTargets_, MakeUnique<VKRenderTarget>(device_, *deviceMemoryMngr_, renderTargetDesc));
}

void VKRenderSystem::Release(RenderTarget& renderTarget)
{
    /* Release device memory region, then release texture object */
    auto& renderTargetVL = LLGL_CAST(VKRenderTarget&, renderTarget);
    RemoveFromUniqueSet(renderTargets_, &renderTarget);
}

/* ----- Shader ----- */

Shader* VKRenderSystem::CreateShader(const ShaderDescriptor& shaderDesc)
{
    AssertCreateShader(shaderDesc);
    return TakeOwnership(shaders_, MakeUnique<VKShader>(device_, shaderDesc));
}

void VKRenderSystem::Release(Shader& shader)
{
    RemoveFromUniqueSet(shaders_, &shader);
}

/* ----- Pipeline Layouts ----- */

PipelineLayout* VKRenderSystem::CreatePipelineLayout(const PipelineLayoutDescriptor& pipelineLayoutDesc)
{
    return TakeOwnership(pipelineLayouts_, MakeUnique<VKPipelineLayout>(device_, pipelineLayoutDesc));
}

void VKRenderSystem::Release(PipelineLayout& pipelineLayout)
{
    RemoveFromUniqueSet(pipelineLayouts_, &pipelineLayout);
}

/* ----- Pipeline States ----- */

PipelineState* VKRenderSystem::CreatePipelineState(const Blob& /*serializedCache*/)
{
    return nullptr;//TODO
}

PipelineState* VKRenderSystem::CreatePipelineState(const GraphicsPipelineDescriptor& pipelineStateDesc, std::unique_ptr<Blob>* /*serializedCache*/)
{
    return TakeOwnership(
        pipelineStates_,
        MakeUnique<VKGraphicsPSO>(
            device_,
            defaultPipelineLayout_,
            (!swapChains_.empty() ? (*swapChains_.begin())->GetRenderPass() : nullptr),
            pipelineStateDesc,
            gfxPipelineLimits_
        )
    );
}

PipelineState* VKRenderSystem::CreatePipelineState(const ComputePipelineDescriptor& pipelineStateDesc, std::unique_ptr<Blob>* /*serializedCache*/)
{
    return TakeOwnership(
        pipelineStates_,
        MakeUnique<VKComputePSO>(device_, pipelineStateDesc, defaultPipelineLayout_)
    );
}

void VKRenderSystem::Release(PipelineState& pipelineState)
{
    RemoveFromUniqueSet(pipelineStates_, &pipelineState);
}

/* ----- Queries ----- */

QueryHeap* VKRenderSystem::CreateQueryHeap(const QueryHeapDescriptor& queryHeapDesc)
{
    if (queryHeapDesc.renderCondition)
        return TakeOwnership(queryHeaps_, MakeUnique<VKPredicateQueryHeap>(device_, *deviceMemoryMngr_, queryHeapDesc));
    else
        return TakeOwnership(queryHeaps_, MakeUnique<VKQueryHeap>(device_, queryHeapDesc));
}

void VKRenderSystem::Release(QueryHeap& queryHeap)
{
    RemoveFromUniqueSet(queryHeaps_, &queryHeap);
}

/* ----- Fences ----- */

Fence* VKRenderSystem::CreateFence()
{
    return TakeOwnership(fences_, MakeUnique<VKFence>(device_));
}

void VKRenderSystem::Release(Fence& fence)
{
    RemoveFromUniqueSet(fences_, &fence);
}


/*
 * ======= Private: =======
 */

#ifndef VK_LAYER_KHRONOS_VALIDATION_NAME
#define VK_LAYER_KHRONOS_VALIDATION_NAME "VK_LAYER_KHRONOS_validation"
#endif

void VKRenderSystem::CreateInstance(const RendererConfigurationVulkan* config)
{
    /* Query instance layer properties */
    auto layerProperties = VKQueryInstanceLayerProperties();
    std::vector<const char*> layerNames;

    for (const auto& prop : layerProperties)
    {
        if (IsLayerRequired(prop.layerName, config))
            layerNames.push_back(prop.layerName);
    }

    /* Query instance extension properties */
    auto extensionProperties = VKQueryInstanceExtensionProperties();
    std::vector<const char*> extensionNames;

    for (const auto& prop : extensionProperties)
    {
        if (IsExtensionRequired(prop.extensionName))
            extensionNames.push_back(prop.extensionName);
    }

    /* Setup Vulkan instance descriptor */
    VkInstanceCreateInfo instanceInfo;
    VkApplicationInfo appInfo;

    instanceInfo.sType                          = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext                          = nullptr;
    instanceInfo.flags                          = 0;

    /* Specify application descriptor */
    if (config != nullptr)
    {
        /* Initialize application information struct */
        {
            appInfo.sType                       = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pNext                       = nullptr;
            appInfo.pApplicationName            = config->application.applicationName;
            appInfo.applicationVersion          = config->application.applicationVersion;
            appInfo.pEngineName                 = config->application.engineName;
            appInfo.engineVersion               = config->application.engineVersion;
            appInfo.apiVersion                  = VK_API_VERSION_1_0;
        }
        instanceInfo.pApplicationInfo           = (&appInfo);
    }
    else
        instanceInfo.pApplicationInfo           = nullptr;

    /* Specify layers to enable  */
    if (layerNames.empty())
    {
        instanceInfo.enabledLayerCount          = 0;
        instanceInfo.ppEnabledLayerNames        = nullptr;
    }
    else
    {
        instanceInfo.enabledLayerCount          = static_cast<std::uint32_t>(layerNames.size());
        instanceInfo.ppEnabledLayerNames        = layerNames.data();
    }

    /* Specify extensions to enable */
    if (extensionNames.empty())
    {
        instanceInfo.enabledExtensionCount      = 0;
        instanceInfo.ppEnabledExtensionNames    = nullptr;
    }
    else
    {
        instanceInfo.enabledExtensionCount      = static_cast<std::uint32_t>(extensionNames.size());
        instanceInfo.ppEnabledExtensionNames    = extensionNames.data();
    }

    /* Create Vulkan instance */
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, instance_.ReleaseAndGetAddressOf());
    VKThrowIfFailed(result, "failed to create Vulkan instance");

    if (debugLayerEnabled_)
        CreateDebugReportCallback();

    /* Load Vulkan instance extensions */
    VKLoadInstanceExtensions(instance_);
}

static Log::ReportType ToReportType(VkDebugReportFlagsEXT flags)
{
    if ((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0)
        return Log::ReportType::Error;
    if ((flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) != 0)
        return Log::ReportType::Warning;
    if ((flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) != 0)
        return Log::ReportType::Performance;
    return Log::ReportType::Information;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VKDebugCallback(
    VkDebugReportFlagsEXT       flags,
    VkDebugReportObjectTypeEXT  objectType,
    uint64_t                    object,
    size_t                      location,
    int32_t                     messageCode,
    const char*                 layerPrefix,
    const char*                 message,
    void*                       userData)
{
    //auto renderSystemVK = reinterpret_cast<VKRenderSystem*>(userData);
    Log::PostReport(ToReportType(flags), message, "vkDebugReportCallback");
    return VK_FALSE;
}

void VKRenderSystem::CreateDebugReportCallback()
{
    /* Initialize flags */
    VkDebugReportFlagsEXT flags = 0;

    //flags |= VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
    flags |= VK_DEBUG_REPORT_WARNING_BIT_EXT;
    //flags |= VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
    flags |= VK_DEBUG_REPORT_ERROR_BIT_EXT;
    //flags |= VK_DEBUG_REPORT_DEBUG_BIT_EXT;

    /* Create report callback */
    VkDebugReportCallbackCreateInfoEXT createInfo;
    {
        createInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        createInfo.pNext        = nullptr;
        createInfo.flags        = flags;
        createInfo.pfnCallback  = VKDebugCallback;
        createInfo.pUserData    = reinterpret_cast<void*>(this);
    }
    auto result = CreateDebugReportCallbackEXT(instance_, &createInfo, nullptr, debugReportCallback_.ReleaseAndGetAddressOf());
    VKThrowIfFailed(result, "failed to create Vulkan debug report callback");
}

void VKRenderSystem::PickPhysicalDevice()
{
    /* Pick physical device with Vulkan support */
    if (!physicalDevice_.PickPhysicalDevice(instance_))
        throw std::runtime_error("failed to find suitable Vulkan device");

    /* Query and store rendering capabilities */
    RendererInfo info;
    RenderingCapabilities caps;

    physicalDevice_.QueryDeviceProperties(info, caps, gfxPipelineLimits_);

    /* Store Vulkan extension names */
    const auto& extensions = physicalDevice_.GetExtensionNames();
    info.extensionNames = std::vector<std::string>(extensions.begin(), extensions.end());

    SetRendererInfo(info);
    SetRenderingCaps(caps);
}

void VKRenderSystem::CreateLogicalDevice()
{
    /* Create logical device with all supported physical device feature */
    device_ = physicalDevice_.CreateLogicalDevice();

    /* Create command queue interface */
    commandQueue_ = MakeUnique<VKCommandQueue>(device_, device_.GetVkQueue());

    /* Load Vulkan device extensions */
    VKLoadDeviceExtensions(device_, physicalDevice_.GetExtensionNames());
}

void VKRenderSystem::CreateDefaultPipelineLayout()
{
    VkPipelineLayoutCreateInfo layoutCreateInfo = {};
    {
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    }
    auto result = vkCreatePipelineLayout(device_, &layoutCreateInfo, nullptr, defaultPipelineLayout_.ReleaseAndGetAddressOf());
    VKThrowIfFailed(result, "failed to create Vulkan default pipeline layout");
}

bool VKRenderSystem::IsLayerRequired(const char* name, const RendererConfigurationVulkan* config) const
{
    if (config != nullptr)
    {
        for (const char* layer : config->enabledLayers)
        {
            if (::strcmp(layer, name) == 0)
                return true;
        }
    }

    if (debugLayerEnabled_)
    {
        if (::strcmp(name, VK_LAYER_KHRONOS_VALIDATION_NAME) == 0)
            return true;
    }

    return false;
}

bool VKRenderSystem::IsExtensionRequired(const std::string& name) const
{
    return
    (
        name == VK_KHR_SURFACE_EXTENSION_NAME
        #ifdef LLGL_OS_WIN32
        || name == VK_KHR_WIN32_SURFACE_EXTENSION_NAME
        #endif
        #ifdef LLGL_OS_LINUX
        || name == VK_KHR_XLIB_SURFACE_EXTENSION_NAME
        #endif
        || (debugLayerEnabled_ && name == VK_EXT_DEBUG_REPORT_EXTENSION_NAME)
    );
}

VKDeviceBuffer VKRenderSystem::CreateStagingBuffer(const VkBufferCreateInfo& createInfo)
{
    return VKDeviceBuffer
    {
        device_,
        createInfo,
        *deviceMemoryMngr_,
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
}

VKDeviceBuffer VKRenderSystem::CreateStagingBufferAndInitialize(
    const VkBufferCreateInfo&   createInfo,
    const void*                 data,
    VkDeviceSize                dataSize)
{
    /* Allocate staging buffer */
    auto stagingBuffer = CreateStagingBuffer(createInfo);

    /* Copy initial data to buffer memory */
    if (data != nullptr && dataSize > 0)
        device_.WriteBuffer(stagingBuffer, data, dataSize);

    return stagingBuffer;
}


} // /namespace LLGL



// ================================================================================
