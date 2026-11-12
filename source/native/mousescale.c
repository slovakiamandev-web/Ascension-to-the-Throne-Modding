/*
 * mousescale.dll - a mouse sensitivity setting for the game (it has none).
 *
 * Why: the game reads the mouse through DirectInput 8 (IDirectInput8A, a buffered
 * GUID_SysMouse device, see 0x46c5c4 in ATThrone.exe) and draws its own cursor from the
 * raw movement - about 2 pixels per count at 1024x768. Remote desktop tools such as
 * Chrome Remote Desktop send cursor POSITIONS, which Windows turns into big jumps of
 * raw movement, so the game cursor races across the screen.
 *
 * How: every DirectInput device of one kind shares one table of functions (vtable) in
 * dinput8.dll. This DLL creates a throw-away mouse device of its own, takes that table
 * and puts its own GetDeviceState (slot 9) and GetDeviceData (slot 10) in it. Both call
 * the original first, then - for mouse devices only (GetCapabilities: type 0x12) - scale
 * the X/Y movement by the factor. Fractions are carried over, so slow movement is not
 * lost. Buttons and the wheel are untouched.
 *
 *   Lua:  package.loadlib("ModFramework/mousescale.dll", "mousescale_start")()
 *         reads mousescale_factor.txt next to the DLL (one number, 0.005 - 4, e.g. 0.1);
 *         "mousescale_reload" re-reads it.
 *   log:  mousescale.log next to the DLL (fresh per process).
 *
 * The DLL pins itself: the game unloads Lua's C libraries when it replaces its Lua
 * state, and the patched table must never point into an unloaded DLL.
 * Build: C:\MinGW\bin\gcc -shared -O2 -Wall -static-libgcc -o mousescale.dll mousescale.c
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *CreateFn)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (WINAPI *StateFn)(void *, DWORD, void *);
typedef HRESULT (WINAPI *DataFn)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *CapsFn)(void *, void *);
typedef ULONG   (WINAPI *ReleaseFn)(void *);
typedef HRESULT (WINAPI *CreateDeviceFn)(void *, REFGUID, void **, void *);

static const GUID IID_DI8A  = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID SYS_MOUSE = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

enum { SLOT_CAPS = 3, SLOT_STATE = 9, SLOT_DATA = 10 };
#define DEVTYPE_MOUSE 0x12

static char folder[MAX_PATH];
static volatile LONG factor_bits;         /* the factor as float bits (read from any thread) */
static StateFn original_state;
static DataFn original_data;
static CapsFn get_caps;
static int installed;

/* which devices are mice (a handful at most) */
static struct { void *dev; int mouse; } known[16];
static int nknown;
/* carried-over fractions, per axis */
static float rest_x, rest_y;
/* totals for the log */
static long long in_x, in_y, out_x, out_y;
static DWORD last_report;

