/*
 * d3d9_skinning.h — FFP Indexed Vertex Blending Extension
 *
 * This file is #included by d3d9_device.c ONLY when ENABLE_SKINNING=1.
 * It converts skinned meshes (BLENDWEIGHT + BLENDINDICES vertex declarations)
 * to D3D9 FFP indexed vertex blending with bone matrices.
 *
 * Bone matrices are uploaded immediately in SetVertexShaderConstantF (not
 * deferred to draw time). Per-object stale bone clearing prevents matrix
 * leakage between separate skinned meshes.
 *
 * EXPAND_SKIN_VERTICES (default 0):
 *   0 = draw with original VB and declaration (simpler, works when FFP
 *       accepts the game's vertex format directly)
 *   1 = expand vertices to a fixed 48-byte layout with VB caching (needed
 *       when the game's vertex format isn't directly FFP-compatible)
 *
 * THIS IS A LATE-STAGE EXTENSION. Enable only after rigid FFP works.
 * To enable: set #define ENABLE_SKINNING 1 in d3d9_device.c
 *
 * All functions are static and require the WrappedDevice struct, slot
 * enums, and D3D9 constants from d3d9_device.c to be defined first.
 */

/* Forward declarations — defined after this header is included */
static void FFP_Engage(WrappedDevice *self);
static void FFP_Disengage(WrappedDevice *self);
static void FFP_RestoreTSS(WrappedDevice *self);

/* Row-major 4x4 matrix multiplication: out = a * b */
static void mat4_mul(float *out, const float *a, const float *b) {
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i*4+j] = a[i*4+0]*b[0*4+j] + a[i*4+1]*b[1*4+j]
                        + a[i*4+2]*b[2*4+j] + a[i*4+3]*b[3*4+j];
}

/* ======================================================================
 * Declaration Cloning (D3DCOLOR → UBYTE4 for BLENDINDICES)
 *
 * RTX Remix requires UBYTE4 for bone indices in captured meshes — D3DCOLOR
 * BLENDINDICES produces corrupted skeleton data in Remix captures. Many
 * MSVC-compiled games use D3DCOLOR. This clones the vertex declaration,
 * patching only the BLENDINDICES type, and uses the original VB unchanged.
 *
 * D3DCOLOR BLENDINDICES stores bone indices as raw bytes in BGRA order.
 * Many game shaders read them with a .zyxw swizzle that undoes the
 * hardware BGRA→RGBA decoding, leaving raw bytes. UBYTE4 reads those
 * same raw bytes in .xyzw order, so no vertex data modification is needed.
 *
 * NOTE: If your game uses a different swizzle convention, bone indices
 * may appear transposed. Check the diagnostic log for correct bone
 * assignment after enabling this.
 * ====================================================================== */

/*
 * Get or create a cloned vertex declaration with BLENDINDICES type
 * changed from D3DCOLOR to UBYTE4. Caches up to SKIN_DECL_CACHE_SIZE
 * original→clone mappings. Returns NULL if the current declaration
 * doesn't use D3DCOLOR BLENDINDICES (no cloning needed).
 */
