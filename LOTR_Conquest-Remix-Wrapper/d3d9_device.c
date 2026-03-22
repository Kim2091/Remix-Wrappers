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
/* LOTR Conquest: Pandemic Magellan engine.
 * HLSL declares row_major float4x4 — constants are already row-major.
 * Game provides combined ViewProj (c239-c242), not separate View+Proj. */
#define VIEWPROJ_COMBINED       1   /* 1=game has combined ViewProj, 0=separate View+Proj */
#define VS_CONSTANTS_ROW_MAJOR  1   /* 1=skip transpose (row_major HLSL), 0=transpose (column-major) */

#if VIEWPROJ_COMBINED
#define VS_REG_VIEWPROJ_START 239   /* g__viewProjMatrix (combined View*Proj) */
#define VS_REG_VIEWPROJ_END   243   /* One past last ViewProj register */
#else
#define VS_REG_VIEW_START       0   /* First register of 4x4 view matrix (4 vec4) */
#define VS_REG_VIEW_END         4   /* One past last view register */
#define VS_REG_PROJ_START       4   /* First register of 4x4 projection matrix */
#define VS_REG_PROJ_END         8   /* One past last projection register */
#endif
#define VS_REG_WORLD_START    178   /* g__worldMatrix */
#define VS_REG_WORLD_END      182   /* One past last world register */

/* Terrain3 UV transform rows (PS constants): u = dot(pos4, c212), v = dot(pos4, c213) */
#define PS_REG_TERRAIN3_UV_ROW0  212
#define PS_REG_TERRAIN3_UV_ROW1  213

/* Bone palette detection — Magellan engine: g__matrixPalette[59] at c0-c176 */
#define VS_REG_BONE_START       0   /* First register of bone palette */
#define VS_REG_BONE_END       177   /* One past last bone register (59 bones x 3 regs) */
#define VS_REG_BONE_THRESHOLD   0   /* startReg >= this triggers bone detection */
#define VS_REGS_PER_BONE        3   /* Registers per bone (3 = row_major float3x4) */

/* Skinning enabled: bones are object-space, composed with World at draw time */
#define ENABLE_SKINNING 1
#define EXPAND_SKIN_VERTICES 1      /* Required: D3DCOLOR BLENDWEIGHT/NORMAL need conversion */

/* Declaration cloning cache: maps original decl -> cloned decl with
 * BLENDINDICES type changed from D3DCOLOR to UBYTE4 for FFP compatibility.
 * Only used when ENABLE_SKINNING=1 and the game uses D3DCOLOR BLENDINDICES. */
#define SKIN_DECL_CACHE_SIZE 4

/* Terrain3 CPU vertex expansion: FLOAT3 pos + D3DCOLOR normal → FLOAT3 pos + FLOAT3 normal + FLOAT2 uv */
#define TERRAIN_VTX_SRC_STRIDE 16   /* FLOAT3 pos + D3DCOLOR normal */
#define TERRAIN_VTX_EXP_STRIDE 32   /* FLOAT3 pos + FLOAT3 normal + FLOAT2 texcoord */
#define TERRAIN_VB_CACHE_SIZE  64

/* D3DCOLOR NORMAL → FLOAT3 vertex expansion (generic, all rigid geometry) */
#define NORMEXP_DECL_CACHE_SIZE 8
#define NORMEXP_VB_CACHE_SIZE   64

/* ---- Diagnostic logging ---- */
#define DIAG_LOG_FRAMES 3
#define DIAG_DELAY_MS 45000   /* 45 seconds — enough time to load into gameplay */
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
#define D3DRS_ZWRITEENABLE      14
#define D3DRS_DEPTHBIAS         195
#define D3DRS_SLOPESCALEDEPTHBIAS 175
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

#define MAX_FFP_BONES 59    /* Magellan engine uses up to 59 bones */

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

#if ENABLE_SKINNING && EXPAND_SKIN_VERTICES
/* Expanded skinned vertex layout: FLOAT3 pos + FLOAT3 weights + UBYTE4 idx + FLOAT3 normal + FLOAT2 uv */
#define SKIN_VTX_SIZE   48
#define SKIN_CACHE_SIZE 64
#endif

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
    float psConst[224 * 4]; /* pixel shader constants (up to 224 vec4) */
    int worldDirty;         /* world matrix registers changed since last SetTransform */
    int viewProjDirty;      /* view/proj registers changed since last SetTransform */
    int psConstDirty;

    void *lastVS;           /* last vertex shader set by the game */
    void *lastPS;           /* last pixel shader set by the game */
    int viewProjValid;      /* set once both View and Proj register ranges have been written */
    int ffpActive;          /* real device currently has NULL shaders (FFP mode) */

    void *lastDecl;         /* current IDirect3DVertexDeclaration9* */
    int curDeclIsSkinned;   /* 1 if current decl has BLENDINDICES (with or without BLENDWEIGHT) */

#if ENABLE_SKINNING
    int curDeclHasBlendWeight; /* 1 if current decl has BLENDWEIGHT element */
    int curDeclNumWeights;   /* number of blend weights (1-3), or 1 for single-bone */
    int numBones;            /* bones uploaded this object (immediate upload counter) */
    int prevNumBones;        /* bone count from previous object (for stale clearing) */
    int bonesDrawn;          /* set to 1 after a skinned draw; triggers reset on next bone write */
    int lastBoneStartReg;    /* startReg of most recent bone write (startReg-jump detection) */
    int skinningSetup;       /* whether FFP skinning state has been configured */

    int curDeclBlendIndicesOff;  /* byte offset of BLENDINDICES in source vertex */
    int curDeclBlendIndicesType; /* D3DDECLTYPE of BLENDINDICES (D3DCOLOR=4, UBYTE4=5) */

    /* Declaration cache: maps original decl -> cloned decl with UBYTE4 BLENDINDICES */
    void *skinDeclOrig[SKIN_DECL_CACHE_SIZE];
    void *skinDeclClone[SKIN_DECL_CACHE_SIZE];
    int   skinDeclCount;

#if EXPAND_SKIN_VERTICES
    /* Per-element offsets/types for skinned vertex expansion */
    int curDeclNormalOff;       /* byte offset of NORMAL in source vertex */
    int curDeclNormalType;      /* D3DDECLTYPE of NORMAL, or -1 if none */
    int curDeclBlendWeightOff;  /* byte offset of BLENDWEIGHT in source vertex */
    int curDeclBlendWeightType; /* D3DDECLTYPE of BLENDWEIGHT element */
    int curDeclPosOff;          /* byte offset of POSITION in source vertex */

    /* Skinned vertex expansion cache */
    void        *skinExpVB[SKIN_CACHE_SIZE];   /* cached expanded IDirect3DVertexBuffer9* */
    unsigned int skinExpKey[SKIN_CACHE_SIZE];   /* hash key per slot */
    unsigned int skinExpNv[SKIN_CACHE_SIZE];    /* vertex count per slot */
    void        *skinExpDecl;                  /* IDirect3DVertexDeclaration9* for expanded layout */
