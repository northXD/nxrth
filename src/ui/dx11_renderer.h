#pragma once

#include <string>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

struct GLFWwindow;
struct ImDrawData;

namespace nxrth::ui {

// DirectX 11 renderer with a WARP software fallback for RDP/headless VDS hosts.
class Dx11Renderer {
public:
    Dx11Renderer() = default;
    ~Dx11Renderer();

    Dx11Renderer(const Dx11Renderer&) = delete;
    Dx11Renderer& operator=(const Dx11Renderer&) = delete;

    bool initialize(GLFWwindow* window, std::string& error);
    void new_frame();
    void render(ImDrawData* draw_data, const float clear_color[4]);
    void shutdown();

    bool using_warp() const { return using_warp_; }

private:
    bool create_device_and_swap_chain(HWND hwnd,
                                      D3D_DRIVER_TYPE driver_type,
                                      std::string& error);
    bool create_render_target(std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    bool imgui_initialized_ = false;
    bool using_warp_ = false;
};

}  // namespace nxrth::ui
