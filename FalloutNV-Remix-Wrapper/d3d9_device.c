/*
 * Wrapped IDirect3DDevice9 — FFP conversion layer for RTX Remix.
 *
 * Intercepts ~15 of 119 device methods; the rest relay via naked ASM thunks.
 * Sections marked GAME-SPECIFIC need per-game updates.
 * See the dx9-ffp-port prompt and extensions/skinning/README.md for full docs.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* No-CRT memcpy: the compiler emits memcpy calls for struct/array copies */
#pragma function(memcpy)
void * __cdecl memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* Logging (from d3d9_main.c) */
extern void log_str(const char *s);
extern void log_hex(const char *prefix, unsigned int val);
extern void log_int(const char *prefix, int val);
extern void log_floats(const char *prefix, float *data, unsigned int count);
extern void log_float_val(const char *prefix, float f);
extern void log_floats_dec(const char *prefix, float *data, unsigned int count);

/* ============================================================
 * GAME-SPECIFIC: VS Constant Register Layout
 *
 * These define which vertex shader constant registers hold the
 * View, Projection, and World matrices. Every game engine uses
 * different register assignments. Discover yours with:
 *
 *   python scripts/find_vs_constants.py <your_game.exe>
 *   python -m livetools trace <SetVSConstF_call_addr> \
 *       --count 50 --read "[esp+4]:4:uint32; [esp+8]:4:uint32"
 *
 * Common patterns across engines:
 *   - View+Proj often adjacent (c0-c7 or c4-c11)
 *   - World matrix at a fixed register (c0, c8, c16, etc.)
 *   - Bone palette starts after world, 3 or 4 regs per bone
 *
 * ============================================================ */
#define VS_REG_VIEW_START       0   /* c0-c3: WorldViewProj (combined) */
#define VS_REG_VIEW_END         4
#define VS_REG_PROJ_START       0   /* Same range — game uploads combined WVP, not separate V+P */
#define VS_REG_PROJ_END         4
#define VS_REG_WORLD_START      8   /* c8-c11: World matrix */
#define VS_REG_WORLD_END       12

/* Bone palette detection (only matters when ENABLE_SKINNING=1) */
#define VS_REGS_PER_BONE        3   /* Registers per bone (3 = 4x3 packed) */

/* GAME-SPECIFIC: Skinning — enable FFP indexed vertex blending for skinned meshes */
#define ENABLE_SKINNING 1

/* Engine light extraction via D3D9 SetLight/LightEnable.
 * Reads the game's ShadowSceneNode point light list and submits via
 * D3D9 FFP light calls, which RTX Remix intercepts for path tracing. */
#define ENABLE_LIGHTS 1
#define MAX_EXTRACTED_LIGHTS 128

/* Fake shadow detection: skip BSSM_NOLIGHTING draws whose vertex colors are
 * all shades of gray (R ≈ G ≈ B). Two-step filter:
 *   1. NiShadeProperty::m_eShaderType == kProp_NoLighting (0x15)
 *   2. All vertex colors are grayscale with at least one dark vertex
 * This catches baked shadow overlays while keeping windows/glass/UI. */
#define ENABLE_SHADOW_SKIP 1
#define SHADOW_CACHE_SIZE 64    /* must be power of 2 */
#define SHADOW_GRAY_TOLERANCE 5 /* max abs(R-G), abs(R-B), abs(G-B) in 0-255 */
#define SHADOW_DARK_THRESHOLD 250 /* at least one vertex must have R < this */

/* Declaration cloning cache size: maps original decl -> cloned decl with
 * BLENDINDICES type changed from D3DCOLOR to UBYTE4 for FFP compatibility. */
#define SKIN_DECL_CACHE_SIZE 4

/* ============================================================
 * GAME-SPECIFIC: NiDX9Renderer Direct Memory Reads
 *
 * The game's NiDX9Renderer singleton stores pre-computed World, View,
 * and Projection matrices at fixed offsets. Reading these directly is
 * more accurate than decomposing VS constants via matrix inversion:
 *   - No precision loss from matrix inversion
 *   - Separate View and Projection (Remix needs View for camera position)
 *   - Matches NewVegasRTXHelper's proven approach
 *
 * Addresses from NewVegasRTXHelper RE / GameNi.h:
 *   NiDX9Renderer::GetSingleton()    = *(NiDX9Renderer**)0x11C73B4
 *   BSShaderManager::GetRenderer()   = *(NiDX9Renderer**)0x11F9508
 *   NiDX9Renderer::worldMatrix       = offset 0x940 (D3DXMATRIXA16)
 *   NiDX9Renderer::viewMatrix        = offset 0x980 (D3DXMATRIX)
 *   NiDX9Renderer::projMatrix        = offset 0x9C0 (D3DXMATRIX)
 * ============================================================ */
#define GAME_RENDERER_SINGLETON  (*(void**)0x11C73B4)
#define RENDERER_WORLD_OFF       0x940
#define RENDERER_VIEW_OFF        0x980
#define RENDERER_PROJ_OFF        0x9C0

/* ---- Diagnostic logging ---- */
#define DIAG_LOG_FRAMES 3
#define DIAG_DELAY_MS 50000   /* 50 seconds after device creation */
#define DIAG_ENABLED 1

#define DIAG_ACTIVE(self) \
    (DIAG_ENABLED && (self)->diagLoggedFrames < DIAG_LOG_FRAMES && \
     GetTickCount() - (self)->createTick >= DIAG_DELAY_MS)

/* ---- D3D9 Constants ---- */

#define D3DTS_VIEW          2
#define D3DTS_PROJECTION    3
#define D3DTS_WORLD         256
#define D3DTS_TEXTURE0      16

#define D3DRS_ZENABLE           7
#define D3DRS_FILLMODE          8
#define D3DRS_LIGHTING          137
#define D3DRS_AMBIENT           139
#define D3DRS_COLORVERTEX       141
#define D3DRS_SPECULARENABLE    29
#define D3DRS_DIFFUSEMATERIALSOURCE   145
#define D3DRS_AMBIENTMATERIALSOURCE   147
#define D3DRS_NORMALIZENORMALS  143
#define D3DRS_ALPHABLENDENABLE  27
#define D3DRS_SRCBLEND          19
#define D3DRS_DESTBLEND         20
#define D3DRS_CULLMODE          22
#define D3DRS_FOGENABLE         28

#define D3DTSS_COLOROP     1
#define D3DTSS_COLORARG1   2
#define D3DTSS_COLORARG2   3
#define D3DTSS_ALPHAOP     4
#define D3DTSS_ALPHAARG1   5
#define D3DTSS_ALPHAARG2   6
#define D3DTSS_TEXCOORDINDEX 11
#define D3DTSS_TEXTURETRANSFORMFLAGS 24

#define D3DTOP_DISABLE     1
#define D3DTOP_SELECTARG1  2
#define D3DTOP_MODULATE    4

#define D3DTA_TEXTURE      2
#define D3DTA_DIFFUSE      0
#define D3DTA_CURRENT      1

#define D3DLIGHT_DIRECTIONAL 3

#define D3DVBF_DISABLE  0
#define D3DVBF_1WEIGHTS  1
#define D3DVBF_2WEIGHTS  2
#define D3DVBF_3WEIGHTS  3

#define D3DRS_VERTEXBLEND              151
#define D3DRS_INDEXEDVERTEXBLENDENABLE  167

#define D3DTS_WORLDMATRIX(n) (256 + (n))

#define D3DDECL_END_STREAM 0xFF
#define D3DDECLUSAGE_POSITION     0
#define D3DDECLUSAGE_BLENDWEIGHT  1
#define D3DDECLUSAGE_BLENDINDICES 2
#define D3DDECLUSAGE_NORMAL       3
#define D3DDECLUSAGE_TEXCOORD     5
#define D3DDECLUSAGE_COLOR        10
#define D3DDECLUSAGE_POSITIONT    9   /* pre-transformed screen-space coords — skips FFP transform */

#define MAX_FFP_BONES 48

/* ---- Game Engine Hook State (GAME-SPECIFIC) ----
 *
 * These globals are written to by inline code cave stubs that hook the game's
 * skinned render batch functions, matching NewVegasRTXHelper's approach:
 *   - on_render_skinned_stub (0xB99598): sets/clears g_renderSkinned around batch
 *   - reset_bones_stub (0xB991E7): sets g_boneResetPending per-object
 *
 * The stubs reference these by absolute address in the .data section. */
#if ENABLE_SKINNING
static volatile int g_renderSkinned = 0;     /* 1 while inside skinned render batch */
static volatile int g_boneResetPending = 0;  /* 1 when per-object bone reset needed */
static int g_hooksInstalled = 0;             /* 1 if game hooks were installed */
#endif

#define D3DDECLTYPE_FLOAT1    0
#define D3DDECLTYPE_FLOAT2    1
#define D3DDECLTYPE_FLOAT3    2
#define D3DDECLTYPE_FLOAT4    3
#define D3DDECLTYPE_D3DCOLOR  4
#define D3DDECLTYPE_UBYTE4    5
#define D3DDECLTYPE_UBYTE4N   8
#define D3DDECLTYPE_SHORT4N   10
#define D3DDECLTYPE_UDEC3     13
#define D3DDECLTYPE_DEC3N     14
#define D3DDECLTYPE_FLOAT16_2 15

#if ENABLE_SHADOW_SKIP
#define SHADOW_UNKNOWN  0
#define SHADOW_IS_FAKE  1
#define SHADOW_NOT_FAKE 2

typedef struct {
    void *vb;
    int   baseVertexIndex;
    int   result;
} ShadowCacheEntry;
#endif

/* (Skinning uses original VB — no vertex expansion needed for simple FFP approach) */

