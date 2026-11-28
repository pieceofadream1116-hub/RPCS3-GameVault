#include "stdafx.h"
#include "D3D12GSRender.h"

#include "Emu/system_config.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"

#include <d3d12.h>
#include <cmath>
#include <vector>

LOG_CHANNEL(d3d12_log, "D3D12");

// The XAML shell shows whatever BGRA frame it is handed (uwp_main.cpp), like the Vulkan renderer's UWP swap chain.
extern void uwp_present_frame(const void* bgra_pixels, u32 width, u32 height);
// The shell takes frames of exactly this size (1920x1080) and drops any other.
extern void uwp_get_display_size(u32& width, u32& height);

namespace
{
	// Set from the shell's display size in init(); 1920 * 4 = 7680 is a multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
	u32 c_width = 1920;
	u32 c_height = 1080;
	u32 c_row_pitch = 1920 * 4;

	template <typename T>
	void release(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}
}

struct D3D12GSRender::impl
{
	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;
	ID3D12Fence* fence = nullptr;
	HANDLE fence_event = nullptr;
	u64 fence_value = 0;

	ID3D12DescriptorHeap* rtv_heap = nullptr;
	ID3D12Resource* target = nullptr;   // 1280x720 BGRA render target
	ID3D12Resource* readback = nullptr; // CPU-readable copy of it

	std::vector<u8> pixels;
	u64 frames = 0;
	bool ok = false;

