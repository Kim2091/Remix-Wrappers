/* ForceBumpTerrain.asi — Force high-quality bump terrain at all zoom levels.
 *
 * NOPs the call to SetUseBakedTerrain(1) inside TerrainCameraState_Update,
 * preventing the zoom-out camera transition from switching to low-quality
 * baked terrain rendering.
 *
 * Target: StarWarsG.exe (64-bit, ASLR-enabled, Forces of Corruption Steam)
 * RVA 0x2CF7D2: call SetUseBakedTerrain  (E8 F9 80 E6 FF) -> NOP x5
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(lib, "kernel32.lib")
int _fltused = 0;

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void log_open(const char *asi_name)
{
    char path[MAX_PATH];
    char *end;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    end = path;
    { char *p = path; while (*p) { if (*p == '\\' || *p == '/') end = p; p++; } }
    end[1] = '\0';
    { const char *s = asi_name; char *d = path; while (*d) d++; while (*s) *d++ = *s++; *d = '\0'; }
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void log_str(const char *s)
{
    DWORD n;
    const char *p = s;
    if (g_log == INVALID_HANDLE_VALUE) return;
    while (*p) p++;
    WriteFile(g_log, s, (DWORD)(p - s), &n, NULL);
}

static void log_hex(UINT_PTR v)
{
    char buf[19];
    const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    log_str(buf);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    UINT_PTR base, target;
    const unsigned char expect[] = { 0xE8, 0xF9, 0x80, 0xE6, 0xFF };
    const unsigned char *p;
    DWORD old;
    unsigned int i;

    (void)lpReserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(hModule);

    log_open("ForceBumpTerrain.log");
    log_str("ForceBumpTerrain loaded\r\n");

    base = (UINT_PTR)GetModuleHandleA(NULL);
    log_str("Base: "); log_hex(base); log_str("\r\n");

    target = base + 0x2CF7D2; /* RVA of call SetUseBakedTerrain(1) */
    log_str("Target: "); log_hex(target); log_str("\r\n");

    /* Verify original bytes */
    if (IsBadReadPtr((void *)target, 5)) {
        log_str("verify FAILED -- target not readable\r\n");
        goto done;
    }
    p = (const unsigned char *)target;
    for (i = 0; i < 5; i++) {
        if (p[i] != expect[i]) {
            log_str("verify FAILED -- byte mismatch at offset ");
            log_hex(i);
            log_str(" (expected ");
            log_hex(expect[i]);
            log_str(", got ");
            log_hex(p[i]);
            log_str(")\r\n");
            goto done;
        }
    }
    log_str("verify OK\r\n");

    /* NOP the 5-byte call */
    if (!VirtualProtect((void *)target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        log_str("VirtualProtect FAILED\r\n");
        goto done;
    }
    for (i = 0; i < 5; i++)
        ((unsigned char *)target)[i] = 0x90;
    VirtualProtect((void *)target, 5, old, &old);

    log_str("[OK] NopSetBakedTerrainCall @ "); log_hex(target); log_str("\r\n");

done:
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
