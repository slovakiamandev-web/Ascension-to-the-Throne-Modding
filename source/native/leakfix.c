/* leakfix.c - plugs the engine's two big memory leaks of a world rebuild (load from the main menu, new game,
 * continent travel). Measured with native/allocprobe.c + native/animprobe.c: ~45 MB per rebuild, which made the
 * 32-bit game run out of address space (std::bad_alloc) after ~17 rebuilds.
 *
 * 1. The animation cache (object at [0x6CA490], built per world, freed when the last skinned model goes) keeps
 *    its .ska files in a hash_map by file name. Its lookup (hash_map::find 0x422D80, called at 0x4C050F) never
 *    finds StandardWorld.ska - the world-map banner animation every army and NPC standard asks for: the loader
 *    lowercases the name in place before it is stored, so the map holds "standardworld.ska", while the units' data
 *    asks for "StandardWorld.ska" (every other animation name is lowercase in the data already). So the file was
 *    read again for each of ~750 banners, and each new copy replaced the last one in the map. Only the one in the
 *    map is freed with the cache: ~749 animations + their file buffers (~33 MB) were lost per rebuild.
 *    The call is redirected to a case-insensitive search of the map's element list (pak names are
 *    case-insensitive anyway; a few hundred entries, a few microseconds per lookup); the original lookup still
 *    runs when the search finds nothing.
 * 2. HeightMap::Load (0x473370, called only at 0x528EA6) allocates a column table and one column per x without
 *    freeing the previous ones (3001 columns, 12 MB per rebuild). The call is wrapped to free them first.
 *
 * 3. Not a leak, a crash at Exit (2026-11-10, the user's game; also a known vanilla bug): Esc -> Exit tears the world
 *    down and destroys the 3D sound system, clearing its pointer [0x6CA4B4] to 0 (0x4C528E). A 3D sound still
 *    marked as playing that is stopped or destroyed after that (Sound3D stop 0x4C5410, destructor 0x4C5440) hands
 *    its channel back to that system (0x4C53A0, thiscall on [0x6CA4B4]) - with this = 0 it read [0+0x34] and the
 *    game died at 0x4C53AB (access violation; the process hung on the error box). Timing-dependent, not
 *    reproducible on demand. Both calls (0x4C542B, 0x4C548C) go through a check: no sound system -> nothing to
 *    hand back (the caller still clears the sound's playing flags).
 *
 * Every patched call is checked byte for byte first; the DLL pins itself, because Lua unloads C libraries when the
 * game replaces its Lua state and the patched calls must keep pointing at live code.
 * Build: C:\MinGW\bin\gcc -shared -O2 -Wall -static-libgcc -o leakfix.dll leakfix.c
 * Lua: package.loadlib("ModFramework/leakfix.dll", "leakfix_start")() -> leakfix_status.txt (next to the DLL)
 */
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FIND_CALL 0x4C050F     /* call hash_map::find in the animation cache (0x4C0240) */
#define FIND_FN 0x422D80
#define HM_CALL 0x528EA6       /* call HeightMap::Load in the continent loader */
#define HM_LOAD 0x473370
#define SND_STOP_CALL 0x4C542B  /* Sound3D stop -> hand the channel back to the 3D sound system */
#define SND_DTOR_CALL 0x4C548C  /* the same from Sound3D's destructor */
#define SND_RELEASE 0x4C53A0    /* thiscall on [0x6CA4B4], 3 stack arguments, ret 0xC */
#define DELETE_THUNK 0x5E3E9C  /* jmp [0x6073A8] = the exe's own operator delete (MSVCR80 ??3@YAXPAX@Z) */

typedef DWORD *(__fastcall *FindFn)(void *map, void *edx, DWORD *out, const BYTE *key);
typedef int (__fastcall *HmLoadFn)(void *self, void *edx, void *a1, void *a2);
typedef void (__cdecl *DeleteFn)(void *p);
typedef void (__fastcall *SndReleaseFn)(void *system, void *edx, void *a, DWORD b, DWORD c);

static const BYTE delete_thunk_code[6] = { 0xFF, 0x25, 0xA8, 0x73, 0x60, 0x00 };
static DeleteFn real_delete;
static int find_ok, hm_ok, snd_ok, started;
static unsigned long found_by_search, heightmaps_freed, sounds_skipped;

/* MSVC 8 std::string: text inline at +4 while capacity (+0x18) < 16, else a pointer there; size at +0x14 */
static const char *str_text(const BYTE *s) { return *(const DWORD *)(s + 0x18) >= 16 ? *(const char * const *)(s + 4) : (const char *)(s + 4); }
static DWORD str_size(const BYTE *s) { return *(const DWORD *)(s + 0x14); }