static void* Skin_GetClonedDecl(WrappedDevice *self) {
    typedef int (__stdcall *FN_GetDecl)(void*, void*, unsigned int*);
    typedef int (__stdcall *FN_CreateDecl)(void*, void*, void**);
    void *origDecl = self->lastDecl;
    int i;
    unsigned char elemBuf[8 * 32];
    unsigned int numElems = 0;
    void **declVt;
    int hr;
    void *newDecl;

    if (!origDecl) return NULL;

    /* Only clone if BLENDINDICES is D3DCOLOR — UBYTE4 needs no patching */
    if (self->curDeclBlendIndicesType != D3DDECLTYPE_D3DCOLOR) return NULL;

    /* Check cache */
    for (i = 0; i < self->skinDeclCount; i++) {
        if (self->skinDeclOrig[i] == origDecl)
            return self->skinDeclClone[i];
    }

    /* Clone: get elements from original declaration */
    declVt = *(void***)origDecl;
    hr = ((FN_GetDecl)declVt[4])(origDecl, NULL, &numElems);
    if (hr != 0 || numElems == 0 || numElems > 32) return NULL;

    hr = ((FN_GetDecl)declVt[4])(origDecl, elemBuf, &numElems);
    if (hr != 0) return NULL;

    /* Patch BLENDINDICES: D3DCOLOR(4) → UBYTE4(5) */
    for (i = 0; i < (int)numElems; i++) {
        unsigned char *el = &elemBuf[i * 8];
        if (el[0] == 0xFF) break;  /* D3DDECL_END */
        if (el[6] == D3DDECLUSAGE_BLENDINDICES && el[4] == D3DDECLTYPE_D3DCOLOR) {
            el[4] = D3DDECLTYPE_UBYTE4;
        }
    }

    /* Create new declaration from patched elements */
    newDecl = NULL;
    hr = ((FN_CreateDecl)RealVtbl(self)[SLOT_CreateVertexDeclaration])(
        self->pReal, (void*)elemBuf, &newDecl);
    if (hr != 0 || !newDecl) return NULL;

    /* Store in cache (evict slot 0 if full) */
    if (self->skinDeclCount < SKIN_DECL_CACHE_SIZE) {
        self->skinDeclOrig[self->skinDeclCount] = origDecl;
        self->skinDeclClone[self->skinDeclCount] = newDecl;
        self->skinDeclCount++;
    } else {
        typedef unsigned long (__stdcall *FN_Release)(void*);
        ((FN_Release)(*(void***)self->skinDeclClone[0])[2])(self->skinDeclClone[0]);
        for (i = 1; i < SKIN_DECL_CACHE_SIZE; i++) {
            self->skinDeclOrig[i-1] = self->skinDeclOrig[i];
            self->skinDeclClone[i-1] = self->skinDeclClone[i];
        }
        self->skinDeclOrig[SKIN_DECL_CACHE_SIZE-1] = origDecl;
        self->skinDeclClone[SKIN_DECL_CACHE_SIZE-1] = newDecl;
    }

    log_hex("  SkinDecl cloned: orig=", (unsigned int)origDecl);
    log_hex("    clone=", (unsigned int)newDecl);
    return newDecl;
}

/* Release all cached cloned declarations */
static void Skin_ReleaseDeclCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < self->skinDeclCount; i++) {
        if (self->skinDeclClone[i])
            ((FN_Release)(*(void***)self->skinDeclClone[i])[2])(self->skinDeclClone[i]);
    }
    self->skinDeclCount = 0;
}

/* ======================================================================
 * Vertex Expansion (only when EXPAND_SKIN_VERTICES=1)
 * ====================================================================== */

#if EXPAND_SKIN_VERTICES

/* ---- Format Decoders ---- */

/* IEEE 754 binary16 (FLOAT16_2) → binary32 */
static float half_to_float(unsigned short h) {
    unsigned int sign = (unsigned int)(h >> 15) << 31;
    unsigned int exp  = (h >> 10) & 0x1F;
    unsigned int mant = (unsigned int)(h & 0x3FF);
    unsigned int f;
    if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {
            exp = 113;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            f = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000 | (mant << 13);
    } else {
        f = sign | ((exp + 112) << 23) | (mant << 13);
    }
    { float r; memcpy(&r, &f, 4); return r; }
}

/* Decode a compressed normal to FLOAT3.
 * Handles SHORT4N, DEC3N, UDEC3, and direct FLOAT3/FLOAT4. */
