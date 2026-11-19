/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <memory>

#include <rex/ui/d3d11/d3d11_api.h>
#include <rex/ui/graphics_provider.h>

namespace rex::ui::d3d11 {

// Direct3D 11 graphics provider, feature level 11_0 only.
//
// The device is created with exactly one feature level in the requested
// array, D3D_FEATURE_LEVEL_11_0, on purpose: a single tested path for old
// GPUs (AMD TeraScale 2, Intel Ivy Bridge and later, NVIDIA Fermi and later).
// GetFeatureLevel() reports what the runtime actually returned.
//
// Threading contract:
// - ID3D11Device methods (resource, view, state and shader creation) are
//   free-threaded and may be called from any thread without locking.
// - The immediate context (GetImmediateContext()) is NOT thread-safe by
//   itself. The provider turns on ID3D10Multithread::SetMultithreadProtected
//   (TRUE), which makes every single immediate-context call atomic with
//   respect to other threads - but it does not make *bound state* private:
//   pipeline state set by one thread is seen by the next call from another.
// - The owner of the immediate context's pipeline state is the GPU command
//   processor thread. No other code binds state on it. The presenter records
//   all of its painting on its own deferred context and replays it with
//   ExecuteCommandList(list, TRUE) (one call, restoring the command
//   processor's state afterwards).
// - A sequence of immediate-context calls that must not interleave with
//   another thread's (for example CopyResource then Map(READ) of a staging
//   texture, or any multi-call sequence of the command processor that depends
//   on state another thread could touch) is wrapped in a ContextLock, which is
//   ID3D10Multithread::Enter / Leave - the same recursive critical section the
//   runtime takes inside each call. Keep such sections short; never hold one
//   across IDXGISwapChain::Present or a blocking wait.
class D3D11Provider : public GraphicsProvider {
 public:
  // RAII lock of the immediate context (ID3D10Multithread::Enter / Leave).
  // Recursive, so nesting it, or calling context methods while holding it, is
  // fine.
  class ContextLock {
   public:
    explicit ContextLock(ID3D10Multithread* multithread) : multithread_(multithread) {
      if (multithread_) {
        multithread_->Enter();
      }
    }
    ~ContextLock() {
      if (multithread_) {
        multithread_->Leave();
      }
    }
    ContextLock(const ContextLock&) = delete;
    ContextLock& operator=(const ContextLock&) = delete;

   private:
    ID3D10Multithread* multithread_;
  };

  ~D3D11Provider();

  static bool IsD3D11APIAvailable();

  static std::unique_ptr<D3D11Provider> Create();

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  // DXGI 1.1 factory (always available); the 1.2 factory is nullptr on DXGI
  // older than 1.2 (Windows 7 without the platform update).
  IDXGIFactory1* GetDXGIFactory() const { return dxgi_factory_; }
  IDXGIFactory2* GetDXGIFactory2() const { return dxgi_factory2_; }
  IDXGIAdapter1* GetAdapter() const { return adapter_; }
  ID3D11Device* GetDevice() const { return device_; }
  // See the threading contract above.
  ID3D11DeviceContext* GetImmediateContext() const { return immediate_context_; }
  ID3D10Multithread* GetMultithread() const { return multithread_; }
  [[nodiscard]] ContextLock LockImmediateContext() const { return ContextLock(multithread_); }

  // Adapter info.
  GpuVendorID GetAdapterVendorID() const { return adapter_vendor_id_; }
  uint32_t GetAdapterDeviceID() const { return adapter_device_id_; }
  // The level D3D11CreateDevice returned (always 11_0, as only 11_0 is
  // requested).
  D3D_FEATURE_LEVEL GetFeatureLevel() const { return feature_level_; }
  bool IsDebugLayerEnabled() const { return debug_layer_enabled_; }

  // Device features.
  // Whether the driver executes deferred command lists natively (otherwise
  // the runtime emulates them, which still works).
  bool AreDriverCommandListsSupported() const { return driver_command_lists_; }
  bool AreDriverConcurrentCreatesSupported() const { return driver_concurrent_creates_; }
  // Whether the guest output format (DXGI_FORMAT_R10G10B10A2_UNORM) supports
  // typed UAV stores (D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW and
  // D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE). Render target and shader
  // resource support is required at initialization.
  bool IsGuestOutputFormatUAVStoreSupported() const { return guest_output_uav_store_supported_; }

 private:
  D3D11Provider() = default;

  bool Initialize();

  typedef HRESULT(WINAPI* PFNCreateDXGIFactory1)(REFIID riid, _COM_Outptr_ void** ppFactory);
  typedef HRESULT(WINAPI* PFNDXGIGetDebugInterface1)(UINT Flags, REFIID riid,
                                                     _COM_Outptr_ void** pDebug);

  HMODULE library_dxgi_ = nullptr;
  PFNCreateDXGIFactory1 pfn_create_dxgi_factory1_ = nullptr;
  // DXGI 1.3+, may be nullptr.
  PFNDXGIGetDebugInterface1 pfn_dxgi_get_debug_interface1_ = nullptr;

  HMODULE library_d3d11_ = nullptr;
  PFN_D3D11_CREATE_DEVICE pfn_d3d11_create_device_ = nullptr;

  IDXGIFactory1* dxgi_factory_ = nullptr;
  IDXGIFactory2* dxgi_factory2_ = nullptr;
  IDXGIAdapter1* adapter_ = nullptr;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* immediate_context_ = nullptr;
  ID3D10Multithread* multithread_ = nullptr;

  GpuVendorID adapter_vendor_id_ = GpuVendorID(0);
  uint32_t adapter_device_id_ = 0;
  D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL(0);
  bool debug_layer_enabled_ = false;

  bool driver_command_lists_ = false;
  bool driver_concurrent_creates_ = false;
  bool guest_output_uav_store_supported_ = false;
};

}  // namespace rex::ui::d3d11
