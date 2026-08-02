#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <iostream>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "DataSource.h"
#include "DashboardUI.h"
#include "TelemetrySender.h"
#include "VirtualLogRecorder.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

// --- DirectX Global Objects ---
static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static IDXGISwapChain*          g_pSwapChain = NULL;
static ID3D11RenderTargetView*  g_mainRenderTargetView = NULL;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- Main Application ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("AC Remote Dashboard"), NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindow(wc.lpszClassName, _T("DJY EV Telemetry - CMake ImGui Dashboard"), WS_OVERLAPPEDWINDOW, 80, 50, 1400, 900, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hWnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    
    ShowWindow(hWnd, SW_SHOWDEFAULT); 
    UpdateWindow(hWnd);

    IMGUI_CHECKVERSION(); 
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    
    ImGui_ImplWin32_Init(hWnd); 
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialization of Application Logic
    LocalMemorySource localSource;
    DemoSource demoSource;
    DashboardUI dashboardUI;
    TelemetrySender telemetrySender;
    VirtualLogRecorder virtualRecorder;
    virtualRecorder.Start();
    dashboardUI.SetVirtualRecorder(&virtualRecorder);
    
    // We don't need to send data from this dashboard anymore, it's a receiver.
    // telemetrySender.Init("127.0.0.1", 8001);
    dashboardUI.enableWebBroadcast = false; // Default OFF for Pit Wall
    
    bool demoMode = false;
    bool done = false;

    while (!done) {
        MSG msg; 
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) { 
            TranslateMessage(&msg); 
            DispatchMessage(&msg); 
            if (msg.message == WM_QUIT) done = true; 
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame(); 
        ImGui_ImplWin32_NewFrame(); 
        ImGui::NewFrame();

        // Update Data Source
        IDataSource* activeSource = &localSource;
        if (demoMode) {
            activeSource = &demoSource;
        }
        
        activeSource->Update(io.DeltaTime);

        // Render UI
        dashboardUI.Render(activeSource, demoMode);
        
        // Broadcast Data if enabled
        if (dashboardUI.enableWebBroadcast && activeSource->IsConnected()) {
            telemetrySender.SendData(activeSource->GetPhysics(), activeSource->GetGraphics(), activeSource->GetStatic());
        }

        // Rendering DirectX
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0, 0, 0, 1 };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // --- Proper Cleanup ---
    virtualRecorder.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd; 
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2; 
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; 
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; 
    sd.OutputWindow = hWnd; 
    sd.SampleDesc.Count = 1; 
    sd.Windowed = TRUE; 
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    D3D_FEATURE_LEVEL fl;
    HRESULT res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    
    CreateRenderTarget(); 
    return true;
}

void CleanupDeviceD3D() { 
    CleanupRenderTarget(); 
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

void CreateRenderTarget() { 
    ID3D11Texture2D* pBackBuffer; 
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)); 
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView); 
    pBackBuffer->Release(); 
}

void CleanupRenderTarget() { 
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE: 
            if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) { 
                CleanupRenderTarget(); 
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0); 
                CreateRenderTarget(); 
            } 
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // Disable ALT application menu
            break;
        case WM_DESTROY: 
            PostQuitMessage(0); 
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
