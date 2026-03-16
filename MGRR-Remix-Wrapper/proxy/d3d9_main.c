/*
 * MGR:R Fixed-Function D3D9 Proxy - Main Entry
 *
 * Loads the next d3d9 in the chain:
 *   - If mgr_proxy.ini has [Remix] Enabled=1 and d3d9_remix.dll exists,
 *     loads d3d9_remix.dll (RTX Remix) from the game directory.
 *   - Otherwise loads the real d3d9.dll from System32.
 *
 * Wraps IDirect3D9 to intercept CreateDevice, wraps IDirect3DDevice9
 * to replace shader-based rendering with fixed-function pipeline
 * equivalents for RTX Remix compatibility.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---- Logging ---- */

static HANDLE g_logFile = INVALID_HANDLE_VALUE;

void log_open(void) {
    g_logFile = CreateFileA("mgr_ffp_proxy.log",
        GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void log_str(const char *s) {
    DWORD written;
    if (g_logFile != INVALID_HANDLE_VALUE) {
        int len = 0;
        while (s[len]) len++;
        WriteFile(g_logFile, s, len, &written, NULL);
    }
}

void log_hex(const char *prefix, unsigned int val) {
    char buf[64];
    const char *hex = "0123456789ABCDEF";
    int i, p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    buf[p++] = '0'; buf[p++] = 'x';
    for (i = 7; i >= 0; i--)
        buf[p++] = hex[(val >> (i * 4)) & 0xF];
    buf[p++] = '\r'; buf[p++] = '\n'; buf[p] = 0;
    log_str(buf);
}

void log_floats(const char *prefix, float *data, unsigned int count) {
    char buf[16];
    const char *hex = "0123456789ABCDEF";
    unsigned int i, val;
    int j;
    log_str(prefix);
    for (i = 0; i < count; i++) {
        val = *(unsigned int*)&data[i];
        for (j = 7; j >= 0; j--)
            buf[j] = hex[(val >> ((7-j) * 4)) & 0xF];
        buf[8] = (i + 1 < count) ? ' ' : '\r';
        buf[9] = (i + 1 < count) ? '\0' : '\n';
        if (i + 1 < count) {
            buf[9] = '\0';
            log_str(buf);
        } else {
            buf[9] = '\n';
            buf[10] = '\0';
            log_str(buf);
        }
    }
}

void log_int(const char *prefix, int val) {
    char buf[64];
    int p = 0, start, end;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    if (val < 0) { buf[p++] = '-'; val = -val; }
    if (val == 0) { buf[p++] = '0'; }
    else {
        start = p;
        while (val > 0) { buf[p++] = '0' + (val % 10); val /= 10; }
        end = p - 1;
        while (start < end) { char t = buf[start]; buf[start] = buf[end]; buf[end] = t; start++; end--; }
    }
    buf[p++] = '\r'; buf[p++] = '\n'; buf[p] = 0;
    log_str(buf);
}

void log_float_val(const char *prefix, float f) {
    /* Log float as hex bits + approximate decimal */
    unsigned int bits = *(unsigned int*)&f;
    log_hex(prefix, bits);
}

/* Write a single float as crude decimal: ±NNNN.NN (no CRT, no float-to-int cast) */
static void write_float_dec(float f) {
    char buf[24];
    int p = 0;
    unsigned int bits = *(unsigned int*)&f;
    int sign, biasedExp, exp;
    unsigned int mantissa, ipart, frac;

    /* Handle special values */
    if ((bits & 0x7F800000) == 0x7F800000) {
        if (bits & 0x007FFFFF) { log_str("NaN"); return; }
        log_str((bits & 0x80000000) ? "-Inf" : "Inf"); return;
    }
    if ((bits & 0x7FFFFFFF) == 0) { log_str((bits & 0x80000000) ? "-0.00" : "0.00"); return; }

    sign = (bits >> 31) & 1;
    biasedExp = (bits >> 23) & 0xFF;
    exp = biasedExp - 127;
    mantissa = (bits & 0x007FFFFF) | 0x00800000; /* add implicit 1 bit */

    if (sign) buf[p++] = '-';

    /* Extract integer part from IEEE 754 bits */
    if (exp < 0) {
        ipart = 0;
    } else if (exp >= 23) {
        ipart = mantissa << (exp - 23);
    } else {
        ipart = mantissa >> (23 - exp);
    }

    /* Extract fractional part: ((mantissa << exp) & fraction_mask) * 100 / fraction_range */
    /* Simpler: frac100 = (lower bits) * 100 >> remaining_bits */
    if (exp < 0) {
        /* Entire value is fractional: val = mantissa * 2^(exp-23) */
        /* frac100 ≈ mantissa * 100 >> (23 - exp), but may overflow for large negative exp */
        if (exp >= -8) {
            frac = (mantissa * 100) >> (23 - exp);
        } else {
            frac = 0; /* too small to show at 2 decimal places */
        }
    } else if (exp < 23) {
        unsigned int fracBits = mantissa & ((1 << (23 - exp)) - 1);
        frac = (fracBits * 100) >> (23 - exp);
    } else {
        frac = 0;
    }
    if (frac > 99) frac = 99;

    /* Integer part (reverse digits) */
    if (ipart == 0) {
        buf[p++] = '0';
    } else {
        int start = p, end;
        unsigned int tmp = ipart;
        while (tmp > 0) { buf[p++] = '0' + (tmp % 10); tmp /= 10; }
        end = p - 1;
        while (start < end) { char t = buf[start]; buf[start] = buf[end]; buf[end] = t; start++; end--; }
    }
    buf[p++] = '.';
    buf[p++] = '0' + (frac / 10);
    buf[p++] = '0' + (frac % 10);
    buf[p] = '\0';
    log_str(buf);
}

/* Log N floats as decimal, space-separated, with prefix + newline */
void log_floats_dec(const char *prefix, float *data, unsigned int count) {
    unsigned int i;
    log_str(prefix);
    for (i = 0; i < count; i++) {
        write_float_dec(data[i]);
        if (i + 1 < count) log_str(" ");
    }
    log_str("\r\n");
}

void log_close(void) {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

/* ---- Forward declarations ---- */

/* Real d3d9.dll function type */
typedef void* (__stdcall *PFN_Direct3DCreate9)(unsigned int);

static HMODULE g_realD3D9 = NULL;
static HMODULE g_preloadDLL = NULL; /* side-effect DLL (e.g. steam006 fix) */
static PFN_Direct3DCreate9 g_realDirect3DCreate9 = NULL;
HINSTANCE g_hInstance = NULL; /* our DLL handle */

/* Our wrapped types - opaque, defined in wrapper files */
typedef struct WrappedD3D9 WrappedD3D9;
typedef struct WrappedDevice WrappedDevice;

/* From d3d9_wrapper.c */
WrappedD3D9* WrappedD3D9_Create(void* pRealD3D9);

/* ---- Exported: Direct3DCreate9 ---- */

/* Build the full path to a file in the same directory as our DLL */
static void get_dll_sibling_path(char *out, int outSize, const char *filename) {
    int i, lastSlash = -1, p;
    GetModuleFileNameA(g_hInstance, out, outSize);
    for (i = 0; out[i]; i++) {
        if (out[i] == '\\' || out[i] == '/') lastSlash = i;
    }
    p = (lastSlash >= 0) ? lastSlash + 1 : 0;
    for (i = 0; filename[i]; i++) out[p++] = filename[i];
    out[p] = '\0';
}

__declspec(dllexport) void* __stdcall Direct3DCreate9(unsigned int SDKVersion) {
    char pathBuf[MAX_PATH];
    char iniBuf[MAX_PATH];
    void *pReal;
    int useRemix = 0;

    if (!g_realD3D9) {
        log_open();
        log_str("=== MGR:R Fixed-Function Proxy ===\r\n");

        /* Check INI for Remix toggle */
        get_dll_sibling_path(iniBuf, MAX_PATH, "mgr_proxy.ini");
        useRemix = GetPrivateProfileIntA("Remix", "Enabled", 0, iniBuf);

        /*
         * PreloadDLL: load a DLL for its side effects (DllMain patches).
         * Used for game-fix wrappers like steam006 that patch game memory
         * at load time. The DLL stays loaded but isn't in the D3D9 call chain.
         */
        {
            char preloadName[MAX_PATH];
            GetPrivateProfileStringA("Chain", "PreloadDLL", "",
                preloadName, MAX_PATH, iniBuf);
            if (preloadName[0]) {
                get_dll_sibling_path(pathBuf, MAX_PATH, preloadName);
                log_str("Preloading DLL: ");
                log_str(pathBuf);
                log_str("\r\n");
                g_preloadDLL = LoadLibraryA(pathBuf);
                if (g_preloadDLL) {
                    log_str("  Preload OK\r\n");
                } else {
                    log_str("  WARNING: Preload failed\r\n");
                }
            }
        }

        if (useRemix) {
            /* Try loading d3d9_remix.dll from the game directory */
            get_dll_sibling_path(pathBuf, MAX_PATH, "d3d9_remix.dll");
            log_str("Remix enabled, loading: ");
            log_str(pathBuf);
            log_str("\r\n");
            g_realD3D9 = LoadLibraryA(pathBuf);
            if (!g_realD3D9) {
                log_str("WARNING: d3d9_remix.dll not found, falling back to system d3d9.dll\r\n");
                useRemix = 0;
            }
        }

        if (!g_realD3D9) {
            /* Load system d3d9.dll */
            GetSystemDirectoryA(pathBuf, MAX_PATH);
            {
                int i = 0;
                while (pathBuf[i]) i++;
                pathBuf[i++] = '\\';
                pathBuf[i++] = 'd'; pathBuf[i++] = '3'; pathBuf[i++] = 'd';
                pathBuf[i++] = '9'; pathBuf[i++] = '.'; pathBuf[i++] = 'd';
                pathBuf[i++] = 'l'; pathBuf[i++] = 'l'; pathBuf[i] = 0;
            }
            log_str("Loading system d3d9.dll: ");
            log_str(pathBuf);
            log_str("\r\n");
            g_realD3D9 = LoadLibraryA(pathBuf);
        }

        if (!g_realD3D9) {
            log_str("FATAL: Failed to load d3d9 backend\r\n");
            log_close();
            return NULL;
        }

        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9, "Direct3DCreate9");
        if (!g_realDirect3DCreate9) {
            log_str("FATAL: Direct3DCreate9 not found in loaded d3d9\r\n");
            log_close();
            return NULL;
        }
    }

    log_hex("Direct3DCreate9 called, SDK version: ", SDKVersion);

    pReal = g_realDirect3DCreate9(SDKVersion);
    if (!pReal) {
        log_str("ERROR: Real Direct3DCreate9 returned NULL\r\n");
        return NULL;
    }

    log_hex("Real IDirect3D9: ", (unsigned int)pReal);
    return (void*)WrappedD3D9_Create(pReal);
}

/* ---- DllMain ---- */

int __stdcall _DllMainCRTStartup(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInstance = hinstDLL;
    }
    if (fdwReason == DLL_PROCESS_DETACH) {
        log_str("Proxy unloading\r\n");
        log_close();
        if (g_realD3D9) {
            FreeLibrary(g_realD3D9);
            g_realD3D9 = NULL;
        }
        if (g_preloadDLL) {
            FreeLibrary(g_preloadDLL);
            g_preloadDLL = NULL;
        }
    }
    return 1;
}

int _fltused = 0;