/* ---- Device vtable slot indices ---- */
enum {
    SLOT_QueryInterface = 0,
    SLOT_AddRef = 1,
    SLOT_Release = 2,
    SLOT_TestCooperativeLevel = 3,
    SLOT_GetAvailableTextureMem = 4,
    SLOT_EvictManagedResources = 5,
    SLOT_GetDirect3D = 6,
    SLOT_GetDeviceCaps = 7,
    SLOT_GetDisplayMode = 8,
    SLOT_GetCreationParameters = 9,
    SLOT_SetCursorProperties = 10,
    SLOT_SetCursorPosition = 11,
    SLOT_ShowCursor = 12,
    SLOT_CreateAdditionalSwapChain = 13,
    SLOT_GetSwapChain = 14,
    SLOT_GetNumberOfSwapChains = 15,
    SLOT_Reset = 16,
    SLOT_Present = 17,
    SLOT_GetBackBuffer = 18,
    SLOT_GetRasterStatus = 19,
    SLOT_SetDialogBoxMode = 20,
    SLOT_SetGammaRamp = 21,
    SLOT_GetGammaRamp = 22,
    SLOT_CreateTexture = 23,
    SLOT_CreateVolumeTexture = 24,
    SLOT_CreateCubeTexture = 25,
    SLOT_CreateVertexBuffer = 26,
    SLOT_CreateIndexBuffer = 27,
    SLOT_CreateRenderTarget = 28,
    SLOT_CreateDepthStencilSurface = 29,
    SLOT_UpdateSurface = 30,
    SLOT_UpdateTexture = 31,
    SLOT_GetRenderTargetData = 32,
    SLOT_GetFrontBufferData = 33,
    SLOT_StretchRect = 34,
    SLOT_ColorFill = 35,
    SLOT_CreateOffscreenPlainSurface = 36,
    SLOT_SetRenderTarget = 37,
    SLOT_GetRenderTarget = 38,
    SLOT_SetDepthStencilSurface = 39,
    SLOT_GetDepthStencilSurface = 40,
    SLOT_BeginScene = 41,
    SLOT_EndScene = 42,
    SLOT_Clear = 43,
    SLOT_SetTransform = 44,
    SLOT_GetTransform = 45,
    SLOT_MultiplyTransform = 46,
    SLOT_SetViewport = 47,
    SLOT_GetViewport = 48,
    SLOT_SetMaterial = 49,
    SLOT_GetMaterial = 50,
    SLOT_SetLight = 51,
    SLOT_GetLight = 52,
    SLOT_LightEnable = 53,
    SLOT_GetLightEnable = 54,
    SLOT_SetClipPlane = 55,
    SLOT_GetClipPlane = 56,
    SLOT_SetRenderState = 57,
    SLOT_GetRenderState = 58,
    SLOT_CreateStateBlock = 59,
    SLOT_BeginStateBlock = 60,
    SLOT_EndStateBlock = 61,
    SLOT_SetClipStatus = 62,
    SLOT_GetClipStatus = 63,
    SLOT_GetTexture = 64,
    SLOT_SetTexture = 65,
    SLOT_GetTextureStageState = 66,
    SLOT_SetTextureStageState = 67,
    SLOT_GetSamplerState = 68,
    SLOT_SetSamplerState = 69,
    SLOT_ValidateDevice = 70,
    SLOT_SetPaletteEntries = 71,
    SLOT_GetPaletteEntries = 72,
    SLOT_SetCurrentTexturePalette = 73,
    SLOT_GetCurrentTexturePalette = 74,
    SLOT_SetScissorRect = 75,
    SLOT_GetScissorRect = 76,
    SLOT_SetSoftwareVertexProcessing = 77,
    SLOT_GetSoftwareVertexProcessing = 78,
    SLOT_SetNPatchMode = 79,
    SLOT_GetNPatchMode = 80,
    SLOT_DrawPrimitive = 81,
    SLOT_DrawIndexedPrimitive = 82,
    SLOT_DrawPrimitiveUP = 83,
    SLOT_DrawIndexedPrimitiveUP = 84,
    SLOT_ProcessVertices = 85,
    SLOT_CreateVertexDeclaration = 86,
    SLOT_SetVertexDeclaration = 87,
    SLOT_GetVertexDeclaration = 88,
    SLOT_SetFVF = 89,
    SLOT_GetFVF = 90,
    SLOT_CreateVertexShader = 91,
    SLOT_SetVertexShader = 92,
    SLOT_GetVertexShader = 93,
    SLOT_SetVertexShaderConstantF = 94,
    SLOT_GetVertexShaderConstantF = 95,
    SLOT_SetVertexShaderConstantI = 96,
    SLOT_GetVertexShaderConstantI = 97,
    SLOT_SetVertexShaderConstantB = 98,
    SLOT_GetVertexShaderConstantB = 99,
    SLOT_SetStreamSource = 100,
    SLOT_GetStreamSource = 101,
    SLOT_SetStreamSourceFreq = 102,
    SLOT_GetStreamSourceFreq = 103,
    SLOT_SetIndices = 104,
    SLOT_GetIndices = 105,
    SLOT_CreatePixelShader = 106,
    SLOT_SetPixelShader = 107,
    SLOT_GetPixelShader = 108,
    SLOT_SetPixelShaderConstantF = 109,
    SLOT_GetPixelShaderConstantF = 110,
    SLOT_SetPixelShaderConstantI = 111,
    SLOT_GetPixelShaderConstantI = 112,
    SLOT_SetPixelShaderConstantB = 113,
    SLOT_GetPixelShaderConstantB = 114,
    SLOT_DrawRectPatch = 115,
    SLOT_DrawTriPatch = 116,
    SLOT_DeletePatch = 117,
    SLOT_CreateQuery = 118,
    DEVICE_VTABLE_SIZE = 119
};

/* ---- WrappedDevice ---- */

typedef struct WrappedDevice {
    void **vtbl;
    void *pReal;            /* real IDirect3DDevice9* */
    int refCount;
    unsigned int frameCount;
    int ffpSetup;           /* whether FFP state has been configured this frame */

    float vsConst[256 * 4]; /* vertex shader constants (up to 256 vec4) */
    float psConst[32 * 4];  /* pixel shader constants (up to 32 vec4) */
    int worldDirty;         /* world matrix registers changed since last SetTransform */
    int viewProjDirty;      /* view/proj registers changed since last SetTransform */
    int psConstDirty;

    void *lastVS;           /* last vertex shader set by the game */
    void *lastPS;           /* last pixel shader set by the game */
    int viewProjValid;      /* set once both View and Proj register ranges have been written */
    int ffpActive;          /* real device currently has NULL shaders (FFP mode) */

    void *lastDecl;         /* current IDirect3DVertexDeclaration9* */
    int curDeclIsSkinned;   /* 1 if current decl has BLENDWEIGHT+BLENDINDICES */

#if ENABLE_SKINNING
    int curDeclNumWeights;  /* number of blend weights (1-3) */
    int numBones;           /* number of bones uploaded via SetTransform this batch */
    int prevNumBones;       /* bones uploaded by previous object (for stale palette clearing) */
    int bonesDrawn;         /* 1 if bones were used in a draw (reset counter on next bone write) */
    int lastBoneStartReg;   /* start register of last bone write */
    int skinningSetup;      /* whether FFP indexed vertex blending state is active */

    int curDeclBlendIndicesOff;  /* byte offset of BLENDINDICES */
    int curDeclBlendIndicesType; /* D3DDECLTYPE of BLENDINDICES (4=D3DCOLOR, 5=UBYTE4) */

    /* Declaration cache: maps original decl -> cloned decl with UBYTE4 BLENDINDICES */
    void *skinDeclOrig[SKIN_DECL_CACHE_SIZE];
    void *skinDeclClone[SKIN_DECL_CACHE_SIZE];
    int   skinDeclCount;
#endif

    /* Vertex element tracking */
    int curDeclHasTexcoord;
    int curDeclHasNormal;
    int curDeclHasColor;
    int curDeclHasPosT;     /* 1 if current decl has POSITIONT (screen-space, skips FFP transform) */

#if ENABLE_SHADOW_SKIP
    int curDeclColorOffset; /* byte offset of COLOR0 in vertex, or -1 */
    int curDeclColorType;   /* D3DDECLTYPE of COLOR0 (3=FLOAT4, 4=D3DCOLOR) */
    ShadowCacheEntry shadowCache[SHADOW_CACHE_SIZE];
    int shadowSkipCount;    /* per-frame diagnostic counter */
    int skipFakeShadows;    /* from proxy.ini [FFP] SkipFakeShadows */
#endif

    /* Texcoord format for diagnostics and skinned vertex expansion */
    int curDeclTexcoordType; /* D3DDECLTYPE of TEXCOORD[0], or -1 if none */
    int curDeclTexcoordOff;  /* byte offset of TEXCOORD[0] in vertex */

    /* Texture tracking (stages 0-7) */
    void *curTexture[8];
    int albedoStage;

    /* Stream source tracking (streams 0-3) */
    void *streamVB[4];
    unsigned int streamOffset[4];
    unsigned int streamStride[4];

#if ENABLE_LIGHTS
    int lightsUpdatedThisFrame;
    int lightsEnabled;          /* from proxy.ini [Lights] Enabled */
    float lightIntensity;       /* from proxy.ini [Lights] IntensityPercent / 100 */
    int lightRangeMode;         /* from proxy.ini [Lights] RangeMode (0/1/2) */
    int lastEnabledLights;      /* number of lights enabled last frame (for cleanup) */
#endif

    /* Diagnostic state */
    void *loggedDecls[32];
    int loggedDeclCount;
    void *diagTexSeen[8][32];
    int diagTexUniq[8];
    unsigned int createTick;
    unsigned int diagLoggedFrames;
    unsigned int drawCallCount;
    unsigned int sceneCount;
    int vsConstWriteLog[256];
} WrappedDevice;

#define REAL(self) (((WrappedDevice*)(self))->pReal)
#define REAL_VT(self) (*(void***)(REAL(self)))

static __inline void** RealVtbl(WrappedDevice *self) {
    return *(void***)(self->pReal);
}

static __inline void shader_addref(void *pShader) {
    if (pShader) {
        typedef unsigned long (__stdcall *FN)(void*);
        ((FN)(*(void***)pShader)[1])(pShader);
    }
}
static __inline void shader_release(void *pShader) {
    if (pShader) {
        typedef unsigned long (__stdcall *FN)(void*);
        ((FN)(*(void***)pShader)[2])(pShader);
    }
}

/* ---- FFP State Setup ---- */

typedef struct {
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
} D3DMATERIAL9;

/*
 * Setup lighting for FFP mode.
 * Disables FFP lighting since vertex declarations typically lack normals
 * and RTX Remix handles lighting via ray tracing. Sets a white material
 * so unlit FFP output is visible.
 */
static void FFP_SetupLighting(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRenderState)(void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetMaterial)(void*, D3DMATERIAL9*);
    void **vt = RealVtbl(self);
    D3DMATERIAL9 mat;
    int i;

    ((FN_SetRenderState)vt[SLOT_SetRenderState])(self->pReal, D3DRS_LIGHTING, 0);

    for (i = 0; i < 4; i++) {
        mat.Diffuse[i] = 1.0f;
        mat.Ambient[i] = 1.0f;
        mat.Specular[i] = 0.0f;
        mat.Emissive[i] = 0.0f;
    }
    mat.Power = 0.0f;
    ((FN_SetMaterial)vt[SLOT_SetMaterial])(self->pReal, &mat);
}

/*
 * Setup texture stages for FFP mode.
 * Stage 0: modulate texture color with vertex/material diffuse.
 * Stage 1+: disabled.
 */
static void FFP_SetupTextureStages(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTSS)(void*, unsigned int, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_TEXCOORDINDEX, 0);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);

    /* Disable stages 1-7: the game binds shadow maps, LUTs, normal maps etc.
     * on higher stages for its pixel shaders. In FFP mode those stages become
     * active and Remix may consume the wrong textures. */
    {
        int s;
        for (s = 1; s <= 7; s++) {
            ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, s, D3DTSS_COLOROP, D3DTOP_DISABLE);
            ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    }
}

/* Transpose a 4x4 matrix (column-major -> row-major or vice versa) */
static void mat4_transpose(float *dst, const float *src) {
    dst[0]  = src[0];  dst[1]  = src[4];  dst[2]  = src[8];  dst[3]  = src[12];
    dst[4]  = src[1];  dst[5]  = src[5];  dst[6]  = src[9];  dst[7]  = src[13];
    dst[8]  = src[2];  dst[9]  = src[6];  dst[10] = src[10]; dst[11] = src[14];
    dst[12] = src[3];  dst[13] = src[7];  dst[14] = src[11]; dst[15] = src[15];
}

/* Returns true if a 4x4 matrix is non-zero and non-identity (worth logging) */
static int mat4_is_interesting(const float *m) {
    int all_zero = 1, i;
    for (i = 0; i < 16; i++) {
        if (m[i] != 0.0f) { all_zero = 0; break; }
    }
    if (all_zero) return 0;
    if (m[0]==1.0f && m[1]==0.0f && m[2]==0.0f  && m[3]==0.0f &&
        m[4]==0.0f && m[5]==1.0f && m[6]==0.0f  && m[7]==0.0f &&
        m[8]==0.0f && m[9]==0.0f && m[10]==1.0f && m[11]==0.0f &&
        m[12]==0.0f && m[13]==0.0f && m[14]==0.0f && m[15]==1.0f) return 0;
    return 1;
}

