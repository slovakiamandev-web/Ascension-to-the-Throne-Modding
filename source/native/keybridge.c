/*
 * keybridge.dll - lets Lua mods react to key presses.
 *
 * Keys never reach the game's Lua (the engine handles them itself), and
 * ATThrone.exe exports none of the Lua C API, so this DLL cannot call into Lua.
 * Instead it hands presses over through a small file:
 *
 *   Lua (ModAPI):  package.loadlib("ModFramework/keybridge.dll", "keybridge_start")()
 *   this DLL:      puts its own window procedure in front of the game window's
 *                  ("subclassing"), so it sees every key message the game gets,
 *                  counts each press (auto-repeat ignored) and passes the
 *                  message on unchanged. A background thread rewrites
 *                  keybridge_keys.txt next to the DLL whenever a count changes:
 *                      keybridge 1 <change counter> window <times subclassed>
 *                      down <vk> <vk> ...        (keys held right now)
 *                      <virtual key code> <presses so far>
 *                      ...
 *   Lua:           reads the file ~30x a second and compares the counts.
 *
 * Measured dead ends (2026-10-24), kept here so nobody retries them:
 *   - GetAsyncKeyState polling from a background thread counted one press
 *     twice and missed short ones.
 *   - A WH_KEYBOARD_LL hook installed fine but was never called, even when
 *     re-installed every 2 s to stay first in the hook chain.
 *
 * Window messages only arrive while the game has focus, so keys typed in other
 * programs are never counted. A key released while the game is NOT in front
 * never sends its key-up here, so held keys are cleared whenever the game is not
 * the foreground window (checked every 100 ms) - otherwise Alt+Tab with Shift
 * down would leave Shift "held" until it was pressed again.
 *
 * keybridge_start returns 0 ("no results"), which is why it needs no Lua API.
 * The threads live as long as the game process; calling keybridge_start again
 * (a new game restarts Lua) does nothing, so counts only ever grow.
 *
 * keybridge_peek - READ-ONLY memory reads for engine fields Lua cannot reach
 * (first use: the hero's facing, see ModAPI.HeroRotation). Lua writes
 * keybridge_peek_req.txt, one request per line:
 *     <hex address> <hex offset> <f|u|s>
 *   f = 32-bit float, u = uint32 as 8 hex digits, s = short text (printable,
 *   NUL-terminated, under PEEK_TEXT_MAX bytes)
 * calls keybridge_peek, and reads keybridge_peek.txt: one line per request with
 * the value, or "nil" if that memory is not readable (checked with
 * VirtualQuery, so a wrong address returns nil instead of crashing the game).
 *
 * Build (32-bit, like the game):
 *   gcc -shared -O2 -Wall -static-libgcc -o keybridge.dll keybridge.c
 */
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_COUNT 256
#define PEEK_TEXT_MAX 32

static volatile LONG started = 0;
static char state_path[MAX_PATH];
static char temp_path[MAX_PATH];
static volatile LONG presses[KEY_COUNT];
static volatile LONG held[KEY_COUNT];       /* 1 while the key is down */
static volatile LONG changes = 0;
static volatile LONG subclassed = 0;
static HANDLE changed_event = NULL;

/* Every window we subclassed, with ITS original procedure. The game has several
   windows (3 at startup, and a minimised fullscreen game can make another one
   the largest); one shared "original procedure" sent the main window's messages
   to another window's procedure and crashed the game (2026-10-25). */
#define MAX_WINDOWS 16
static struct { HWND window; WNDPROC proc; int unicode; } subs[MAX_WINDOWS];
static volatile LONG sub_count = 0;

/* Write the counts to a temporary file and swap it in, so Lua never reads a
   half-written file. */
static int write_state(void)
{
    FILE *f = fopen(temp_path, "w");
    int vk;

    if (f == NULL)
        return 0;
    fprintf(f, "keybridge 1 %ld window %ld\n", changes, subclassed);
    fprintf(f, "down");
    for (vk = 1; vk < KEY_COUNT; vk++)
        if (held[vk])
            fprintf(f, " %d", vk);
    fprintf(f, "\n");
    for (vk = 1; vk < KEY_COUNT; vk++)
        if (presses[vk] != 0)
            fprintf(f, "%d %ld\n", vk, presses[vk]);
    fclose(f);
    /* Fails while Lua has the file open; the caller retries. */
    return MoveFileExA(temp_path, state_path, MOVEFILE_REPLACE_EXISTING) != 0;
}