static void decode_normal(float *out3, const unsigned char *src, int type) {
    switch (type) {
        case D3DDECLTYPE_SHORT4N: {
            signed short s0 = (signed short)((unsigned short)(src[0] | ((unsigned short)src[1] << 8)));
            signed short s1 = (signed short)((unsigned short)(src[2] | ((unsigned short)src[3] << 8)));
            signed short s2 = (signed short)((unsigned short)(src[4] | ((unsigned short)src[5] << 8)));
            out3[0] = s0 / 32767.0f;
            out3[1] = s1 / 32767.0f;
            out3[2] = s2 / 32767.0f;
            break;
        }
        case D3DDECLTYPE_DEC3N: {
            unsigned int p = src[0] | ((unsigned int)src[1]<<8) | ((unsigned int)src[2]<<16) | ((unsigned int)src[3]<<24);
            int x = (int)(p & 0x3FF);         if (x >= 512) x -= 1024;
            int y = (int)((p >> 10) & 0x3FF);  if (y >= 512) y -= 1024;
            int z = (int)((p >> 20) & 0x3FF);  if (z >= 512) z -= 1024;
            out3[0] = x / 511.0f;
            out3[1] = y / 511.0f;
            out3[2] = z / 511.0f;
            break;
        }
        case D3DDECLTYPE_UDEC3: {
            unsigned int p = src[0] | ((unsigned int)src[1]<<8) | ((unsigned int)src[2]<<16) | ((unsigned int)src[3]<<24);
            out3[0] = (float)(p & 0x3FF) / 1023.0f;
            out3[1] = (float)((p >> 10) & 0x3FF) / 1023.0f;
            out3[2] = (float)((p >> 20) & 0x3FF) / 1023.0f;
            break;
        }
        case D3DDECLTYPE_D3DCOLOR: {
            /* D3DCOLOR: memory bytes [B,G,R,A] → shader float4(R/255,G/255,B/255,A/255)
             * Shader remaps [0,1]→[-1,1] via mad v3, 2, -1. Apply that here. */
            out3[0] = src[2] / 127.5f - 1.0f;  /* R → normal.x */
            out3[1] = src[1] / 127.5f - 1.0f;  /* G → normal.y */
            out3[2] = src[0] / 127.5f - 1.0f;  /* B → normal.z */
            break;
        }
        default: { /* FLOAT3, FLOAT4: direct copy of first 3 components */
            float *f = (float*)src;
            out3[0] = f[0]; out3[1] = f[1]; out3[2] = f[2];
            break;
        }
    }
}

/* ---- Vertex Expansion ---- */

/*
 * Expand one source vertex (src) into the fixed 48-byte skinned layout (dst):
 *   offset  0: FLOAT3 POSITION
 *   offset 12: FLOAT3 BLENDWEIGHT (3 components; unused slots padded with 0.0)
 *   offset 24: UBYTE4 BLENDINDICES (copied as-is)
 *   offset 28: FLOAT3 NORMAL      (decoded from any compressed source format)
 *   offset 40: FLOAT2 TEXCOORD[0] (decoded from FLOAT16_2, or direct copy)
 */
static void expand_skin_vertex(unsigned char *dst, const unsigned char *src, WrappedDevice *self) {
    float *dstF = (float*)dst;

    /* POSITION: FLOAT3 */
    { float *sp = (float*)(src + self->curDeclPosOff); dstF[0]=sp[0]; dstF[1]=sp[1]; dstF[2]=sp[2]; }

    /* BLENDWEIGHT: up to FLOAT3, pad remaining with 0 */
    {
        int bwT = self->curDeclBlendWeightType;
        if (!self->curDeclHasBlendWeight) {
            /* Single-bone: no BLENDWEIGHT element → full weight on one bone */
            dstF[3] = 1.0f; dstF[4] = 0.0f; dstF[5] = 0.0f;
        } else if (bwT >= D3DDECLTYPE_FLOAT1 && bwT <= D3DDECLTYPE_FLOAT4) {
            float *sw = (float*)(src + self->curDeclBlendWeightOff);
            dstF[3] = sw[0];
            dstF[4] = (bwT >= D3DDECLTYPE_FLOAT2) ? sw[1] : 0.0f;
            dstF[5] = (bwT >= D3DDECLTYPE_FLOAT3) ? sw[2] : 0.0f;
        } else if (bwT == D3DDECLTYPE_D3DCOLOR) {
            /* D3DCOLOR: memory [B,G,R,A] → shader float4(R/255,G/255,B/255,A/255) */
            const unsigned char *sw = src + self->curDeclBlendWeightOff;
            dstF[3] = sw[2] / 255.0f;  /* R → weight 0 */
            dstF[4] = sw[1] / 255.0f;  /* G → weight 1 */
            dstF[5] = sw[0] / 255.0f;  /* B → weight 2 */
        } else if (bwT == D3DDECLTYPE_UBYTE4N) {
            const unsigned char *sw = src + self->curDeclBlendWeightOff;
            dstF[3] = sw[0] / 255.0f;
            dstF[4] = sw[1] / 255.0f;
            dstF[5] = sw[2] / 255.0f;
        } else { dstF[3] = 0.0f; dstF[4] = 0.0f; dstF[5] = 0.0f; }
    }

    /* BLENDINDICES: UBYTE4 → 4 bytes at dst offset 24 */
    { const unsigned char *si = src + self->curDeclBlendIndicesOff;
      dst[24]=si[0]; dst[25]=si[1]; dst[26]=si[2]; dst[27]=si[3]; }

    /* NORMAL: decoded to FLOAT3 at dst offset 28 (dstF[7..9]) */
    if (self->curDeclNormalType >= 0)
        decode_normal(&dstF[7], src + self->curDeclNormalOff, self->curDeclNormalType);
    else
        { dstF[7]=0.0f; dstF[8]=0.0f; dstF[9]=1.0f; }

    /* TEXCOORD[0]: output FLOAT2 at dst offset 40 (dstF[10..11]) */
    { int uvT = self->curDeclTexcoordType;
      if (uvT == D3DDECLTYPE_FLOAT16_2) {
          unsigned short *sh = (unsigned short*)(src + self->curDeclTexcoordOff);
          dstF[10] = half_to_float(sh[0]); dstF[11] = half_to_float(sh[1]);
      } else if (uvT >= D3DDECLTYPE_FLOAT1 && uvT <= D3DDECLTYPE_FLOAT4) {
          float *su = (float*)(src + self->curDeclTexcoordOff);
          dstF[10] = su[0];
          dstF[11] = (uvT >= D3DDECLTYPE_FLOAT2) ? su[1] : 0.0f;
      } else { dstF[10]=0.0f; dstF[11]=0.0f; }
    }
}