/* Log a 4x4 matrix row by row (for diagnostics) */
static void diag_log_matrix(const char *name, const float *m) {
    log_str(name);
    log_str(":\r\n");
    log_floats_dec("  row0: ", (float*)&m[0], 4);
    log_floats_dec("  row1: ", (float*)&m[4], 4);
    log_floats_dec("  row2: ", (float*)&m[8], 4);
    log_floats_dec("  row3: ", (float*)&m[12], 4);
}

/*
 * Inverse of a row-major 4x4 affine matrix (row3 = [0,0,0,1]).
 * Uses cofactor expansion of the upper-left 3x3 block.
 */
static void mat4_affine_inverse(float *out, const float *m) {
    float c00 = m[5]*m[10] - m[6]*m[9];
    float c01 = m[6]*m[8]  - m[4]*m[10];
    float c02 = m[4]*m[9]  - m[5]*m[8];
    float det = m[0]*c00 + m[1]*c01 + m[2]*c02;
    float inv;
    if (det > -1e-12f && det < 1e-12f) {
        static float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(out, id, 64);
        return;
    }
    inv = 1.0f / det;
    out[0]  = c00 * inv;
    out[4]  = c01 * inv;
    out[8]  = c02 * inv;
    out[1]  = (m[2]*m[9]  - m[1]*m[10]) * inv;
    out[5]  = (m[0]*m[10] - m[2]*m[8])  * inv;
    out[9]  = (m[1]*m[8]  - m[0]*m[9])  * inv;
    out[2]  = (m[1]*m[6]  - m[2]*m[5])  * inv;
    out[6]  = (m[2]*m[4]  - m[0]*m[6])  * inv;
    out[10] = (m[0]*m[5]  - m[1]*m[4])  * inv;
    out[3]  = -(out[0]*m[3] + out[1]*m[7] + out[2]*m[11]);
    out[7]  = -(out[4]*m[3] + out[5]*m[7] + out[6]*m[11]);
    out[11] = -(out[8]*m[3] + out[9]*m[7] + out[10]*m[11]);
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
}

/* Row-major 4x4 matrix multiply: out = a * b */
static void mat4_multiply(float *out, const float *a, const float *b) {
    float tmp[16];
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i*4+j] = a[i*4+0]*b[0*4+j] + a[i*4+1]*b[1*4+j]
                        + a[i*4+2]*b[2*4+j] + a[i*4+3]*b[3*4+j];
        }
    }
    memcpy(out, tmp, 64);
}

/*
 * Read a 4x4 matrix from the game's NiDX9Renderer at the given byte offset.
 * Returns pointer to 16 floats (row-major D3DXMATRIX), or NULL if renderer unavailable.
 */
static float* GameRenderer_GetMatrix(unsigned int byteOffset) {
    void *ren = GAME_RENDERER_SINGLETON;
    if (!ren) return NULL;
    return (float*)((unsigned char*)ren + byteOffset);
}

/*
 * Check if the current projection matrix is orthographic (2D/UI content).
 * Orthographic: m[3][3] == 1.0 and m[2][3] == 0.0 (no perspective divide).
 * D3DXMATRIX is row-major: m[row][col] = float[row*4 + col].
 *
 * From NewVegasRTXHelper Render.cpp:
 *   bool is2D = (ren->projMatrix.m[3][3] == 1.0f && ren->projMatrix.m[2][3] == 0.0f);
 */
static int GameRenderer_Is2D(void) {
    float *proj = GameRenderer_GetMatrix(RENDERER_PROJ_OFF);
    if (!proj) return 0;
    return (proj[15] == 1.0f && proj[11] == 0.0f);
}

/*
 * Check if the current draw is sky geometry by reading the shader type from
 * the game's NiDX9Renderer property state chain.
 *
 * From NewVegasRTXHelper Render.cpp:
 *   ren->m_pkCurrProp->m_spShadeProperty->m_eShaderType == kProp_Sky
 *
 * Property chain: NiDX9Renderer +0x0C → NiPropertyState +0x0C → NiShadeProperty +0x1C
 */
#define KPROP_SKY 0x0D
#define KPROP_NOLIGHTING 0x15
static int GameRenderer_IsSky(void) {
    void *ren = GAME_RENDERER_SINGLETON;
    void *propState, *shadeProp;
    unsigned int shaderType;
    if (!ren) return 0;
    propState = *(void**)((unsigned char*)ren + 0x0C);
    if (!propState) return 0;
    shadeProp = *(void**)((unsigned char*)propState + 0x0C);
    if (!shadeProp) return 0;
    shaderType = *(unsigned int*)((unsigned char*)shadeProp + 0x1C);
    return (shaderType == KPROP_SKY);
}

#if ENABLE_SHADOW_SKIP
/* Check if the current draw uses BSShaderNoLightingProperty (kProp_NoLighting = 0x15).
 * Same property chain as GameRenderer_IsSky. */
static int GameRenderer_IsNoLighting(void) {
    void *ren = GAME_RENDERER_SINGLETON;
    void *propState, *shadeProp;
    unsigned int shaderType;
    if (!ren) return 0;
    propState = *(void**)((unsigned char*)ren + 0x0C);
    if (!propState) return 0;
    shadeProp = *(void**)((unsigned char*)propState + 0x0C);
    if (!shadeProp) return 0;
    shaderType = *(unsigned int*)((unsigned char*)shadeProp + 0x1C);
    return (shaderType == KPROP_NOLIGHTING);
}
#endif

/*
 * Apply transforms by reading the game's NiDX9Renderer matrices directly.
 *
 * The renderer stores pre-computed row-major World, View, and Projection
 * matrices that can be passed straight to SetTransform. This replaces the
 * previous approach of decomposing VS constants (WVP / World) via matrix
 * inversion, matching how NewVegasRTXHelper applies transforms:
 *
 *   dev->SetTransform(D3DTS_WORLD, &ren->worldMatrix);
 *   dev->SetTransform(D3DTS_VIEW,  &ren->viewMatrix);
 *   dev->SetTransform(D3DTS_PROJECTION, &ren->projMatrix);
 *
 * Key improvements over VS-constant decomposition:
 *   - No matrix inversion (no precision loss, no singular-matrix failures)
 *   - Separate View matrix (Remix uses it for camera position in ray tracing)
 *   - Separate Projection (correct near/far plane info for Remix)
 */
static void FFP_ApplyTransforms(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
    void **vt = RealVtbl(self);
    float *world = GameRenderer_GetMatrix(RENDERER_WORLD_OFF);
    float *view  = GameRenderer_GetMatrix(RENDERER_VIEW_OFF);
    float *proj  = GameRenderer_GetMatrix(RENDERER_PROJ_OFF);

    if (world && view && proj) {
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, world);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, view);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, proj);
    }
}

#if ENABLE_SKINNING
#include "d3d9_skinning.h"
#endif

#if ENABLE_LIGHTS
#include "d3d9_lights.h"
#endif

/* ---- Fake shadow detection ---- */

#if ENABLE_SHADOW_SKIP
/*
 * Check if all vertex colors in a buffer region are shades of gray.
 * Returns SHADOW_IS_FAKE if all verts have R ≈ G ≈ B (within tolerance)
 * and at least one vertex is darker than SHADOW_DARK_THRESHOLD.
 */
static int CheckVertexColorsGray(unsigned char *vbData, unsigned int numVerts,
                                  unsigned int stride, unsigned int colorOffset,
                                  int colorType)
{
    unsigned int v;
    int hasDark = 0;

    for (v = 0; v < numVerts; v++) {
        unsigned char *vert = vbData + v * stride;
        int r, g, b;

        if (colorType == D3DDECLTYPE_D3DCOLOR) {
            unsigned char *c = vert + colorOffset;
            b = c[0]; g = c[1]; r = c[2];
        } else if (colorType == D3DDECLTYPE_FLOAT4) {
            float *c = (float*)(vert + colorOffset);
            r = (int)(c[0] * 255.0f + 0.5f);
            g = (int)(c[1] * 255.0f + 0.5f);
            b = (int)(c[2] * 255.0f + 0.5f);
        } else {
            return SHADOW_NOT_FAKE;
        }

        {
            int dRG, dRB, dGB;
            dRG = r - g; if (dRG < 0) dRG = -dRG;
            dRB = r - b; if (dRB < 0) dRB = -dRB;
            dGB = g - b; if (dGB < 0) dGB = -dGB;
            if (dRG > SHADOW_GRAY_TOLERANCE || dRB > SHADOW_GRAY_TOLERANCE ||
                dGB > SHADOW_GRAY_TOLERANCE) {
                return SHADOW_NOT_FAKE;
            }
        }

        if (r < SHADOW_DARK_THRESHOLD)
            hasDark = 1;
    }

    return hasDark ? SHADOW_IS_FAKE : SHADOW_NOT_FAKE;
}

/*
 * Two-step fake shadow detection:
 *   1. Is the current draw NOLIGHTING? (NiShadeProperty type check)
 *   2. Are all vertex colors grayscale with at least one dark vertex?
 * Caches results per (VB, baseVertexIndex) to avoid repeated VB locks.
 */
static int IsFakeShadow(WrappedDevice *self, int baseVertexIndex, unsigned int numVerts) {
    void *vb;
    unsigned int slot;
    ShadowCacheEntry *entry;

    if (!self->skipFakeShadows) return 0;
    if (!GameRenderer_IsNoLighting()) return 0;
    if (!self->curDeclHasColor || self->curDeclColorOffset < 0) return 0;

    vb = self->streamVB[0];
    if (!vb || self->streamStride[0] == 0 || numVerts == 0) return 0;

    /* Direct-mapped cache lookup */
    slot = (((unsigned int)(uintptr_t)vb >> 4) ^ (unsigned int)baseVertexIndex) & (SHADOW_CACHE_SIZE - 1);
    entry = &self->shadowCache[slot];

    if (entry->vb == vb && entry->baseVertexIndex == baseVertexIndex) {
        return (entry->result == SHADOW_IS_FAKE);
    }

    /* Cache miss — lock VB read-only and check vertex colors */
    {
        typedef int (__stdcall *FN_Lock)(void*, unsigned int, unsigned int, void**, unsigned int);
        typedef int (__stdcall *FN_Unlock)(void*);
        void **vbVt = *(void***)vb;
        unsigned char *vbData = NULL;
        unsigned int stride = self->streamStride[0];
        unsigned int readOff = self->streamOffset[0] + (unsigned int)baseVertexIndex * stride;
        unsigned int readSize = numVerts * stride;
        int lockHr, result;

        lockHr = ((FN_Lock)vbVt[11])(vb, readOff, readSize, (void**)&vbData, 0x10 /*D3DLOCK_READONLY*/);
        if (lockHr != 0 || !vbData) return 0;

        result = CheckVertexColorsGray(vbData, numVerts, stride,
                                        self->curDeclColorOffset, self->curDeclColorType);

        ((FN_Unlock)vbVt[12])(vb);

        entry->vb = vb;
        entry->baseVertexIndex = baseVertexIndex;
        entry->result = result;

#if DIAG_ENABLED
        if (DIAG_ACTIVE(self)) {
            log_hex("  NOLIGHTING draw: vb=", (unsigned int)(uintptr_t)vb);
            log_int("    bvi=", baseVertexIndex);
            log_int("    numVerts=", numVerts);
            log_int("    result=", result);
        }
#endif
        return (result == SHADOW_IS_FAKE);
    }
}
#endif /* ENABLE_SHADOW_SKIP */