#endif /* EXPAND_SKIN_VERTICES */
#endif /* ENABLE_SKINNING */

    float cachedView[16];    /* View matrix from last decompose_viewproj (row-major) */
    int   cachedViewValid;   /* 1 once decompose_viewproj has run */

    /* Terrain3 CPU vertex expansion cache */
    void        *terrainExpVB[TERRAIN_VB_CACHE_SIZE];
    unsigned int terrainExpKey[TERRAIN_VB_CACHE_SIZE];
    unsigned int terrainExpNv[TERRAIN_VB_CACHE_SIZE];
    void        *terrainExpDecl;  /* IDirect3DVertexDeclaration9* for expanded terrain layout */
    float       *worldOverride;  /* if non-NULL, FFP_ApplyTransforms uses this instead of c178 */

    /* D3DCOLOR NORMAL → FLOAT3 expansion cache (generic rigid geometry) */
    void        *normExpDeclOrig[NORMEXP_DECL_CACHE_SIZE];
    void        *normExpDeclClone[NORMEXP_DECL_CACHE_SIZE];
    int          normExpDeclCount;
    void        *normExpVB[NORMEXP_VB_CACHE_SIZE];
    unsigned int normExpKey[NORMEXP_VB_CACHE_SIZE];
    unsigned int normExpNv[NORMEXP_VB_CACHE_SIZE];

    /* Vertex element tracking */
    int curDeclHasTexcoord;
    int curDeclHasNormal;
    int curDeclHasColor;
    int curDeclHasPosT;     /* 1 if current decl has POSITIONT (screen-space, skips FFP transform) */

    /* Texcoord format for diagnostics and skinned vertex expansion */
    int curDeclTexcoordType; /* D3DDECLTYPE of TEXCOORD[0], or -1 if none */
    int curDeclTexcoordOff;  /* byte offset of TEXCOORD[0] in vertex */

    /* Render state tracking for overlay filtering */
    int curAlphaBlendEnabled;  /* D3DRS_ALPHABLENDENABLE */
    int curZWriteEnabled;      /* D3DRS_ZWRITEENABLE */
    unsigned int gameZEnable;  /* last D3DRS_ZENABLE value (for re-apply in FFP_Engage) */

    /* Configurable depth bias for FFP draws (from proxy.ini) */
    float depthBias;
    float slopeScaleDepthBias;

    /* Saved texture stage states — restored after each FFP draw */
    struct { unsigned int colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2; } savedTSS[8];
    unsigned int savedLighting;

    /* Texture tracking (stages 0-7) */
    void *curTexture[8];
    int albedoStage;

    /* Stream source tracking (streams 0-3) */
    void *streamVB[4];
    unsigned int streamOffset[4];
    unsigned int streamStride[4];

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

    /* Per-frame draw method counters */
    unsigned int dipCount;
    unsigned int dpCount;
    unsigned int dpupCount;
    unsigned int dipupCount;
    unsigned int fvfSetCount;
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
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
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

/*
 * Inverse of a row-major 4x4 affine matrix (row3 = [0,0,0,1]).
 * Uses cofactor expansion of the upper-left 3x3 block.
 * Useful for decomposing combined WVP: VP = W_inv * WVP.
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
 * Apply captured VS constants as FFP transforms.
 * Only updates transforms whose dirty flags are set.
 *
 * GAME-SPECIFIC: The register ranges used here come from the
 * VS_REG_* defines at the top of this file.
 */
/* sqrt via Newton iteration (no CRT) */
static float fsqrt(float x) {
    float r, prev;
    int i;
    if (x <= 0.0f) return 0.0f;
    r = x;
    for (i = 0; i < 10; i++) {
        prev = r;
        r = 0.5f * (r + x / r);
        if (r == prev) break;
    }
    return r;
}

/*
 * Decompose a row-major ViewProj matrix into separate View and Projection.
 *
 * Assumes standard D3D9 perspective projection:
 *   P = [sx  0   0    0]
 *       [0   sy  0    0]
 *       [0   0   zn   1]
 *       [0   0   zf   0]
 *
 * Key identity: column 3 of VP = column 2 of View (the forward axis + Tz).
 * sx, sy are recovered from column vector lengths of VP.
 */
static void decompose_viewproj(const float *vp, float *view, float *proj) {
    /* Extract columns from row-major VP */
    float c0[4], c1[4], c2[4], c3[4];
    float sx, sy, zn, zf;
    int i;
    for (i = 0; i < 4; i++) {
        c0[i] = vp[i*4 + 0];
        c1[i] = vp[i*4 + 1];
        c2[i] = vp[i*4 + 2];
        c3[i] = vp[i*4 + 3];
    }

    /* sx, sy from upper 3 entries of columns 0 and 1 */
    sx = fsqrt(c0[0]*c0[0] + c0[1]*c0[1] + c0[2]*c0[2]);
    sy = fsqrt(c1[0]*c1[0] + c1[1]*c1[1] + c1[2]*c1[2]);
    if (sx < 1e-6f) sx = 1.0f;
    if (sy < 1e-6f) sy = 1.0f;

    /* zn = P[2][2] = f/(f-n), from column ratio (any row 0-2 where col3 != 0) */
    if (c3[2] > 1e-6f || c3[2] < -1e-6f)
        zn = c2[2] / c3[2];
    else if (c3[0] > 1e-6f || c3[0] < -1e-6f)
        zn = c2[0] / c3[0];
    else
        zn = 1.0f;

    /* zf = P[3][2] = -n*f/(f-n), from row 3 */
    zf = c2[3] - c3[3] * zn;

    /* Build row-major View: columns are VP columns / scale, col2 = col3 of VP, col3 = [0,0,0,1] */
    for (i = 0; i < 4; i++) {
        view[i*4 + 0] = c0[i] / sx;
        view[i*4 + 1] = c1[i] / sy;
        view[i*4 + 2] = c3[i];       /* VP col3 = V col2 */
        view[i*4 + 3] = (i == 3) ? 1.0f : 0.0f;
    }

    /* Build row-major Projection */
    proj[0]  = sx;   proj[1]  = 0.0f; proj[2]  = 0.0f; proj[3]  = 0.0f;
    proj[4]  = 0.0f; proj[5]  = sy;   proj[6]  = 0.0f; proj[7]  = 0.0f;
    proj[8]  = 0.0f; proj[9]  = 0.0f; proj[10] = zn;   proj[11] = 1.0f;
    proj[12] = 0.0f; proj[13] = 0.0f; proj[14] = zf;   proj[15] = 0.0f;
}