/* ---- Vertex Buffer Caching ---- */

/*
 * Look up or build an expanded vertex buffer for source vertices [baseVtx, baseVtx+nv-1].
 * Results are cached by a hash of (source VB ptr, baseVtx, nv, stride, decl ptr).
 * Returns the expanded IDirect3DVertexBuffer9* or NULL on failure.
 */
static void* SkinVB_GetExpanded(WrappedDevice *self, unsigned int baseVtx, unsigned int nv) {
    typedef int (__stdcall *FN_CreateVB)(void*, unsigned int, unsigned long, unsigned long, unsigned int, void**, void*);
    typedef int (__stdcall *FN_Lock)   (void*, unsigned int, unsigned int, void**, unsigned int);
    typedef int (__stdcall *FN_Unlock) (void*);
    typedef unsigned long (__stdcall *FN_Release)(void*);
    unsigned int key, v, srcStride, lockOff, lockSize;
    int slot, hrC, hrL;
    void *newVB, **srcVt, **dstVt;
    unsigned char *srcData, *dstData;

    if (!self->streamVB[0]) return NULL;

    key  = (unsigned int)self->streamVB[0]
         ^ (baseVtx * 2654435761u)
         ^ (nv * 40503u)
         ^ (self->streamStride[0] * 6700417u)
         ^ ((unsigned int)self->lastDecl * 2246822519u);
    slot = (int)(key % SKIN_CACHE_SIZE);

    if (self->skinExpVB[slot] && self->skinExpKey[slot] == key && self->skinExpNv[slot] == nv)
        return self->skinExpVB[slot];

    if (self->skinExpVB[slot]) {
        ((FN_Release)(*(void***)(self->skinExpVB[slot]))[2])(self->skinExpVB[slot]);
        self->skinExpVB[slot] = NULL;
    }

    newVB = NULL;
    hrC = ((FN_CreateVB)RealVtbl(self)[SLOT_CreateVertexBuffer])(
        self->pReal, nv * SKIN_VTX_SIZE, 8 /*WRITEONLY*/, 0, 1 /*MANAGED*/, &newVB, NULL);
    if (hrC != 0 || !newVB) { log_hex("SkinVB CreateVB failed hr=", (unsigned int)hrC); return NULL; }

    srcStride = self->streamStride[0];
    lockOff   = self->streamOffset[0] + baseVtx * srcStride;
    lockSize  = nv * srcStride;
    srcVt     = *(void***)self->streamVB[0];
    srcData   = NULL;
    hrL = ((FN_Lock)srcVt[11])(self->streamVB[0], lockOff, lockSize, (void**)&srcData, 0x10 /*READONLY*/);
    if (hrL != 0 || !srcData) {
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        log_hex("SkinVB src lock failed hr=", (unsigned int)hrL); return NULL;
    }

    dstVt   = *(void***)newVB;
    dstData = NULL;
    hrL = ((FN_Lock)dstVt[11])(newVB, 0, nv * SKIN_VTX_SIZE, (void**)&dstData, 0);
    if (hrL != 0 || !dstData) {
        ((FN_Unlock)srcVt[12])(self->streamVB[0]);
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        return NULL;
    }

    for (v = 0; v < nv; v++)
        expand_skin_vertex(dstData + v * SKIN_VTX_SIZE, srcData + v * srcStride, self);

    ((FN_Unlock)dstVt[12])(newVB);
    ((FN_Unlock)srcVt[12])(self->streamVB[0]);

    self->skinExpVB[slot]  = newVB;
    self->skinExpKey[slot] = key;
    self->skinExpNv[slot]  = nv;
    return newVB;
}