/*
 * Enter FFP mode: NULL both shaders on the real device, apply transforms,
 * setup texture stages and lighting. Stays in FFP between consecutive
 * DIP calls to avoid redundant state switches.
 */
static void FFP_Engage(WrappedDevice *self) {
    void **vt = RealVtbl(self);

    if (!self->ffpActive) {
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, NULL);
        ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, NULL);
        self->ffpActive = 1;
    }

    FFP_ApplyTransforms(self);
    FFP_SetupTextureStages(self);

    if (!self->ffpSetup) {
        FFP_SetupLighting(self);
        self->ffpSetup = 1;
    }
}

/* Leave FFP mode: restore game's shaders, reset world transform to identity.
 * Matches NewVegasRTXHelper post_drawindexedprim() cleanup. */
static void FFP_Disengage(WrappedDevice *self) {
    if (self->ffpActive) {
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
        static float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        void **vt = RealVtbl(self);

#if ENABLE_SKINNING
        FFP_DisableSkinning(self);
#endif

        ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, self->lastVS);
        ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, self->lastPS);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, identity);
        self->ffpActive = 0;
    }
}

/* ---- Vtable method implementations ---- */

static void *s_device_vtbl[DEVICE_VTABLE_SIZE];

/* 0: QueryInterface */
static int __stdcall WD_QueryInterface(WrappedDevice *self, void *riid, void **ppv) {
    typedef int (__stdcall *FN)(void*, void*, void**);
    return ((FN)RealVtbl(self)[0])(self->pReal, riid, ppv);
}

/* 1: AddRef */
static unsigned long __stdcall WD_AddRef(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN)(void*);
    self->refCount++;
    return ((FN)RealVtbl(self)[1])(self->pReal);
}

/* 2: Release */
static unsigned long __stdcall WD_Release(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN)(void*);
    unsigned long rc = ((FN)RealVtbl(self)[2])(self->pReal);
    self->refCount--;
    if (self->refCount <= 0) {
        log_str("WrappedDevice released\r\n");
        shader_release(self->lastVS);
        shader_release(self->lastPS);
        self->lastVS = NULL;
        self->lastPS = NULL;
#if ENABLE_SKINNING
        Skin_ReleaseDevice(self);
#endif
        HeapFree(GetProcessHeap(), 0, self);
    }
    return rc;
}

/* ---- Relay thunks for non-intercepted methods ---- */

#ifdef _MSC_VER
/* MSVC x86 naked thunks: replace 'this' with pReal and jump to real vtable */
#define RELAY_THUNK(name, slot) \
    static __declspec(naked) void __stdcall name(void) { \
        __asm { mov eax, [esp+4] }      /* eax = WrappedDevice* */ \
        __asm { mov ecx, [eax+4] }      /* ecx = pReal */ \
        __asm { mov [esp+4], ecx }      /* replace this */ \
        __asm { mov eax, [ecx] }        /* eax = real vtable */ \
        __asm { jmp dword ptr [eax + slot*4] } \
    }

RELAY_THUNK(Relay_03, 3)    /* TestCooperativeLevel */
RELAY_THUNK(Relay_04, 4)    /* GetAvailableTextureMem */
RELAY_THUNK(Relay_05, 5)    /* EvictManagedResources */
RELAY_THUNK(Relay_06, 6)    /* GetDirect3D */
RELAY_THUNK(Relay_07, 7)    /* GetDeviceCaps */
RELAY_THUNK(Relay_08, 8)    /* GetDisplayMode */
RELAY_THUNK(Relay_09, 9)    /* GetCreationParameters */
RELAY_THUNK(Relay_10, 10)   /* SetCursorProperties */
RELAY_THUNK(Relay_11, 11)   /* SetCursorPosition */
RELAY_THUNK(Relay_12, 12)   /* ShowCursor */
RELAY_THUNK(Relay_13, 13)   /* CreateAdditionalSwapChain */
RELAY_THUNK(Relay_14, 14)   /* GetSwapChain */
RELAY_THUNK(Relay_15, 15)   /* GetNumberOfSwapChains */
RELAY_THUNK(Relay_18, 18)   /* GetBackBuffer */
RELAY_THUNK(Relay_19, 19)   /* GetRasterStatus */
RELAY_THUNK(Relay_20, 20)   /* SetDialogBoxMode */
RELAY_THUNK(Relay_21, 21)   /* SetGammaRamp */
RELAY_THUNK(Relay_22, 22)   /* GetGammaRamp */
RELAY_THUNK(Relay_23, 23)   /* CreateTexture */
RELAY_THUNK(Relay_24, 24)   /* CreateVolumeTexture */
RELAY_THUNK(Relay_25, 25)   /* CreateCubeTexture */
RELAY_THUNK(Relay_26, 26)   /* CreateVertexBuffer */
RELAY_THUNK(Relay_27, 27)   /* CreateIndexBuffer */
RELAY_THUNK(Relay_28, 28)   /* CreateRenderTarget */
RELAY_THUNK(Relay_29, 29)   /* CreateDepthStencilSurface */
RELAY_THUNK(Relay_30, 30)   /* UpdateSurface */
RELAY_THUNK(Relay_31, 31)   /* UpdateTexture */
RELAY_THUNK(Relay_32, 32)   /* GetRenderTargetData */
RELAY_THUNK(Relay_33, 33)   /* GetFrontBufferData */
RELAY_THUNK(Relay_34, 34)   /* StretchRect */
RELAY_THUNK(Relay_35, 35)   /* ColorFill */
RELAY_THUNK(Relay_36, 36)   /* CreateOffscreenPlainSurface */
RELAY_THUNK(Relay_37, 37)   /* SetRenderTarget */
RELAY_THUNK(Relay_38, 38)   /* GetRenderTarget */
RELAY_THUNK(Relay_39, 39)   /* SetDepthStencilSurface */
RELAY_THUNK(Relay_40, 40)   /* GetDepthStencilSurface */
RELAY_THUNK(Relay_43, 43)   /* Clear */
RELAY_THUNK(Relay_44, 44)   /* SetTransform */
RELAY_THUNK(Relay_45, 45)   /* GetTransform */
RELAY_THUNK(Relay_46, 46)   /* MultiplyTransform */
RELAY_THUNK(Relay_47, 47)   /* SetViewport */
RELAY_THUNK(Relay_48, 48)   /* GetViewport */
RELAY_THUNK(Relay_49, 49)   /* SetMaterial */
RELAY_THUNK(Relay_50, 50)   /* GetMaterial */
RELAY_THUNK(Relay_51, 51)   /* SetLight */
RELAY_THUNK(Relay_52, 52)   /* GetLight */
RELAY_THUNK(Relay_53, 53)   /* LightEnable */
RELAY_THUNK(Relay_54, 54)   /* GetLightEnable */
RELAY_THUNK(Relay_55, 55)   /* SetClipPlane */
RELAY_THUNK(Relay_56, 56)   /* GetClipPlane */
RELAY_THUNK(Relay_57, 57)   /* SetRenderState */
RELAY_THUNK(Relay_58, 58)   /* GetRenderState */
RELAY_THUNK(Relay_59, 59)   /* CreateStateBlock */
RELAY_THUNK(Relay_60, 60)   /* BeginStateBlock */
RELAY_THUNK(Relay_61, 61)   /* EndStateBlock */
RELAY_THUNK(Relay_62, 62)   /* SetClipStatus */
RELAY_THUNK(Relay_63, 63)   /* GetClipStatus */
RELAY_THUNK(Relay_64, 64)   /* GetTexture */
RELAY_THUNK(Relay_66, 66)   /* GetTextureStageState */
RELAY_THUNK(Relay_67, 67)   /* SetTextureStageState */
RELAY_THUNK(Relay_68, 68)   /* GetSamplerState */
RELAY_THUNK(Relay_69, 69)   /* SetSamplerState */
RELAY_THUNK(Relay_70, 70)   /* ValidateDevice */
RELAY_THUNK(Relay_71, 71)   /* SetPaletteEntries */
RELAY_THUNK(Relay_72, 72)   /* GetPaletteEntries */
RELAY_THUNK(Relay_73, 73)   /* SetCurrentTexturePalette */
RELAY_THUNK(Relay_74, 74)   /* GetCurrentTexturePalette */
RELAY_THUNK(Relay_75, 75)   /* SetScissorRect */
RELAY_THUNK(Relay_76, 76)   /* GetScissorRect */
RELAY_THUNK(Relay_77, 77)   /* SetSoftwareVertexProcessing */
RELAY_THUNK(Relay_78, 78)   /* GetSoftwareVertexProcessing */
RELAY_THUNK(Relay_79, 79)   /* SetNPatchMode */
RELAY_THUNK(Relay_80, 80)   /* GetNPatchMode */
RELAY_THUNK(Relay_83, 83)   /* DrawPrimitiveUP */
RELAY_THUNK(Relay_84, 84)   /* DrawIndexedPrimitiveUP */
RELAY_THUNK(Relay_85, 85)   /* ProcessVertices */
RELAY_THUNK(Relay_86, 86)   /* CreateVertexDeclaration */
RELAY_THUNK(Relay_88, 88)   /* GetVertexDeclaration */
RELAY_THUNK(Relay_89, 89)   /* SetFVF */
RELAY_THUNK(Relay_90, 90)   /* GetFVF */
RELAY_THUNK(Relay_91, 91)   /* CreateVertexShader */
RELAY_THUNK(Relay_93, 93)   /* GetVertexShader */
RELAY_THUNK(Relay_95, 95)   /* GetVertexShaderConstantF */
RELAY_THUNK(Relay_96, 96)   /* SetVertexShaderConstantI */
RELAY_THUNK(Relay_97, 97)   /* GetVertexShaderConstantI */
RELAY_THUNK(Relay_98, 98)   /* SetVertexShaderConstantB */
RELAY_THUNK(Relay_99, 99)   /* GetVertexShaderConstantB */
RELAY_THUNK(Relay_101, 101) /* GetStreamSource */
RELAY_THUNK(Relay_102, 102) /* SetStreamSourceFreq */
RELAY_THUNK(Relay_103, 103) /* GetStreamSourceFreq */
RELAY_THUNK(Relay_104, 104) /* SetIndices */
RELAY_THUNK(Relay_105, 105) /* GetIndices */
RELAY_THUNK(Relay_106, 106) /* CreatePixelShader */
RELAY_THUNK(Relay_108, 108) /* GetPixelShader */
RELAY_THUNK(Relay_110, 110) /* GetPixelShaderConstantF */
RELAY_THUNK(Relay_111, 111) /* SetPixelShaderConstantI */
RELAY_THUNK(Relay_112, 112) /* GetPixelShaderConstantI */
RELAY_THUNK(Relay_113, 113) /* SetPixelShaderConstantB */
RELAY_THUNK(Relay_114, 114) /* GetPixelShaderConstantB */
RELAY_THUNK(Relay_115, 115) /* DrawRectPatch */
RELAY_THUNK(Relay_116, 116) /* DrawTriPatch */
RELAY_THUNK(Relay_117, 117) /* DeletePatch */
RELAY_THUNK(Relay_118, 118) /* CreateQuery */

#else
#error "Only MSVC x86 is supported (needs __declspec(naked) + inline asm)"
#endif

/* ---- Intercepted method implementations ---- */

