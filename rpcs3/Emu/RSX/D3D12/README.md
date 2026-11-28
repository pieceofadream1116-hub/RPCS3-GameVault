# DirectX 12 renderer for the Xbox (in progress)

Goal: draw PS3 graphics straight with the Xbox's own DirectX 12, instead of Vulkan through
Mesa's Dozen (Vulkan-on-D3D12), which is where the black screens and GPU crashes come from.

`legacy_2019/` is RPCS3's own D3D12 renderer as it was in v0.0.6 (January 2019), before it was
removed upstream. It is not compiled; it is the reference for the port. It targets feature level
11_0, which is what Xbox Dev Mode gives apps.

What carries over:
- Shader translators (vertex/fragment program decompilers to HLSL): today's decompiler base
  classes still match about 95% of the virtual methods the 2019 code overrides.
- Format tables (D3D12Formats).
- From the PS2 app (XBSX2, pcsx2/GS/Renderers/DX12): device setup, descriptor heaps, stream
  buffers and the shader cache, already proven on the console.

To rewire to today's design (rsx::render_backend): render targets, textures, buffers, pipeline
state and present. Gaps with no old code: today's surface store and texture cache, overlays,
resolution scaling, write color buffers, newer accuracy features.

Steps:
1. Done: old code found; it fits the Xbox's feature level.
2. Now: a "DirectX 12" Graphics engine setting that shows a pulsing blue test screen with a
   sweeping white bar (D3D12GSRender.cpp). Proves device, command list, readback and hand-off work.
3. One game's menu on screen.
4. The rest of the drawing, one problem at a time.
