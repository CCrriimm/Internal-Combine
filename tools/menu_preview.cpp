// Render the real menu code without loading the game, hooks, IPC or user config.
#define CIYE_MENU_PREVIEW
#include "../src/overlay.cpp"
#include <wincodec.h>
#include <filesystem>
#include <iostream>

static unsigned key_messages=0;
static LRESULT CALLBACK KeyboardGameProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_KEYDOWN||message==WM_KEYUP||message==WM_CHAR||message==WM_SYSKEYDOWN||
       message==WM_SYSKEYUP||message==WM_INPUT) ++key_messages;
    return DefWindowProcW(h,message,w,l);
}
static int CheckKeyboardInput() {
    WNDCLASSW cls{};cls.lpfnWndProc=KeyboardGameProc;cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"Comet.KeyboardCheck";RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,L"Keyboard check",WS_POPUP,-10000,-10000,640,480,
                              nullptr,nullptr,cls.hInstance,nullptr);
    if(!window) return 2;
    ImGui::CreateContext();ImGui_ImplWin32_Init(window);DefaultCfg();g_imgui_ready=true;g_wndproc=KeyboardGameProc;
    auto require=[](bool value) {if(!value) throw 10;};
    auto send=[&](UINT m,WPARAM w) {SendMessageW(window,m,w,0);};
    try {
        g_hwnd=window;
        auto& io=ImGui::GetIO();
        for(bool menu : {false,true}) {
            SetF(F_MENU_OPEN,menu);EnsureWndProc();io.WantTextInput=false;io.WantCaptureKeyboard=true;
            require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==
                    (menu?HookWndProc:KeyboardGameProc));
            key_messages=0;send(WM_KEYDOWN,'W');send(WM_KEYUP,'W');send(WM_KEYDOWN,VK_OEM_3);send(WM_KEYUP,VK_OEM_3);
            require(key_messages==4); // Menu navigation focus alone must not block game keys.
        }
        SetF(F_MENU_OPEN,true);EnsureWndProc();io.WantTextInput=true;
        key_messages=0;send(WM_KEYDOWN,'A');send(WM_CHAR,'a');require(key_messages==0);
        send(WM_KEYUP,'A');require(key_messages==1); // Release a key held before editing started.
        io.WantTextInput=false;g_bind_slot=1;g_bind_armed=true;
        key_messages=0;send(WM_KEYDOWN,'B');require(key_messages==0);
        send(WM_KEYUP,'B');require(key_messages==1);
        g_bind_slot=0;g_bind_armed=false;
        SetF(F_MENU_OPEN,true);key_messages=0;send(WM_KEYDOWN,VK_ESCAPE);
        require(!Has(F_MENU_OPEN)&&key_messages==0);EnsureWndProc();
        require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==KeyboardGameProc);
        send(WM_KEYUP,VK_ESCAPE);send(WM_KEYDOWN,VK_ESCAPE);require(key_messages==2);
        io.WantTextInput=true;io.WantCaptureKeyboard=true;
        key_messages=0;send(WM_KEYDOWN,'W');send(WM_KEYUP,'W');send(WM_KEYDOWN,VK_OEM_3);require(key_messages==3);
        for(int cycle=0;cycle<5;++cycle) {
            SetF(F_MENU_OPEN,true);EnsureWndProc();require(g_input_attached);
            SetF(F_MENU_OPEN,false);EnsureWndProc();require(!g_input_attached);
            require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==KeyboardGameProc);
            key_messages=0;send(WM_KEYDOWN,'W');send(WM_KEYUP,'W');require(key_messages==2);
        }
        ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();DestroyWindow(window);
        UnregisterClassW(cls.lpszClassName,cls.hInstance);
        std::cout<<"Keyboard routing checks passed: actual window chain restored on close, repeated open/close, game keys, text/keybind capture, releases and Escape/backtick.\n";
        return 0;
    } catch(...) {std::cerr<<"Keyboard routing check failed\n";return 3;}
}