static void FFP_ApplyTransforms(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
    void **vt = RealVtbl(self);
#if !VS_CONSTANTS_ROW_MAJOR
    float tmp[16];
#endif

    if (self->viewProjDirty) {
#if VIEWPROJ_COMBINED
        {
            const float *vp = &self->vsConst[VS_REG_VIEWPROJ_START * 4];
            float viewMat[16], projMat[16];
  #if !VS_CONSTANTS_ROW_MAJOR
            float vpT[16];
            mat4_transpose(vpT, vp);
            vp = vpT;
  #endif
            decompose_viewproj(vp, viewMat, projMat);
            ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, viewMat);
            ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, projMat);
            memcpy(self->cachedView, viewMat, 64);
            self->cachedViewValid = 1;
        }
#else
  #if VS_CONSTANTS_ROW_MAJOR
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW,
            &self->vsConst[VS_REG_VIEW_START * 4]);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION,
            &self->vsConst[VS_REG_PROJ_START * 4]);
  #else
        mat4_transpose(tmp, &self->vsConst[VS_REG_VIEW_START * 4]);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, tmp);
        mat4_transpose(tmp, &self->vsConst[VS_REG_PROJ_START * 4]);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, tmp);
  #endif
#endif
        self->viewProjDirty = 0;
    }

    if (self->worldOverride) {
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD,
            self->worldOverride);
        self->worldOverride = NULL;
        self->worldDirty = 0;
    } else if (self->worldDirty) {
#if VS_CONSTANTS_ROW_MAJOR
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD,
            &self->vsConst[VS_REG_WORLD_START * 4]);
#else
        mat4_transpose(tmp, &self->vsConst[VS_REG_WORLD_START * 4]);
        ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLD, tmp);
#endif
        self->worldDirty = 0;
    }
}

#if ENABLE_SKINNING
#include "d3d9_skinning.h"
#endif

#include "d3d9_normalexpand.h"

/* ======================================================================
 * Terrain3 CPU Vertex Expansion
 *
 * Terrain3 vertices (FLOAT3 POSITION + D3DCOLOR NORMAL, 16 bytes, no TEXCOORD)
 * are expanded to 32 bytes (FLOAT3 pos + FLOAT3 normal + FLOAT2 uv) by decoding
 * D3DCOLOR normals to FLOAT3 and computing FLOAT2 TEXCOORD from world position:
 *   u = dot(pos4, c212),  v = dot(pos4, c213)
 * where c212/c213 are PS constants capturing the terrain UV transform.
 * ====================================================================== */

/* Release all cached expanded terrain vertex buffers. */
static void TerrainVB_ReleaseCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < TERRAIN_VB_CACHE_SIZE; i++) {
        if (self->terrainExpVB[i]) {
            ((FN_Release)(*(void***)(self->terrainExpVB[i]))[2])(self->terrainExpVB[i]);
            self->terrainExpVB[i] = NULL;
        }
    }
}

/*
 * Get or build an expanded terrain VB with computed TEXCOORD for vertices
 * [baseVtx, baseVtx+nv-1]. Results cached by (source VB, baseVtx, nv, c212/c213 bits).
 */
static void* TerrainVB_GetExpanded(WrappedDevice *self, unsigned int baseVtx, unsigned int nv) {
    typedef int (__stdcall *FN_CreateVB)(void*, unsigned int, unsigned long, unsigned long, unsigned int, void**, void*);
    typedef int (__stdcall *FN_Lock)(void*, unsigned int, unsigned int, void**, unsigned int);
    typedef int (__stdcall *FN_Unlock)(void*);
    typedef unsigned long (__stdcall *FN_Release)(void*);
    const float *r0, *r1;
    unsigned int key, v, lockOff, lockSize, *fb0, *fb1;
    int slot, hr;
    void *newVB, **srcVt, **dstVt;
    unsigned char *srcData, *dstData;

    if (!self->streamVB[0]) return NULL;

    r0 = &self->psConst[PS_REG_TERRAIN3_UV_ROW0 * 4];
    r1 = &self->psConst[PS_REG_TERRAIN3_UV_ROW1 * 4];

    /* Hash key includes source VB, vertex range, and UV constant bits */
    fb0 = (unsigned int*)r0;
    fb1 = (unsigned int*)r1;
    key  = (unsigned int)self->streamVB[0]
         ^ (baseVtx * 2654435761u)
         ^ (nv * 40503u)
         ^ (fb0[0] ^ fb0[1] ^ fb0[2] ^ fb0[3])
         ^ (fb1[0] ^ fb1[1] ^ fb1[2] ^ fb1[3]);
    slot = (int)(key % TERRAIN_VB_CACHE_SIZE);

    if (self->terrainExpVB[slot] && self->terrainExpKey[slot] == key && self->terrainExpNv[slot] == nv)
        return self->terrainExpVB[slot];

    /* Cache miss — log for diagnostics */
    if (DIAG_ACTIVE(self)) {
        log_hex("  TerrainVB MISS slot=", slot);
        log_hex("    key=", key);
        log_int("    nv=", nv);
        log_hex("    srcVB=", (unsigned int)self->streamVB[0]);
        log_int("    baseVtx=", baseVtx);
        log_floats("    c212=", (float*)r0, 4);
        log_floats("    c213=", (float*)r1, 4);
        if (self->terrainExpVB[slot])
            log_str("    (evicting existing entry)\r\n");
    }

    /* Evict old entry */
    if (self->terrainExpVB[slot]) {
        ((FN_Release)(*(void***)(self->terrainExpVB[slot]))[2])(self->terrainExpVB[slot]);
        self->terrainExpVB[slot] = NULL;
    }

    newVB = NULL;
    hr = ((FN_CreateVB)RealVtbl(self)[SLOT_CreateVertexBuffer])(
        self->pReal, nv * TERRAIN_VTX_EXP_STRIDE, 8 /*WRITEONLY*/, 0, 0 /*DEFAULT*/, &newVB, NULL);
    if (hr != 0 || !newVB) { log_hex("TerrainVB CreateVB failed hr=", (unsigned int)hr); return NULL; }

    /* Lock source */
    lockOff  = self->streamOffset[0] + baseVtx * TERRAIN_VTX_SRC_STRIDE;
    lockSize = nv * TERRAIN_VTX_SRC_STRIDE;
    srcVt    = *(void***)self->streamVB[0];
    srcData  = NULL;
    hr = ((FN_Lock)srcVt[11])(self->streamVB[0], lockOff, lockSize, (void**)&srcData, 0x10 /*READONLY*/);
    if (hr != 0 || !srcData) {
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        log_hex("TerrainVB src lock failed hr=", (unsigned int)hr);
        return NULL;
    }

    /* Lock dest */
    dstVt   = *(void***)newVB;
    dstData = NULL;
    hr = ((FN_Lock)dstVt[11])(newVB, 0, nv * TERRAIN_VTX_EXP_STRIDE, (void**)&dstData, 0);
    if (hr != 0 || !dstData) {
        ((FN_Unlock)srcVt[12])(self->streamVB[0]);
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        return NULL;
    }

    /* Expand: FLOAT3 pos + decode D3DCOLOR normal to FLOAT3 + compute FLOAT2 uv */
    for (v = 0; v < nv; v++) {
        const unsigned char *s = srcData + v * TERRAIN_VTX_SRC_STRIDE;
        float *d = (float*)(dstData + v * TERRAIN_VTX_EXP_STRIDE);
        const float *pos = (const float*)s;
        const unsigned char *nrm = s + 12; /* D3DCOLOR normal: memory [B,G,R,A] */

        /* FLOAT3 POSITION */
        d[0] = pos[0]; d[1] = pos[1]; d[2] = pos[2];

        /* D3DCOLOR normal → FLOAT3: hardware reads BGRA, shader does mad(v,2,-1) */
        d[3] = nrm[2] / 127.5f - 1.0f;  /* R → normal.x */
        d[4] = nrm[1] / 127.5f - 1.0f;  /* G → normal.y */
        d[5] = nrm[0] / 127.5f - 1.0f;  /* B → normal.z */

        /* FLOAT2 TEXCOORD from world position */
        d[6] = pos[0]*r0[0] + pos[1]*r0[1] + pos[2]*r0[2] + r0[3];
        d[7] = pos[0]*r1[0] + pos[1]*r1[1] + pos[2]*r1[2] + r1[3];
    }

    ((FN_Unlock)dstVt[12])(newVB);
    ((FN_Unlock)srcVt[12])(self->streamVB[0]);

    self->terrainExpVB[slot]  = newVB;
    self->terrainExpKey[slot] = key;
    self->terrainExpNv[slot]  = nv;
    return newVB;
}

