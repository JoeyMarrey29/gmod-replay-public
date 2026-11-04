#pragma once

#include <d3d12.h>
#include <wrl.h>

#include <stdexcept>
#include <string>

namespace gmod::dx12 {

using Microsoft::WRL::ComPtr;

inline ComPtr<ID3D12Resource> create_buffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES initial_state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    const HRESULT result = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, initial_state,
        nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result)) {
        throw std::runtime_error(
            "ID3D12Device::CreateCommittedResource failed: 0x"
            + std::to_string(static_cast<unsigned long>(result)));
    }
    return resource;
}

} // namespace gmod::dx12
