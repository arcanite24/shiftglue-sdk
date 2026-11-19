/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/d3d11/d3d11_provider.h>

REXCVAR_DEFINE_BOOL(d3d11_debug, false, "UI/D3D11",
                    "Enable the Direct3D 11 debug layer (needs the Graphics Tools optional "
                    "feature; created without it if missing)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(d3d11_adapter, -1, "UI/D3D11",
                     "Index of the DXGI adapter to use (-1 for any physical, the "
                     "high-performance one first; -2 for WARP)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::ui::d3d11 {

namespace {

// Only 11_0 on purpose: one tested path (see d3d11_provider.h).
constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {D3D_FEATURE_LEVEL_11_0};

std::string WideToUtf8(const wchar_t* text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(size_t(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  return result;
}

}  // namespace

bool D3D11Provider::IsD3D11APIAvailable() {
  HMODULE library_d3d11 = LoadLibraryW(L"d3d11.dll");
  if (!library_d3d11) {
    return false;
  }
  bool available = GetProcAddress(library_d3d11, "D3D11CreateDevice") != nullptr;
  FreeLibrary(library_d3d11);
  return available;
}

std::unique_ptr<D3D11Provider> D3D11Provider::Create() {
  std::unique_ptr<D3D11Provider> provider(new D3D11Provider);
  if (!provider->Initialize()) {
    rex::FatalError(
        "Unable to initialize Direct3D 11 graphics subsystem.\n"
        "\n"
        "Ensure that you have the latest drivers for your GPU and it supports "
        "Direct3D 11 with the feature level 11_0.");
    return nullptr;
  }
  return provider;
}

D3D11Provider::~D3D11Provider() {
  if (immediate_context_ != nullptr) {
    // Not ClearState: nothing else may be using the context at this point, but
    // the owner of its state (the command processor) is responsible for it.
    immediate_context_->Flush();
  }
  if (multithread_ != nullptr) {
    multithread_->Release();
  }
  if (immediate_context_ != nullptr) {
    immediate_context_->Release();
  }
  if (device_ != nullptr) {
    if (debug_layer_enabled_) {
      Microsoft::WRL::ComPtr<ID3D11Debug> d3d11_debug;
      if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&d3d11_debug)))) {
        d3d11_debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
      }
    }
    device_->Release();
  }
  if (adapter_ != nullptr) {
    adapter_->Release();
  }
  if (dxgi_factory2_ != nullptr) {
    dxgi_factory2_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (debug_layer_enabled_ && pfn_dxgi_get_debug_interface1_) {
    Microsoft::WRL::ComPtr<IDXGIDebug> dxgi_debug;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_debug)))) {
      dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
  }

  if (library_d3d11_ != nullptr) {
    FreeLibrary(library_d3d11_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

bool D3D11Provider::Initialize() {
  // Load the core libraries.
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d11_ = LoadLibraryW(L"d3d11.dll");
  if (!library_dxgi_ || !library_d3d11_) {
    REXLOG_ERROR("Failed to load dxgi.dll or d3d11.dll");
    return false;
  }
  pfn_create_dxgi_factory1_ =
      PFNCreateDXGIFactory1(GetProcAddress(library_dxgi_, "CreateDXGIFactory1"));
  pfn_dxgi_get_debug_interface1_ =
      PFNDXGIGetDebugInterface1(GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"));
  pfn_d3d11_create_device_ =
      PFN_D3D11_CREATE_DEVICE(GetProcAddress(library_d3d11_, "D3D11CreateDevice"));
  if (!pfn_create_dxgi_factory1_ || !pfn_d3d11_create_device_) {
    REXLOG_ERROR("Failed to get CreateDXGIFactory1 or D3D11CreateDevice");
    return false;
  }

  // The DXGI factory. Not a debug factory: CreateDXGIFactory2 isn't on every
  // supported OS, and the D3D11 debug layer covers DXGI misuse it reports.
  IDXGIFactory1* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory1_(IID_PPV_ARGS(&dxgi_factory)))) {
    REXLOG_ERROR("Failed to create a DXGI 1.1 factory");
    return false;
  }
  dxgi_factory_ = dxgi_factory;
  if (FAILED(dxgi_factory_->QueryInterface(IID_PPV_ARGS(&dxgi_factory2_)))) {
    dxgi_factory2_ = nullptr;
  }

  auto adapter_supports_11_0 = [this](IDXGIAdapter1* adapter) {
    // With null device / context pointers, only checks the support.
    return SUCCEEDED(pfn_d3d11_create_device_(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                              kFeatureLevels, UINT(rex::countof(kFeatureLevels)),
                                              D3D11_SDK_VERSION, nullptr, nullptr, nullptr));
  };

  // Choose the adapter like the Direct3D 12 provider. Without a forced one,
  // the high-performance GPU first: EnumAdapters1 lists a laptop's integrated
  // GPU first.
  const int32_t adapter_cvar = REXCVAR_GET(d3d11_adapter);
  IDXGIAdapter1* adapter = nullptr;
  if (adapter_cvar == -1) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgi_factory6;
    if (SUCCEEDED(dxgi_factory_->QueryInterface(IID_PPV_ARGS(&dxgi_factory6)))) {
      for (UINT i = 0; dxgi_factory6->EnumAdapterByGpuPreference(
                           i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK;
           ++i) {
        DXGI_ADAPTER_DESC1 adapter_desc;
        if (SUCCEEDED(adapter->GetDesc1(&adapter_desc)) &&
            !(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && adapter_supports_11_0(adapter)) {
          break;
        }
        adapter->Release();
        adapter = nullptr;
      }
    }
  }
  for (UINT adapter_index = 0;
       adapter == nullptr && dxgi_factory_->EnumAdapters1(adapter_index, &adapter) == S_OK;
       ++adapter_index) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc)) && adapter_supports_11_0(adapter)) {
      bool is_software = (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      if (adapter_cvar >= 0) {
        if (adapter_index == UINT(adapter_cvar)) {
          break;
        }
      } else if (adapter_cvar == -2) {
        if (is_software) {
          break;
        }
      } else if (!is_software) {
        break;
      }
    }
    adapter->Release();
    adapter = nullptr;
  }
  if (adapter == nullptr) {
    REXLOG_ERROR(
        "Failed to get an adapter supporting Direct3D 11 with the feature level "
        "11_0 (d3d11_adapter {})",
        adapter_cvar);
    return false;
  }
  adapter_ = adapter;
  DXGI_ADAPTER_DESC1 adapter_desc;
  if (FAILED(adapter_->GetDesc1(&adapter_desc))) {
    REXLOG_ERROR("Failed to get the DXGI adapter description");
    return false;
  }
  adapter_vendor_id_ = GpuVendorID(adapter_desc.VendorId);
  adapter_device_id_ = adapter_desc.DeviceId;
  REXGPU_INFO("DXGI adapter: {} (vendor 0x{:04X}, device 0x{:04X})",
              WideToUtf8(adapter_desc.Description), adapter_desc.VendorId,
              adapter_desc.DeviceId);

  // Create the device. BGRA support for the B8G8R8A8 swap chain and UI.
  UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  bool debug = REXCVAR_GET(d3d11_debug);
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* immediate_context = nullptr;
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL(0);
  HRESULT create_result = E_FAIL;
  if (debug) {
    create_result = pfn_d3d11_create_device_(
        adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_flags | D3D11_CREATE_DEVICE_DEBUG,
        kFeatureLevels, UINT(rex::countof(kFeatureLevels)), D3D11_SDK_VERSION, &device,
        &feature_level, &immediate_context);
    if (FAILED(create_result)) {
      // DXGI_ERROR_SDK_COMPONENT_MISSING if the SDK layers aren't installed.
      REXLOG_WARN(
          "Failed to create the Direct3D 11 device with the debug layer (HRESULT 0x{:08X}), "
          "creating it without",
          static_cast<unsigned>(create_result));
      debug = false;
    }
  }
  if (!debug) {
    create_result = pfn_d3d11_create_device_(
        adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_flags, kFeatureLevels,
        UINT(rex::countof(kFeatureLevels)), D3D11_SDK_VERSION, &device, &feature_level,
        &immediate_context);
  }
  if (FAILED(create_result) || !device || !immediate_context) {
    REXLOG_ERROR("Failed to create a Direct3D 11 feature level 11_0 device (HRESULT 0x{:08X})",
                 static_cast<unsigned>(create_result));
    if (immediate_context) {
      immediate_context->Release();
    }
    if (device) {
      device->Release();
    }
    return false;
  }
  device_ = device;
  immediate_context_ = immediate_context;
  feature_level_ = feature_level;
  debug_layer_enabled_ = debug;

  // The immediate context is shared by the command processor thread and the
  // presenter (which only executes command lists and presents on it): make
  // each call atomic. See the threading contract in d3d11_provider.h.
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&multithread_)))) {
    REXLOG_ERROR("Failed to get ID3D10Multithread from the Direct3D 11 device");
    return false;
  }
  multithread_->SetMultithreadProtected(TRUE);

  if (debug_layer_enabled_) {
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> info_queue;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
      D3D11_MESSAGE_SEVERITY denied_severities[] = {
          D3D11_MESSAGE_SEVERITY_INFO,
      };
      D3D11_INFO_QUEUE_FILTER filter = {};
      filter.DenyList.NumSeverities = UINT(rex::countof(denied_severities));
      filter.DenyList.pSeverityList = denied_severities;
      info_queue->PushStorageFilter(&filter);
    }
  }

  // Device features.
  D3D11_FEATURE_DATA_THREADING threading = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading,
                                             sizeof(threading)))) {
    driver_command_lists_ = threading.DriverCommandLists != FALSE;
    driver_concurrent_creates_ = threading.DriverConcurrentCreates != FALSE;
  }

  // The guest output and the presenter's intermediate textures are
  // R10G10B10A2_UNORM render targets sampled by the presenter; FL 11_0
  // requires both, but fail loudly rather than black-screen on a bad driver.
  UINT guest_output_format_support = 0;
  device_->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &guest_output_format_support);
  constexpr UINT kGuestOutputRequiredSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                                               D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                                               D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
  if ((guest_output_format_support & kGuestOutputRequiredSupport) != kGuestOutputRequiredSupport) {
    REXLOG_ERROR(
        "The Direct3D 11 device doesn't support DXGI_FORMAT_R10G10B10A2_UNORM render targets "
        "with sampling (support 0x{:08X})",
        guest_output_format_support);
    return false;
  }
  guest_output_uav_store_supported_ = false;
  if (guest_output_format_support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 format_support2 = {};
    format_support2.InFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &format_support2,
                                               sizeof(format_support2)))) {
      guest_output_uav_store_supported_ =
          (format_support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    }
  }

  REXGPU_INFO(
      "Direct3D 11 device:\n"
      "* Feature level: {}_{}\n"
      "* Debug layer: {}\n"
      "* Driver command lists: {}\n"
      "* Driver concurrent creates: {}\n"
      "* R10G10B10A2_UNORM typed UAV store: {}\n"
      "* DXGI 1.2 factory: {}",
      (uint32_t(feature_level_) >> 12) & 0xF, (uint32_t(feature_level_) >> 8) & 0xF,
      debug_layer_enabled_ ? "yes" : "no", driver_command_lists_ ? "yes" : "no (emulated)",
      driver_concurrent_creates_ ? "yes" : "no", guest_output_uav_store_supported_ ? "yes" : "no",
      dxgi_factory2_ ? "yes" : "no");

  return true;
}

std::unique_ptr<Presenter> D3D11Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D11Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D11Provider::CreateImmediateDrawer() {
  return D3D11ImmediateDrawer::Create(*this);
}

}  // namespace rex::ui::d3d11
