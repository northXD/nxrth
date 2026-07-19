#include "ui/dx11_renderer.h"

#include <array>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui_impl_dx11.h"

namespace nxrth::ui {
namespace {

std::string hresult_text(HRESULT hr) {
    char message[512]{};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        message,
        static_cast<DWORD>(sizeof(message)),
        nullptr);

    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    if (length != 0) {
        std::string detail(message, length);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n'))
            detail.pop_back();
        out << " (" << detail << ')';
    }
    return out.str();
}

bool force_warp_from_environment() {
    const char* raw = std::getenv("NXRTH_FORCE_WARP");
    if (!raw || *raw == '\0') return false;
    const std::string_view value(raw);
    return value != "0" && value != "false" && value != "FALSE" && value != "off" &&
           value != "OFF" && value != "no" && value != "NO";
}

}  // namespace

Dx11Renderer::~Dx11Renderer() {
    shutdown();
}

bool Dx11Renderer::initialize(GLFWwindow* window, std::string& error) {
    if (!window) {
        error = "DirectX 11 failed to initialize: GLFW window is invalid.";
        return false;
    }

    const HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) {
        error = "DirectX 11 failed to initialize: could not obtain the Windows window handle.";
        return false;
    }

    const bool prefer_warp = force_warp_from_environment() ||
                             GetSystemMetrics(SM_REMOTESESSION) != 0;
    std::string first_error;

    if (prefer_warp) {
        if (create_device_and_swap_chain(hwnd, D3D_DRIVER_TYPE_WARP, error)) {
            using_warp_ = true;
        } else if (create_device_and_swap_chain(hwnd, D3D_DRIVER_TYPE_HARDWARE, first_error)) {
            using_warp_ = false;
        } else {
            error += "\nHardware renderer attempt also failed: " + first_error;
            return false;
        }
    } else {
        if (create_device_and_swap_chain(hwnd, D3D_DRIVER_TYPE_HARDWARE, error)) {
            using_warp_ = false;
        } else {
            first_error = error;
            if (create_device_and_swap_chain(hwnd, D3D_DRIVER_TYPE_WARP, error)) {
                using_warp_ = true;
            } else {
                error = "Hardware renderer failed: " + first_error +
                        "\nWARP software renderer failed: " + error;
                return false;
            }
        }
    }

    if (!create_render_target(error)) {
        shutdown();
        return false;
    }

    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        error = "ImGui DirectX 11 backend failed to initialize.";
        shutdown();
        return false;
    }
    imgui_initialized_ = true;
    return true;
}

bool Dx11Renderer::create_device_and_swap_chain(HWND hwnd,
                                                D3D_DRIVER_TYPE driver_type,
                                                std::string& error) {
    render_target_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array feature_levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_level{};

    auto create = [&](const D3D_FEATURE_LEVEL* levels, UINT level_count) {
        return D3D11CreateDeviceAndSwapChain(
            nullptr,
            driver_type,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            level_count,
            D3D11_SDK_VERSION,
            &desc,
            swap_chain_.ReleaseAndGetAddressOf(),
            device_.ReleaseAndGetAddressOf(),
            &selected_level,
            context_.ReleaseAndGetAddressOf());
    };

    HRESULT hr = create(feature_levels.data(), static_cast<UINT>(feature_levels.size()));
    if (hr == E_INVALIDARG) {
        swap_chain_.Reset();
        context_.Reset();
        device_.Reset();
        hr = create(feature_levels.data() + 1,
                    static_cast<UINT>(feature_levels.size() - 1));
    }
    if (FAILED(hr)) {
        const char* name = driver_type == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware";
        error = std::string("DirectX 11 ") + name + " device could not be created: " +
                hresult_text(hr);
        swap_chain_.Reset();
        context_.Reset();
        device_.Reset();
        return false;
    }
    return true;
}

bool Dx11Renderer::create_render_target(std::string& error) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(hr)) {
        error = "DirectX 11 back buffer could not be obtained: " + hresult_text(hr);
        return false;
    }

    hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                         render_target_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "DirectX 11 render target could not be created: " + hresult_text(hr);
        return false;
    }
    return true;
}

void Dx11Renderer::new_frame() {
    ImGui_ImplDX11_NewFrame();
}

void Dx11Renderer::render(ImDrawData* draw_data, const float clear_color[4]) {
    if (!context_ || !render_target_ || !swap_chain_) return;
    ID3D11RenderTargetView* target = render_target_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(render_target_.Get(), clear_color);
    ImGui_ImplDX11_RenderDrawData(draw_data);
    swap_chain_->Present(1, 0);
}

void Dx11Renderer::shutdown() {
    if (imgui_initialized_) {
        ImGui_ImplDX11_Shutdown();
        imgui_initialized_ = false;
    }
    render_target_.Reset();
    swap_chain_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
    using_warp_ = false;
}

}  // namespace nxrth::ui