/* 16: Reset — invalidates all resources */
static int __stdcall WD_Reset(WrappedDevice *self, void *pPresentParams) {
    typedef int (__stdcall *FN)(void*, void*);
    int hr;

    log_str("== Device Reset ==\r\n");

    shader_release(self->lastVS);
    shader_release(self->lastPS);
    self->lastVS = NULL;
    self->lastPS = NULL;
    self->viewProjValid = 0;
    self->ffpSetup = 0;
    self->worldDirty = 0;
    self->viewProjDirty = 0;
    self->psConstDirty = 0;
    self->ffpActive = 0;
#if ENABLE_SKINNING
    self->numBones = 0;
    self->prevNumBones = 0;
    self->bonesDrawn = 0;
    self->skinningSetup = 0;
#endif
#if ENABLE_LIGHTS
    self->lightsUpdatedThisFrame = 0;
    self->lastEnabledLights = 0;
#endif
    hr = ((FN)RealVtbl(self)[SLOT_Reset])(self->pReal, pPresentParams);
    log_hex("  Reset hr=", hr);
    return hr;
}

/* 17: Present */
static int __stdcall WD_Present(WrappedDevice *self, void *a, void *b, void *c, void *d) {
    typedef int (__stdcall *FN)(void*, void*, void*, void*, void*);
    int hr;

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_str("==== PRESENT frame ");
        log_int("", self->frameCount);
        log_int("  diagFrame: ", self->diagLoggedFrames);
        log_int("  drawCalls: ", self->drawCallCount);
        log_int("  scenes: ", self->sceneCount);
#if ENABLE_SHADOW_SKIP
        log_int("  shadowSkipped: ", self->shadowSkipCount);
#endif
        {
            int r;
            log_str("  VS regs written: ");
            for (r = 0; r < 256; r++) {
                if (self->vsConstWriteLog[r]) {
                    log_int("c", r);
                }
            }
            log_str("\r\n");
        }
        {
            int ts;
            log_str("  Unique textures per stage:\r\n");
            for (ts = 0; ts < 8; ts++) {
                if (self->diagTexUniq[ts] > 0) {
                    log_int("    stage ", ts);
                    log_int("      unique=", self->diagTexUniq[ts]);
                }
            }
        }
        self->diagLoggedFrames++;
        { int ts; for (ts = 0; ts < 8; ts++) self->diagTexUniq[ts] = 0; }
    }
#endif

    self->frameCount++;
    self->ffpSetup = 0;
    self->drawCallCount = 0;
    self->sceneCount = 0;
#if ENABLE_SHADOW_SKIP
    self->shadowSkipCount = 0;
#endif
#if ENABLE_LIGHTS
    self->lightsUpdatedThisFrame = 0;
#endif
    FFP_Disengage(self);
    {
        int r;
        for (r = 0; r < 256; r++) self->vsConstWriteLog[r] = 0;
    }
    hr = ((FN)RealVtbl(self)[SLOT_Present])(self->pReal, a, b, c, d);

    return hr;
}

/* 41: BeginScene */
static int __stdcall WD_BeginScene(WrappedDevice *self) {
    typedef int (__stdcall *FN)(void*);
    self->ffpSetup = 0;
    self->sceneCount++;
#if ENABLE_LIGHTS
    FFP_UpdateLights(self);
#endif
#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_str("-- BeginScene #");
        log_int("", self->sceneCount);
    }
#endif
    return ((FN)RealVtbl(self)[SLOT_BeginScene])(self->pReal);
}

/* 42: EndScene */
static int __stdcall WD_EndScene(WrappedDevice *self) {
    typedef int (__stdcall *FN)(void*);
    return ((FN)RealVtbl(self)[SLOT_EndScene])(self->pReal);
}

/* 81: DrawPrimitive — GAME-SPECIFIC draw routing for non-indexed draws */
static int __stdcall WD_DrawPrimitive(WrappedDevice *self, unsigned int pt, unsigned int sv, unsigned int pc) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int);
    int hr;
    self->drawCallCount++;

    if (self->viewProjValid && self->lastDecl && !self->curDeclHasPosT && !self->curDeclIsSkinned
        && (self->curDeclHasNormal || GameRenderer_IsSky()) && !GameRenderer_Is2D()) {
#if ENABLE_SHADOW_SKIP
        if (IsFakeShadow(self, (int)sv, pc * 3)) {
            self->shadowSkipCount++;
            hr = 0; /* S_OK — skip fake shadow */
        } else
#endif
        {
        /* World-space non-indexed draw or sky: engage FFP */
        FFP_Engage(self);
        {
            typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
            int as = self->albedoStage;
            void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
            int ts;
            ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, 0, albedo);
            for (ts = 1; ts < 8; ts++)
                ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, NULL);
        }
        hr = ((FN)RealVtbl(self)[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
        /* Restore original texture bindings */
        {
            typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
            int ts;
            for (ts = 0; ts < 8; ts++)
                ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        }
        }
    } else {
        /* Passthrough: POSITIONT / no decl / pre-viewProj / skinned */
        FFP_Disengage(self);
        hr = ((FN)RealVtbl(self)[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
    }

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self) && self->drawCallCount <= 200) {
        log_int("  DP #", self->drawCallCount);
        log_int("    type=", pt);
        log_int("    startVtx=", sv);
        log_int("    primCount=", pc);
        log_hex("    hr=", hr);
    }
#endif
    return hr;
}

/* 82: DrawIndexedPrimitive — GAME-SPECIFIC draw routing (see prompt for decision tree) */
static int __stdcall WD_DrawIndexedPrimitive(WrappedDevice *self,
    unsigned int pt, int bvi, unsigned int mi, unsigned int nv,
    unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    int hr;
    self->drawCallCount++;

    if (self->viewProjValid) {
        /* 2D content (orthographic projection) — Pip-Boy, menus, UI overlays.
         * Skip FFP entirely so it renders with the game's original shaders.
         * From NewVegasRTXHelper: is2D = (projMatrix.m[3][3]==1.0 && projMatrix.m[2][3]==0.0) */
        if (GameRenderer_Is2D()) {
            FFP_Disengage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
#if ENABLE_SHADOW_SKIP
        } else if (IsFakeShadow(self, bvi, nv)) {
            /* NOLIGHTING + all-gray vertex colors = fake baked shadow.
             * Skip — Remix ray-traces real shadows. */
            FFP_Disengage(self);
            self->shadowSkipCount++;
            hr = 0; /* S_OK */
#endif
        } else if (self->curDeclIsSkinned) {
#if ENABLE_SKINNING
            hr = Skin_DrawDIP(self, pt, bvi, mi, nv, si, pc);
#else
            /* Skinned meshes pass through with original shaders (includes viewmodels) */
            FFP_Disengage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
#endif
        } else if (self->curDeclHasPosT) {
            /* POSITIONT = screen-space pre-transformed → passthrough */
            FFP_Disengage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        } else if (GameRenderer_IsSky()) {
            /* Sky geometry: FFP so Remix can handle it as path-traced geometry.
             * Detected via NiShadeProperty::m_eShaderType == kProp_Sky (0x0D).
             * Matches NewVegasRTXHelper Render.cpp sky detection. */
            FFP_Engage(self);
#if ENABLE_SKINNING
            if (self->skinningSetup) {
                FFP_DisableSkinning(self);
            }
#endif
            {
                typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
                int as = self->albedoStage;
                void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
                int ts;
                ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, 0, albedo);
                for (ts = 1; ts < 8; ts++)
                    ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, NULL);
            }
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            {
                typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
                int ts;
                for (ts = 0; ts < 8; ts++)
                    ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
            }
        } else if (!self->curDeclHasNormal) {
            /* No NORMAL = LOD terrain, effects, post-process.
             * Pass through with original shaders — Remix only path-traces
             * FFP geometry, so these are invisible to it. HQ terrain (has NORMAL)
             * is the only terrain Remix sees. */
            FFP_Disengage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        } else {
            /* World geometry with NORMAL: engage FFP for Remix path tracing */
            FFP_Engage(self);
#if ENABLE_SKINNING
            if (self->skinningSetup) {
                FFP_DisableSkinning(self);
            }
#endif
            /* Rebind albedo to stage 0, NULL stages 1-7 */
            {
                typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
                int as = self->albedoStage;
                void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
                int ts;
                ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, 0, albedo);
                for (ts = 1; ts < 8; ts++)
                    ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, NULL);
            }

            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
                bvi, mi, nv, si, pc);

            /* Restore all original texture bindings */
            {
                typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
                int ts;
                for (ts = 0; ts < 8; ts++)
                    ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
            }
        }
    } else {
        /* Transforms not ready yet, pass through with shaders */
        hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
    }

#if DIAG_ENABLED
    /* Track unique textures per stage for this frame */
    if (DIAG_ACTIVE(self)) {
        int ts;
        for (ts = 0; ts < 8; ts++) {
            if (self->curTexture[ts]) {
                int found = 0, k;
                for (k = 0; k < self->diagTexUniq[ts] && k < 32; k++) {
                    if (self->diagTexSeen[ts][k] == self->curTexture[ts]) { found = 1; break; }
                }
                if (!found && self->diagTexUniq[ts] < 32) {
                    self->diagTexSeen[ts][self->diagTexUniq[ts]] = self->curTexture[ts];
                    self->diagTexUniq[ts]++;
                }
            }
        }
    }
    if (DIAG_ACTIVE(self) && self->drawCallCount <= 200) {
        log_int("  DIP #", self->drawCallCount);
        log_hex("    decl=", (unsigned int)self->lastDecl);
        log_int("    type=", pt);
        log_int("    baseVtx=", bvi);
        log_int("    numVerts=", nv);
        log_int("    primCount=", pc);
        log_hex("    hr=", hr);
        log_int("    stride0=", self->streamStride[0]);
        log_int("    stride1=", self->streamStride[1]);
        if (self->curDeclIsSkinned) {
            log_str("    [SKINNED]\r\n");
#if ENABLE_SKINNING
            log_int("    numBones=", self->numBones);
#endif
        }
        log_int("    hasNormal=", self->curDeclHasNormal);
        log_int("    hasTexcoord=", self->curDeclHasTexcoord);
        log_int("    tcType=", self->curDeclTexcoordType);
        {
            int ts;
            for (ts = 0; ts < 8; ts++) {
                if (self->curTexture[ts]) {
                    log_int("    tex", ts);
                    log_hex("     =", (unsigned int)self->curTexture[ts]);
                }
            }
        }
        /* Log raw bytes of first vertex for early calls (helps diagnose vertex layout) */
        if (self->drawCallCount <= 10 && self->streamVB[0] && self->streamStride[0] > 0) {
            typedef int (__stdcall *FN_Lock)(void*, unsigned int, unsigned int, void**, unsigned int);
            typedef int (__stdcall *FN_Unlock)(void*);
            void **vbVt = *(void***)self->streamVB[0];
            unsigned char *vbData = NULL;
            unsigned int readOff = self->streamOffset[0] + (unsigned int)bvi * self->streamStride[0];
            int lockHr = ((FN_Lock)vbVt[11])(self->streamVB[0], readOff, self->streamStride[0] * 2, (void**)&vbData, 0x10 /*READONLY*/);
            if (lockHr == 0 && vbData) {
                unsigned int stride = self->streamStride[0];
                unsigned int b;
                log_int("    vtx0 raw (", stride);
                log_str(" bytes):\r\n      ");
                for (b = 0; b < stride && b < 64; b++) {
                    const char *hex = "0123456789ABCDEF";
                    char hx[4];
                    hx[0] = hex[(vbData[b] >> 4) & 0xF];
                    hx[1] = hex[vbData[b] & 0xF];
                    hx[2] = ' '; hx[3] = 0;
                    log_str(hx);
                    if (b == 11 || b == 15 || b == 19 || b == 23 || b == 27 || b == 31) log_str("| ");
                }
                log_str("\r\n");
                if (stride >= 12) {
                    float *fp = (float*)vbData;
                    log_floats_dec("      pos: ", fp, 3);
                }
                ((FN_Unlock)vbVt[12])(self->streamVB[0]);
            }
        }
        /* Log key VS constant register blocks on first 5 calls */
        if (self->drawCallCount <= 5) {
            if (mat4_is_interesting(&self->vsConst[0]))        diag_log_matrix("    c0-c3",   &self->vsConst[0]);
            if (mat4_is_interesting(&self->vsConst[4*4]))      diag_log_matrix("    c4-c7",   &self->vsConst[4*4]);
            if (mat4_is_interesting(&self->vsConst[8*4]))      diag_log_matrix("    c8-c11",  &self->vsConst[8*4]);
            if (mat4_is_interesting(&self->vsConst[12*4]))     diag_log_matrix("    c12-c15", &self->vsConst[12*4]);
            if (mat4_is_interesting(&self->vsConst[16*4]))     diag_log_matrix("    c16-c19", &self->vsConst[16*4]);
            if (mat4_is_interesting(&self->vsConst[20*4]))     diag_log_matrix("    c20-c23", &self->vsConst[20*4]);
            if (mat4_is_interesting(&self->vsConst[36*4]))     diag_log_matrix("    c36-c39", &self->vsConst[36*4]);
#if ENABLE_SKINNING
            if (self->curDeclIsSkinned && self->numBones > 0) {
                log_int("    bones uploaded=", self->numBones);
            }
#endif
        }
    }