static void logmsg(const char *fmt, ...)
{
    char path[MAX_PATH];
    FILE *f;
    va_list ap;
    snprintf(path, sizeof(path), "%smousescale.log", folder);
    f = fopen(path, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void init_folder(void)
{
    HMODULE self = NULL;
    char *slash;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&init_folder, &self);
    GetModuleFileNameA(self, folder, MAX_PATH);
    slash = strrchr(folder, '\\');
    if (slash) slash[1] = '\0';
}

static float factor(void)
{
    LONG bits = factor_bits;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void read_factor(void)
{
    char path[MAX_PATH], line[64];
    float f = 1.0f;
    LONG bits;
    FILE *in;
    snprintf(path, sizeof(path), "%smousescale_factor.txt", folder);
    in = fopen(path, "r");
    if (in) {
        if (fgets(line, sizeof(line), in)) f = (float)atof(line);
        fclose(in);
    }
    if (!(f >= 0.005f && f <= 4.0f)) f = 1.0f;    /* also catches NaN */
    memcpy(&bits, &f, sizeof(bits));
    InterlockedExchange(&factor_bits, bits);
    logmsg("factor %.2f", f);
}

static int is_mouse(void *dev)
{
    int i;
    DWORD caps[11];
    for (i = 0; i < nknown; i++) if (known[i].dev == dev) return known[i].mouse;
    memset(caps, 0, sizeof(caps));
    caps[0] = sizeof(caps);                        /* DIDEVCAPS: dwSize, dwFlags, dwDevType, ... */
    i = SUCCEEDED(get_caps(dev, caps)) && (caps[2] & 0xFF) == DEVTYPE_MOUSE;
    if (nknown < 16) { known[nknown].dev = dev; known[nknown].mouse = i; nknown++; }
    logmsg("device %p: %s", dev, i ? "mouse - scaled" : "not a mouse");
    return i;
}

static LONG scale(LONG v, float *rest)
{
    float exact = v * factor() + *rest;
    LONG whole = (LONG)exact;                      /* toward zero */
    *rest = exact - (float)whole;
    return whole;
}

static void report(void)
{
    DWORD now = GetTickCount();
    if (now - last_report < 5000 || (in_x == 0 && in_y == 0)) return;
    last_report = now;
    logmsg("moved in %lld,%lld -> out %lld,%lld", in_x, in_y, out_x, out_y);
}

static HRESULT WINAPI state_hook(void *dev, DWORD size, void *data)
{
    HRESULT r = original_state(dev, size, data);
    if (SUCCEEDED(r) && data && (size == 16 || size == 20) && factor() != 1.0f && is_mouse(dev)) {
        LONG *axes = (LONG *)data;                 /* DIMOUSESTATE: lX, lY, lZ, buttons */
        in_x += axes[0]; in_y += axes[1];
        axes[0] = scale(axes[0], &rest_x);
        axes[1] = scale(axes[1], &rest_y);
        out_x += axes[0]; out_y += axes[1];
        report();
    }
    return r;
}

static HRESULT WINAPI data_hook(void *dev, DWORD each, void *items, DWORD *count, DWORD flags)
{
    HRESULT r = original_data(dev, each, items, count, flags);
    if (SUCCEEDED(r) && items && count && each >= 8 && factor() != 1.0f && is_mouse(dev)) {
        DWORD i;
        for (i = 0; i < *count; i++) {             /* DIDEVICEOBJECTDATA: dwOfs, dwData, ... */
            DWORD *item = (DWORD *)((char *)items + i * each);
            LONG v = (LONG)item[1];
            if (item[0] == 0)      { in_x += v; v = scale(v, &rest_x); out_x += v; }
            else if (item[0] == 4) { in_y += v; v = scale(v, &rest_y); out_y += v; }
            else continue;                         /* DIMOFS_Z (wheel), buttons */
            item[1] = (DWORD)v;
        }
        report();
    }
    return r;
}

__declspec(dllexport) int mousescale_reload(void *L)
{
    (void)L;
    if (!folder[0]) init_folder();
    read_factor();
    return 0;
}

__declspec(dllexport) int mousescale_start(void *L)
{
    HMODULE dinput, self;
    CreateFn create;
    void *di = NULL, *dev = NULL;
    void **vtable;
    DWORD old;

    (void)L;
    if (!folder[0]) {
        char path[MAX_PATH];
        init_folder();
        snprintf(path, sizeof(path), "%smousescale.log", folder);
        DeleteFileA(path);
    }
    read_factor();
    if (installed) { logmsg("already installed"); return 0; }

    dinput = GetModuleHandleA("dinput8.dll");
    create = dinput ? (CreateFn)(void *)GetProcAddress(dinput, "DirectInput8Create") : NULL;
    if (!create) { logmsg("dinput8.dll is not loaded - not installed"); return 0; }
    if (FAILED(create(GetModuleHandleA(NULL), 0x0800, &IID_DI8A, &di, NULL)) || !di) {
        logmsg("DirectInput8Create failed - not installed");
        return 0;
    }
    if (FAILED(((CreateDeviceFn)(*(void ***)di)[3])(di, &SYS_MOUSE, &dev, NULL)) || !dev) {
        logmsg("CreateDevice(mouse) failed - not installed");
        ((ReleaseFn)(*(void ***)di)[2])(di);
        return 0;
    }
    vtable = *(void ***)dev;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)&state_hook, &self)) {
        logmsg("could not pin the DLL - not installed");
    } else if (vtable[SLOT_STATE] == (void *)state_hook) {
        installed = 1;                             /* patched by an earlier load of this DLL */
        logmsg("table already patched");
    } else if (VirtualProtect(&vtable[SLOT_CAPS], (SLOT_DATA - SLOT_CAPS + 1) * sizeof(void *),
                              PAGE_EXECUTE_READWRITE, &old)) {
        get_caps = (CapsFn)vtable[SLOT_CAPS];
        original_state = (StateFn)vtable[SLOT_STATE];
        original_data = (DataFn)vtable[SLOT_DATA];
        vtable[SLOT_STATE] = (void *)state_hook;
        vtable[SLOT_DATA] = (void *)data_hook;
        VirtualProtect(&vtable[SLOT_CAPS], (SLOT_DATA - SLOT_CAPS + 1) * sizeof(void *), old, &old);
        installed = 1;
        logmsg("installed (device table %p)", (void *)vtable);
    } else {
        logmsg("VirtualProtect failed - not installed");
    }
    ((ReleaseFn)(*(void ***)dev)[2])(dev);
    ((ReleaseFn)(*(void ***)di)[2])(di);
    return 0;
}