/*
 * Enter FFP mode: NULL both shaders on the real device, apply transforms,
 * setup texture stages and lighting. Stays in FFP between consecutive
 * DIP calls to avoid redundant state switches.
 */
static void FFP_Engage(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_GetRS)(void*, unsigned int, unsigned int*);
    typedef int (__stdcall *FN_GetTSS)(void*, unsigned int, unsigned int, unsigned int*);
    void **vt = RealVtbl(self);

    if (!self->ffpActive) {
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, NULL);
        ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, NULL);
        self->ffpActive = 1;
    }

    /* Save state that FFP setup will clobber — the game's material cache
     * doesn't know about our modifications, so we must restore after draw */
    {
        int s;
        for (s = 0; s < 8; s++) {
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_COLOROP,   &self->savedTSS[s].colorOp);
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_COLORARG1,  &self->savedTSS[s].colorArg1);
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_COLORARG2,  &self->savedTSS[s].colorArg2);
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_ALPHAOP,    &self->savedTSS[s].alphaOp);
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_ALPHAARG1,  &self->savedTSS[s].alphaArg1);
            ((FN_GetTSS)vt[SLOT_GetTextureStageState])(self->pReal, s, D3DTSS_ALPHAARG2,  &self->savedTSS[s].alphaArg2);
        }
    }
    ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_LIGHTING, &self->savedLighting);

    FFP_ApplyTransforms(self);
    FFP_SetupTextureStages(self);

    if (!self->ffpSetup) {
        FFP_SetupLighting(self);
        self->ffpSetup = 1;
    }

    if (self->depthBias != 0.0f || self->slopeScaleDepthBias != 0.0f) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_DEPTHBIAS, *(unsigned int*)&self->depthBias);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_SLOPESCALEDEPTHBIAS, *(unsigned int*)&self->slopeScaleDepthBias);
    }
}

/* Leave FFP mode: restore game's shaders and all clobbered state */
static void FFP_Disengage(WrappedDevice *self) {
    if (self->ffpActive) {
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
        void **vt = RealVtbl(self);

#if ENABLE_SKINNING
        FFP_DisableSkinning(self);
#endif

        ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, self->lastVS);
        ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, self->lastPS);

        if (self->depthBias != 0.0f || self->slopeScaleDepthBias != 0.0f) {
            unsigned int zero = 0;
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_DEPTHBIAS, zero);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_SLOPESCALEDEPTHBIAS, zero);
        }

        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_LIGHTING, self->savedLighting);

        self->ffpActive = 0;
    }
}

/* Restore texture stage states that FFP_Engage saved. */
static void FFP_RestoreTSS(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetTSS)(void*, unsigned int, unsigned int, unsigned int);
    void **vt = RealVtbl(self);
    int ts;
    for (ts = 0; ts < 8; ts++) {
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLOROP,   self->savedTSS[ts].colorOp);
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG1,  self->savedTSS[ts].colorArg1);
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG2,  self->savedTSS[ts].colorArg2);
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAOP,    self->savedTSS[ts].alphaOp);
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG1,  self->savedTSS[ts].alphaArg1);
        ((FN_SetTSS)vt[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG2,  self->savedTSS[ts].alphaArg2);
    }
}

/*
 * Terrain3 indexed draw: expand vertices on CPU, draw with identity world.
 * Uses worldOverride so identity is set inside FFP_ApplyTransforms, before
 * FFP_SetupTextureStages — Remix sees identity throughout all FFP state setup.
 */
static int Terrain3_DrawDIP(WrappedDevice *self, unsigned int pt, int bvi,
    unsigned int mi, unsigned int nv, unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN_DIP)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
    typedef int (__stdcall *FN_SetStream)(void*, unsigned int, void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetDecl)(void*, void*);
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_GetRS)(void*, unsigned int, unsigned int*);
    static float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    void **vt = RealVtbl(self);
    int as = self->albedoStage;
    void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
    void *expVB;
    int hr, ts;
    int isOverlay = self->curAlphaBlendEnabled;
    unsigned int savedZE = 1, savedZW = 1;

    /* Stride guard: only expand known 16-byte terrain vertices (Terrain3).
     * Non-16-byte terrain goes through normal expansion if D3DCOLOR NORMAL. */
    if (self->streamStride[0] != TERRAIN_VTX_SRC_STRIDE) {
        self->worldOverride = identity;
        FFP_Engage(self);
        if (isOverlay) {
            ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZENABLE, &savedZE);
            ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, &savedZW);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, 0);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, 0);
        }
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
        /* D3DCOLOR NORMAL → FLOAT3 expansion */
        if (self->curDeclNormalType == D3DDECLTYPE_D3DCOLOR) {
            void *neDecl = NormExp_GetClonedDecl(self);
            void *neVB   = NormExp_GetExpandedVB(self, bvi, mi, nv);
            if (neDecl && neVB) {
                ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
                    neVB, 0, self->streamStride[0] + 8);
                ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, neDecl);
                hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
                    -(int)mi, 0, nv, si, pc);
                ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
                    self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
                ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
            } else {
                hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            }
        } else {
            hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        }
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        FFP_RestoreTSS(self);
        if (isOverlay) {
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
        }
        self->worldDirty = 1;
        return hr;
    }

    self->worldOverride = identity;
    FFP_Engage(self);
    if (isOverlay) {
        ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZENABLE, &savedZE);
        ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, &savedZW);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, 0);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, 0);
    }

    expVB = TerrainVB_GetExpanded(self, mi, nv);
    if (!expVB) {
        /* Expansion failed — draw without texcoords */
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
        hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        FFP_RestoreTSS(self);
        if (isOverlay) {
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
        }
        self->worldDirty = 1;
        return hr;
    }

    /* Bind expanded VB + terrain declaration */
    ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0, expVB, 0, TERRAIN_VTX_EXP_STRIDE);
    ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->terrainExpDecl);

    ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
    for (ts = 1; ts < 8; ts++)
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);

    /* Remap indices: expanded VB starts at vertex 0, offset by -mi */
    hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
        -(int)mi, 0, nv, si, pc);

    /* Restore original VB, declaration, textures, TSS */
    ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
        self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
    ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
    for (ts = 0; ts < 8; ts++)
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
    FFP_RestoreTSS(self);
    if (isOverlay) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
    }

    self->worldDirty = 1;
    return hr;
}