static void CaptureMenu(IDXGISwapChain* swap, ID3D11Device* device, ID3D11DeviceContext* context,
                        const std::filesystem::path& path) {
    ID3D11Texture2D *back=nullptr,*staging=nullptr;
    if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back)))) throw 1;
    D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&staging))) throw 2;
    context->CopyResource(staging,back);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) throw 3;
    IWICImagingFactory* factory=nullptr;IWICStream* stream=nullptr;
    IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) throw 4;
    factory->CreateStream(&stream);stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);
    factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);encoder->Initialize(stream,WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame,nullptr);frame->Initialize(nullptr);frame->SetSize(d.Width,d.Height);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;frame->SetPixelFormat(&format);
    std::vector<BYTE> pixels(mapped.RowPitch*d.Height);
    memcpy(pixels.data(),mapped.pData,pixels.size());
    for(UINT y=0;y<d.Height;++y) for(UINT x=0;x<d.Width;++x)
        std::swap(pixels[y*mapped.RowPitch+x*4],pixels[y*mapped.RowPitch+x*4+2]);
    if(FAILED(frame->WritePixels(d.Height,mapped.RowPitch,static_cast<UINT>(pixels.size()),pixels.data()))) throw 5;
    frame->Commit();encoder->Commit();frame->Release();encoder->Release();stream->Release();factory->Release();
    context->Unmap(staging,0);staging->Release();back->Release();
}

int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--check-keyboard") return CheckKeyboardInput();
    try {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const std::filesystem::path output=argc>1?argv[1]:"menu-preview";
        std::filesystem::create_directories(output);
        wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
        std::filesystem::create_directories(std::filesystem::path(executable).parent_path()/"ipc");
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"CometImGuiPreview";
        RegisterClassW(&wc);
        HWND window=CreateWindowW(wc.lpszClassName,L"Comet ImGui Preview",WS_POPUP,0,0,1200,900,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=1200;desc.BufferDesc.Height=900;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* swap=nullptr;ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
        if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                                              &desc,&swap,&device,nullptr,&context))) return 2;
        ID3D11Texture2D* back=nullptr;ID3D11RenderTargetView* rtv=nullptr;
        swap->GetBuffer(0,IID_PPV_ARGS(&back));device->CreateRenderTargetView(back,nullptr,&rtv);back->Release();
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplWin32_Init(window);ImGui_ImplDX11_Init(device,context);
        InitFonts();DefaultCfg();g_cfg.reserved|=OPT_AIM_NECK|OPT_AIM_BODY;
        if(argc>2&&std::string(argv[2])=="--silent") g_cfg.reserved|=OPT_AIM_SILENT;
        ApplyMenuPreferences();g_cfg_ready=false;
        g_catalog_ready=true;g_inventory_loaded=true;g_inventory_exists=true;g_inventory_writable=false;
        g_heroes={{101,1,3001010001,0,"NOVA - Radiant"},{113,1,3001130002,0,"ZERO - Scarlet"}};
        g_hero_ids={101,113};g_hero_skins[101]={0};g_hero_skins[113]={1};
        g_guns={{11199003,1,4,1,"M4A1 - Lava [Lv 3]"},{14199012,10,7,1,"KAG-6 - Inkforge [Lv 4]"},
                {11000015,20102,6,2,"Chainblade - Chainflux"}};
        g_gun_skins[1]={0};g_gun_skins[10]={1};g_gun_skins[20102]={2};
        g_weapon_ids={1,10};g_knife_ids={20102};g_gun_label[1]="M4A1";g_gun_label[10]="KAG-6";
        g_skin_inventory={{1,101,3001010001,1,"NOVA - Radiant"},{2,1,11199003,1,"M4A1 - Lava [Lv 3]"}};
        const char* names[]={"visuals","aim","skins","settings"};
        for(int compact=0;compact<2;++compact) for(int page=0;page<4;++page) {
            g_tab=page;
            const ImVec2 size=compact?ImVec2(820,560):ImVec2(960,680);
            for(int frame=0;frame<50;++frame) {
                ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();io.DisplaySize=ImVec2(1200,900);io.DeltaTime=1.f/60;
                ImGui::NewFrame();
                if(ImGui::FindWindowByName("##comet_main")) {
                    ImGui::SetWindowSize("##comet_main",size);
                    ImGui::SetWindowPos("##comet_main",ImVec2((1200-size.x)*.5f,(900-size.y)*.5f));
                }
                DrawMenu();ImGui::Render();
                const float clear[]={.045f,.055f,.073f,1};context->OMSetRenderTargets(1,&rtv,nullptr);
                context->ClearRenderTargetView(rtv,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            CaptureMenu(swap,device,context,output/(std::string(names[page])+(compact?"-compact.png":".png")));
        }
        ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
        rtv->Release();swap->Release();context->Release();device->Release();DestroyWindow(window);CoUninitialize();
        std::cout<<"Rendered 8 real ImGui menu previews without game/D2D.\n";
        return 0;
    } catch(...) {std::cerr<<"Menu preview capture failed.\n";return 1;}
}
