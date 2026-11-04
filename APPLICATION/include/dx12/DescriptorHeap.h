#pragma once

#include <d3d12.h>
#include <wrl.h>

#include <stdexcept>
#include <string>

namespace gmod::dx12 {

using Microsoft::WRL::ComPtr;

class DescriptorHeap {
public:
    void create(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT count,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = type;
        description.NumDescriptors = count;
        description.Flags = flags;
        const HRESULT result = device->CreateDescriptorHeap(
            &description, IID_PPV_ARGS(&heap_));
        if (FAILED(result)) {
            throw std::runtime_error(
                "ID3D12Device::CreateDescriptorHeap failed: 0x"
                + std::to_string(static_cast<unsigned long>(result)));
        }
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        count_ = count;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index = 0) const {
        if (index >= count_) {
            throw std::out_of_range("Descriptor heap CPU index is out of range.");
        }
        auto handle = heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * increment_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index = 0) const {
        if (index >= count_) {
            throw std::out_of_range("Descriptor heap GPU index is out of range.");
        }
        auto handle = heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * increment_;
        return handle;
    }

    ID3D12DescriptorHeap* get() const noexcept {
        return heap_.Get();
    }

    UINT size() const noexcept {
        return count_;
    }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT increment_ = 0;
    UINT count_ = 0;
};

} // namespace gmod::dx12
