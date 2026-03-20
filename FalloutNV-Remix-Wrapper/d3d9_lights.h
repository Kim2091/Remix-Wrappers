/*
 * d3d9_lights.h — Engine Point Light Extraction via D3D9 SetLight/LightEnable
 *
 * #included by d3d9_device.c when ENABLE_LIGHTS=1.
 *
 * Reads FalloutNV's ShadowSceneNode point light list from game memory,
 * converts to D3DLIGHT9 structs, and submits via SetLight/LightEnable.
 * RTX Remix intercepts these D3D9 calls to create path-traced lights.
 *
 * Approach ported from NewVegasRTXHelper's FixedFunctionLighting().
 */

/* ======================================================================
 * Game Engine Addresses
 * ====================================================================== */

/* ShadowSceneNode* — the primary shadow/light scene graph node.
 * From NewVegasRTXHelper Game.cpp: SceneNode = *(ShadowSceneNode**)0x011F91C8 */
#define SHADOW_SCENE_NODE_PTR (*(void**)0x011F91C8)

/* Camera world position — used to convert world-space light positions to
 * camera-relative coordinates (matching how the proxy sets transforms).
 * From NewVegasRTXHelper Lights.cpp: NiPoint3* CameraPosition = (NiPoint3*)0x011F8E9C */
#define CAMERA_POSITION_PTR ((float*)0x011F8E9C)

/* ======================================================================
 * Structure Offsets
 *
 * All offsets from NewVegasRTXHelper GameNi.h (static_assert-verified).
 * ====================================================================== */

/* ShadowSceneNode (inherits NiNode, size 0x200) */
#define SSN_LIGHTS_START_OFF  0xB4  /* NiTList<ShadowSceneLight>.start (Entry*) */

/* NiTList<T>::Entry */
#define ENTRY_NEXT_OFF  0x00  /* Entry* next */
#define ENTRY_DATA_OFF  0x08  /* T* data */

/* ShadowSceneLight (size 0x250) */
#define SSL_SOURCE_LIGHT_OFF  0xF8  /* NiPointLight* sourceLight */

/* NiAVObject (size 0x9C) */
#define NI_WORLD_POS_OFF  0x8C  /* m_worldTransform.pos (NiPoint3: 3 floats) */

/* NiLight (inherits NiDynamicEffect inherits NiAVObject, size 0xF0) */
#define NI_DIMMER_OFF  0xC4  /* float Dimmer */
#define NI_AMB_OFF     0xC8  /* NiColor Amb (3 floats: r, g, b) */
#define NI_DIFF_OFF    0xD4  /* NiColor Diff (3 floats: r, g, b) */
#define NI_SPEC_OFF    0xE0  /* NiColor Spec (3 floats: Spec.r = range for point lights) */

/* NiPointLight (inherits NiLight, size 0xFC) */
#define NI_ATTEN0_OFF  0xF0  /* float Atten0 */
#define NI_ATTEN1_OFF  0xF4  /* float Atten1 */
#define NI_ATTEN2_OFF  0xF8  /* float Atten2 */

/* ======================================================================
 * Helper: read a float / pointer from a game pointer + byte offset
 * ====================================================================== */

static __inline float game_float(void *base, unsigned int off) {
    return *(float*)((unsigned char*)base + off);
}

static __inline float* game_float_ptr(void *base, unsigned int off) {
    return (float*)((unsigned char*)base + off);
}

static __inline void* game_ptr(void *base, unsigned int off) {
    return *(void**)((unsigned char*)base + off);
}

/* No-CRT sqrt via x87 */
static __inline float game_sqrt(float x) {
    float result;
    __asm {
        fld x
        fsqrt
        fstp result
    }
    return result;
}

/* ======================================================================
 * D3DLIGHT9 — matches the real struct layout (no D3D9 header dependency)
 * ====================================================================== */

typedef struct {
    unsigned int Type;       /* D3DLIGHTTYPE: 1=POINT, 2=SPOT, 3=DIRECTIONAL */
    float Diffuse[4];        /* D3DCOLORVALUE (r, g, b, a) */
    float Specular[4];
    float Ambient[4];
    float Position[3];       /* D3DVECTOR */
    float Direction[3];      /* D3DVECTOR */
    float Range;
    float Falloff;
    float Attenuation0;
    float Attenuation1;
    float Attenuation2;
    float Theta;
    float Phi;
} GameD3DLIGHT9;

#define D3DLIGHT_POINT 1