#endif
    return hr;
}

/* 92: SetVertexShader */
static int __stdcall WD_SetVertexShader(WrappedDevice *self, void *pShader) {
    typedef int (__stdcall *FN)(void*, void*);
#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_hex("  SetVS shader=", (unsigned int)pShader);
    }
#endif
    shader_addref(pShader);
    shader_release(self->lastVS);
    self->lastVS = pShader;
    self->ffpActive = 0;
    return ((FN)RealVtbl(self)[SLOT_SetVertexShader])(self->pReal, pShader);
}

/* 94: SetVertexShaderConstantF — GAME-SPECIFIC: dirty tracking uses VS_REG_* defines */
static int __stdcall WD_SetVertexShaderConstantF(WrappedDevice *self,
    unsigned int startReg, float *pData, unsigned int count)
{
    typedef int (__stdcall *FN)(void*, unsigned int, float*, unsigned int);
    unsigned int i;

    if (pData && startReg + count <= 256) {
        for (i = 0; i < count * 4; i++) {
            self->vsConst[(startReg * 4) + i] = pData[i];
        }

        /* Dirty tracking keyed to the game-specific register layout */
        {
            unsigned int endReg = startReg + count;
            if (startReg < VS_REG_PROJ_END && endReg > VS_REG_VIEW_START)
                self->viewProjDirty = 1;
            if (startReg < VS_REG_WORLD_END && endReg > VS_REG_WORLD_START)
                self->worldDirty = 1;
        }

        /* Mark View+Proj valid once both ranges have been written */
        if (startReg <= VS_REG_PROJ_START && startReg + count >= VS_REG_PROJ_END)
            self->viewProjValid = 1;
        else if (startReg == VS_REG_VIEW_START && count >= 4 && self->vsConstWriteLog[VS_REG_PROJ_START])
            self->viewProjValid = 1;
        else if (startReg == VS_REG_PROJ_START && count >= 4 && self->vsConstWriteLog[VS_REG_VIEW_START])
            self->viewProjValid = 1;

        for (i = 0; i < count; i++) {
            if (startReg + i < 256)
                self->vsConstWriteLog[startReg + i] = 1;
        }

#if ENABLE_SKINNING
        /* Immediate bone upload — matches NewVegasRTXHelper Device.cpp exactly.
         * When Vector4fCount==3 during skinned rendering, each write is one bone
         * (3x4 packed matrix). Transpose to 4x4 and upload via SetTransform
         * immediately, using a sequential counter (no register threshold).
         *
         * Bone detection gating:
         *   - Primary: g_renderSkinned (from game engine hook at 0xB99598)
         *   - Fallback: curDeclIsSkinned (vertex declaration heuristic)
         *
         * Per-object reset:
         *   - Primary: g_boneResetPending (from game engine hook at 0xB991E7)
         *   - Fallback: bonesDrawn (first bone write after a draw used bones) */
        if (count == VS_REGS_PER_BONE &&
            (g_hooksInstalled ? g_renderSkinned : self->curDeclIsSkinned)) {
            /* New bone batch: reset counter on per-object boundary or after draw */
            if (g_boneResetPending || self->bonesDrawn) {
                /* Clear stale bone matrices from previous object.
                 * Without this, if Object B uploads fewer bones than Object A,
                 * vertices referencing bone indices >= B's count get transformed
                 * by A's stale matrices, stretching geometry across the map. */
                {
                    typedef int (__stdcall *FN_ST)(void*, unsigned int, float*);
                    static float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                    int slot;
                    for (slot = 0; slot < self->numBones; slot++) {
                        ((FN_ST)RealVtbl(self)[SLOT_SetTransform])(
                            self->pReal, D3DTS_WORLDMATRIX(slot), ident);
                    }
                }
                self->prevNumBones = self->numBones;
                self->numBones = 0;
                self->bonesDrawn = 0;
                g_boneResetPending = 0;
            }
            if (self->numBones < MAX_FFP_BONES) {
                typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
                float boneMat[16];
                /* Build 4x4 from 4x3 packed (transpose column->row major) */
                boneMat[0]  = pData[0];  boneMat[1]  = pData[4];  boneMat[2]  = pData[8];   boneMat[3]  = 0.0f;
                boneMat[4]  = pData[1];  boneMat[5]  = pData[5];  boneMat[6]  = pData[9];   boneMat[7]  = 0.0f;
                boneMat[8]  = pData[2];  boneMat[9]  = pData[6];  boneMat[10] = pData[10];  boneMat[11] = 0.0f;
                boneMat[12] = pData[3];  boneMat[13] = pData[7];  boneMat[14] = pData[11];  boneMat[15] = 1.0f;

                ((FN_SetTransform)RealVtbl(self)[SLOT_SetTransform])(self->pReal,
                    D3DTS_WORLDMATRIX(self->numBones), boneMat);
                self->numBones++;
            }
        }
#endif

#if DIAG_ENABLED
        if (DIAG_ACTIVE(self)) {
            log_int("  SetVSConstF start=", startReg);
            log_int("    count=", count);
            if (count == 16) {
                /* 16-register pack: log as 4 separate 4x4 matrices */
                char label[32];
                int m;
                for (m = 0; m < 4; m++) {
                    int s = startReg + m * 4, e2 = s + 3, p2 = 0;
                    label[p2++] = 'c';
                    if (s >= 100) label[p2++] = '0' + (s / 100);
                    if (s >= 10)  label[p2++] = '0' + ((s / 10) % 10);
                    label[p2++] = '0' + (s % 10);
                    label[p2++] = '-'; label[p2++] = 'c';
                    if (e2 >= 100) label[p2++] = '0' + (e2 / 100);
                    if (e2 >= 10)  label[p2++] = '0' + ((e2 / 10) % 10);
                    label[p2++] = '0' + (e2 % 10);
                    label[p2] = '\0';
                    diag_log_matrix(label, &pData[m * 16]);
                }
            } else if (count == 4) {
                char label[32];
                int s = startReg, e2 = startReg + 3, p2 = 0;
                label[p2++] = 'c';
                if (s >= 100) label[p2++] = '0' + (s / 100);
                if (s >= 10)  label[p2++] = '0' + ((s / 10) % 10);
                label[p2++] = '0' + (s % 10);
                label[p2++] = '-'; label[p2++] = 'c';
                if (e2 >= 100) label[p2++] = '0' + (e2 / 100);
                if (e2 >= 10)  label[p2++] = '0' + ((e2 / 10) % 10);
                label[p2++] = '0' + (e2 % 10);
                label[p2] = '\0';
                diag_log_matrix(label, pData);
            } else if (count >= 1 && count <= 2) {
                log_floats_dec("    data: ", pData, count * 4);
            } else if (count >= 5) {
                log_str("    (large write, first 4x4):\r\n");
                log_floats_dec("      ", pData, 16);
            }
        }
#endif
    }

    return ((FN)RealVtbl(self)[SLOT_SetVertexShaderConstantF])(self->pReal, startReg, pData, count);
}

/* 107: SetPixelShader */
static int __stdcall WD_SetPixelShader(WrappedDevice *self, void *pShader) {
    typedef int (__stdcall *FN)(void*, void*);
    shader_addref(pShader);
    shader_release(self->lastPS);
    self->lastPS = pShader;
    if (!self->ffpActive)
        return ((FN)RealVtbl(self)[SLOT_SetPixelShader])(self->pReal, pShader);
    return 0; /* swallowed while in FFP mode */
}

/* 109: SetPixelShaderConstantF */
static int __stdcall WD_SetPixelShaderConstantF(WrappedDevice *self,
    unsigned int startReg, float *pData, unsigned int count)
{
    typedef int (__stdcall *FN)(void*, unsigned int, float*, unsigned int);
    unsigned int i;
    if (pData && startReg + count <= 32) {
        for (i = 0; i < count * 4; i++) {
            self->psConst[(startReg * 4) + i] = pData[i];
        }
        self->psConstDirty = 1;
    }
    return ((FN)RealVtbl(self)[SLOT_SetPixelShaderConstantF])(self->pReal, startReg, pData, count);
}

/* 65: SetTexture */
static int __stdcall WD_SetTexture(WrappedDevice *self, unsigned int stage, void *pTexture) {
    typedef int (__stdcall *FN)(void*, unsigned int, void*);
    if (stage < 8) {
        self->curTexture[stage] = pTexture;
    }
    return ((FN)RealVtbl(self)[SLOT_SetTexture])(self->pReal, stage, pTexture);
}

/* 100: SetStreamSource */
static int __stdcall WD_SetStreamSource(WrappedDevice *self,
    unsigned int stream, void *pVB, unsigned int offset, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, void*, unsigned int, unsigned int);
    if (stream < 4) {
        self->streamVB[stream] = pVB;
        self->streamOffset[stream] = offset;
        self->streamStride[stream] = stride;
    }
    return ((FN)RealVtbl(self)[SLOT_SetStreamSource])(self->pReal, stream, pVB, offset, stride);
}