/*
 * Terrain3 non-indexed draw: expand vertices on CPU, draw with identity world.
 */
static int Terrain3_DrawDP(WrappedDevice *self, unsigned int pt, unsigned int sv, unsigned int pc)
{
    typedef int (__stdcall *FN_DP)(void*, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
    typedef int (__stdcall *FN_SetStream)(void*, unsigned int, void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetDecl)(void*, void*);
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    typedef int (__stdcall *FN_GetRS)(void*, unsigned int, unsigned int*);
    static float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    void **vt = RealVtbl(self);
    int as = self->albedoStage;
    void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
    /* For DP, vertex count = pc * vertices_per_prim; assume triangles (most common) */
    unsigned int nv = (pt == 4) ? pc * 3 : pc + 2;  /* TRIANGLELIST=4 or TRIANGLESTRIP=5 */
    void *expVB;
    int hr, ts;
    int isOverlay = self->curAlphaBlendEnabled;
    unsigned int savedZE = 1, savedZW = 1;

    /* Stride guard — non-16-byte terrain goes through normal expansion */
    if (self->streamStride[0] != TERRAIN_VTX_SRC_STRIDE) {
        self->worldOverride = identity;
        FFP_Engage(self);
        if (isOverlay) {
            ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZENABLE, &savedZE);
            ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, &savedZW);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, 0);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, 0);
        }
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
        /* D3DCOLOR NORMAL → FLOAT3 expansion */
        if (self->curDeclNormalType == D3DDECLTYPE_D3DCOLOR) {
            void *neDecl = NormExp_GetClonedDecl(self);
            void *neVB   = NormExp_GetExpandedVB(self, 0, sv, nv);
            if (neDecl && neVB) {
                ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
                    neVB, 0, self->streamStride[0] + 8);
                ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, neDecl);
                hr = ((FN_DP)vt[SLOT_DrawPrimitive])(self->pReal, pt, 0, pc);
                ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
                    self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
                ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
            } else {
                hr = ((FN_DP)vt[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
            }
        } else {
            hr = ((FN_DP)vt[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
        }
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        FFP_RestoreTSS(self);
        if (isOverlay) {
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
        }
        self->worldDirty = 1;
        return hr;
    }

    self->worldOverride = identity;
    FFP_Engage(self);
    if (isOverlay) {
        ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZENABLE, &savedZE);
        ((FN_GetRS)vt[SLOT_GetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, &savedZW);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, 0);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, 0);
    }

    expVB = TerrainVB_GetExpanded(self, sv, nv);
    if (!expVB) {
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
        hr = ((FN_DP)vt[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        FFP_RestoreTSS(self);
        if (isOverlay) {
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
            ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
        }
        self->worldDirty = 1;
        return hr;
    }

    ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0, expVB, 0, TERRAIN_VTX_EXP_STRIDE);
    ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->terrainExpDecl);

    ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
    for (ts = 1; ts < 8; ts++)
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);

    hr = ((FN_DP)vt[SLOT_DrawPrimitive])(self->pReal, pt, 0, pc);

    ((FN_SetStream)vt[SLOT_SetStreamSource])(self->pReal, 0,
        self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
    ((FN_SetDecl)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
    for (ts = 0; ts < 8; ts++)
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
    FFP_RestoreTSS(self);
    if (isOverlay) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZENABLE, savedZE);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_ZWRITEENABLE, savedZW);
    }

    self->worldDirty = 1;
    return hr;
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
        NormExp_ReleaseDevice(self);
        TerrainVB_ReleaseCache(self);
        if (self->terrainExpDecl) {
            typedef unsigned long (__stdcall *FN_Rel)(void*);
            ((FN_Rel)(*(void***)(self->terrainExpDecl))[2])(self->terrainExpDecl);
        }
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
    self->cachedViewValid = 0;
    self->ffpActive = 0;
#if ENABLE_SKINNING && EXPAND_SKIN_VERTICES
    SkinVB_ReleaseCache(self);
#endif
    NormExp_ReleaseVBCache(self);
    TerrainVB_ReleaseCache(self);

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
        log_int("  DIP: ", self->dipCount);
        log_int("  DP: ", self->dpCount);
        log_int("  DPUP: ", self->dpupCount);
        log_int("  DIPUP: ", self->dipupCount);
        log_int("  SetFVF: ", self->fvfSetCount);
        log_int("  scenes: ", self->sceneCount);
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
    self->dipCount = 0;
    self->dpCount = 0;
    self->dpupCount = 0;
    self->dipupCount = 0;
    self->fvfSetCount = 0;
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

/* 57: SetRenderState — track depth and alpha state for overlay filtering */
static int __stdcall WD_SetRenderState(WrappedDevice *self, unsigned int state, unsigned int value) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int);
    if (state == D3DRS_ALPHABLENDENABLE)
        self->curAlphaBlendEnabled = (value != 0);
    else if (state == D3DRS_ZWRITEENABLE)
        self->curZWriteEnabled = (value != 0);
    else if (state == D3DRS_ZENABLE)
        self->gameZEnable = value;
    return ((FN)RealVtbl(self)[SLOT_SetRenderState])(self->pReal, state, value);
}

