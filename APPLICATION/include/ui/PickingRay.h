#pragma once

#include <DirectXMath.h>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace gmod::ui {

struct PickingRay {
    std::array<float, 3> origin;
    std::array<float, 3> direction;
};

// Inverts the rendered view (including a scope view) into a Source-space ray.
inline std::optional<PickingRay> picking_ray(const float* view_projection,
                                            int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return std::nullopt;
    DirectX::XMFLOAT4X4 matrix;
    std::memcpy(&matrix, view_projection, sizeof(matrix));
    DirectX::XMVECTOR determinant;
    const auto inverse = DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&matrix));
    const float det = DirectX::XMVectorGetX(determinant);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float nx = 2.0f * static_cast<float>(x) / width - 1.0f;
    const float ny = 1.0f - 2.0f * static_cast<float>(y) / height;
    DirectX::XMFLOAT3 near_point, far_point;
    DirectX::XMStoreFloat3(&near_point, DirectX::XMVector3TransformCoord(
        DirectX::XMVectorSet(nx, ny, 0, 1), inverse));
    DirectX::XMStoreFloat3(&far_point, DirectX::XMVector3TransformCoord(
        DirectX::XMVectorSet(nx, ny, 0.5f, 1), inverse));
    PickingRay ray{{-near_point.x, -near_point.z, near_point.y},
                   {near_point.x - far_point.x, near_point.z - far_point.z, far_point.y - near_point.y}};
    float length = 0;
    for (float value : ray.direction) length += value * value;
    length = std::sqrt(length);
    if (!std::isfinite(length) || length < 1e-6f) return std::nullopt;
    for (float& value : ray.direction) value /= length;
    return ray;
}

} // namespace gmod::ui