/* ======================================================================
 * Core: Extract point lights and submit via D3D9 SetLight/LightEnable
 *
 * Ported from NewVegasRTXHelper FixedFunctionLighting():
 *   - Walks ShadowSceneNode->lights (NiTList<ShadowSceneLight>)
 *   - Extracts NiPointLight properties: diffuse, ambient, position, attenuation
 *   - Submits as D3DLIGHT_POINT via SetLight/LightEnable on the real device
 *   - Disables stale lights from previous frame
 *
 * Light range mode (from proxy.ini [Lights] RangeMode):
 *   0: Use Spec.r directly (actual range value, per Wall_SoGB)
 *   1: Calculate from attenuation equation (solve for 1/255 falloff)
 *   2: INFINITY (let Remix determine range)
 * ====================================================================== */

static void FFP_UpdateLights(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetLight)(void*, unsigned int, const GameD3DLIGHT9*);
    typedef int (__stdcall *FN_LightEnable)(void*, unsigned int, int);

    void *sceneNode;
    float *camPos;
    float strength;
    void **vt;
    int i;
    void *entry;

    if (!self->lightsEnabled) return;
    if (self->lightsUpdatedThisFrame) return;

    sceneNode = SHADOW_SCENE_NODE_PTR;
    if (!sceneNode) return;

    camPos = CAMERA_POSITION_PTR;
    if (!camPos) return;

    self->lightsUpdatedThisFrame = 1;

    vt = RealVtbl(self);
    strength = self->lightIntensity;

    /* Walk point light list */
    entry = game_ptr(sceneNode, SSN_LIGHTS_START_OFF);
    i = 0;

    while (entry && i < MAX_EXTRACTED_LIGHTS) {
        void *ssl = game_ptr(entry, ENTRY_DATA_OFF);
        if (ssl) {
            void *niLight = game_ptr(ssl, SSL_SOURCE_LIGHT_OFF);
            if (niLight) {
                float *pos  = game_float_ptr(niLight, NI_WORLD_POS_OFF);
                float dimmer = game_float(niLight, NI_DIMMER_OFF);
                float *diff = game_float_ptr(niLight, NI_DIFF_OFF);
                float *amb  = game_float_ptr(niLight, NI_AMB_OFF);
                float specR = game_float(niLight, NI_SPEC_OFF);
                float atten0 = game_float(niLight, NI_ATTEN0_OFF);
                float atten1 = game_float(niLight, NI_ATTEN1_OFF);
                float atten2 = game_float(niLight, NI_ATTEN2_OFF);

                GameD3DLIGHT9 light;

                /* Zero-init (no CRT memset) */
                { unsigned int *p = (unsigned int*)&light; int z;
                  for (z = 0; z < (int)(sizeof(light)/4); z++) p[z] = 0; }

                light.Type = D3DLIGHT_POINT;

                light.Diffuse[0] = diff[0] * dimmer * strength;
                light.Diffuse[1] = diff[1] * dimmer * strength;
                light.Diffuse[2] = diff[2] * dimmer * strength;
                light.Diffuse[3] = 1.0f;

                light.Ambient[0] = amb[0];
                light.Ambient[1] = amb[1];
                light.Ambient[2] = amb[2];
                light.Ambient[3] = 1.0f;

                /* Camera-relative position */
                light.Position[0] = pos[0] - camPos[0];
                light.Position[1] = pos[1] - camPos[1];
                light.Position[2] = pos[2] - camPos[2];

                /* Range: configurable mode */
                if (self->lightRangeMode == 0) {
                    /* Actual range value (Spec.r), per Wall_SoGB */
                    light.Range = specR;
                } else if (self->lightRangeMode == 1) {
                    /* Calculate range such that attenuation = 1/255 */
                    if (atten2 > 0.0f) {
                        light.Range = (-atten1 + game_sqrt(atten1 * atten1 + 1020.0f * atten2))
                                      / (2.0f * atten2);
                    } else if (atten1 > 0.0f) {
                        light.Range = 255.0f / atten1;
                    }
                    /* else: Range stays 0 from zero-init */
                } else {
                    /* Mode 2: let Remix figure it out */
                    unsigned int inf = 0x7F800000;
                    light.Range = *(float*)&inf;
                }

                light.Attenuation0 = atten0;
                light.Attenuation1 = atten1;
                light.Attenuation2 = atten2;

                ((FN_SetLight)vt[SLOT_SetLight])(self->pReal, (unsigned int)i, &light);
                ((FN_LightEnable)vt[SLOT_LightEnable])(self->pReal, (unsigned int)i, 1);
                i++;
            }
        }
        entry = game_ptr(entry, ENTRY_NEXT_OFF);
    }

    /* Disable stale lights from previous frame */
    { int j;
      for (j = i; j < self->lastEnabledLights; j++) {
          ((FN_LightEnable)vt[SLOT_LightEnable])(self->pReal, (unsigned int)j, 0);
      }
    }
    self->lastEnabledLights = i;

#if DIAG_ENABLED
    if (DIAG_ACTIVE(self) && i > 0) {
        log_int("  Point lights submitted: ", i);
    }
#endif
}