/* Release all cached expanded vertex buffers */
static void SkinVB_ReleaseCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < SKIN_CACHE_SIZE; i++) {
        if (self->skinExpVB[i]) {
            ((FN_Release)(*(void***)(self->skinExpVB[i]))[2])(self->skinExpVB[i]);
            self->skinExpVB[i] = NULL;
        }
    }
}

#endif /* EXPAND_SKIN_VERTICES */

/* ======================================================================
 * Bone Render State Setup
 * ====================================================================== */

/*
 * Compose World x Bone for each bone, upload via SetTransform, enable
 * FFP indexed vertex blending. Bones are read from vsConst[] (deferred
 * composition — bones are object-space in Magellan engine).
 */
static void Skin_UploadComposedBones(WrappedDevice *self) {
    typedef int (__stdcall *FN_ST)(void*, unsigned int, float*);
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);
    const float *world = &self->vsConst[VS_REG_WORLD_START * 4];
    int b;

    if (self->numBones <= 0) return;

    for (b = 0; b < self->numBones && b < MAX_FFP_BONES; b++) {
        const float *src = &self->vsConst[b * VS_REGS_PER_BONE * 4];
        float bone4x4[16], composed[16];

#if VS_CONSTANTS_ROW_MAJOR
        /* Transpose row_major float3x4 → D3D9 row-major 4x4.
         * Shader uses mul(bone, col_vec): each register is a row [Rx, Ry, Rz, T].
         * FFP uses row_vec * WORLDMATRIX: translation must be in row 3. */
        bone4x4[0]  = src[0];  bone4x4[1]  = src[4];  bone4x4[2]  = src[8];  bone4x4[3]  = 0.0f;
        bone4x4[4]  = src[1];  bone4x4[5]  = src[5];  bone4x4[6]  = src[9];  bone4x4[7]  = 0.0f;
        bone4x4[8]  = src[2];  bone4x4[9]  = src[6];  bone4x4[10] = src[10]; bone4x4[11] = 0.0f;
        bone4x4[12] = src[3];  bone4x4[13] = src[7];  bone4x4[14] = src[11]; bone4x4[15] = 1.0f;
#else
        /* Column-major float3x4: registers are columns, already D3D9 layout */
        bone4x4[0]  = src[0];  bone4x4[1]  = src[1];  bone4x4[2]  = src[2];  bone4x4[3]  = src[3];
        bone4x4[4]  = src[4];  bone4x4[5]  = src[5];  bone4x4[6]  = src[6];  bone4x4[7]  = src[7];
        bone4x4[8]  = src[8];  bone4x4[9]  = src[9];  bone4x4[10] = src[10]; bone4x4[11] = src[11];
        bone4x4[12] = 0.0f;    bone4x4[13] = 0.0f;    bone4x4[14] = 0.0f;    bone4x4[15] = 1.0f;
#endif

        /* Bone × World: bone transforms to object space, then World to world space */
        mat4_mul(composed, bone4x4, world);

        ((FN_ST)vt[SLOT_SetTransform])(self->pReal, D3DTS_WORLDMATRIX(b), composed);
    }

    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 1);
    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND,
        self->curDeclNumWeights <= 1 ? D3DVBF_1WEIGHTS :
        self->curDeclNumWeights == 2 ? D3DVBF_2WEIGHTS : D3DVBF_3WEIGHTS);

    self->worldDirty = 1;
    self->skinningSetup = 1;
}