/* Runs on the game's own thread for every message of its window, so it only
   counts and returns. */
static LRESULT CALLBACK on_message(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    LONG i;

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wparam > 0 && wparam < KEY_COUNT
            && (lparam & (1L << 30)) == 0) {          /* bit 30 set = auto-repeat */
        held[wparam] = 1;
        InterlockedIncrement(&presses[wparam]);
        InterlockedIncrement(&changes);
        SetEvent(changed_event);
    } else if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wparam > 0 && wparam < KEY_COUNT) {
        held[wparam] = 0;
        InterlockedIncrement(&changes);
        SetEvent(changed_event);
    }

    for (i = 0; i < sub_count; i++)
        if (subs[i].window == window) {
            if (subs[i].unicode)
                return CallWindowProcW(subs[i].proc, window, msg, wparam, lparam);
            return CallWindowProcA(subs[i].proc, window, msg, wparam, lparam);
        }
    return DefWindowProcA(window, msg, wparam, lparam);   /* not reached */
}

/* The game's main window: the largest visible top-level window it owns. */
static BOOL CALLBACK pick_window(HWND window, LPARAM best)
{
    DWORD pid = 0;
    RECT r, br;
    HWND *pick = (HWND *)best;

    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window))
        return TRUE;
    GetWindowRect(window, &r);
    if (*pick == NULL)
        *pick = window;
    else {
        GetWindowRect(*pick, &br);
        if ((r.right - r.left) * (r.bottom - r.top) > (br.right - br.left) * (br.bottom - br.top))
            *pick = window;
    }
    return TRUE;
}

static void subclass_game_window(void)
{
    HWND window = NULL;
    LONG i, n = sub_count;
    int unicode;

    EnumWindows(pick_window, (LPARAM)&window);
    if (window == NULL || n >= MAX_WINDOWS)
        return;
    /* Never wrap a window twice: its "original" procedure would be our own, and
       on_message would call itself forever. */
    for (i = 0; i < n; i++)
        if (subs[i].window == window)
            return;

    unicode = IsWindowUnicode(window);
    subs[n].unicode = unicode;
    subs[n].proc = (WNDPROC)(unicode ? GetWindowLongPtrW(window, GWLP_WNDPROC)
                                     : GetWindowLongPtrA(window, GWLP_WNDPROC));
    subs[n].window = window;
    InterlockedIncrement(&sub_count);  /* publish the entry before the swap */
    if (unicode)
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)on_message);
    else
        SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)on_message);
    InterlockedIncrement(&subclassed);
    SetEvent(changed_event);
}
static int game_in_front(void)
{
    DWORD pid = 0;
    HWND window = GetForegroundWindow();

    if (window == NULL)
        return 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

/* Keys released while another program is in front never reach the game, so
   held keys are cleared as soon as the game is not the foreground window.
   (WM_KILLFOCUS / WM_ACTIVATEAPP were tried first: they never arrived at the
   subclassed window, measured 2026-10-25.) */
static void release_all_if_unfocused(void)
{
    int vk, any = 0;

    if (game_in_front())
        return;
    for (vk = 1; vk < KEY_COUNT; vk++)
        if (held[vk]) {
            held[vk] = 0;
            any = 1;
        }
    if (any) {
        InterlockedIncrement(&changes);
        SetEvent(changed_event);
    }
}

static DWORD WINAPI watch_window(LPVOID unused)
{
    int tick;

    (void)unused;
    /* The window may not exist yet when Lua starts the bridge, and the game
       could recreate it (e.g. on a resolution change), so keep checking. */
    for (tick = 0;; tick++) {
        if (tick % 10 == 0)
            subclass_game_window();
        release_all_if_unfocused();
        Sleep(100);
    }
    return 0;
}

static DWORD WINAPI write_keys(LPVOID unused)
{
    (void)unused;
    write_state();                     /* lets Lua see that the bridge is alive */
    for (;;) {
        int tries;

        WaitForSingleObject(changed_event, INFINITE);
        for (tries = 0; tries < 50 && !write_state(); tries++)
            Sleep(5);
    }
    return 0;
}

/* The folder this DLL was loaded from, with a trailing backslash. With
   keep_loaded, takes a reference so the DLL stays loaded for good. */
static void dll_folder(char *out, int keep_loaded)
{
    HMODULE self = NULL;
    char *slash;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       (keep_loaded ? 0 : GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT),
                       (LPCSTR)&dll_folder, &self);
    GetModuleFileNameA(self, out, MAX_PATH);
    slash = strrchr(out, '\\');
    if (slash != NULL)
        slash[1] = '\0';
    else
        out[0] = '\0';
}

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return 0;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD))
        return 0;
    /* the whole value must lie inside this region */
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

