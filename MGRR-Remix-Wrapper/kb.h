// Metal Gear Rising: Revengeance - Knowledge Base
// Binary: METAL GEAR RISING REVENGEANCE.exe (GOG, x86/32-bit)
// D3D9 shader-based renderer → fixed-function conversion project
// ImageBase: 0x00400000

// ============================================================
// Structs
// ============================================================

// ============================================================
// Globals
// ============================================================

// D3D9 core
$ 0x1F206D4 IDirect3DDevice9* g_pD3DDevice
$ 0x1F206D8 IDirect3D9* g_pD3D9

// Display settings
$ 0x1F205E8 int g_displayWidth
$ 0x1F205EC int g_displayHeight
$ 0x1F20620 int g_configWidth
$ 0x1F20624 int g_configHeight
$ 0x1F206B0 int g_renderWidth
$ 0x1F206B4 int g_renderHeight
$ 0x1F206B8 float g_aspectRatio
$ 0x1F206DC int g_backbufferWidth
$ 0x1F206E0 int g_backbufferHeight
$ 0x1F206BC int g_supportsMultisample
$ 0x1F206D0 int g_deviceType

// Render state
$ 0x1F20698 int g_shaderModel3Support
$ 0x1F206A4 int g_maxVertexShaderConst

// ============================================================
// Functions
// ============================================================

// D3D initialization
@ 0x00FA0D80 int __cdecl D3D_Init(int* pDisplayParams)
@ 0x00F9B2B0 void __cdecl D3D_CreateDeviceAndSetup(int* pDisplayParams)
@ 0x00F97F30 int __cdecl D3D_EnsureD3DCreated(void)

// Import thunks
@ 0x01436F26 IDirect3D9* __stdcall Direct3DCreate9_thunk(UINT SDKVersion)
