#pragma once

// Direct DirectX 12 renderer for the Xbox (Dev Mode), step 2 of the plan in README.md: it proves the
// console's own D3D12 works from the emulator (device, command list, readback, hand-off to the shell)
// by showing a slowly pulsing blue screen. It draws nothing of the game yet.

#include "Emu/RSX/Core/RSXRenderBackend.h"
#include "Emu/RSX/GSFrameBase.h"

#include <memory>

class D3D12GSRender final : public rsx::render_backend
{
public:
	D3D12GSRender(rsx::context* ctx, rsx::display_output* output) noexcept;
	~D3D12GSRender() override;

	void on_init() override;
	void on_shutdown() override;

	// No host caches yet
	std::unique_ptr<rsx::context_resources> create_context_resources(rsx::context*) override { return std::make_unique<rsx::context_resources>(); }

	bool present_frame(const rsx::display_flip_info_t& info) override;

	// No host surfaces to bind yet. Keeps the GRAPH engine on its fast path (like the Null renderer).
	bool is_framebuffer_bound() const override { return true; }

	std::string logical_id() const override { return "D3D12"; }
	std::string configured_logical_id() const override;

private:
	struct impl;
	std::unique_ptr<impl> m_impl;
};