/* map = the cache's hash_map; its list head is at +8: nodes {next, prev, pair<string, Animation*>} */
static DWORD *__fastcall find_fix(void *map, void *edx, DWORD *out, const BYTE *key)
{
    BYTE *head = *(BYTE **)((BYTE *)map + 8);
    DWORD n = str_size(key);
    const char *k = str_text(key);
    BYTE *node;
    int guard = 0;

    if (head != NULL)
        for (node = *(BYTE **)head; node != head && node != NULL && guard < 100000; node = *(BYTE **)node, guard++)
            if (str_size(node + 8) == n && _strnicmp(str_text(node + 8), k, n) == 0) {
                *out = (DWORD)node;
                found_by_search++;
                return out;
            }
    return ((FindFn)FIND_FN)(map, edx, out, key);
}

/* HeightMap: +4 column table, +8 columns, columns of u16 heights; all made with operator new */
static int __fastcall hm_load_fix(void *self, void *edx, void *a1, void *a2)
{
    DWORD *hm = (DWORD *)self;
    void **table = (void **)hm[1];
    DWORD i;

    if (table != NULL && real_delete != NULL) {
        for (i = 0; i < hm[2]; i++)
            real_delete(table[i]);
        real_delete(table);
        hm[1] = 0;
        heightmaps_freed++;
    }
    return ((HmLoadFn)HM_LOAD)(self, edx, a1, a2);
}

/* a sound handed back after the 3D sound system was destroyed: nothing to hand it back to */
static void __fastcall snd_release_fix(void *system, void *edx, void *a, DWORD b, DWORD c)
{
    if (system == NULL) {
        sounds_skipped++;
        return;
    }
    ((SndReleaseFn)SND_RELEASE)(system, edx, a, b, c);
}

static int patch_call(DWORD site, DWORD expect, void *to)
{
    BYTE *p = (BYTE *)site;
    DWORD old;

    if (IsBadReadPtr(p, 5) || p[0] != 0xE8 || *(DWORD *)(p + 1) != expect - (site + 5))
        return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(DWORD *)(p + 1) = (DWORD)to - (site + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static int already(DWORD site, void *to)
{
    BYTE *p = (BYTE *)site;
    return !IsBadReadPtr(p, 5) && p[0] == 0xE8 && *(DWORD *)(p + 1) == (DWORD)to - (site + 5);
}

/* a file next to this DLL (ModFramework/ in an installed game) */
static void beside_dll(char *out, const char *name)
{
    HMODULE self = NULL;
    char *slash;
    out[0] = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&beside_dll, &self);
    GetModuleFileNameA(self, out, MAX_PATH);
    slash = strrchr(out, '\\');
    if (slash != NULL)
        slash[1] = 0;
    else
        out[0] = 0;
    strcat(out, name);
}

static void write_status(void)
{
    char path[MAX_PATH];
    FILE *f;
    beside_dll(path, "leakfix_status.txt");
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "animation cache lookup: %s\nheightmap: %s\n", find_ok ? "fixed" : "NOT patched (unknown game build)",
            hm_ok ? "fixed" : (real_delete ? "NOT patched (unknown game build)" : "NOT patched (no MSVCR80 delete)"));
    fprintf(f, "3D sounds after the sound system's shutdown: %s\n", snd_ok ? "guarded" : "NOT patched (unknown game build)");
    fprintf(f, "found by the search: %lu, heightmaps freed: %lu, late sounds skipped: %lu\n", found_by_search,
            heightmaps_freed, sounds_skipped);
    fclose(f);
}

__declspec(dllexport) int leakfix_start(void *L)
{
    HMODULE self, crt;

    (void)L;
    if (!started) {
        started = 1;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCSTR)leakfix_start, &self);
        crt = GetModuleHandleA("MSVCR80.dll");
        /* free through the exe's own import, so the columns go back exactly the way the engine frees memory */
        if (!IsBadReadPtr((void *)DELETE_THUNK, 6) && memcmp((void *)DELETE_THUNK, delete_thunk_code, 6) == 0 &&
            crt != NULL && *(void **)0x6073A8 != NULL)
            real_delete = (DeleteFn)DELETE_THUNK;
        find_ok = already(FIND_CALL, (void *)find_fix) || patch_call(FIND_CALL, FIND_FN, (void *)find_fix);
        hm_ok = real_delete != NULL &&
                (already(HM_CALL, (void *)hm_load_fix) || patch_call(HM_CALL, HM_LOAD, (void *)hm_load_fix));
        snd_ok = (already(SND_STOP_CALL, (void *)snd_release_fix) ||
                  patch_call(SND_STOP_CALL, SND_RELEASE, (void *)snd_release_fix)) &&
                 (already(SND_DTOR_CALL, (void *)snd_release_fix) ||
                  patch_call(SND_DTOR_CALL, SND_RELEASE, (void *)snd_release_fix));
    }
    write_status();
    return 0;
}
