/*
 * d3d9_normalexpand.h — D3DCOLOR NORMAL → FLOAT3 vertex expansion
 *
 * #included by d3d9_device.c. Expands vertices that use D3DCOLOR for the
 * NORMAL semantic into FLOAT3, decoding with the standard *2-1 remap.
 *
 * RTX Remix does not support D3DCOLOR as a vertex normal format — it
 * expects FLOAT3. Without expansion, geometry with packed normals appears
 * to have no normals in Remix.
 *
 * The expansion is generic: any stream-0 vertex declaration with a
 * D3DCOLOR NORMAL element gets a cloned declaration (NORMAL → FLOAT3,
 * subsequent offsets shifted +8) and an expanded vertex buffer with
 * decoded FLOAT3 normals. Both are cached for reuse.
 */

/* Forward declarations */
static void FFP_Engage(WrappedDevice *self);
static void FFP_Disengage(WrappedDevice *self);

/* ======================================================================
 * Declaration Cloning
 *
 * Clones the current vertex declaration, replacing D3DCOLOR NORMAL0
 * with FLOAT3 NORMAL0 (4 → 12 bytes) and shifting all subsequent
 * stream-0 element offsets by +8.
 * ====================================================================== */

static void* NormExp_GetClonedDecl(WrappedDevice *self) {
    typedef int (__stdcall *FN_GetDecl)(void*, void*, unsigned int*);
    typedef int (__stdcall *FN_CreateDecl)(void*, void*, void**);
    void *origDecl = self->lastDecl;
    unsigned char elemBuf[8 * 32];
    unsigned int numElems = 0;
    void **declVt;
    int i, hr;
    void *newDecl;
    unsigned short normalOff;

    if (!origDecl) return NULL;

    /* Check cache */
    for (i = 0; i < self->normExpDeclCount; i++) {
        if (self->normExpDeclOrig[i] == origDecl)
            return self->normExpDeclClone[i];
    }

    /* Get elements from original declaration */
    declVt = *(void***)origDecl;
    hr = ((FN_GetDecl)declVt[4])(origDecl, NULL, &numElems);
    if (hr != 0 || numElems == 0 || numElems > 32) return NULL;

    hr = ((FN_GetDecl)declVt[4])(origDecl, elemBuf, &numElems);
    if (hr != 0) return NULL;

    normalOff = (unsigned short)self->curDeclNormalOff;

    /* Patch: NORMAL D3DCOLOR → FLOAT3, shift subsequent stream-0 offsets +8 */
    for (i = 0; i < (int)numElems; i++) {
        unsigned char *el = &elemBuf[i * 8];
        unsigned short stream = *(unsigned short*)&el[0];
        unsigned short offset = *(unsigned short*)&el[2];
        if (stream == 0xFF || stream == 0xFFFF) break;
        if (stream != 0) continue;

        if (el[6] == D3DDECLUSAGE_NORMAL && el[7] == 0 && el[4] == D3DDECLTYPE_D3DCOLOR) {
            el[4] = D3DDECLTYPE_FLOAT3;
        } else if (offset > normalOff) {
            *(unsigned short*)&el[2] = offset + 8;
        }
    }

    newDecl = NULL;
    hr = ((FN_CreateDecl)RealVtbl(self)[SLOT_CreateVertexDeclaration])(
        self->pReal, (void*)elemBuf, &newDecl);
    if (hr != 0 || !newDecl) return NULL;

    /* Store in cache (evict slot 0 if full) */
    if (self->normExpDeclCount < NORMEXP_DECL_CACHE_SIZE) {
        self->normExpDeclOrig[self->normExpDeclCount] = origDecl;
        self->normExpDeclClone[self->normExpDeclCount] = newDecl;
        self->normExpDeclCount++;
    } else {
        typedef unsigned long (__stdcall *FN_Release)(void*);
        ((FN_Release)(*(void***)self->normExpDeclClone[0])[2])(self->normExpDeclClone[0]);
        for (i = 1; i < NORMEXP_DECL_CACHE_SIZE; i++) {
            self->normExpDeclOrig[i-1] = self->normExpDeclOrig[i];
            self->normExpDeclClone[i-1] = self->normExpDeclClone[i];
        }
        self->normExpDeclOrig[NORMEXP_DECL_CACHE_SIZE-1] = origDecl;
        self->normExpDeclClone[NORMEXP_DECL_CACHE_SIZE-1] = newDecl;
    }

    log_hex("  NormExp cloned decl: orig=", (unsigned int)origDecl);
    log_hex("    clone=", (unsigned int)newDecl);
    return newDecl;
}

/* ======================================================================
 * Vertex Buffer Expansion
 *
 * Copies source vertices into a new VB with D3DCOLOR normal decoded
 * to FLOAT3. Destination stride = source stride + 8.
 *
 * Layout transformation per vertex:
 *   [0, normalOff)        → copy as-is
 *   [normalOff, +4)       → decode D3DCOLOR to FLOAT3 (12 bytes)
 *   [normalOff+4, stride) → copy as-is (shifted +8 in dest)
 * ====================================================================== */

