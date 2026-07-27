#include "std_include.hpp"
#include "d3d9ex.hpp"
#include "../d3d9_proxy.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "renderer.hpp"
#include "tracer.hpp"
#include "skinning.hpp"
#include "shared/common/shader_cache.hpp"
#include "shared/common/ps_reflect.hpp"

using comp::tracer;
#include "tracer_dispatch.inc"
#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"

namespace comp
{
#pragma region D3D9Device

	HRESULT d3d9ex::D3D9Device::QueryInterface(REFIID riid, void** ppvObj)
	{
		TRACE_IF_ACTIVE(trace_QueryInterface, &riid, ppvObj);
		if (!ppvObj) return E_POINTER;
		*ppvObj = nullptr;

		// Only substitute the proxy for interfaces whose vtable we actually
		// implement. The old code returned `this` for ANY riid the real device
		// accepted, so a caller asking for something we don't implement got a
		// D3D9Device under a foreign vtable contract and crashed on first use.
		const bool wants_device9   = (riid == IID_IDirect3DDevice9) || (riid == IID_IUnknown);
		const bool wants_device9ex = (riid == IID_IDirect3DDevice9Ex);

		if (wants_device9 || (wants_device9ex && m_pIDirect3DDevice9Ex))
		{
			// Reference counting runs through the inner device (see AddRef /
			// Release), so take the ref there to keep the pairing balanced.
			m_pIDirect3DDevice9->AddRef();
			*ppvObj = wants_device9ex ? static_cast<void*>(static_cast<IDirect3DDevice9Ex*>(this))
			                          : static_cast<void*>(static_cast<IDirect3DDevice9*>(this));
			return S_OK;
		}

		// Everything else (including IID_IDirect3DDevice9Ex on a non-Ex device):
		// let the real device answer, and hand back whatever it returns unaltered.
		return m_pIDirect3DDevice9->QueryInterface(riid, ppvObj);
	}