__declspec(dllexport) int keybridge_peek(void *L)
{
    char dir[MAX_PATH], path[MAX_PATH], line[128];
    FILE *in, *out;

    (void)L;
    dll_folder(dir, 0);
    snprintf(path, sizeof(path), "%skeybridge_peek_req.txt", dir);
    in = fopen(path, "r");
    if (in == NULL)
        return 0;
    snprintf(path, sizeof(path), "%skeybridge_peek.txt", dir);
    out = fopen(path, "w");
    if (out == NULL) {
        fclose(in);
        return 0;
    }
    while (fgets(line, sizeof(line), in) != NULL) {
        char *rest;
        unsigned long base = strtoul(line, &rest, 16);
        unsigned long offset = strtoul(rest, &rest, 16);
        const void *p = (const void *)(base + offset);

        while (*rest == ' ')
            rest++;
        if (base == 0 || !readable(p, *rest == 's' ? PEEK_TEXT_MAX : 4))
            fprintf(out, "nil\n");
        else if (*rest == 's') {
            /* Short text: printable ASCII up to the first NUL, else nil. */
            const char *c = (const char *)p;
            int n = 0;

            while (n < PEEK_TEXT_MAX && c[n] >= 0x20 && c[n] < 0x7F)
                n++;
            if (n == 0 || n == PEEK_TEXT_MAX || c[n] != '\0')
                fprintf(out, "nil\n");
            else
                fprintf(out, "%.*s\n", n, c);
        } else if (*rest == 'f')
            fprintf(out, "%.6f\n", *(const float *)p);
        else
            fprintf(out, "%08lX\n", (unsigned long)*(const DWORD *)p);   /* hex text: Lua numbers are floats */
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int writable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return 0;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD))
        return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

/* keybridge_poke - WRITE 32-bit floats into engine memory. Lua writes
   keybridge_poke_req.txt lines "<hex address> <hex offset> <float value>" and
   calls it; keybridge_poke.txt gets "ok" or "nil" (not writable) per line.
   Only for fields ModAPI has verified (see ModAPI.HeroObject). */
__declspec(dllexport) int keybridge_poke(void *L)
{
    char dir[MAX_PATH], path[MAX_PATH], line[128];
    FILE *in, *out;

    (void)L;
    dll_folder(dir, 0);
    snprintf(path, sizeof(path), "%skeybridge_poke_req.txt", dir);
    in = fopen(path, "r");
    if (in == NULL)
        return 0;
    snprintf(path, sizeof(path), "%skeybridge_poke.txt", dir);
    out = fopen(path, "w");
    if (out == NULL) {
        fclose(in);
        return 0;
    }
    while (fgets(line, sizeof(line), in) != NULL) {
        char *rest;
        unsigned long base = strtoul(line, &rest, 16);
        unsigned long offset = strtoul(rest, &rest, 16);
        float value = (float)strtod(rest, NULL);
        void *p = (void *)(base + offset);

        if (base == 0 || !writable(p, 4)) {
            fprintf(out, "nil\n");
        } else {
            *(volatile float *)p = value;
            fprintf(out, "ok\n");
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

__declspec(dllexport) int keybridge_start(void *L)
{
    (void)L;
    if (InterlockedCompareExchange(&started, 1, 0) != 0)
        return 0;                      /* already running in this process */

    /* Put the state file next to this DLL, wherever it was loaded from. Taking
       a reference also keeps the DLL loaded while the window points into it. */
    dll_folder(state_path, 1);
    strncat(state_path, "keybridge_keys.txt", MAX_PATH - strlen(state_path) - 1);
    strcpy(temp_path, state_path);
    strncat(temp_path, ".tmp", MAX_PATH - strlen(temp_path) - 1);

    changed_event = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    CreateThread(NULL, 0, write_keys, NULL, 0, NULL);
    CreateThread(NULL, 0, watch_window, NULL, 0, NULL);
    return 0;
}