static void* NormExp_GetExpandedVB(WrappedDevice *self, int bvi,
    unsigned int mi, unsigned int nv)
{
    typedef int (__stdcall *FN_CreateVB)(void*, unsigned int, unsigned long,
        unsigned long, unsigned int, void**, void*);
    typedef int (__stdcall *FN_Lock)(void*, unsigned int, unsigned int, void**, unsigned int);
    typedef int (__stdcall *FN_Unlock)(void*);
    typedef unsigned long (__stdcall *FN_Release)(void*);

    unsigned int srcStride = self->streamStride[0];
    unsigned int dstStride = srcStride + 8;
    unsigned int normalOff = (unsigned int)self->curDeclNormalOff;
    unsigned int tailOff   = normalOff + 4;  /* first byte after D3DCOLOR normal */
    unsigned int tailLen   = srcStride - tailOff;
    unsigned int baseVtx   = mi;
    unsigned int key, v, lockOff, lockSize;
    int slot, hr;
    void *newVB, **srcVt, **dstVt;
    unsigned char *srcData, *dstData;

    if (!self->streamVB[0]) return NULL;

    /* Hash key: source VB, base vertex, count, stride, declaration */
    key  = (unsigned int)self->streamVB[0]
         ^ (baseVtx * 2654435761u)
         ^ (nv * 40503u)
         ^ (srcStride * 6700417u)
         ^ ((unsigned int)self->lastDecl * 2246822519u);
    slot = (int)(key % NORMEXP_VB_CACHE_SIZE);

    if (self->normExpVB[slot] && self->normExpKey[slot] == key
        && self->normExpNv[slot] == nv)
        return self->normExpVB[slot];

    /* Evict old entry */
    if (self->normExpVB[slot]) {
        ((FN_Release)(*(void***)(self->normExpVB[slot]))[2])(self->normExpVB[slot]);
        self->normExpVB[slot] = NULL;
    }

    newVB = NULL;
    hr = ((FN_CreateVB)RealVtbl(self)[SLOT_CreateVertexBuffer])(
        self->pReal, nv * dstStride, 8 /*WRITEONLY*/, 0, 0 /*DEFAULT*/, &newVB, NULL);
    if (hr != 0 || !newVB) {
        log_hex("NormExp CreateVB failed hr=", (unsigned int)hr);
        return NULL;
    }

    /* Lock source region */
    lockOff  = self->streamOffset[0] + baseVtx * srcStride;
    lockSize = nv * srcStride;
    srcVt    = *(void***)self->streamVB[0];
    srcData  = NULL;
    hr = ((FN_Lock)srcVt[11])(self->streamVB[0], lockOff, lockSize,
        (void**)&srcData, 0x10 /*READONLY*/);
    if (hr != 0 || !srcData) {
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        log_hex("NormExp src lock failed hr=", (unsigned int)hr);
        return NULL;
    }

    /* Lock destination */
    dstVt   = *(void***)newVB;
    dstData = NULL;
    hr = ((FN_Lock)dstVt[11])(newVB, 0, nv * dstStride, (void**)&dstData, 0);
    if (hr != 0 || !dstData) {
        ((FN_Unlock)srcVt[12])(self->streamVB[0]);
        ((FN_Release)(*(void***)newVB)[2])(newVB);
        return NULL;
    }

    /* Expand each vertex */
    for (v = 0; v < nv; v++) {
        const unsigned char *s = srcData + v * srcStride;
        unsigned char *d = dstData + v * dstStride;
        float *dNorm;
        const unsigned char *nrm;

        /* Copy bytes before normal */
        if (normalOff > 0)
            memcpy(d, s, normalOff);

        /* Decode D3DCOLOR normal → FLOAT3 */
        nrm = s + normalOff;
        dNorm = (float*)(d + normalOff);
        dNorm[0] = nrm[2] / 127.5f - 1.0f;  /* R → x */
        dNorm[1] = nrm[1] / 127.5f - 1.0f;  /* G → y */
        dNorm[2] = nrm[0] / 127.5f - 1.0f;  /* B → z */

        /* Copy bytes after normal (shifted +8 in dest) */
        if (tailLen > 0)
            memcpy(d + normalOff + 12, s + tailOff, tailLen);
    }

    ((FN_Unlock)dstVt[12])(newVB);
    ((FN_Unlock)srcVt[12])(self->streamVB[0]);

    self->normExpVB[slot]  = newVB;
    self->normExpKey[slot] = key;
    self->normExpNv[slot]  = nv;
    return newVB;
}

/* ======================================================================
 * Cache Management
 * ====================================================================== */

static void NormExp_ReleaseVBCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < NORMEXP_VB_CACHE_SIZE; i++) {
        if (self->normExpVB[i]) {
            ((FN_Release)(*(void***)(self->normExpVB[i]))[2])(self->normExpVB[i]);
            self->normExpVB[i] = NULL;
        }
    }
}

static void NormExp_ReleaseDeclCache(WrappedDevice *self) {
    typedef unsigned long (__stdcall *FN_Release)(void*);
    int i;
    for (i = 0; i < self->normExpDeclCount; i++) {
        if (self->normExpDeclClone[i])
            ((FN_Release)(*(void***)self->normExpDeclClone[i])[2])(self->normExpDeclClone[i]);
    }
    self->normExpDeclCount = 0;
}

static void NormExp_InitDevice(WrappedDevice *w) {
    int i;
    w->normExpDeclCount = 0;
    for (i = 0; i < NORMEXP_DECL_CACHE_SIZE; i++) {
        w->normExpDeclOrig[i] = NULL;
        w->normExpDeclClone[i] = NULL;
    }
    for (i = 0; i < NORMEXP_VB_CACHE_SIZE; i++) {
        w->normExpVB[i] = NULL;
    }
    log_str("  NormExp: D3DCOLOR NORMAL -> FLOAT3 expansion enabled\r\n");
}

static void NormExp_ReleaseDevice(WrappedDevice *self) {
    NormExp_ReleaseVBCache(self);
    NormExp_ReleaseDeclCache(self);
}