	bool init()
	{
		uwp_get_display_size(c_width, c_height);
		c_row_pitch = (c_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		d3d12_log.always()("D3D12CreateDevice (feature level 11_0): 0x%08x", static_cast<u32>(hr));
		if (FAILED(hr))
			return false;

		D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
		{
			d3d12_log.always()("D3D12: resource binding tier %d, tiled resources tier %d, ROVs %d, conservative raster tier %d",
				static_cast<int>(opts.ResourceBindingTier), static_cast<int>(opts.TiledResourcesTier),
				static_cast<int>(opts.ROVsSupported), static_cast<int>(opts.ConservativeRasterizationTier));
		}

		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
			FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
			FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) ||
			FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		{
			d3d12_log.error("D3D12: creating the queue, command list or fence failed: 0x%08x", static_cast<u32>(hr));
			return false;
		}
		list->Close();
		fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		if (FAILED(hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))))
		{
			d3d12_log.error("D3D12: CreateDescriptorHeap failed: 0x%08x", static_cast<u32>(hr));
			return false;
		}

		D3D12_HEAP_PROPERTIES def_heap{};
		def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC td{};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = c_width;
		td.Height = c_height;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE cv{};
		cv.Format = td.Format;
		if (FAILED(hr = device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, &cv, IID_PPV_ARGS(&target))))
		{
			d3d12_log.error("D3D12: creating the render target failed: 0x%08x", static_cast<u32>(hr));
			return false;
		}
		device->CreateRenderTargetView(target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

		D3D12_HEAP_PROPERTIES rb_heap{};
		rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = u64{c_row_pitch} * c_height;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
		{
			d3d12_log.error("D3D12: creating the readback buffer failed: 0x%08x", static_cast<u32>(hr));
			return false;
		}

		pixels.resize(usz{c_width} * c_height * 4);
		d3d12_log.always()("D3D12: ready (test picture: pulsing blue screen, %ux%u)", c_width, c_height);
		return true;
	}

	static void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		cl->ResourceBarrier(1, &b);
	}

	void wait_idle()
	{
		fence_value++;
		queue->Signal(fence, fence_value);
		if (fence->GetCompletedValue() < fence_value)
		{
			fence->SetEventOnCompletion(fence_value, fence_event);
			WaitForSingleObject(fence_event, 5000);
		}
	}

	void draw_test_frame()
	{
		allocator->Reset();
		list->Reset(allocator, nullptr);

		transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

		// Slowly pulsing blue, so a frozen picture is easy to tell from a running one.
		const f32 t = 0.5f + 0.5f * std::sin(static_cast<f32>(frames) * 0.05f);
		const f32 colour[4] = {0.04f + 0.06f * t, 0.10f + 0.20f * t, 0.30f + 0.55f * t, 1.0f};
		const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		list->ClearRenderTargetView(rtv, colour, 0, nullptr);

		// A white bar sweeping across the middle: the command list really draws per frame.
		const LONG bar_x = static_cast<LONG>((frames * 12) % c_width);
		const D3D12_RECT bar{bar_x, static_cast<LONG>(c_height / 2 - 30), std::min<LONG>(bar_x + 240, static_cast<LONG>(c_width)), static_cast<LONG>(c_height / 2 + 30)};
		const f32 white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		list->ClearRenderTargetView(rtv, white, 1, &bar);

		transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		dst.PlacedFootprint.Footprint.Width = c_width;
		dst.PlacedFootprint.Footprint.Height = c_height;
		dst.PlacedFootprint.Footprint.Depth = 1;
		dst.PlacedFootprint.Footprint.RowPitch = c_row_pitch;
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = target;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		HRESULT hr = list->Close();
		if (FAILED(hr))
		{
			d3d12_log.error("D3D12: command list Close failed: 0x%08x", static_cast<u32>(hr));
			return;
		}
		ID3D12CommandList* lists[] = {list};
		queue->ExecuteCommandLists(1, lists);
		wait_idle();

		void* mapped = nullptr;
		const D3D12_RANGE read{0, usz{c_row_pitch} * c_height};
		if (SUCCEEDED(hr = readback->Map(0, &read, &mapped)))
		{
			for (u32 y = 0; y < c_height; y++)
				std::memcpy(pixels.data() + usz{y} * c_width * 4, static_cast<const u8*>(mapped) + usz{y} * c_row_pitch, usz{c_width} * 4);
			const D3D12_RANGE none{0, 0};
			readback->Unmap(0, &none);
			uwp_present_frame(pixels.data(), c_width, c_height);
		}
		else if (frames < 5)
		{
			d3d12_log.error("D3D12: readback Map failed: 0x%08x", static_cast<u32>(hr));
		}

		if (frames == 0 || frames == 60 || frames % 3600 == 0)
		{
			const HRESULT removed = device->GetDeviceRemovedReason();
			d3d12_log.always()("D3D12: frame %u shown (device status 0x%08x)", frames, static_cast<u32>(removed));
		}
		frames++;
	}

	void shutdown()
	{
		if (queue && fence)
			wait_idle();
		release(readback);
		release(target);
		release(rtv_heap);
		release(fence);
		release(list);
		release(allocator);
		release(queue);
		release(device);
		if (fence_event)
		{
			CloseHandle(fence_event);
			fence_event = nullptr;
		}
	}
};

D3D12GSRender::D3D12GSRender(rsx::context* ctx, rsx::display_output* output) noexcept
	: render_backend(ctx, output)
	, m_impl(std::make_unique<impl>())
{
}

D3D12GSRender::~D3D12GSRender()
{
	m_impl->shutdown();
}

void D3D12GSRender::on_init()
{
	create_frame_context();
	m_impl->ok = m_impl->init();
	if (!m_impl->ok)
		d3d12_log.error("D3D12: the DirectX 12 test renderer could not start; the screen stays black");
}

void D3D12GSRender::on_shutdown()
{
	m_impl->shutdown();
	m_impl->ok = false;
	destroy_frame_context();
}

std::string D3D12GSRender::configured_logical_id() const
{
	return (rsx::resolve_render_backend_type(g_cfg.video.renderer) == video_renderer::d3d12) ? "D3D12" : "";
}

bool D3D12GSRender::present_frame(const rsx::display_flip_info_t&)
{
	if (m_impl->ok)
		m_impl->draw_test_frame();

	if (m_frame)
		m_frame->flip(m_context);

	return false;
}
