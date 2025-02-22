/*
 * DXTypes.h
 *
 * This file is part of the "LLGL" project (Copyright (c) 2015-2019 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#ifndef LLGL_DX_TYPES_H
#define LLGL_DX_TYPES_H


#include <LLGL/VertexAttribute.h>
#include <LLGL/ShaderReflection.h>
#include <LLGL/RenderSystemFlags.h>
#include <LLGL/PipelineStateFlags.h>
#include <LLGL/TextureFlags.h>
#include <LLGL/BufferFlags.h>
#include <dxgiformat.h>
#include <d3dcommon.h>


namespace LLGL
{

namespace DXTypes
{


[[noreturn]]
void MapFailed(const char* typeName, const char* dxTypeName);

[[noreturn]]
void UnmapFailed(const char* typeName, const char* dxTypeName);

[[noreturn]]
void ParamNotSupported(const char* paramName, const char* requirement);


DXGI_FORMAT ToDXGIFormat(const DataType dataType);
DXGI_FORMAT ToDXGIFormat(const Format format);
DXGI_FORMAT ToDXGIFormatDSV(const DXGI_FORMAT format);
DXGI_FORMAT ToDXGIFormatSRV(const DXGI_FORMAT format);
DXGI_FORMAT ToDXGIFormatUAV(const DXGI_FORMAT format);

// Returns the specified DXGI_FORMAT as typeless format or DXGI_FORMAT_UNKNOWN if the format cannot be converted to a typeless format.
DXGI_FORMAT ToDXGIFormatUInt(const DXGI_FORMAT format);

// Returns the specified DXGI_FORMAT as typeless format or DXGI_FORMAT_UNKNOWN if the format cannot be converted to a typeless format.
DXGI_FORMAT ToDXGIFormatTypeless(const DXGI_FORMAT format);

bool IsTypelessDXGIFormat(const DXGI_FORMAT format);

D3D_PRIMITIVE_TOPOLOGY  ToD3DPrimitiveTopology(const PrimitiveTopology topology);

Format                  Unmap( const DXGI_FORMAT            format    );
StorageBufferType       Unmap( const D3D_SHADER_INPUT_TYPE  inputType );
SystemValue             Unmap( const D3D_NAME               name      );
ResourceType            Unmap( const D3D_SRV_DIMENSION      dimension );

bool HasStencilComponent(const DXGI_FORMAT format);
bool IsDXGIFormatSRGB(const DXGI_FORMAT format);
bool MakeUAVClearVector(const DXGI_FORMAT format, UINT (&valuesVec4)[4], UINT value);


} // /namespace DXTypes

} // /namespace LLGL


#endif



// ================================================================================