/* ---- Skinning State Management ---- */

static void FFP_DisableSkinning(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    if (self->skinningSetup) {
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 0);
        ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
        self->skinningSetup = 0;
    }
}

/* ======================================================================
 * Skinned Draw Entry Point
 * ====================================================================== */

/*
 * Handle a skinned DrawIndexedPrimitive call.
 *
 * When EXPAND_SKIN_VERTICES=1: expands vertices to fixed layout, draws with
 * expanded VB. Falls back to shader passthrough if expansion fails.
 *
 * When EXPAND_SKIN_VERTICES=0: draws with the original VB and declaration
 * directly (the game's vertex format must be FFP-compatible).
 *
 * In both modes: enables indexed vertex blending, sets bonesDrawn=1 after
 * draw so the next object's bone writes trigger stale matrix clearing.
 */
static int Skin_DrawDIP(WrappedDevice *self, unsigned int pt, int bvi,
    unsigned int mi, unsigned int nv, unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN_DIP)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
    void **vt = RealVtbl(self);
    int hr;

    if (self->numBones <= 0) {
        /* No bones uploaded — passthrough with original shaders */
        FFP_Disengage(self);
        hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
        return hr;
    }

#if EXPAND_SKIN_VERTICES
    {
        typedef int (__stdcall *FN_SetSS)(void*, unsigned int, void*, unsigned int, unsigned int);
        typedef int (__stdcall *FN_SetVD)(void*, void*);
        unsigned int baseVtx = (unsigned int)bvi + mi;
        void *expVB = (self->skinExpDecl) ? SkinVB_GetExpanded(self, baseVtx, nv) : NULL;

        if (expVB) {
            FFP_Engage(self);
            Skin_UploadComposedBones(self);

            /* Albedo to stage 0, NULL remaining stages */
            {
                int as = self->albedoStage;
                void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
                int ts;
                ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
                for (ts = 1; ts < 8; ts++)
                    ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
            }

            /* Bind the expansion VB and declaration */
            ((FN_SetSS)vt[SLOT_SetStreamSource])(self->pReal, 0, expVB, 0, SKIN_VTX_SIZE);
            ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->skinExpDecl);

            /* Vertices [mi..mi+nv-1] map to expansion slots [0..nv-1] via bvi=-mi */
            hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal,
                pt, -(int)mi, mi, nv, si, pc);

            /* Restore source VB, original declaration, TSS, and texture bindings */
            ((FN_SetSS)vt[SLOT_SetStreamSource])(self->pReal, 0,
                self->streamVB[0], self->streamOffset[0], self->streamStride[0]);
            ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
            FFP_RestoreTSS(self);
            {
                int ts;
                for (ts = 0; ts < 8; ts++)
                    ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
            }
        } else {
            /* Expansion unavailable — shader passthrough */
            FFP_Disengage(self);
            hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
            return hr;
        }
    }
#else
    {
    /* No expansion — draw with original VB, optionally with cloned declaration
     * (D3DCOLOR BLENDINDICES → UBYTE4 for FFP indexed vertex blending). */
    typedef int (__stdcall *FN_SetVD)(void*, void*);
    void *clonedDecl = Skin_GetClonedDecl(self);

    FFP_Engage(self);
    Skin_UploadComposedBones(self);

    /* Albedo to stage 0, NULL remaining stages */
    {
        int as = self->albedoStage;
        void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
        int ts;
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
    }

    /* Bind cloned declaration if BLENDINDICES needed patching */
    if (clonedDecl)
        ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, clonedDecl);

    hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);

    /* Restore original declaration and texture bindings */
    if (clonedDecl)
        ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
    {
        int ts;
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
    }
    }
#endif

    /* Mark bones as drawn — counter resets on next bone write (new object).
     * Do NOT reset numBones here: multiple sub-meshes of the same skinned
     * object share the same bone set across consecutive draw calls. */
    self->bonesDrawn = 1;
    FFP_DisableSkinning(self);

    return hr;
}