/* 87: SetVertexDeclaration — Parse vertex elements, detect skinning */
static int __stdcall WD_SetVertexDeclaration(WrappedDevice *self, void *pDecl) {
    typedef int (__stdcall *FN)(void*, void*);

    self->lastDecl = pDecl;
    self->curDeclIsSkinned = 0;
    self->curDeclHasTexcoord = 0;
    self->curDeclHasNormal = 0;
    self->curDeclHasColor = 0;
    self->curDeclHasPosT = 0;
#if ENABLE_SHADOW_SKIP
    self->curDeclColorOffset = -1;
    self->curDeclColorType = -1;
#endif
    self->curDeclTexcoordType = -1;
    self->curDeclTexcoordOff = 0;
#if ENABLE_SKINNING
    self->curDeclNumWeights = 0;
    self->curDeclBlendIndicesOff = 0;
    self->curDeclBlendIndicesType = 0;
#endif

    if (pDecl) {
        typedef int (__stdcall *FN_GetDecl)(void*, void*, unsigned int*);
        void **declVt = *(void***)pDecl;
        unsigned char elemBuf[8 * 32];
        unsigned int numElems = 0;
        int hr2 = ((FN_GetDecl)declVt[4])(pDecl, NULL, &numElems);
        if (hr2 == 0 && numElems > 0 && numElems <= 32) {
            hr2 = ((FN_GetDecl)declVt[4])(pDecl, elemBuf, &numElems);
            if (hr2 == 0) {
                unsigned int e;
                int hasBlendWeight = 0, hasBlendIndices = 0;
                int blendWeightType = 0, blendIndicesType = 0;

                for (e = 0; e < numElems; e++) {
                    unsigned char *el = &elemBuf[e * 8];
                    unsigned short stream  = *(unsigned short*)&el[0];
                    unsigned short offset  = *(unsigned short*)&el[2];
                    unsigned char  type    = el[4];
                    unsigned char  usage   = el[6];
                    unsigned char  usageIdx = el[7];
                    if (stream == 0xFF || stream == 0xFFFF) break;

                    if (usage == D3DDECLUSAGE_POSITIONT) {
                        self->curDeclHasPosT = 1;
                    }
                    if (usage == D3DDECLUSAGE_BLENDWEIGHT) {
                        hasBlendWeight = 1;
                        blendWeightType = type;
                    }
                    if (usage == D3DDECLUSAGE_BLENDINDICES) {
                        hasBlendIndices = 1;
                        blendIndicesType = type;
#if ENABLE_SKINNING
                        self->curDeclBlendIndicesOff = offset;
                        self->curDeclBlendIndicesType = type;
#endif
                    }
                    if (usage == D3DDECLUSAGE_NORMAL && stream == 0) {
                        self->curDeclHasNormal = 1;
                    }
                    if (usage == D3DDECLUSAGE_TEXCOORD && usageIdx == 0 && stream == 0) {
                        self->curDeclHasTexcoord    = 1;
                        self->curDeclTexcoordType   = type;
                        self->curDeclTexcoordOff    = offset;
                    }
                    if (usage == D3DDECLUSAGE_COLOR && usageIdx == 0 && stream == 0) {
                        self->curDeclHasColor = 1;
#if ENABLE_SHADOW_SKIP
                        self->curDeclColorOffset = offset;
                        self->curDeclColorType = type;
#endif
                    }
                }

                if (hasBlendWeight && hasBlendIndices) {
                    self->curDeclIsSkinned = 1;
#if ENABLE_SKINNING
                    /* Infer weight count from BLENDWEIGHT element type */
                    switch (blendWeightType) {
                        case D3DDECLTYPE_FLOAT1:  self->curDeclNumWeights = 1; break;
                        case D3DDECLTYPE_FLOAT2:  self->curDeclNumWeights = 2; break;
                        case D3DDECLTYPE_FLOAT3:  self->curDeclNumWeights = 3; break;
                        case D3DDECLTYPE_FLOAT4:  self->curDeclNumWeights = 3; break;
                        case D3DDECLTYPE_UBYTE4N: self->curDeclNumWeights = 3; break;
                        default:                  self->curDeclNumWeights = 3; break;
                    }
#endif
                    /* Log blend indices type once */
                    {
                        static int s_blendTypeLogged = 0;
                        if (!s_blendTypeLogged) {
                            s_blendTypeLogged = 1;
                            log_int("  BLENDINDICES type=", blendIndicesType);
                            log_str(blendIndicesType == D3DDECLTYPE_D3DCOLOR ?
                                "    (D3DCOLOR — will swizzle BGRA to RGBA for UBYTE4)\r\n" :
                                "    (expected UBYTE4=5)\r\n");
                        }
                    }
                }

#if DIAG_ENABLED
                if (DIAG_ACTIVE(self)) {
                    int alreadyLogged = 0, di;
                    for (di = 0; di < self->loggedDeclCount; di++) {
                        if (self->loggedDecls[di] == pDecl) { alreadyLogged = 1; break; }
                    }
                    if (!alreadyLogged && self->loggedDeclCount < 32) {
                        static const char *usageNames[] = {
                            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",
                            "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
                            "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"
                        };
                        static const char *typeNames[] = {
                            "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR",
                            "UBYTE4", "SHORT2", "SHORT4", "UBYTE4N", "SHORT2N",
                            "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N",
                            "FLOAT16_2", "FLOAT16_4", "UNUSED"
                        };
                        self->loggedDecls[self->loggedDeclCount++] = pDecl;
                        log_hex("  DECL decl=", (unsigned int)pDecl);
                        log_int("    numElems=", numElems);
                        if (self->curDeclIsSkinned) {
#if ENABLE_SKINNING
                            log_int("    SKINNED numWeights=", self->curDeclNumWeights);
#else
                            log_str("    SKINNED\r\n");
#endif
                            log_int("    blendIndicesType=", blendIndicesType);
                            log_int("    blendWeightType=", blendWeightType);
                        }
                        if (self->curDeclHasPosT) {
                            log_str("    POSITIONT\r\n");
                        }
                        for (e = 0; e < numElems; e++) {
                            unsigned char *el = &elemBuf[e * 8];
                            unsigned short eStream = *(unsigned short*)&el[0];
                            unsigned short eOff    = *(unsigned short*)&el[2];
                            unsigned char  eType   = el[4];
                            unsigned char  eUsage  = el[6];
                            unsigned char  eUIdx   = el[7];
                            if (eStream == 0xFF || eStream == 0xFFFF) break;
                            log_str("    [s");
                            log_int("", eStream);
                            log_str("    +");
                            log_int("", eOff);
                            log_str("    ] ");
                            if (eUsage < 14) log_str(usageNames[eUsage]);
                            else log_int("usage=", eUsage);
                            log_str("[");
                            {
                                char ub[4]; ub[0] = '0' + eUIdx; ub[1] = ']'; ub[2] = ' '; ub[3] = 0;
                                log_str(ub);
                            }
                            if (eType <= 17) log_str(typeNames[eType]);
                            else log_int("type=", eType);
                            log_str("\r\n");
                        }
                    }
                }
#endif
            }
        }
    }

    return ((FN)RealVtbl(self)[SLOT_SetVertexDeclaration])(self->pReal, pDecl);
}

/* ---- Build vtable ---- */