	ULONG d3d9ex::D3D9Device::AddRef()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_AddRef);
		return m_pIDirect3DDevice9->AddRef();
	}

	ULONG d3d9ex::D3D9Device::Release()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_Release);
		ULONG count = m_pIDirect3DDevice9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT d3d9ex::D3D9Device::TestCooperativeLevel()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_TestCooperativeLevel);
		return m_pIDirect3DDevice9->TestCooperativeLevel();
	}

	UINT d3d9ex::D3D9Device::GetAvailableTextureMem()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetAvailableTextureMem);
		return m_pIDirect3DDevice9->GetAvailableTextureMem();
	}

	HRESULT d3d9ex::D3D9Device::EvictManagedResources()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_EvictManagedResources);
		return m_pIDirect3DDevice9->EvictManagedResources();
	}

	HRESULT d3d9ex::D3D9Device::GetDirect3D(IDirect3D9** ppD3D9)
	{
		TRACE_IF_ACTIVE(trace_GetDirect3D, ppD3D9);
		return m_pIDirect3DDevice9->GetDirect3D(ppD3D9);
	}

	HRESULT d3d9ex::D3D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
	{
		TRACE_IF_ACTIVE(trace_GetDeviceCaps, pCaps);
		return m_pIDirect3DDevice9->GetDeviceCaps(pCaps);
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
	{
		TRACE_IF_ACTIVE(trace_GetDisplayMode, iSwapChain, pMode);
		return m_pIDirect3DDevice9->GetDisplayMode(iSwapChain, pMode);
	}

	HRESULT d3d9ex::D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
	{
		TRACE_IF_ACTIVE(trace_GetCreationParameters, pParameters);
		return m_pIDirect3DDevice9->GetCreationParameters(pParameters);
	}

	HRESULT d3d9ex::D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
	{
		TRACE_IF_ACTIVE(trace_SetCursorProperties, XHotSpot, YHotSpot, pCursorBitmap);
		return m_pIDirect3DDevice9->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
	}

	void d3d9ex::D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags)
	{
		TRACE_IF_ACTIVE(trace_SetCursorPosition, X, Y, Flags);
		return m_pIDirect3DDevice9->SetCursorPosition(X, Y, Flags);
	}

	BOOL d3d9ex::D3D9Device::ShowCursor(BOOL bShow)
	{
		TRACE_IF_ACTIVE(trace_ShowCursor, bShow);
		return m_pIDirect3DDevice9->ShowCursor(bShow);
	}

	HRESULT d3d9ex::D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_CreateAdditionalSwapChain, pPresentationParameters, pSwapChain);
		return m_pIDirect3DDevice9->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_GetSwapChain, iSwapChain, pSwapChain);
		return m_pIDirect3DDevice9->GetSwapChain(iSwapChain, pSwapChain);
	}

	UINT d3d9ex::D3D9Device::GetNumberOfSwapChains()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNumberOfSwapChains);
		return m_pIDirect3DDevice9->GetNumberOfSwapChains();
	}

	void d3d9ex::D3D9Device::pre_reset()
	{
		if (auto* t = tracer::get()) t->on_reset();
		shared::common::ffp_state::get().on_reset();
		game::on_reset();
		if (auto* s = skinning::get()) s->on_reset();
		renderer_on_reset();
		shared::common::g_shader_cache.clear_cache();
		tex_addons::init_texture_addons(true);
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}

	void d3d9ex::D3D9Device::post_reset(HRESULT hr)
	{
		// Reset commonly returns D3DERR_DEVICELOST while the device is still
		// unavailable; the game retries next frame. Recreating device objects
		// against a device that is still lost fails and, for the ImGui backend,
		// leaves it believing it owns objects it doesn't.
		if (FAILED(hr)) return;
		tex_addons::init_texture_addons();
		ImGui_ImplDX9_CreateDeviceObjects();
	}

	HRESULT d3d9ex::D3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
	{
		PROFILE_ZONE_N("d3d9::Reset");
		TRACE_IF_ACTIVE(trace_Reset, pPresentationParameters);
		pre_reset();
		const auto hr = m_pIDirect3DDevice9->Reset(pPresentationParameters);
		post_reset(hr);
		return hr;
	}

	void d3d9ex::D3D9Device::pre_present()
	{
		{
			auto& ffp = shared::common::ffp_state::get();
			if (auto* d = diagnostics::get())
				d->on_present(ffp.frame_count(), ffp.draw_call_count(), ffp.scene_count());

			// Emit Tracy plots BEFORE ffp_state::on_present(), which zeros the counters.
			PROFILE_PLOT_I64("DrawCalls/frame", ffp.draw_call_count());
			PROFILE_PLOT_I64("Scenes/frame",    ffp.scene_count());
			if (auto* s = skinning::get())
				PROFILE_PLOT_I64("Skin expand/frame", s->expansions_this_frame());
		}
		if (auto* s = skinning::get()) s->on_present();
		shared::common::ffp_state::get().on_present();
		game::lights_updated_frame = false;
		game::on_frame_end();
	}

	void d3d9ex::D3D9Device::post_present()
	{
		if (auto* t = tracer::get()) t->on_present();
		PROFILE_FRAME_MARK();
	}

	HRESULT d3d9ex::D3D9Device::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
	{
		PROFILE_ZONE_N("Present");
		TRACE_IF_ACTIVE(trace_Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		pre_present();
		auto hr = m_pIDirect3DDevice9->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		post_present();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
	{
		TRACE_IF_ACTIVE(trace_GetBackBuffer, iSwapChain, iBackBuffer, Type, ppBackBuffer);
		return m_pIDirect3DDevice9->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
	}

	HRESULT d3d9ex::D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
	{
		TRACE_IF_ACTIVE(trace_GetRasterStatus, iSwapChain, pRasterStatus);
		return m_pIDirect3DDevice9->GetRasterStatus(iSwapChain, pRasterStatus);
	}

	HRESULT d3d9ex::D3D9Device::SetDialogBoxMode(BOOL bEnableDialogs)
	{
		TRACE_IF_ACTIVE(trace_SetDialogBoxMode, bEnableDialogs);
		return m_pIDirect3DDevice9->SetDialogBoxMode(bEnableDialogs);
	}

	void d3d9ex::D3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_SetGammaRamp, iSwapChain, Flags, pRamp);
		return m_pIDirect3DDevice9->SetGammaRamp(iSwapChain, Flags, pRamp);
	}

	void d3d9ex::D3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_GetGammaRamp, iSwapChain, pRamp);
		return m_pIDirect3DDevice9->GetGammaRamp(iSwapChain, pRamp);
	}

	HRESULT d3d9ex::D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateTexture");
		TRACE_IF_ACTIVE(trace_CreateTexture, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		return m_pIDirect3DDevice9->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateVolumeTexture");
		TRACE_IF_ACTIVE(trace_CreateVolumeTexture, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
		return m_pIDirect3DDevice9->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateCubeTexture");
		TRACE_IF_ACTIVE(trace_CreateCubeTexture, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		return m_pIDirect3DDevice9->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexBuffer");
		TRACE_IF_ACTIVE(trace_CreateVertexBuffer, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		return m_pIDirect3DDevice9->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateIndexBuffer");
		TRACE_IF_ACTIVE(trace_CreateIndexBuffer, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		return m_pIDirect3DDevice9->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateRenderTarget");
		TRACE_IF_ACTIVE(trace_CreateRenderTarget, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateDepthStencilSurface");
		TRACE_IF_ACTIVE(trace_CreateDepthStencilSurface, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
	{
		PROFILE_ZONE_N("d3d9::UpdateSurface");
		TRACE_IF_ACTIVE(trace_UpdateSurface, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
		return m_pIDirect3DDevice9->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
	}

	HRESULT d3d9ex::D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
	{
		PROFILE_ZONE_N("d3d9::UpdateTexture");
		TRACE_IF_ACTIVE(trace_UpdateTexture, pSourceTexture, pDestinationTexture);
		return m_pIDirect3DDevice9->UpdateTexture(pSourceTexture, pDestinationTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
	{
		PROFILE_ZONE_N("d3d9::GetRenderTargetData");
		TRACE_IF_ACTIVE(trace_GetRenderTargetData, pRenderTarget, pDestSurface);
		return m_pIDirect3DDevice9->GetRenderTargetData(pRenderTarget, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
	{
		PROFILE_ZONE_N("d3d9::GetFrontBufferData");
		TRACE_IF_ACTIVE(trace_GetFrontBufferData, iSwapChain, pDestSurface);
		return m_pIDirect3DDevice9->GetFrontBufferData(iSwapChain, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
	{
		PROFILE_ZONE_N("d3d9::StretchRect");
		TRACE_IF_ACTIVE(trace_StretchRect, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
		return m_pIDirect3DDevice9->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
	}

	HRESULT d3d9ex::D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
	{
		PROFILE_ZONE_N("d3d9::ColorFill");
		TRACE_IF_ACTIVE(trace_ColorFill, pSurface, pRect, color);
		return m_pIDirect3DDevice9->ColorFill(pSurface, pRect, color);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		PROFILE_ZONE_N("d3d9::CreateOffscreenPlainSurface");
		TRACE_IF_ACTIVE(trace_CreateOffscreenPlainSurface, Width, Height, Format, Pool, ppSurface, pSharedHandle);
		return m_pIDirect3DDevice9->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
	{
		PROFILE_ZONE_N("d3d9::SetRenderTarget");
		TRACE_IF_ACTIVE(trace_SetRenderTarget, RenderTargetIndex, pRenderTarget);
		game::on_set_render_target(m_pIDirect3DDevice9, RenderTargetIndex, pRenderTarget);
		return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, pRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
	{
		TRACE_IF_ACTIVE(trace_GetRenderTarget, RenderTargetIndex, ppRenderTarget);
		return m_pIDirect3DDevice9->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
	{
		PROFILE_ZONE_N("d3d9::SetDepthStencilSurface");
		TRACE_IF_ACTIVE(trace_SetDepthStencilSurface, pNewZStencil);
		return m_pIDirect3DDevice9->SetDepthStencilSurface(pNewZStencil);
	}

	HRESULT d3d9ex::D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
	{
		TRACE_IF_ACTIVE(trace_GetDepthStencilSurface, ppZStencilSurface);
		return m_pIDirect3DDevice9->GetDepthStencilSurface(ppZStencilSurface);
	}

	HRESULT d3d9ex::D3D9Device::BeginScene()
	{
		PROFILE_ZONE_N("d3d9::BeginScene");
		TRACE_IF_ACTIVE_NOARGS(trace_BeginScene);
		shared::common::ffp_state::get().on_begin_scene();

		if (renderer::is_initialized()) {
			on_begin_scene_cb();
		}

		return m_pIDirect3DDevice9->BeginScene();
	}

	HRESULT d3d9ex::D3D9Device::EndScene()
	{
		PROFILE_ZONE_N("EndScene");
		TRACE_IF_ACTIVE_NOARGS(trace_EndScene);
		if (imgui::is_initialized()) {
			imgui::get()->on_present();
		}

		return m_pIDirect3DDevice9->EndScene();
	}

	HRESULT d3d9ex::D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
	{
		PROFILE_ZONE_N("d3d9::Clear");
		TRACE_IF_ACTIVE(trace_Clear, Count, pRects, Flags, Color, Z, Stencil);
		// example
		// g_is_doing_something_special = shared::utils::float_equal(Z, 0.1337f);

		return m_pIDirect3DDevice9->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}

	HRESULT d3d9ex::D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		PROFILE_ZONE_N("d3d9::SetTransform");
		TRACE_IF_ACTIVE(trace_SetTransform, State, pMatrix);
		return m_pIDirect3DDevice9->SetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_GetTransform, State, pMatrix);
		return m_pIDirect3DDevice9->GetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_MultiplyTransform, State, pMatrix);
		return m_pIDirect3DDevice9->MultiplyTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
	{
		PROFILE_ZONE_N("d3d9::SetViewport");
		TRACE_IF_ACTIVE(trace_SetViewport, pViewport);
		return m_pIDirect3DDevice9->SetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::GetViewport(D3DVIEWPORT9* pViewport)
	{
		TRACE_IF_ACTIVE(trace_GetViewport, pViewport);
		return m_pIDirect3DDevice9->GetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
	{
		PROFILE_ZONE_N("d3d9::SetMaterial");
		TRACE_IF_ACTIVE(trace_SetMaterial, pMaterial);
		return m_pIDirect3DDevice9->SetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
	{
		TRACE_IF_ACTIVE(trace_GetMaterial, pMaterial);
		return m_pIDirect3DDevice9->GetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
	{
		PROFILE_ZONE_N("d3d9::SetLight");
		TRACE_IF_ACTIVE(trace_SetLight, Index, pLight);
		return m_pIDirect3DDevice9->SetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight)
	{
		TRACE_IF_ACTIVE(trace_GetLight, Index, pLight);
		return m_pIDirect3DDevice9->GetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::LightEnable(DWORD Index, BOOL Enable)
	{
		PROFILE_ZONE_N("d3d9::LightEnable");
		TRACE_IF_ACTIVE(trace_LightEnable, Index, Enable);
		return m_pIDirect3DDevice9->LightEnable(Index, Enable);
	}

	HRESULT d3d9ex::D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable)
	{
		TRACE_IF_ACTIVE(trace_GetLightEnable, Index, pEnable);
		return m_pIDirect3DDevice9->GetLightEnable(Index, pEnable);
	}

	HRESULT d3d9ex::D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_SetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->SetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::GetClipPlane(DWORD Index, float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_GetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->GetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetRenderState");
		TRACE_IF_ACTIVE(trace_SetRenderState, State, Value);
		return m_pIDirect3DDevice9->SetRenderState(State, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetRenderState, State, pValue);
		return m_pIDirect3DDevice9->GetRenderState(State, pValue);
	}

	HRESULT d3d9ex::D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
	{
		PROFILE_ZONE_N("d3d9::CreateStateBlock");
		TRACE_IF_ACTIVE(trace_CreateStateBlock, Type, ppSB);
		return m_pIDirect3DDevice9->CreateStateBlock(Type, ppSB);
	}

	HRESULT d3d9ex::D3D9Device::BeginStateBlock()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_BeginStateBlock);
		return m_pIDirect3DDevice9->BeginStateBlock();
	}

	HRESULT d3d9ex::D3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
	{
		TRACE_IF_ACTIVE(trace_EndStateBlock, ppSB);
		return m_pIDirect3DDevice9->EndStateBlock(ppSB);
	}

	HRESULT d3d9ex::D3D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_SetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->SetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_GetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->GetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
	{
		TRACE_IF_ACTIVE(trace_GetTexture, Stage, ppTexture);
		return m_pIDirect3DDevice9->GetTexture(Stage, ppTexture);
	}

	HRESULT d3d9ex::D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
	{
		PROFILE_ZONE_N("d3d9::SetTexture");
		TRACE_IF_ACTIVE(trace_SetTexture, Stage, pTexture);
		shared::common::ffp_state::get().on_set_texture(Stage, pTexture);
		return m_pIDirect3DDevice9->SetTexture(Stage, pTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetTextureStageState, Stage, Type, pValue);
		return m_pIDirect3DDevice9->GetTextureStageState(Stage, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetTextureStageState");
		TRACE_IF_ACTIVE(trace_SetTextureStageState, Stage, Type, Value);
		return m_pIDirect3DDevice9->SetTextureStageState(Stage, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetSamplerState, Sampler, Type, pValue);
		return m_pIDirect3DDevice9->GetSamplerState(Sampler, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
	{
		PROFILE_ZONE_N("d3d9::SetSamplerState");
		TRACE_IF_ACTIVE(trace_SetSamplerState, Sampler, Type, Value);
		return m_pIDirect3DDevice9->SetSamplerState(Sampler, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::ValidateDevice(DWORD* pNumPasses)
	{
		TRACE_IF_ACTIVE(trace_ValidateDevice, pNumPasses);
		return m_pIDirect3DDevice9->ValidateDevice(pNumPasses);
	}

	HRESULT d3d9ex::D3D9Device::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_SetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->SetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_GetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->GetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_SetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->SetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_GetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->GetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::SetScissorRect(CONST RECT* pRect)
	{
		PROFILE_ZONE_N("d3d9::SetScissorRect");
		TRACE_IF_ACTIVE(trace_SetScissorRect, pRect);
		return m_pIDirect3DDevice9->SetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::GetScissorRect(RECT* pRect)
	{
		TRACE_IF_ACTIVE(trace_GetScissorRect, pRect);
		return m_pIDirect3DDevice9->GetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware)
	{
		TRACE_IF_ACTIVE(trace_SetSoftwareVertexProcessing, bSoftware);
		return m_pIDirect3DDevice9->SetSoftwareVertexProcessing(bSoftware);
	}

	BOOL d3d9ex::D3D9Device::GetSoftwareVertexProcessing()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetSoftwareVertexProcessing);
		return m_pIDirect3DDevice9->GetSoftwareVertexProcessing();
	}

	HRESULT d3d9ex::D3D9Device::SetNPatchMode(float nSegments)
	{
		TRACE_IF_ACTIVE(trace_SetNPatchMode, nSegments);
		return m_pIDirect3DDevice9->SetNPatchMode(nSegments);
	}

	float d3d9ex::D3D9Device::GetNPatchMode()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNPatchMode);
		return m_pIDirect3DDevice9->GetNPatchMode();
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitive([[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType, [[maybe_unused]] UINT StartVertex, [[maybe_unused]] UINT PrimitiveCount)
	{
		PROFILE_ZONE_N("d3d9::DrawPrimitive");
		TRACE_IF_ACTIVE(trace_DrawPrimitive, PrimitiveType, StartVertex, PrimitiveCount);
		const auto hr = renderer::get()->on_draw_primitive(m_pIDirect3DDevice9, PrimitiveType, StartVertex, PrimitiveCount);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		PROFILE_ZONE_N("d3d9::DrawIndexedPrimitive");
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitive, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		const auto hr = renderer::get()->on_draw_indexed_prim(m_pIDirect3DDevice9, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		PROFILE_ZONE_N("d3d9::DrawPrimitiveUP");
		TRACE_IF_ACTIVE(trace_DrawPrimitiveUP, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		// You might want to wrap this if your game uses this
		const auto hr = m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitiveUP(
		[[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType,
		[[maybe_unused]] UINT MinVertexIndex,
		[[maybe_unused]] UINT NumVertices,
		[[maybe_unused]] UINT PrimitiveCount,
		[[maybe_unused]] CONST void* pIndexData,
		[[maybe_unused]] D3DFORMAT IndexDataFormat,
		[[maybe_unused]] CONST void* pVertexStreamZeroData,
		[[maybe_unused]] UINT VertexStreamZeroStride)
	{
		PROFILE_ZONE_N("d3d9::DrawIndexedPrimitiveUP");
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitiveUP, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		// You might want to wrap this if your game uses this
		return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT d3d9ex::D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
	{
		PROFILE_ZONE_N("d3d9::ProcessVertices");
		TRACE_IF_ACTIVE(trace_ProcessVertices, SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
		return m_pIDirect3DDevice9->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexDeclaration");
		TRACE_IF_ACTIVE(trace_CreateVertexDeclaration, pVertexElements, ppDecl);
		auto hr = m_pIDirect3DDevice9->CreateVertexDeclaration(pVertexElements, ppDecl);
		if (SUCCEEDED(hr) && ppDecl && *ppDecl)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppDecl);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
	{
		PROFILE_ZONE_N("d3d9::SetVertexDeclaration");
		TRACE_IF_ACTIVE(trace_SetVertexDeclaration, pDecl);
		shared::common::ffp_state::get().on_set_vertex_declaration(pDecl);
		return m_pIDirect3DDevice9->SetVertexDeclaration(pDecl);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
	{
		TRACE_IF_ACTIVE(trace_GetVertexDeclaration, ppDecl);
		return m_pIDirect3DDevice9->GetVertexDeclaration(ppDecl);
	}

	HRESULT d3d9ex::D3D9Device::SetFVF(DWORD FVF)
	{
		PROFILE_ZONE_N("d3d9::SetFVF");
		TRACE_IF_ACTIVE(trace_SetFVF, FVF);
		return m_pIDirect3DDevice9->SetFVF(FVF);
	}

	HRESULT d3d9ex::D3D9Device::GetFVF(DWORD* pFVF)
	{
		TRACE_IF_ACTIVE(trace_GetFVF, pFVF);
		return m_pIDirect3DDevice9->GetFVF(pFVF);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
	{
		PROFILE_ZONE_N("d3d9::CreateVertexShader");
		TRACE_IF_ACTIVE(trace_CreateVertexShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreateVertexShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
	{
		PROFILE_ZONE_N("d3d9::SetVertexShader");
		TRACE_IF_ACTIVE(trace_SetVertexShader, pShader);
		shared::common::ffp_state::get().on_set_vertex_shader(pShader);
		return m_pIDirect3DDevice9->SetVertexShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShader, ppShader);
		return m_pIDirect3DDevice9->GetVertexShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		PROFILE_ZONE_N("d3d9::SetVSConstF");
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);

		// Redundant-write filter for non-bone constants. count==3 is the bone
		// upload signature consumed by game::on_set_vs_const_f's num_bones /
		// bones_drawn state machine; those must always fire, even with
		// identical data, to keep per-object bone tracking correct.
		auto& ffp = shared::common::ffp_state::get();
		if (Vector4fCount != 3 && ffp.vs_const_matches(StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		ffp.on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);
		if (auto* d = diagnostics::get())
			d->on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);

		// When the upload is consumed as a skinned bone it is replayed as a
		// SetTransform and the FFP path nulls the VS, so the raw constant is
		// dead on the device. Skip the forward (one fewer bridge crossing per
		// bone); draw_skinned_dip's real-shader fallbacks flush it back.
		if (game::on_set_vs_const_f(m_pIDirect3DDevice9, StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		return m_pIDirect3DDevice9->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
	{
		PROFILE_ZONE_N("d3d9::SetStreamSource");
		TRACE_IF_ACTIVE(trace_SetStreamSource, StreamNumber, pStreamData, OffsetInBytes, Stride);
		shared::common::ffp_state::get().on_set_stream_source(StreamNumber, pStreamData, OffsetInBytes, Stride);
		return m_pIDirect3DDevice9->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSource, StreamNumber, ppStreamData, OffsetInBytes, pStride);
		return m_pIDirect3DDevice9->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
	{
		TRACE_IF_ACTIVE(trace_SetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->SetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->GetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
	{
		PROFILE_ZONE_N("d3d9::SetIndices");
		TRACE_IF_ACTIVE(trace_SetIndices, pIndexData);
		return m_pIDirect3DDevice9->SetIndices(pIndexData);
	}

	HRESULT d3d9ex::D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
	{
		TRACE_IF_ACTIVE(trace_GetIndices, ppIndexData);
		return m_pIDirect3DDevice9->GetIndices(ppIndexData);
	}

	HRESULT d3d9ex::D3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
	{
		PROFILE_ZONE_N("d3d9::CreatePixelShader");
		TRACE_IF_ACTIVE(trace_CreatePixelShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreatePixelShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
	{
		PROFILE_ZONE_N("d3d9::SetPixelShader");
		TRACE_IF_ACTIVE(trace_SetPixelShader, pShader);
		// Pre-populate the PS classifier cache on first sighting. The renderer
		// queries it per draw to map (sampler name -> slot) and drive the
		// RS-protocol payload that dxvk-remix decodes.
		shared::common::g_ps_classifier.classify(pShader);
		if (shared::common::ffp_state::get().on_set_pixel_shader(pShader))
			return S_OK; // swallowed while FFP is active
		return m_pIDirect3DDevice9->SetPixelShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShader, ppShader);
		return m_pIDirect3DDevice9->GetPixelShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		PROFILE_ZONE_N("d3d9::SetPSConstF");
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);

		// Redundant-write filter. PS constants have no game-side handler with
		// state machine — only ffp_state's cache update, which is itself a
		// no-op when data is unchanged.
		auto& ffp = shared::common::ffp_state::get();
		if (ffp.ps_const_matches(StartRegister, pConstantData, Vector4fCount))
			return D3D_OK;

		ffp.on_set_ps_const_f(StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawRectPatch, Handle, pNumSegs, pRectPatchInfo);
		return m_pIDirect3DDevice9->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawTriPatch, Handle, pNumSegs, pTriPatchInfo);
		return m_pIDirect3DDevice9->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DeletePatch(UINT Handle)
	{
		TRACE_IF_ACTIVE(trace_DeletePatch, Handle);
		return m_pIDirect3DDevice9->DeletePatch(Handle);
	}

	HRESULT d3d9ex::D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		PROFILE_ZONE_N("d3d9::CreateQuery");
		TRACE_IF_ACTIVE(trace_CreateQuery, Type, ppQuery);
		return m_pIDirect3DDevice9->CreateQuery(Type, ppQuery);
	}

	// ---- IDirect3DDevice9Ex ----
	// Reached only when the device was created through CreateDeviceEx. On a
	// plain CreateDevice device m_pIDirect3DDevice9Ex is null and these report
	// D3DERR_INVALIDCALL, mirroring what QueryInterface already tells callers.

#define REQUIRE_DEVICE9EX() if (!m_pIDirect3DDevice9Ex) return D3DERR_INVALIDCALL

	HRESULT d3d9ex::D3D9Device::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetConvolutionMonoKernel(width, height, rows, columns);
	}

	HRESULT d3d9ex::D3D9Device::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset);
	}

	HRESULT d3d9ex::D3D9Device::PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags)
	{
		PROFILE_ZONE_N("PresentEx");
		REQUIRE_DEVICE9EX();
		TRACE_IF_ACTIVE(trace_Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		pre_present();
		auto hr = m_pIDirect3DDevice9Ex->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
		post_present();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetGPUThreadPriority(INT* pPriority)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetGPUThreadPriority(pPriority);
	}

	HRESULT d3d9ex::D3D9Device::SetGPUThreadPriority(INT Priority)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetGPUThreadPriority(Priority);
	}

	HRESULT d3d9ex::D3D9Device::WaitForVBlank(UINT iSwapChain)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->WaitForVBlank(iSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CheckResourceResidency(pResourceArray, NumResources);
	}

	HRESULT d3d9ex::D3D9Device::SetMaximumFrameLatency(UINT MaxLatency)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->SetMaximumFrameLatency(MaxLatency);
	}

	HRESULT d3d9ex::D3D9Device::GetMaximumFrameLatency(UINT* pMaxLatency)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetMaximumFrameLatency(pMaxLatency);
	}

	HRESULT d3d9ex::D3D9Device::CheckDeviceState(HWND hDestinationWindow)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CheckDeviceState(hDestinationWindow);
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateRenderTargetEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateOffscreenPlainSurfaceEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage)
	{
		PROFILE_ZONE_N("d3d9::CreateDepthStencilSurfaceEx");
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage);
	}

	HRESULT d3d9ex::D3D9Device::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode)
	{
		PROFILE_ZONE_N("d3d9::ResetEx");
		REQUIRE_DEVICE9EX();
		TRACE_IF_ACTIVE(trace_Reset, pPresentationParameters);
		pre_reset();
		const auto hr = m_pIDirect3DDevice9Ex->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
		post_reset(hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		REQUIRE_DEVICE9EX();
		return m_pIDirect3DDevice9Ex->GetDisplayModeEx(iSwapChain, pMode, pRotation);
	}

#undef REQUIRE_DEVICE9EX

#pragma endregion

#pragma region _D3D9

	HRESULT __stdcall d3d9ex::_d3d9::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3D9->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9::AddRef()
	{
		return m_pIDirect3D9->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9::Release()
	{
		ULONG count = m_pIDirect3D9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterCount()
	{
		return m_pIDirect3D9->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		// D3D9 nulls the out-pointer on failure, and games retry CreateDevice
		// with different parameters as a matter of course. Wrapping the failed
		// result published a proxy around a null device that null-dereferenced
		// on the first call through it.
		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDevice failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		shared::globals::d3d_device = *ppReturnedDeviceInterface;

		return hres;
	}


#pragma endregion

#pragma region _D3D9Ex

	HRESULT __stdcall d3d9ex::_d3d9ex::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;
		HRESULT hRes = m_pIDirect3D9Ex->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9ex::AddRef()
	{
		return m_pIDirect3D9Ex->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9ex::Release()
	{
		ULONG count = m_pIDirect3D9Ex->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9Ex->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterCount()
	{
		return m_pIDirect3D9Ex->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9Ex->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9Ex->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9Ex->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9Ex->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9Ex->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9Ex->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9ex::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9Ex->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9Ex->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9Ex->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDevice failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		shared::globals::d3d_device = *ppReturnedDeviceInterface;

		return hres;
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter)
	{
		return (m_pIDirect3D9Ex->GetAdapterModeCountEx(Adapter, pFilter));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode)
	{
		return (m_pIDirect3D9Ex->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		return (m_pIDirect3D9Ex->GetAdapterDisplayModeEx(Adapter, pMode, pRotation));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, IDirect3DDevice9Ex** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9Ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9Ex->CreateDeviceEx", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		if (FAILED(hres) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface)
		{
			shared::common::log("d3d9",
				std::format("CreateDeviceEx failed (hr=0x{:08X}); not wrapping device", static_cast<unsigned>(hres)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return hres;
		}

		// Previously returned unwrapped: none of the comp's hooks fired and
		// globals::d3d_device stayed null, silently disabling the whole mod for
		// anything that creates its device through the Ex path. D3D9Device now
		// implements IDirect3DDevice9Ex so it can stand in here too.
		auto* proxy = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		*ppReturnedDeviceInterface = proxy;
		shared::globals::d3d_device = proxy;

		return hres;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterLUID(UINT Adapter, LUID* pLUID)
	{
		return (m_pIDirect3D9Ex->GetAdapterLUID(Adapter, pLUID));
	}
#pragma endregion

	d3d9ex::d3d9ex() {}
}

// ---- Exported entry points (linked via d3d9.def) ----

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
	shared::common::log("d3d9", "Direct3DCreate9 called. Creating proxy interface.");
	auto real_fn = d3d9_proxy::get_Direct3DCreate9();
	if (!real_fn) return nullptr;

	IDirect3D9* real_d3d9 = real_fn(SDKVersion);
	if (!real_d3d9) return nullptr;

	shared::globals::d3d9_interface = new comp::d3d9ex::_d3d9(real_d3d9);
	return shared::globals::d3d9_interface;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	shared::common::log("d3d9", "Direct3DCreate9Ex called. Creating proxy interface.");
	auto real_fn = d3d9_proxy::get_Direct3DCreate9Ex();
	if (!real_fn || !ppD3D) return D3DERR_NOTAVAILABLE;

	IDirect3D9Ex* real_d3d9ex = nullptr;
	HRESULT hr = real_fn(SDKVersion, &real_d3d9ex);
	if (FAILED(hr) || !real_d3d9ex) return hr;

	auto* proxy = new comp::d3d9ex::_d3d9ex(real_d3d9ex);
	shared::globals::d3d9_interface = proxy;
	*ppD3D = proxy;
	return hr;
}