/* 81: DrawPrimitive — GAME-SPECIFIC draw routing for non-indexed draws */
static int __stdcall WD_DrawPrimitive(WrappedDevice *self, unsigned int pt, unsigned int sv, unsigned int pc) {
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int);
    int hr;
    self->drawCallCount++;
    self->dpCount++;

    if (self->viewProjValid && self->lastDecl && !self->curDeclHasPosT && self->curDeclHasNormal
        && !self->curDeclHasTexcoord) {
        /* Terrain3 (base or overlay): CPU-expand vertices with computed
         * TEXCOORD from c212/c213. Checked before alpha-blend drop so
         * terrain overlay draws route here instead of being dropped. */
        hr = Terrain3_DrawDP(self, pt, sv, pc);
    } else if (self->viewProjValid && self->lastDecl && !self->curDeclHasPosT && self->curDeclHasNormal
        && self->curAlphaBlendEnabled) {
        /* Alpha-blended 3D overlays with TEXCOORD (particles, clouds): drop */
        hr = 0;
    } else if (self->viewProjValid && self->lastDecl && !self->curDeclHasPosT && self->curDeclHasNormal) {
        /* World-space non-indexed draw with NORMAL + TEXCOORD: engage FFP */
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
        /* D3DCOLOR NORMAL → FLOAT3 expansion for Remix compatibility */
        if (self->curDeclNormalType == D3DDECLTYPE_D3DCOLOR) {
            typedef int (__stdcall *FN_SetStream)(void*, unsigned int, void*, unsigned int, unsigned int);
            typedef int (__stdcall *FN_SetDecl)(void*, void*);
            void **vt2 = RealVtbl(self);
            unsigned int dpNv = (pt == 4) ? pc * 3 : pc + 2;
            void *expDecl = NormExp_GetClonedDecl(self);
            void *expVB   = NormExp_GetExpandedVB(self, 0, sv, dpNv);
            if (expDecl && expVB) {
                ((FN_SetStream)vt2[SLOT_SetStreamSource])(self->pReal, 0,
                    expVB, 0, self->streamStride[0] + 8);
                ((FN_SetDecl)vt2[SLOT_SetVertexDeclaration])(self->pReal, expDecl);
                hr = ((FN)vt2[SLOT_DrawPrimitive])(self->pReal, pt, 0, pc);
                ((FN_SetStream)vt2[SLOT_SetStreamSource])(self->pReal, 0,
                    self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
                ((FN_SetDecl)vt2[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
            } else {
                hr = ((FN)vt2[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
            }
        } else {
            hr = ((FN)RealVtbl(self)[SLOT_DrawPrimitive])(self->pReal, pt, sv, pc);
        }
        /* Restore texture stage states, then texture bindings */
        {
            typedef int (__stdcall *FN_SetTSS)(void*, unsigned int, unsigned int, unsigned int);
            typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
            int ts;
            for (ts = 0; ts < 8; ts++) {
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLOROP,   self->savedTSS[ts].colorOp);
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG1,  self->savedTSS[ts].colorArg1);
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG2,  self->savedTSS[ts].colorArg2);
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAOP,    self->savedTSS[ts].alphaOp);
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG1,  self->savedTSS[ts].alphaArg1);
                ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG2,  self->savedTSS[ts].alphaArg2);
            }
            for (ts = 0; ts < 8; ts++)
                ((FN_SetTex)RealVtbl(self)[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
        }
    } else {
        /* Passthrough: POSITIONT / no decl / pre-viewProj / no NORMAL */
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
    self->dipCount++;

    if (self->viewProjValid) {
        if (self->curDeclHasPosT) {
            /* Screen-space (POSITIONT) -> HUD/UI passthrough */
            FFP_Disengage(self);
            hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        } else if (self->curDeclIsSkinned) {
            hr = Skin_DrawDIP(self, pt, bvi, mi, nv, si, pc);
        } else if (!self->curDeclHasNormal) {
            if (self->curDeclHasColor) {
                /* No NORMAL but has COLOR (Scaleform UI: text glyphs, colored
                 * shapes). Passthrough with original shaders. */
                FFP_Disengage(self);
                hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            } else {
                /* Position-only geometry (z-prepass, shadow maps): skip.
                 * Remix does its own depth and shadow passes. */
                hr = 0;
            }
        } else if (!self->curDeclHasTexcoord) {
            /* Terrain3 (base or overlay): CPU-expand vertices with computed
             * TEXCOORD from c212/c213. Checked before alpha-blend drop so
             * terrain overlay draws route here instead of being dropped. */
            hr = Terrain3_DrawDIP(self, pt, bvi, mi, nv, si, pc);
        } else if (self->curAlphaBlendEnabled) {
            /* Alpha-blended 3D overlays with TEXCOORD (particles, clouds): drop.
             * These are shader-driven effects rendered on top of opaque geometry
             * that Remix cannot handle. */
            hr = 0;
        } else {
            /* Rigid 3D geometry with NORMAL + TEXCOORD: engage FFP */
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

            /* D3DCOLOR NORMAL → FLOAT3 expansion for Remix compatibility */
            if (self->curDeclNormalType == D3DDECLTYPE_D3DCOLOR) {
                typedef int (__stdcall *FN_SetStream)(void*, unsigned int, void*, unsigned int, unsigned int);
                typedef int (__stdcall *FN_SetDecl)(void*, void*);
                void **vt2 = RealVtbl(self);
                void *expDecl = NormExp_GetClonedDecl(self);
                void *expVB   = NormExp_GetExpandedVB(self, bvi, mi, nv);
                if (expDecl && expVB) {
                    ((FN_SetStream)vt2[SLOT_SetStreamSource])(self->pReal, 0,
                        expVB, 0, self->streamStride[0] + 8);
                    ((FN_SetDecl)vt2[SLOT_SetVertexDeclaration])(self->pReal, expDecl);
                    hr = ((FN)vt2[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
                        -(int)mi, 0, nv, si, pc);
                    ((FN_SetStream)vt2[SLOT_SetStreamSource])(self->pReal, 0,
                        self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
                    ((FN_SetDecl)vt2[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
                } else {
                    hr = ((FN)vt2[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
                        bvi, mi, nv, si, pc);
                }
            } else {
                hr = ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitive])(self->pReal, pt,
                    bvi, mi, nv, si, pc);
            }

            /* Restore texture stage states, then texture bindings */
            {
                typedef int (__stdcall *FN_SetTSS)(void*, unsigned int, unsigned int, unsigned int);
                typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
                int ts;
                for (ts = 0; ts < 8; ts++) {
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLOROP,   self->savedTSS[ts].colorOp);
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG1,  self->savedTSS[ts].colorArg1);
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_COLORARG2,  self->savedTSS[ts].colorArg2);
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAOP,    self->savedTSS[ts].alphaOp);
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG1,  self->savedTSS[ts].alphaArg1);
                    ((FN_SetTSS)RealVtbl(self)[SLOT_SetTextureStageState])(self->pReal, ts, D3DTSS_ALPHAARG2,  self->savedTSS[ts].alphaArg2);
                }
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
            log_int("    bonesDrawn=", self->bonesDrawn);
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
            if (mat4_is_interesting(&self->vsConst[VS_REG_WORLD_START*4]))
                diag_log_matrix("    World c178-c181", &self->vsConst[VS_REG_WORLD_START*4]);
#if VIEWPROJ_COMBINED
            if (mat4_is_interesting(&self->vsConst[VS_REG_VIEWPROJ_START*4]))
                diag_log_matrix("    ViewProj c239-c242", &self->vsConst[VS_REG_VIEWPROJ_START*4]);
#else
            if (mat4_is_interesting(&self->vsConst[VS_REG_VIEW_START*4]))
                diag_log_matrix("    View", &self->vsConst[VS_REG_VIEW_START*4]);
            if (mat4_is_interesting(&self->vsConst[VS_REG_PROJ_START*4]))
                diag_log_matrix("    Proj", &self->vsConst[VS_REG_PROJ_START*4]);
#endif
            /* z-prepass WVP (c198-c201) */
            if (mat4_is_interesting(&self->vsConst[198*4]))
                diag_log_matrix("    zWVP c198-c201", &self->vsConst[198*4]);
#if ENABLE_SKINNING
            if (self->curDeclIsSkinned && self->numBones > 0) {
                log_int("    bones uploaded=", self->numBones);
                log_int("    lastBoneStartReg=", self->lastBoneStartReg);
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
#if VIEWPROJ_COMBINED
            if (startReg < VS_REG_VIEWPROJ_END && endReg > VS_REG_VIEWPROJ_START)
                self->viewProjDirty = 1;
#else
            if (startReg < VS_REG_PROJ_END && endReg > VS_REG_VIEW_START)
                self->viewProjDirty = 1;
#endif
            if (startReg < VS_REG_WORLD_END && endReg > VS_REG_WORLD_START)
                self->worldDirty = 1;
        }

        /* Mark ViewProj valid once the relevant ranges have been written */
#if VIEWPROJ_COMBINED
        if (startReg <= VS_REG_VIEWPROJ_START && startReg + count >= VS_REG_VIEWPROJ_END)
            self->viewProjValid = 1;
        else if (startReg == VS_REG_VIEWPROJ_START && count >= 4)
            self->viewProjValid = 1;
#else
        if (startReg <= VS_REG_PROJ_START && startReg + count >= VS_REG_PROJ_END)
            self->viewProjValid = 1;
        else if (startReg == VS_REG_VIEW_START && count >= 4 && self->vsConstWriteLog[VS_REG_PROJ_START])
            self->viewProjValid = 1;
        else if (startReg == VS_REG_PROJ_START && count >= 4 && self->vsConstWriteLog[VS_REG_VIEW_START])
            self->viewProjValid = 1;
#endif

        for (i = 0; i < count; i++) {
            if (startReg + i < 256)
                self->vsConstWriteLog[startReg + i] = 1;
        }

#if ENABLE_SKINNING
        /* Deferred bone tracking: bones are composed with World at draw time.
         * Here we just track which bones were written (data is in vsConst[]). */
        if (startReg < VS_REG_BONE_END &&
            count >= VS_REGS_PER_BONE &&
            (count % VS_REGS_PER_BONE) == 0 &&
            self->curDeclIsSkinned) {

            int boneEnd = (startReg + count) / VS_REGS_PER_BONE;

            /* Detect object boundary */
            if (self->bonesDrawn || (self->numBones > 0 && (int)startReg < self->lastBoneStartReg)) {
                self->prevNumBones = self->numBones;
                self->numBones = 0;
                self->bonesDrawn = 0;
            }
            self->lastBoneStartReg = startReg;

            /* Track highest bone index seen */
            if (boneEnd > self->numBones)
                self->numBones = boneEnd;
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
    if (pData && startReg + count <= 224) {
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

/* 83: DrawPrimitiveUP — count and passthrough (diagnostic) */
static int __stdcall WD_DrawPrimitiveUP(WrappedDevice *self,
    unsigned int pt, unsigned int pc, const void *pData, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, const void*, unsigned int);
    self->drawCallCount++;
    self->dpupCount++;
    return ((FN)RealVtbl(self)[SLOT_DrawPrimitiveUP])(self->pReal, pt, pc, pData, stride);
}

/* 84: DrawIndexedPrimitiveUP — count and passthrough (diagnostic) */
static int __stdcall WD_DrawIndexedPrimitiveUP(WrappedDevice *self,
    unsigned int pt, unsigned int mi, unsigned int nv, unsigned int pc,
    const void *pIdx, unsigned int idxFmt, const void *pVtx, unsigned int stride)
{
    typedef int (__stdcall *FN)(void*, unsigned int, unsigned int, unsigned int, unsigned int,
        const void*, unsigned int, const void*, unsigned int);
    self->drawCallCount++;
    self->dipupCount++;
    return ((FN)RealVtbl(self)[SLOT_DrawIndexedPrimitiveUP])(self->pReal,
        pt, mi, nv, pc, pIdx, idxFmt, pVtx, stride);
}

/* 89: SetFVF — track usage (diagnostic) */
static int __stdcall WD_SetFVF(WrappedDevice *self, unsigned int fvf) {
    typedef int (__stdcall *FN)(void*, unsigned int);
    self->fvfSetCount++;
#if DIAG_ENABLED
    if (DIAG_ACTIVE(self)) {
        log_hex("  SetFVF fvf=", fvf);
    }
#endif
    return ((FN)RealVtbl(self)[SLOT_SetFVF])(self->pReal, fvf);
}

/* 87: SetVertexDeclaration — Parse vertex elements, detect skinning */
static int __stdcall WD_SetVertexDeclaration(WrappedDevice *self, void *pDecl) {
    typedef int (__stdcall *FN)(void*, void*);

#if ENABLE_SKINNING
    /* Declaration change while bones are loaded → likely new object */
    if (self->curDeclIsSkinned && self->numBones > 0 && pDecl != self->lastDecl)
        self->bonesDrawn = 1;
#endif

    self->lastDecl = pDecl;
    self->curDeclIsSkinned = 0;
    self->curDeclHasTexcoord = 0;
    self->curDeclHasNormal = 0;
    self->curDeclHasColor = 0;
    self->curDeclHasPosT = 0;
    self->curDeclTexcoordType = -1;
    self->curDeclTexcoordOff = 0;
#if ENABLE_SKINNING
    self->curDeclHasBlendWeight = 0;
    self->curDeclNumWeights = 0;
    self->curDeclBlendIndicesOff = 0;
    self->curDeclBlendIndicesType = 0;
#if EXPAND_SKIN_VERTICES
    self->curDeclNormalOff = 0;    self->curDeclNormalType = -1;
    self->curDeclBlendWeightOff = 0;
    self->curDeclBlendWeightType = -1;
    self->curDeclPosOff = 0;
#endif
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
                int blendWeightType = 0;

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
#if ENABLE_SKINNING && EXPAND_SKIN_VERTICES
                        self->curDeclBlendWeightOff  = offset;
                        self->curDeclBlendWeightType = type;
#endif
                    }
                    if (usage == D3DDECLUSAGE_BLENDINDICES) {
                        hasBlendIndices = 1;
#if ENABLE_SKINNING
                        self->curDeclBlendIndicesOff = offset;
                        self->curDeclBlendIndicesType = type;
#endif
                    }
                    if (usage == D3DDECLUSAGE_POSITION && stream == 0) {
#if ENABLE_SKINNING && EXPAND_SKIN_VERTICES
                        self->curDeclPosOff = offset;
#endif
                    }
                    if (usage == D3DDECLUSAGE_NORMAL && stream == 0) {
                        self->curDeclHasNormal = 1;
#if ENABLE_SKINNING && EXPAND_SKIN_VERTICES
                        self->curDeclNormalOff  = offset;
                        self->curDeclNormalType = type;
#endif
                    }
                    if (usage == D3DDECLUSAGE_TEXCOORD && usageIdx == 0 && stream == 0) {
                        self->curDeclHasTexcoord    = 1;
                        self->curDeclTexcoordType   = type;
                        self->curDeclTexcoordOff    = offset;
                    }
                    if (usage == D3DDECLUSAGE_COLOR) {
                        self->curDeclHasColor = 1;
                    }
                }

                if (hasBlendWeight && hasBlendIndices) {
                    self->curDeclIsSkinned = 1;
#if ENABLE_SKINNING
                    self->curDeclHasBlendWeight = 1;
                    /* Infer weight count from BLENDWEIGHT element type */
                    switch (blendWeightType) {
                        case D3DDECLTYPE_FLOAT1:  self->curDeclNumWeights = 1; break;
                        case D3DDECLTYPE_FLOAT2:  self->curDeclNumWeights = 2; break;
                        case D3DDECLTYPE_FLOAT3:  self->curDeclNumWeights = 3; break;
                        case D3DDECLTYPE_FLOAT4:  self->curDeclNumWeights = 3; break;
                        case D3DDECLTYPE_UBYTE4N: self->curDeclNumWeights = 3; break;
                        case D3DDECLTYPE_D3DCOLOR: self->curDeclNumWeights = 3; break;
                        default:                  self->curDeclNumWeights = 3; break;
                    }
#endif
                } else if (hasBlendIndices) {
                    /* Single-bone: BLENDINDICES without BLENDWEIGHT */
                    self->curDeclIsSkinned = 1;
#if ENABLE_SKINNING
                    self->curDeclHasBlendWeight = 0;
                    self->curDeclNumWeights = 1;
#endif
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
    s_device_vtbl[57] = (void*)WD_SetRenderState;  /* INTERCEPTED */
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
    s_device_vtbl[83] = (void*)WD_DrawPrimitiveUP;     /* INTERCEPTED (diag) */
    s_device_vtbl[84] = (void*)WD_DrawIndexedPrimitiveUP; /* INTERCEPTED (diag) */
    s_device_vtbl[85] = (void*)Relay_85;
    s_device_vtbl[86] = (void*)Relay_86;
    s_device_vtbl[87] = (void*)WD_SetVertexDeclaration; /* INTERCEPTED */
    s_device_vtbl[88] = (void*)Relay_88;
    s_device_vtbl[89] = (void*)WD_SetFVF;              /* INTERCEPTED (diag) */
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
    w->curAlphaBlendEnabled = 0;
    w->curZWriteEnabled = 1;  /* D3D default is TRUE */
    w->gameZEnable = 1;       /* D3DZB_TRUE */
    w->depthBias = 0.0f;
    w->slopeScaleDepthBias = 0.0f;
    w->savedLighting = 1;
#if ENABLE_SKINNING
    Skin_InitDevice(w, pRealDevice);
#endif
    NormExp_InitDevice(w);

    /* Terrain3 expanded vertex declaration: FLOAT3 pos + FLOAT3 normal + FLOAT2 uv */
    {
        typedef int (__stdcall *FN_CreateDecl)(void*, void*, void**);
        typedef struct { unsigned short Stream; unsigned short Offset;
                         unsigned char Type; unsigned char Method;
                         unsigned char Usage; unsigned char UsageIndex; } D3DVE;
        static const D3DVE s_terrain_decl[] = {
            {0,  0, 2/*FLOAT3*/,  0, 0/*POSITION*/, 0},
            {0, 12, 2/*FLOAT3*/,  0, 3/*NORMAL*/,   0},
            {0, 24, 1/*FLOAT2*/,  0, 5/*TEXCOORD*/, 0},
            {0xFF,0,17,           0, 0,              0}  /* D3DDECL_END */
        };
        w->terrainExpDecl = NULL;
        ((FN_CreateDecl)(*(void***)pRealDevice)[SLOT_CreateVertexDeclaration])(
            pRealDevice, (void*)s_terrain_decl, &w->terrainExpDecl);
        if (w->terrainExpDecl)
            log_hex("  Terrain decl: ", (unsigned int)w->terrainExpDecl);
        else
            log_str("  WARNING: terrain decl creation failed\r\n");
    }

    { int s; for (s = 0; s < 4; s++) { w->streamVB[s] = NULL; w->streamOffset[s] = 0; w->streamStride[s] = 0; } }
    { int t; for (t = 0; t < 8; t++) w->curTexture[t] = NULL; }
    w->loggedDeclCount = 0;
    { int ts; for (ts = 0; ts < 8; ts++) w->diagTexUniq[ts] = 0; }
    w->createTick = GetTickCount();
    w->diagLoggedFrames = 0;

    /* Read config from INI */
    {
        char iniBuf[260];
        char valBuf[32];
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

        /* DepthBias / SlopeScaleDepthBias — float values read as strings */
        GetPrivateProfileStringA("FFP", "DepthBias", "0.0", valBuf, 32, iniBuf);
        {
            float result = 0.0f, frac = 0.0f, div = 1.0f;
            int neg = 0, p2 = 0, hasDot = 0;
            if (valBuf[0] == '-') { neg = 1; p2 = 1; }
            for (; valBuf[p2]; p2++) {
                if (valBuf[p2] == '.') { hasDot = 1; continue; }
                if (valBuf[p2] < '0' || valBuf[p2] > '9') break;
                if (hasDot) { div *= 10.0f; frac += (valBuf[p2] - '0') / div; }
                else { result = result * 10.0f + (valBuf[p2] - '0'); }
            }
            w->depthBias = neg ? -(result + frac) : (result + frac);
        }
        GetPrivateProfileStringA("FFP", "SlopeScaleDepthBias", "0.0", valBuf, 32, iniBuf);
        {
            float result = 0.0f, frac = 0.0f, div = 1.0f;
            int neg = 0, p2 = 0, hasDot = 0;
            if (valBuf[0] == '-') { neg = 1; p2 = 1; }
            for (; valBuf[p2]; p2++) {
                if (valBuf[p2] == '.') { hasDot = 1; continue; }
                if (valBuf[p2] < '0' || valBuf[p2] > '9') break;
                if (hasDot) { div *= 10.0f; frac += (valBuf[p2] - '0') / div; }
                else { result = result * 10.0f + (valBuf[p2] - '0'); }
            }
            w->slopeScaleDepthBias = neg ? -(result + frac) : (result + frac);
        }
    }

    log_str("WrappedDevice created with FFP conversion\r\n");
    log_int("  Diag delay (ms): ", DIAG_DELAY_MS);
    log_int("  AlbedoStage: ", w->albedoStage);
    log_float_val("  DepthBias: ", w->depthBias);
    log_float_val("  SlopeScaleDepthBias: ", w->slopeScaleDepthBias);
    log_hex("  Real device: ", (unsigned int)pRealDevice);
    return w;
}