WrappedDevice* WrappedDevice_Create(void *pRealDevice) {
    WrappedDevice *w;

    w = (WrappedDevice*)HeapAlloc(GetProcessHeap(), 8 /*HEAP_ZERO_MEMORY*/, sizeof(WrappedDevice));
    if (!w) return NULL;

    s_device_vtbl[0]  = (void*)WD_QueryInterface;
    s_device_vtbl[1]  = (void*)WD_AddRef;
    s_device_vtbl[2]  = (void*)WD_Release;
    s_device_vtbl[3]  = (void*)Relay_03;
    s_device_vtbl[4]  = (void*)Relay_04;
    s_device_vtbl[5]  = (void*)Relay_05;
    s_device_vtbl[6]  = (void*)Relay_06;
    s_device_vtbl[7]  = (void*)Relay_07;
    s_device_vtbl[8]  = (void*)Relay_08;
    s_device_vtbl[9]  = (void*)Relay_09;
    s_device_vtbl[10] = (void*)Relay_10;
    s_device_vtbl[11] = (void*)Relay_11;
    s_device_vtbl[12] = (void*)Relay_12;
    s_device_vtbl[13] = (void*)Relay_13;
    s_device_vtbl[14] = (void*)Relay_14;
    s_device_vtbl[15] = (void*)Relay_15;
    s_device_vtbl[16] = (void*)WD_Reset;             /* INTERCEPTED */
    s_device_vtbl[17] = (void*)WD_Present;           /* INTERCEPTED */
    s_device_vtbl[18] = (void*)Relay_18;
    s_device_vtbl[19] = (void*)Relay_19;
    s_device_vtbl[20] = (void*)Relay_20;
    s_device_vtbl[21] = (void*)Relay_21;
    s_device_vtbl[22] = (void*)Relay_22;
    s_device_vtbl[23] = (void*)Relay_23;
    s_device_vtbl[24] = (void*)Relay_24;
    s_device_vtbl[25] = (void*)Relay_25;
    s_device_vtbl[26] = (void*)Relay_26;
    s_device_vtbl[27] = (void*)Relay_27;
    s_device_vtbl[28] = (void*)Relay_28;
    s_device_vtbl[29] = (void*)Relay_29;
    s_device_vtbl[30] = (void*)Relay_30;
    s_device_vtbl[31] = (void*)Relay_31;
    s_device_vtbl[32] = (void*)Relay_32;
    s_device_vtbl[33] = (void*)Relay_33;
    s_device_vtbl[34] = (void*)Relay_34;
    s_device_vtbl[35] = (void*)Relay_35;
    s_device_vtbl[36] = (void*)Relay_36;
    s_device_vtbl[37] = (void*)Relay_37;
    s_device_vtbl[38] = (void*)Relay_38;
    s_device_vtbl[39] = (void*)Relay_39;
    s_device_vtbl[40] = (void*)Relay_40;
    s_device_vtbl[41] = (void*)WD_BeginScene;        /* INTERCEPTED */
    s_device_vtbl[42] = (void*)WD_EndScene;           /* INTERCEPTED */
    s_device_vtbl[43] = (void*)Relay_43;
    s_device_vtbl[44] = (void*)Relay_44;
    s_device_vtbl[45] = (void*)Relay_45;
    s_device_vtbl[46] = (void*)Relay_46;
    s_device_vtbl[47] = (void*)Relay_47;
    s_device_vtbl[48] = (void*)Relay_48;
    s_device_vtbl[49] = (void*)Relay_49;
    s_device_vtbl[50] = (void*)Relay_50;
    s_device_vtbl[51] = (void*)Relay_51;
    s_device_vtbl[52] = (void*)Relay_52;
    s_device_vtbl[53] = (void*)Relay_53;
    s_device_vtbl[54] = (void*)Relay_54;
    s_device_vtbl[55] = (void*)Relay_55;
    s_device_vtbl[56] = (void*)Relay_56;
    s_device_vtbl[57] = (void*)Relay_57;
    s_device_vtbl[58] = (void*)Relay_58;
    s_device_vtbl[59] = (void*)Relay_59;
    s_device_vtbl[60] = (void*)Relay_60;
    s_device_vtbl[61] = (void*)Relay_61;
    s_device_vtbl[62] = (void*)Relay_62;
    s_device_vtbl[63] = (void*)Relay_63;
    s_device_vtbl[64] = (void*)Relay_64;
    s_device_vtbl[65] = (void*)WD_SetTexture;        /* INTERCEPTED */
    s_device_vtbl[66] = (void*)Relay_66;
    s_device_vtbl[67] = (void*)Relay_67;
    s_device_vtbl[68] = (void*)Relay_68;
    s_device_vtbl[69] = (void*)Relay_69;
    s_device_vtbl[70] = (void*)Relay_70;
    s_device_vtbl[71] = (void*)Relay_71;
    s_device_vtbl[72] = (void*)Relay_72;
    s_device_vtbl[73] = (void*)Relay_73;
    s_device_vtbl[74] = (void*)Relay_74;
    s_device_vtbl[75] = (void*)Relay_75;
    s_device_vtbl[76] = (void*)Relay_76;
    s_device_vtbl[77] = (void*)Relay_77;
    s_device_vtbl[78] = (void*)Relay_78;
    s_device_vtbl[79] = (void*)Relay_79;
    s_device_vtbl[80] = (void*)Relay_80;
    s_device_vtbl[81] = (void*)WD_DrawPrimitive;     /* INTERCEPTED */
    s_device_vtbl[82] = (void*)WD_DrawIndexedPrimitive; /* INTERCEPTED */
    s_device_vtbl[83] = (void*)Relay_83;
    s_device_vtbl[84] = (void*)Relay_84;
    s_device_vtbl[85] = (void*)Relay_85;
    s_device_vtbl[86] = (void*)Relay_86;
    s_device_vtbl[87] = (void*)WD_SetVertexDeclaration; /* INTERCEPTED */
    s_device_vtbl[88] = (void*)Relay_88;
    s_device_vtbl[89] = (void*)Relay_89;
    s_device_vtbl[90] = (void*)Relay_90;
    s_device_vtbl[91] = (void*)Relay_91;
    s_device_vtbl[92] = (void*)WD_SetVertexShader;   /* INTERCEPTED */
    s_device_vtbl[93] = (void*)Relay_93;
    s_device_vtbl[94] = (void*)WD_SetVertexShaderConstantF; /* INTERCEPTED */
    s_device_vtbl[95] = (void*)Relay_95;
    s_device_vtbl[96] = (void*)Relay_96;
    s_device_vtbl[97] = (void*)Relay_97;
    s_device_vtbl[98] = (void*)Relay_98;
    s_device_vtbl[99] = (void*)Relay_99;
    s_device_vtbl[100] = (void*)WD_SetStreamSource;  /* INTERCEPTED */
    s_device_vtbl[101] = (void*)Relay_101;
    s_device_vtbl[102] = (void*)Relay_102;
    s_device_vtbl[103] = (void*)Relay_103;
    s_device_vtbl[104] = (void*)Relay_104;
    s_device_vtbl[105] = (void*)Relay_105;
    s_device_vtbl[106] = (void*)Relay_106;
    s_device_vtbl[107] = (void*)WD_SetPixelShader;   /* INTERCEPTED */
    s_device_vtbl[108] = (void*)Relay_108;
    s_device_vtbl[109] = (void*)WD_SetPixelShaderConstantF; /* INTERCEPTED */
    s_device_vtbl[110] = (void*)Relay_110;
    s_device_vtbl[111] = (void*)Relay_111;
    s_device_vtbl[112] = (void*)Relay_112;
    s_device_vtbl[113] = (void*)Relay_113;
    s_device_vtbl[114] = (void*)Relay_114;
    s_device_vtbl[115] = (void*)Relay_115;
    s_device_vtbl[116] = (void*)Relay_116;
    s_device_vtbl[117] = (void*)Relay_117;
    s_device_vtbl[118] = (void*)Relay_118;

    w->vtbl = s_device_vtbl;
    w->pReal = pRealDevice;
    w->refCount = 1;
    w->frameCount = 0;
    w->ffpSetup = 0;
    w->worldDirty = 0;
    w->viewProjDirty = 0;
    w->psConstDirty = 0;
    w->lastVS = NULL;
    w->lastPS = NULL;
    w->viewProjValid = 0;
    w->lastDecl = NULL;
    w->curDeclIsSkinned = 0;
    w->curDeclHasTexcoord = 0;
    w->curDeclHasNormal = 0;
    w->curDeclHasColor = 0;
    w->curDeclHasPosT = 0;
    w->curDeclTexcoordType = -1;
    w->curDeclTexcoordOff = 0;
#if ENABLE_SKINNING
    Skin_InitDevice(w, pRealDevice);
    log_str("  Skin_InitDevice done\r\n");
    /* GAME-SPECIFIC: Patch conditional jump at 0xB992F2 to unconditional (0xEB).
     * Matches NewVegasRTXHelper's SafeWrite8(0xB992F2, 0xEB) — "fix broken skinning".
     * This bypasses a game engine check that skips bone matrix uploads for certain
     * sub-meshes, causing arms/legs to miss their bone data. */
    {
        unsigned char *patchAddr = (unsigned char *)0xB992F2;
        DWORD oldProtect;
        if (VirtualProtect(patchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *patchAddr = 0xEB;
            VirtualProtect(patchAddr, 1, oldProtect, &oldProtect);
            log_str("  Skinning: patched 0xB992F2 (fix broken skinning)\r\n");
        } else {
            log_str("  Skinning: WARNING — failed to patch 0xB992F2\r\n");
        }
    }
    /* GAME-SPECIFIC: Install game engine hooks for proper per-object bone reset.
     * Matches NewVegasRTXHelper's on_render_skinned_stub and reset_bones_stub.
     *
     * Hook 1 (0xB99598): Wraps the skinned render batch call (0xB99110).
     *   Sets g_renderSkinned=1 before, clears after, signals bone reset.
     *
     * Hook 2 (0xB991E7): Wraps per-object bone init call (0x43D450).
     *   Signals g_boneResetPending=1 so next bone write clears stale palette.
     *
     * Both stubs are written to a single VirtualAlloc'd code cave.
     * Original 5-byte CALLs are overwritten with 5-byte JMPs to stubs. */
    {
        unsigned char *cave = (unsigned char *)VirtualAlloc(NULL, 4096,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (cave) {
            unsigned char *p;
            unsigned int addr_renderSkinned = (unsigned int)&g_renderSkinned;
            unsigned int addr_boneResetPending = (unsigned int)&g_boneResetPending;
            int rel;

            /* ---- Stub 1: on_render_skinned (at cave+0) ---- */
            p = cave;
            /* mov dword ptr [g_renderSkinned], 1 */
            *p++ = 0xC7; *p++ = 0x05;
            *(unsigned int *)p = addr_renderSkinned; p += 4;
            *(unsigned int *)p = 1; p += 4;
            /* call 0xB99110 */
            *p++ = 0xE8;
            rel = (int)0xB99110 - (int)(p + 4);
            *(int *)p = rel; p += 4;
            /* mov dword ptr [g_renderSkinned], 0 */
            *p++ = 0xC7; *p++ = 0x05;
            *(unsigned int *)p = addr_renderSkinned; p += 4;
            *(unsigned int *)p = 0; p += 4;
            /* mov dword ptr [g_boneResetPending], 1 */
            *p++ = 0xC7; *p++ = 0x05;
            *(unsigned int *)p = addr_boneResetPending; p += 4;
            *(unsigned int *)p = 1; p += 4;
            /* jmp 0xB9959D */
            *p++ = 0xE9;
            rel = (int)0xB9959D - (int)(p + 4);
            *(int *)p = rel; p += 4;

            /* ---- Stub 2: reset_bones (at cave+64) ---- */
            p = cave + 64;
            /* call 0x43D450 */
            *p++ = 0xE8;
            rel = (int)0x43D450 - (int)(p + 4);
            *(int *)p = rel; p += 4;
            /* mov dword ptr [g_boneResetPending], 1 */
            *p++ = 0xC7; *p++ = 0x05;
            *(unsigned int *)p = addr_boneResetPending; p += 4;
            *(unsigned int *)p = 1; p += 4;
            /* jmp 0xB991EC */
            *p++ = 0xE9;
            rel = (int)0xB991EC - (int)(p + 4);
            *(int *)p = rel; p += 4;

            /* ---- Patch game code: overwrite 5-byte CALLs with JMPs to stubs ---- */
            {
                DWORD oldProt1, oldProt2;
                unsigned char *hook1 = (unsigned char *)0xB99598;
                unsigned char *hook2 = (unsigned char *)0xB991E7;
                int ok = 1;

                if (VirtualProtect(hook1, 5, PAGE_EXECUTE_READWRITE, &oldProt1)) {
                    hook1[0] = 0xE9; /* JMP rel32 */
                    *(int *)(hook1 + 1) = (int)cave - (int)(hook1 + 5);
                    VirtualProtect(hook1, 5, oldProt1, &oldProt1);
                } else { ok = 0; }

                if (VirtualProtect(hook2, 5, PAGE_EXECUTE_READWRITE, &oldProt2)) {
                    hook2[0] = 0xE9;
                    *(int *)(hook2 + 1) = (int)(cave + 64) - (int)(hook2 + 5);
                    VirtualProtect(hook2, 5, oldProt2, &oldProt2);
                } else { ok = 0; }

                if (ok) {
                    g_hooksInstalled = 1;
                    log_str("  Skinning: hooks installed (render_skinned + reset_bones)\r\n");
                } else {
                    log_str("  Skinning: WARNING — failed to install game hooks\r\n");
                }
            }
        } else {
            log_str("  Skinning: WARNING — VirtualAlloc failed for code cave\r\n");
        }
    }
#endif
    { int s; for (s = 0; s < 4; s++) { w->streamVB[s] = NULL; w->streamOffset[s] = 0; w->streamStride[s] = 0; } }
    { int t; for (t = 0; t < 8; t++) w->curTexture[t] = NULL; }
#if ENABLE_SHADOW_SKIP
    { int i; for (i = 0; i < SHADOW_CACHE_SIZE; i++) { w->shadowCache[i].vb = NULL; w->shadowCache[i].result = SHADOW_UNKNOWN; } }
    w->shadowSkipCount = 0;
    w->curDeclColorOffset = -1;
    w->curDeclColorType = -1;
#endif
    w->loggedDeclCount = 0;
    { int ts; for (ts = 0; ts < 8; ts++) w->diagTexUniq[ts] = 0; }
    w->createTick = GetTickCount();
    w->diagLoggedFrames = 0;

    /* Read AlbedoStage from INI */
    {
        char iniBuf[260];
        extern HINSTANCE g_hInstance;
        int i, lastSlash = -1;
        GetModuleFileNameA(g_hInstance, iniBuf, 260);
        for (i = 0; iniBuf[i]; i++) {
            if (iniBuf[i] == '\\' || iniBuf[i] == '/') lastSlash = i;
        }
        if (lastSlash >= 0) {
            const char *fn = "proxy.ini";
            int p = lastSlash + 1, j;
            for (j = 0; fn[j]; j++) iniBuf[p++] = fn[j];
            iniBuf[p] = '\0';
        }
        w->albedoStage = GetPrivateProfileIntA("FFP", "AlbedoStage", 0, iniBuf);
        if (w->albedoStage < 0 || w->albedoStage > 7) w->albedoStage = 0;
#if ENABLE_SHADOW_SKIP
        w->skipFakeShadows = GetPrivateProfileIntA("FFP", "SkipFakeShadows", 1, iniBuf);
#endif
#if ENABLE_LIGHTS
        w->lightsEnabled = GetPrivateProfileIntA("Lights", "Enabled", 1, iniBuf);
        {
            int pct = GetPrivateProfileIntA("Lights", "IntensityPercent", 100, iniBuf);
            w->lightIntensity = (float)pct / 100.0f;
        }
        w->lightRangeMode = GetPrivateProfileIntA("Lights", "RangeMode", 0, iniBuf);
        w->lightsUpdatedThisFrame = 0;
        w->lastEnabledLights = 0;
#endif
    }

    log_str("WrappedDevice created with FFP conversion\r\n");
    log_int("  Diag delay (ms): ", DIAG_DELAY_MS);
    log_int("  AlbedoStage: ", w->albedoStage);
#if ENABLE_SHADOW_SKIP
    log_int("  SkipFakeShadows: ", w->skipFakeShadows);
#endif
#if ENABLE_LIGHTS
    log_int("  Lights enabled: ", w->lightsEnabled);
    log_int("  IntensityPercent: ", (int)(w->lightIntensity * 100.0f));
    log_int("  RangeMode: ", w->lightRangeMode);
#endif
    log_hex("  Real device: ", (unsigned int)pRealDevice);
    return w;
}
