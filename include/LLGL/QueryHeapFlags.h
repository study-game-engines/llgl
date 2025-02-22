/*
 * QueryHeapFlags.h
 *
 * This file is part of the "LLGL" project (Copyright (c) 2015-2019 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#ifndef LLGL_QUERY_HEAP_FLAGS_H
#define LLGL_QUERY_HEAP_FLAGS_H


#include <LLGL/Constants.h>
#include <cstdint>


namespace LLGL
{


/* ----- Enumerations ----- */

/**
\brief Query type enumeration.
\see QueryHeapDescriptor::type
*/
enum class QueryType
{
    //! Number of samples that passed the depth test. This can be used as render condition.
    SamplesPassed,

    //! Non-zero if any samples passed the depth test. This can be used as render condition.
    AnySamplesPassed,

    //! Non-zero if any samples passed the depth test within a conservative rasterization. This can be used as render condition.
    AnySamplesPassedConservative,

    //! Elapsed time (in nanoseconds) between the begin- and end query command.
    TimeElapsed,

    //! Number of vertices that have been written into a stream output (also called "Transform Feedback").
    StreamOutPrimitivesWritten,

    //! Non-zero if any of the streaming output buffers (also called "Transform Feedback Buffers") has an overflow.
    StreamOutOverflow,

    /**
    \brief Pipeline statistics such as number of shader invocations, generated primitives, etc.
    \see QueryPipelineStatistics
    \see RenderingFeatures::hasPipelineStatistics
    */
    PipelineStatistics,
};


/* ----- Structures ----- */

/**
\brief Query data structure for pipeline statistics.
\remarks This structure is byte aligned, i.e. it can be reinterpret casted to a buffer in CPU memory space.
\see QueryType::PipelineStatistics
\see CommandQueue::QueryResult
\see RenderingFeatures::hasPipelineStatistics
\see Direct3D11 counterpart \c D3D11_QUERY_DATA_PIPELINE_STATISTICS: https://docs.microsoft.com/en-us/windows/desktop/api/d3d11/ns-d3d11-d3d11_query_data_pipeline_statistics
\see Direct3D12 counterpart \c D3D12_QUERY_DATA_PIPELINE_STATISTICS: https://docs.microsoft.com/en-us/windows/desktop/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics
\see Vulkan counterpart \c VkQueryPipelineStatisticFlagBits: https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkQueryPipelineStatisticFlagBits.html
*/
struct QueryPipelineStatistics
{
    //! Number of vertices submitted to the input-assembly.
    std::uint64_t inputAssemblyVertices             = 0;

    //! Number of primitives submitted to the input-assembly.
    std::uint64_t inputAssemblyPrimitives           = 0;

    //! Number of vertex shader invocations.
    std::uint64_t vertexShaderInvocations           = 0;

    //! Number of geometry shader invocations.
    std::uint64_t geometryShaderInvocations         = 0;

    //! Number of primitives generated by the geometry shader.
    std::uint64_t geometryShaderPrimitives          = 0;

    //! Number of primitives that reached the primitive clipping stage.
    std::uint64_t clippingInvocations               = 0;

    //! Number of primitives that passed the primitive clipping stage.
    std::uint64_t clippingPrimitives                = 0;

    //! Number of fragment shader invocations.
    std::uint64_t fragmentShaderInvocations         = 0;

    //! Number of tessellation-control shader invocations.
    std::uint64_t tessControlShaderInvocations      = 0;

    //! Number of tessellation-evaluation shader invocations.
    std::uint64_t tessEvaluationShaderInvocations   = 0;

    //! Number of compute shader invocations.
    std::uint64_t computeShaderInvocations          = 0;
};

/**
\brief Query heap descriptor structure.
\see RenderSystem::CreateQueryHeap
*/
struct QueryHeapDescriptor
{
    //! Specifies the type of queries in the heap. By default QueryType::SamplesPassed.
    QueryType       type            = QueryType::SamplesPassed;

    //! Specifies the number of queries in the heap. This must be greater than zero. By default 1.
    std::uint32_t   numQueries      = 1;

    /**
    \brief Specifies whether the queries are to be used as render conditions. By default false.
    \remarks If this is true, the results of the queries cannot be retrieved by CommandBuffer::QueryResult and the member \c type can only have one of the following values:
    - QueryType::SamplesPassed
    - QueryType::AnySamplesPassed
    - QueryType::AnySamplesPassedConservative
    - QueryType::StreamOutOverflow
    \remarks Render conditions can be used to render complex geometry under the condition that
    a previous (commonly significantly smaller) geometry has passed the depth and stencil tests.
    \see CommandBuffer::BeginRenderCondition
    \see CommandBuffer::EndRenderCondition
    \note Only supported with: OpenGL, Direct3D 11, Direct3D 12.
    */
    bool            renderCondition = false;
};


} // /namespace LLGL


#endif



// ================================================================================