/* ======================================================================
 * Device Lifecycle
 * ====================================================================== */

#if EXPAND_SKIN_VERTICES
/*
 * Create the shared expansion vertex declaration at device creation.
 * All skinned draws are expanded into a fixed 48-byte layout.
 */
static void Skin_InitDevice(WrappedDevice *w, void *pRealDevice) {
    typedef int (__stdcall *FN_CreateDecl)(void*, void*, void**);
    typedef struct { unsigned short Stream; unsigned short Offset;
                     unsigned char Type; unsigned char Method;
                     unsigned char Usage; unsigned char UsageIndex; } D3DVE;
    static const D3DVE s_skin_decl[] = {
        {0,  0, 2/*FLOAT3*/,0, 0/*POSITION*/,    0},
        {0, 12, 2/*FLOAT3*/,0, 1/*BLENDWEIGHT*/, 0},
        {0, 24, 5/*UBYTE4*/,0, 2/*BLENDINDICES*/,0},
        {0, 28, 2/*FLOAT3*/,0, 3/*NORMAL*/,      0},
        {0, 40, 1/*FLOAT2*/,0, 5/*TEXCOORD*/,    0},
        {0xFF,0,17,          0, 0,                0}  /* D3DDECL_END */
    };

    w->curDeclHasBlendWeight = 0;
    w->curDeclNumWeights = 0;
    w->numBones = 0;
    w->prevNumBones = 0;
    w->bonesDrawn = 0;
    w->lastBoneStartReg = 0;
    w->skinningSetup = 0;
    w->curDeclBlendIndicesOff = 0;
    w->curDeclBlendIndicesType = 0;
    w->skinDeclCount = 0;
    { int i; for (i = 0; i < SKIN_DECL_CACHE_SIZE; i++) { w->skinDeclOrig[i] = NULL; w->skinDeclClone[i] = NULL; } }
    w->curDeclNormalOff = 0;   w->curDeclNormalType = -1;
    w->curDeclBlendWeightOff = 0;
    w->curDeclBlendWeightType = -1;
    w->curDeclPosOff = 0;
    { int i; for (i = 0; i < SKIN_CACHE_SIZE; i++) { w->skinExpVB[i] = NULL; w->skinExpKey[i] = 0; w->skinExpNv[i] = 0; } }
    w->skinExpDecl = NULL;

    ((FN_CreateDecl)RealVtbl(w)[SLOT_CreateVertexDeclaration])(
        pRealDevice, (void*)s_skin_decl, &w->skinExpDecl);
    log_hex("  skinExpDecl: ", (unsigned int)w->skinExpDecl);
}

/* Free VB cache + skinExpDecl + decl clones on device release */
static void Skin_ReleaseDevice(WrappedDevice *self) {
    SkinVB_ReleaseCache(self);
    Skin_ReleaseDeclCache(self);
    if (self->skinExpDecl) {
        typedef unsigned long (__stdcall *FN_Release)(void*);
        ((FN_Release)(*(void***)self->skinExpDecl)[2])(self->skinExpDecl);
        self->skinExpDecl = NULL;
    }
}

#else /* !EXPAND_SKIN_VERTICES */

/* Initialize skinning state at device creation (no expansion) */
static void Skin_InitDevice(WrappedDevice *w, void *pRealDevice) {
    (void)pRealDevice;
    w->curDeclNumWeights = 0;
    w->numBones = 0;
    w->prevNumBones = 0;
    w->bonesDrawn = 0;
    w->lastBoneStartReg = 0;
    w->skinningSetup = 0;
    w->curDeclBlendIndicesOff = 0;
    w->curDeclBlendIndicesType = 0;
    w->skinDeclCount = 0;
    { int i; for (i = 0; i < SKIN_DECL_CACHE_SIZE; i++) { w->skinDeclOrig[i] = NULL; w->skinDeclClone[i] = NULL; } }
    log_str("  Skinning: enabled (immediate bone upload, decl-clone for D3DCOLOR)\r\n");
}

/* Release cloned declaration cache */
static void Skin_ReleaseDevice(WrappedDevice *self) {
    Skin_ReleaseDeclCache(self);
}

#endif /* EXPAND_SKIN_VERTICES */
