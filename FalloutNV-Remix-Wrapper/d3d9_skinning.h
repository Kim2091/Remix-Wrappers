/*
 * d3d9_skinning.h — FFP Indexed Vertex Blending for FalloutNV
 *
 * #included by d3d9_device.c when ENABLE_SKINNING=1.
 *
 * Converts skinned meshes (BLENDWEIGHT + BLENDINDICES) to D3D9 FFP indexed
 * vertex blending. The original vertex buffer is used unchanged — only the
 * vertex declaration is cloned with BLENDINDICES type changed from D3DCOLOR
 * to UBYTE4.
 *
 * FNV's skinning shader reads D3DCOLOR BLENDINDICES with a v3.zyxw swizzle
 * that undoes the hardware BGRA→RGBA decoding, meaning bone indices are
 * stored as raw bytes: byte[i] = bone index for weight[i]. UBYTE4 reads
 * raw bytes in the same order (.x=byte[0], .y=byte[1], ...), so no vertex
 * data modification is needed — just the declaration type change.
 *
 * Bone matrices are uploaded immediately in SetVertexShaderConstantF
 * (transpose 4×3 packed → 4×4 row-major via SetTransform). Per-object
 * bone reset is driven by game engine hooks (render_skinned + reset_bones).
 */

/* Forward declarations — defined after this header is included */
static void FFP_Engage(WrappedDevice *self);
static void FFP_Disengage(WrappedDevice *self);

/* ======================================================================
 * Declaration Cloning
 * ====================================================================== */

/*
 * Get or create a cloned vertex declaration with BLENDINDICES type
 * changed from D3DCOLOR to UBYTE4. Caches up to SKIN_DECL_CACHE_SIZE
 * original→clone mappings.
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

/* ======================================================================
 * Bone Render State Setup
 * ====================================================================== */

static void FFP_UploadBones(WrappedDevice *self) {
    typedef int (__stdcall *FN_SetRS)(void*, unsigned int, unsigned int);
    void **vt = RealVtbl(self);

    if (self->numBones <= 0) return;

    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_INDEXEDVERTEXBLENDENABLE, 1);
    ((FN_SetRS)vt[SLOT_SetRenderState])(self->pReal, D3DRS_VERTEXBLEND,
        self->curDeclNumWeights == 1 ? D3DVBF_1WEIGHTS :
        self->curDeclNumWeights == 2 ? D3DVBF_2WEIGHTS : D3DVBF_3WEIGHTS);

    self->worldDirty = 1;
    self->skinningSetup = 1;
}

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

static int Skin_DrawDIP(WrappedDevice *self, unsigned int pt, int bvi,
    unsigned int mi, unsigned int nv, unsigned int si, unsigned int pc)
{
    typedef int (__stdcall *FN_DIP)(void*, unsigned int, int, unsigned int, unsigned int, unsigned int, unsigned int);
    typedef int (__stdcall *FN_SetTex)(void*, unsigned int, void*);
    typedef int (__stdcall *FN_SetTransform)(void*, unsigned int, float*);
    typedef int (__stdcall *FN_SetVD)(void*, void*);
    void **vt = RealVtbl(self);
    void *clonedDecl;
    int hr;

    if (self->numBones <= 0) {
        FFP_Disengage(self);
        return ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
    }

    clonedDecl = Skin_GetClonedDecl(self);
    if (!clonedDecl) {
        FFP_Disengage(self);
        return ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);
    }

    /* Enter FFP — NULL shaders manually. Do NOT call FFP_Engage which
     * sets D3DTS_WORLD and would clobber WORLDMATRIX(0) = bone 0. */
    if (!self->ffpActive) {
        typedef int (__stdcall *FN_SetVS)(void*, void*);
        typedef int (__stdcall *FN_SetPS)(void*, void*);
        ((FN_SetVS)vt[SLOT_SetVertexShader])(self->pReal, NULL);
        ((FN_SetPS)vt[SLOT_SetPixelShader])(self->pReal, NULL);
        self->ffpActive = 1;
    }

    /* VIEW + PROJECTION from NiDX9Renderer */
    {
        float *view = GameRenderer_GetMatrix(RENDERER_VIEW_OFF);
        float *proj = GameRenderer_GetMatrix(RENDERER_PROJ_OFF);
        if (view && proj) {
            ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_VIEW, view);
            ((FN_SetTransform)vt[SLOT_SetTransform])(self->pReal, D3DTS_PROJECTION, proj);
        }
    }

    FFP_SetupTextureStages(self);
    if (!self->ffpSetup) {
        FFP_SetupLighting(self);
        self->ffpSetup = 1;
    }

    FFP_UploadBones(self);

    /* Albedo to stage 0, NULL stages 1-7 */
    {
        int as = self->albedoStage;
        void *albedo = (as >= 0 && as < 8) ? self->curTexture[as] : self->curTexture[0];
        int ts;
        ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, 0, albedo);
        for (ts = 1; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, NULL);
    }

    /* Bind cloned declaration (UBYTE4 BLENDINDICES) — original VB unchanged */
    ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, clonedDecl);

    /* Draw with original VB and index buffer */
    hr = ((FN_DIP)vt[SLOT_DrawIndexedPrimitive])(self->pReal, pt, bvi, mi, nv, si, pc);

    /* Restore original declaration and textures */
    ((FN_SetVD)vt[SLOT_SetVertexDeclaration])(self->pReal, self->lastDecl);
    {
        int ts;
        for (ts = 0; ts < 8; ts++)
            ((FN_SetTex)vt[SLOT_SetTexture])(self->pReal, ts, self->curTexture[ts]);
    }

    self->bonesDrawn = 1;
    FFP_DisableSkinning(self);

    return hr;
}

/* ======================================================================
 * Device Lifecycle
 * ====================================================================== */

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
    log_str("  Skinning: enabled (decl-clone, UBYTE4)\r\n");
}

static void Skin_ReleaseDevice(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < self->skinDeclCount; i++) {
        if (self->skinDeclClone[i])
            ((FN_Release)(*(void***)self->skinDeclClone[i])[2])(self->skinDeclClone[i]);
    }
    self->skinDeclCount = 0;
}
