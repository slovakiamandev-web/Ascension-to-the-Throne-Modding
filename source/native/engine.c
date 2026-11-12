/*
 * engine.dll - a real Lua extension for AttT, built on the Lua 5.1 API that is
 * compiled INTO ATThrone.exe (statically linked, not exported).
 *
 *   local Engine = package.loadlib("ModFramework/engine.dll", "engine_open")()
 *   -- nil if this exe's code cannot be recognised (another game build)
 *
 * Mods should not use it directly: ModAPI.Battle wraps it (attaches it to the
 * battle, falls back to nil on other builds).
 *
 * ---------------------------------------------------------------- binding
 * Nothing is taken on trust; everything is FOUND in the running exe and
 * checked (2026-10-26, engine 3):
 *   1. The unit methods are registered from a table of 16-byte entries
 *      (name at +4, C++ function at +0xC). The table is found from the string
 *      "GetHeight" and the code that stores its address.
 *   2. GetHeight's code reads the global Lua state (mov eax, ds:[G]) and calls
 *      lua_pushnumber - both are read out of that code.
 *   3. The rest of lapi.c is compiled next to lua_pushnumber, in source order.
 *      Each function is located by its distance from lua_pushnumber and its
 *      first bytes are compared (call targets ignored).
 *   4. The unit class pointer (vtable) and the field offsets are confirmed
 *      against a live unit with Engine.Attach(unit, health, maxHealth), using
 *      the values the game's own BattleInfo() reports.
 * Any failure means "unavailable" - nothing is called.
 *
 * ---------------------------------------------------------------- safety
 * Only narrow functions with validated arguments are exposed. Every unit
 * handle is re-checked on every call (class pointer, health range, troop and
 * side readable); engine methods are only ever called with arguments built
 * here. The raw readers (Engine.Debug) are read-only and research-only.
 *
 * Build (32-bit, like the game):  gcc -shared -O2 -Wall -static-libgcc -o engine.dll engine.c
 */
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lua_State lua_State;
typedef float lua_Number;                       /* the game's custom number type */
typedef int (*lua_CFunction)(lua_State *L);

#define LUA_TNUMBER 3
#define LUA_TUSERDATA 7

static int         (*lua_gettop)(lua_State *L);
static void        (*lua_settop)(lua_State *L, int idx);
static int         (*lua_type)(lua_State *L, int idx);
static lua_Number  (*lua_tonumber)(lua_State *L, int idx);
static const char *(*lua_tolstring)(lua_State *L, int idx, size_t *len);
static void       *(*lua_touserdata)(lua_State *L, int idx);
static void        (*lua_pushnumber)(lua_State *L, lua_Number n);
static void        (*lua_pushlstring)(lua_State *L, const char *s, size_t len);
static void        (*lua_pushstring)(lua_State *L, const char *s);
static void        (*lua_pushcclosure)(lua_State *L, lua_CFunction fn, int n);
static void        (*lua_pushboolean)(lua_State *L, int b);
static void        (*lua_getfield)(lua_State *L, int idx, const char *k);
static void        (*lua_createtable)(lua_State *L, int narr, int nrec);
static void        (*lua_setfield)(lua_State *L, int idx, const char *k);
static void        (*lua_rawseti)(lua_State *L, int idx, int n);

/* Each API function: distance from lua_pushnumber, expected first bytes
   (0x100 = any byte, used for call targets) and where to store it.
   Measured in the Steam exe 1.1.128.0 (lua_pushnumber at 0x4030A0). */
#define ANY 0x100
static const struct {
    const char *name;
    long delta;
    short sig[12];
    void **slot;
} API[] = {
    { "lua_gettop",      0x402A60 - 0x4030A0, { 0x8B,0x4C,0x24,0x04,0x8B,0x41,0x08,0x2B,0x41,0x0C,0xC1,0xF8 }, (void **)&lua_gettop },
    { "lua_settop",      0x402A70 - 0x4030A0, { 0x8B,0x4C,0x24,0x08,0x85,0xC9,0x8B,0x44,0x24,0x04,0x7C,ANY  }, (void **)&lua_settop },
    { "lua_type",        0x402C20 - 0x4030A0, { 0x8B,0x4C,0x24,0x08,0x8B,0x54,0x24,0x04,0xE8,ANY, ANY, ANY  }, (void **)&lua_type },
    { "lua_tonumber",    0x402DE0 - 0x4030A0, { 0x8B,0x4C,0x24,0x08,0x8B,0x54,0x24,0x04,0x83,0xEC,0x0C,0x56 }, (void **)&lua_tonumber },
    { "lua_tolstring",   0x402ED0 - 0x4030A0, { 0x53,0x55,0x8B,0x6C,0x24,0x10,0x56,0x57,0x8B,0x7C,0x24,0x14 }, (void **)&lua_tolstring },
    { "lua_touserdata",  0x402FD0 - 0x4030A0, { 0x8B,0x4C,0x24,0x08,0x8B,0x54,0x24,0x04,0xE8,ANY, ANY, ANY  }, (void **)&lua_touserdata },
    { "lua_pushnumber",  0,                   { 0x8B,0x44,0x24,0x04,0xD9,0x44,0x24,0x08,0x8B,0x48,0x08,0xD9 }, (void **)&lua_pushnumber },
    { "lua_pushlstring", 0x4030E0 - 0x4030A0, { 0x53,0x8B,0x5C,0x24,0x08,0x8B,0x43,0x10,0x8B,0x48,0x44,0x3B }, (void **)&lua_pushlstring },
    { "lua_pushstring",  0x403120 - 0x4030A0, { 0x51,0x53,0x55,0x8B,0x6C,0x24,0x14,0x85,0xED,0x56,0x57,0x75 }, (void **)&lua_pushstring },
    { "lua_pushcclosure",0x4031F0 - 0x4030A0, { 0x53,0x8B,0x5C,0x24,0x08,0x8B,0x43,0x10,0x8B,0x48,0x44,0x3B }, (void **)&lua_pushcclosure },
    { "lua_pushboolean", 0x403280 - 0x4030A0, { 0x8B,0x44,0x24,0x04,0x8B,0x48,0x08,0x33,0xD2,0x39,0x54,0x24 }, (void **)&lua_pushboolean },
    { "lua_getfield",    0x403300 - 0x4030A0, { 0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x8B,0x4D,0x0C,0x83,0xEC,0x0C }, (void **)&lua_getfield },
    { "lua_createtable", 0x4033D0 - 0x4030A0, { 0x53,0x8B,0x5C,0x24,0x08,0x8B,0x43,0x10,0x8B,0x48,0x44,0x3B }, (void **)&lua_createtable },
    { "lua_setfield",    0x4034F0 - 0x4030A0, { 0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x8B,0x4D,0x0C,0x83,0xEC,0x0C }, (void **)&lua_setfield },
    { "lua_rawseti",     0x403610 - 0x4030A0, { 0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x83,0xEC,0x0C,0x8B,0x4D,0x0C }, (void **)&lua_rawseti },
};

/* Unit (TacticCreature) layout, confirmed by Attach. */
#define U_NAME       0x00C
#define U_MAX_HEALTH 0x1E0
#define U_HEALTH     0x1EC
#define U_BAR_SHOWN  0x3BC                       /* byte: 1 = health bar drawn */
#define U_TROOP      0x2C8
#define T_SIDE       0x094                       /* 0 = player, 1 = enemy */

typedef int  (__attribute__((thiscall)) *method_fn)(void *self, void *ci);
typedef struct { DWORD unknown[3]; int nargs; int base; DWORD pad[4]; } CallInfo;

static DWORD text_lo, text_hi, data_lo, data_hi; /* exe code / everything else */
static DWORD method_table;                       /* first entry of the unit methods */
static lua_State **global_state;
static DWORD unit_vtable;                        /* learned by Attach */
static int api_state = -1;                       /* -1 unchecked, 0 bad, 1 good */
static char api_error[128] = "";

/* ---------------------------------------------------------------- memory */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    if (p == NULL || VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return 0;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD))
        return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static int is_code(DWORD a) { return a >= text_lo && a < text_hi; }

static int match(DWORD a, const short *sig, int n)
{
    int i;

    if (!readable((void *)a, n))
        return 0;
    for (i = 0; i < n; i++)
        if (sig[i] != ANY && ((unsigned char *)a)[i] != sig[i])
            return 0;
    return 1;
}

static int fail(const char *why)
{
    snprintf(api_error, sizeof(api_error), "%s", why);
    return api_state = 0;
}

/* ---------------------------------------------------------------- binding */

static int find_image(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *s;
    int i;

    if (base == NULL || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    s = IMAGE_FIRST_SECTION(nt);
    data_lo = (DWORD)base;
    data_hi = (DWORD)base + nt->OptionalHeader.SizeOfImage;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (s->Characteristics & IMAGE_SCN_CNT_CODE) {
            text_lo = (DWORD)base + s->VirtualAddress;
            text_hi = text_lo + s->Misc.VirtualSize;
        }
    return text_lo != 0;
}

/* The address of a NUL-terminated string in the exe's non-code part. */
static DWORD find_string(const char *str)
{
    size_t n = strlen(str) + 1;
    DWORD a;

    for (a = text_hi; a + n < data_hi; a++)
        if (readable((void *)a, n) && memcmp((void *)a, str, n) == 0 && ((char *)a)[-1] == '\0')
            return a;
    return 0;
}

/* Code that does  mov dword ptr [X], imm32  with imm32 == value -> X. */
static DWORD find_store_of(DWORD value)
{
    DWORD a;

    for (a = text_lo; a + 10 < text_hi; a++) {
        BYTE *p = (BYTE *)a;
        if (p[0] == 0xC7 && p[1] == 0x05 && *(DWORD *)(p + 6) == value)
            return *(DWORD *)(p + 2);
    }
    return 0;
}

static int bind_api(void)
{
    DWORD str, slot, gh, a, pushnumber = 0;
    size_t i;

    if (api_state >= 0)
        return api_state;
    if (!find_image())
        return fail("exe image not found");

    /* 1. GetHeight's registration entry -> the method table */
    if ((str = find_string("GetHeight")) == 0)
        return fail("string GetHeight not found");
    if ((slot = find_store_of(str)) == 0 || !readable((void *)(slot + 8), 4))
        return fail("GetHeight registration not found");
    method_table = slot - 4;                     /* entry: +4 name, +0xC function */
    while (readable((void *)(method_table - 16), 16)) {
        const char *n = *(const char **)(method_table - 16 + 4);
        if (n == NULL || !readable(n, 2) || n[0] < 'A' || n[0] > 'Z')
            break;
        method_table -= 16;
    }
    gh = *(DWORD *)(slot + 8);
    if (!is_code(gh))
        return fail("GetHeight function not in code");

    /* 2. inside GetHeight: mov eax, ds:[G]; push ecx; fstp [esp]; push eax; call pushnumber */
    for (a = gh; a < gh + 0x80; a++) {
        BYTE *p = (BYTE *)a;
        if (p[0] == 0xA1 && p[5] == 0x51 && p[6] == 0xD9 && p[7] == 0x1C && p[8] == 0x24 &&
            p[9] == 0x50 && p[10] == 0xE8) {
            global_state = *(lua_State ***)(p + 1);
            pushnumber = a + 15 + *(long *)(p + 11);
            break;
        }
    }
    if (pushnumber == 0 || !is_code(pushnumber) || !readable(global_state, 4))
        return fail("lua_pushnumber not found in GetHeight");

    /* 3. the rest of lapi.c around it */
    for (i = 0; i < sizeof(API) / sizeof(API[0]); i++) {
        DWORD f = pushnumber + API[i].delta;
        if (!is_code(f) || !match(f, API[i].sig, 12)) {
            snprintf(api_error, sizeof(api_error), "%s does not match", API[i].name);
            return api_state = 0;
        }
    }
    for (i = 0; i < sizeof(API) / sizeof(API[0]); i++)
        *API[i].slot = (void *)(pushnumber + API[i].delta);
    return api_state = 1;
}

/* ---------------------------------------------------------------- helpers */

/* Handles travel as hex strings: Lua numbers are 32-bit floats here. */
static DWORD arg_handle(lua_State *L, int idx)
{
    const char *s;

    if (lua_type(L, idx) == LUA_TUSERDATA) {
        DWORD *block = (DWORD *)lua_touserdata(L, idx);
        return (block != NULL && readable(block, 4)) ? *block : 0;
    }
    s = lua_tolstring(L, idx, NULL);
    return s != NULL ? (DWORD)strtoul(s, NULL, 16) : 0;
}

static void push_handle(lua_State *L, DWORD a)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%08lX", (unsigned long)a);
    lua_pushstring(L, buf);
}

static int arg_number(lua_State *L, int idx, lua_Number *out)
{
    if (lua_type(L, idx) != LUA_TNUMBER)
        return 0;
    *out = lua_tonumber(L, idx);
    return 1;
}

static DWORD find_method(const char *name)
{
    DWORD e;

    for (e = method_table; readable((void *)e, 16); e += 16) {
        const char *n = *(const char **)(e + 4);
        DWORD fn = *(DWORD *)(e + 12);
        if (n == NULL || !readable(n, 1))
            break;
        if (strcmp(n, name) == 0)
            return is_code(fn) ? fn : 0;
    }
    return 0;
}

/* A live unit: our class, sane health, readable troop with a 0/1 side. */
static int side_of(DWORD u)
{
    DWORD troop;
    int side, max, hp;

    if (unit_vtable == 0 || !readable((void *)u, 0x400) || *(DWORD *)u != unit_vtable)
        return -1;
    max = *(int *)(u + U_MAX_HEALTH);
    hp = *(int *)(u + U_HEALTH);
    if (max <= 0 || max > 1000000 || hp < 0 || hp > max)
        return -1;
    troop = *(DWORD *)(u + U_TROOP);
    if (!readable((void *)troop, T_SIDE + 4))
        return -1;
    side = *(int *)(troop + T_SIDE);
    return (side == 0 || side == 1) ? side : -1;
}

static int valid_unit(DWORD u) { return side_of(u) >= 0; }

/* Call a unit method with numbers we supply; returns how many results it
   left on the stack (the caller must restore the stack). */
static int call_method(lua_State *L, DWORD u, const char *name, const lua_Number *args, int n)
{
    DWORD fn = find_method(name);
    CallInfo ci;
    int i;

    if (fn == 0 || *global_state != L)
        return -1;
    memset(&ci, 0, sizeof(ci));
    ci.base = lua_gettop(L);                     /* 'self' slot; args follow */
    for (i = 0; i < n; i++)
        lua_pushnumber(L, args[i]);
    ci.nargs = n;
    return ((method_fn)fn)((void *)u, &ci);
}

/* Position of a unit via its own GetPos (returns a table {x, y, z}). */
static int unit_pos(lua_State *L, DWORD u, lua_Number *x, lua_Number *z)
{
    int top = lua_gettop(L), ok = 0;

    if (call_method(L, u, "GetPos", NULL, 0) >= 1) {
        lua_getfield(L, -1, "x");
        *x = lua_tonumber(L, -1);
        lua_getfield(L, -2, "z");
        *z = lua_tonumber(L, -1);
        ok = 1;
    }
    lua_settop(L, top);
    return ok;
}

/* The highest address the process can use: 2 GB normally, 4 GB when the exe is large-address-aware
   (64-bit Windows) - a fixed 2 GB limit would miss units allocated above it. */
static DWORD top_address(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (DWORD)si.lpMaximumApplicationAddress;
}

/* All living units in memory (by class pointer, then validated; health > 0). */
static int collect_units(DWORD *out, int max)
{
    MEMORY_BASIC_INFORMATION mbi;
    char *p = (char *)0x10000;
    int n = 0;
    DWORD top = top_address();

    while ((DWORD)p < top && (DWORD)p >= 0x10000 && VirtualQuery(p, &mbi, sizeof(mbi)) != 0) {
        char *start = (char *)mbi.BaseAddress, *end = start + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            DWORD *q;
            for (q = (DWORD *)start; (char *)(q + 1) <= end && n < max; q++)
                if (*q == unit_vtable && valid_unit((DWORD)q) && *(int *)((DWORD)q + U_HEALTH) > 0)
                    out[n++] = (DWORD)q;      /* dead units stay in memory as corpses */
        }
        p = end;
    }
    return n;
}

/* --------------------------------------------------------- the functions */

static int e_version(lua_State *L)
{
    lua_pushstring(L, "engine 14");
    return 1;
}

/* Engine.Attach(unit, health, maxHealth) -> true | nil, reason
   unit = a TacticCreature userdata the game gave Lua (e.g. the current troop's
   unit); health/maxHealth = what its BattleInfo() reports. Learns the class
   pointer and confirms the field offsets. Needed once per process. */
static int e_attach(lua_State *L)
{
    DWORD *block, u, vt, troop;
    lua_Number hp, max;
    int side;

    if (lua_type(L, 1) != LUA_TUSERDATA || !arg_number(L, 2, &hp) || !arg_number(L, 3, &max)) {
        lua_pushboolean(L, 0); lua_pushstring(L, "usage: Attach(unit userdata, health, maxHealth)");
        return 2;
    }
    block = (DWORD *)lua_touserdata(L, 1);
    if (block == NULL || !readable(block, 4) || !readable((void *)(u = *block), 0x400)) {
        lua_pushboolean(L, 0); lua_pushstring(L, "unit object not readable");
        return 2;
    }
    vt = *(DWORD *)u;
    if (vt < text_hi || vt >= data_hi) {
        lua_pushboolean(L, 0); lua_pushstring(L, "unit class pointer not in the exe");
        return 2;
    }
    if (*(int *)(u + U_HEALTH) != (int)hp || *(int *)(u + U_MAX_HEALTH) != (int)max) {
        lua_pushboolean(L, 0); lua_pushstring(L, "health fields do not match BattleInfo");
        return 2;
    }
    troop = *(DWORD *)(u + U_TROOP);
    side = readable((void *)troop, T_SIDE + 4) ? *(int *)(troop + T_SIDE) : -1;
    if (side != 0 && side != 1) {
        lua_pushboolean(L, 0); lua_pushstring(L, "troop side not found");
        return 2;
    }
    if (find_method("GetPos") == 0 || find_method("SubHealth") == 0 ||
        find_method("SetSubHealth") == 0 || find_method("PlayDefense") == 0) {
        lua_pushboolean(L, 0); lua_pushstring(L, "unit methods not found");
        return 2;
    }
    unit_vtable = vt;
    lua_pushboolean(L, 1);
    return 1;
}

/* Engine.Units([side]) -> list of unit handles; side 0 = player, 1 = enemy */
static int e_units(lua_State *L)
{
    static DWORD found[512];
    lua_Number want = -1;
    int n, i, k = 0;

    if (unit_vtable == 0)
        return 0;
    arg_number(L, 1, &want);
    n = collect_units(found, 512);
    lua_createtable(L, n, 0);
    for (i = 0; i < n; i++)
        if (want < 0 || side_of(found[i]) == (int)want) {
            push_handle(L, found[i]);
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

/* Engine.UnitsNear(x, z, radius[, side]) -> list of handles, nearest first */
static int e_units_near(lua_State *L)
{
    static DWORD found[512];
    static float dist[512];
    lua_Number x, z, r, want = -1;
    int n, i, j, k = 0;

    if (unit_vtable == 0 || !arg_number(L, 1, &x) || !arg_number(L, 2, &z) || !arg_number(L, 3, &r))
        return 0;
    arg_number(L, 4, &want);
    n = collect_units(found, 512);
    for (i = 0; i < n; i++) {                   /* keep the ones in range */
        lua_Number ux, uz;
        if ((want >= 0 && side_of(found[i]) != (int)want) || !unit_pos(L, found[i], &ux, &uz))
            continue;
        dist[k] = (float)sqrt((ux - x) * (ux - x) + (uz - z) * (uz - z));
        if (dist[k] <= r)
            found[k++] = found[i];
    }
    for (i = 1; i < k; i++)                     /* nearest first */
        for (j = i; j > 0 && dist[j] < dist[j - 1]; j--) {
            float d = dist[j]; DWORD f = found[j];
            dist[j] = dist[j - 1]; found[j] = found[j - 1];
            dist[j - 1] = d; found[j - 1] = f;
        }
    lua_createtable(L, k, 0);
    for (i = 0; i < k; i++) {
        push_handle(L, found[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* Engine.UnitName(u) -> "uPeter". The name is an MSVC std::string: 16 bytes of
 * text or a pointer, then size (+0x10) and capacity (+0x14); capacity >= 16 means
 * the text lives behind the pointer (seen with "uModHarbourGuard", 16 chars). */
static int e_unit_name(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    DWORD size, cap;
    const char *text;

    if (!valid_unit(u))
        return 0;
    size = *(DWORD *)(u + U_NAME + 0x10);
    cap = *(DWORD *)(u + U_NAME + 0x14);
    if (size > 255 || cap < size)
        return 0;
    text = cap >= 16 ? *(const char **)(u + U_NAME) : (const char *)(u + U_NAME);
    if (!readable(text, size + 1))
        return 0;
    lua_pushlstring(L, text, size);
    return 1;
}

/* Engine.UnitHealth(u) -> health, maxHealth */
static int e_unit_health(lua_State *L)
{
    DWORD u = arg_handle(L, 1);

    if (!valid_unit(u))
        return 0;
    lua_pushnumber(L, (lua_Number)*(int *)(u + U_HEALTH));
    lua_pushnumber(L, (lua_Number)*(int *)(u + U_MAX_HEALTH));
    return 2;
}

/* Engine.UnitSide(u) -> 0 player, 1 enemy */
static int e_unit_side(lua_State *L)
{
    int side = side_of(arg_handle(L, 1));

    if (side < 0)
        return 0;
    lua_pushnumber(L, (lua_Number)side);
    return 1;
}

/* Engine.UnitStats(u) -> the table the unit's own BattleInfo() returns
   (Health, TabHealth, Armor, Damage, Speed, MagicResist, ...). */
static int e_unit_stats(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    int top, n;

    if (!valid_unit(u))
        return 0;
    top = lua_gettop(L);
    n = call_method(L, u, "BattleInfo", NULL, 0);
    if (n < 1 || lua_gettop(L) < top + 1) {
        lua_settop(L, top);
        return 0;
    }
    lua_settop(L, top + 1);                     /* keep just the table */
    return 1;
}

/* Engine.UnitPosition(u) -> x, z */
static int e_unit_position(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number x, z;

    if (!valid_unit(u) || !unit_pos(L, u, &x, &z))
        return 0;
    lua_pushnumber(L, x);
    lua_pushnumber(L, z);
    return 2;
}

/* Engine.DamageUnit(u, amount[, fromX, fromZ]) -> damage dealt
   Instant health loss (with an optional reaction; its floating number shows 0).
   For a normal-looking hit use Engine.HitUnit.
   amount is clamped to 0..current health. */
static int e_damage_unit(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number amount, args[3];
    int hp, top = lua_gettop(L), dmg;

    if (!valid_unit(u) || !arg_number(L, 2, &amount))
        return 0;
    hp = *(int *)(u + U_HEALTH);
    dmg = (int)amount;
    if (dmg < 0) dmg = 0;
    if (dmg > hp) dmg = hp;
    args[0] = (lua_Number)dmg;
    call_method(L, u, "SetSubHealth", args, 1);
    lua_settop(L, top);
    if (arg_number(L, 3, &args[0]) && arg_number(L, 4, &args[1])) {
        args[2] = 5;                             /* frames before the reaction */
        call_method(L, u, "PlayDefense", args, 3);
        lua_settop(L, top);
    }
    args[0] = (lua_Number)dmg;
    call_method(L, u, "SubHealth", args, 1);
    lua_settop(L, top);
    lua_pushnumber(L, (lua_Number)dmg);
    return 1;
}

/* Engine.HitUnit(u, amount, fromX, fromZ[, delayFrames]) -> damage
   A proper hit, as the game's spells do it: the unit turns to the attacker
   and plays its defence reaction (PlayDefense), the floating number is set
   (SetSubHealth) and THE ENGINE applies that damage itself when the reaction
   lands (~0.5 s later; verified 2026-10-26 - calling SubHealth on top of it
   would deal it twice). amount is clamped to 0..current health; a unit
   brought to 0 dies when the engine next checks (end of the action/turn). */
static int e_hit_unit(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number amount, args[3], delay = 5;
    int hp, top = lua_gettop(L), dmg;

    if (!valid_unit(u) || !arg_number(L, 2, &amount) || !arg_number(L, 3, &args[0]) || !arg_number(L, 4, &args[1]))
        return 0;
    arg_number(L, 5, &delay);
    if (delay < 0) delay = 0;
    if (delay > 300) delay = 300;
    hp = *(int *)(u + U_HEALTH);
    dmg = (int)amount;
    if (dmg < 0) dmg = 0;
    if (dmg > hp) dmg = hp;
    args[2] = delay;
    call_method(L, u, "PlayDefense", args, 3);
    lua_settop(L, top);
    args[0] = (lua_Number)dmg;
    call_method(L, u, "SetSubHealth", args, 1);
    lua_settop(L, top);
    lua_pushnumber(L, (lua_Number)dmg);
    return 1;
}
/* Engine.PreviewDamage(u, amount) -> amount shown
   The orange "damage to come" part of the unit's health bar, as the game's
   spells show it while aiming (the unit's own SetHealthBar: it only stores the
   number at +0x1E4; the engine draws it). 0 clears it. Clamped to 0..health. */
static int e_preview_damage(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number amount, args[1];
    int hp, top = lua_gettop(L), n;

    if (!valid_unit(u) || !arg_number(L, 2, &amount))
        return 0;
    hp = *(int *)(u + U_HEALTH);
    n = (int)amount;
    if (n < 0) n = 0;
    if (n > hp) n = hp;
    args[0] = (lua_Number)n;
    call_method(L, u, "SetHealthBar", args, 1);
    lua_settop(L, top);
    lua_pushnumber(L, (lua_Number)n);
    return 1;
}
/* The engine draws a unit's health bar while this byte is 1 - it sets it for
   the troop under the cursor. Anything but 0/1 there means the layout is not
   the one we know, so nothing is read or written. */
static unsigned char *bar_flag(DWORD u)
{
    unsigned char *p;

    if (!valid_unit(u))
        return NULL;
    p = (unsigned char *)(u + U_BAR_SHOWN);
    if (!readable(p, 1) || *p > 1)
        return NULL;
    return p;
}

/* Engine.HealthBarShown(u) -> true/false (nothing if not a unit) */
static int e_health_bar_shown(lua_State *L)
{
    unsigned char *p = bar_flag(arg_handle(L, 1));

    if (p == NULL)
        return 0;
    lua_pushboolean(L, *p);
    return 1;
}

/* Engine.ShowHealthBar(u, on) -> true if set; on = 1 shows, 0 hides. Shows or
   hides a unit's health bar (and with it the PreviewDamage part), as hovering
   its troop does. */
static int e_show_health_bar(lua_State *L)
{
    unsigned char *p = bar_flag(arg_handle(L, 1));
    lua_Number on;

    if (p == NULL || !arg_number(L, 2, &on))
        return 0;
    *p = on != 0;
    lua_pushboolean(L, 1);
    return 1;
}
/* ---------------------------------------------------- Engine.Debug (read) */

static int d_read_u32(lua_State *L)
{
    lua_Number off = 0;
    DWORD *p;

    arg_number(L, 2, &off);
    p = (DWORD *)(arg_handle(L, 1) + (DWORD)off);
    if (!readable(p, 4))
        return 0;
    push_handle(L, *p);
    return 1;
}

static int d_read_int(lua_State *L)
{
    lua_Number off = 0;
    int *p;

    arg_number(L, 2, &off);
    p = (int *)(arg_handle(L, 1) + (DWORD)off);
    if (!readable(p, 4))
        return 0;
    lua_pushnumber(L, (lua_Number)*p);
    return 1;
}

static int d_read_float(lua_State *L)
{
    lua_Number off = 0;
    float *p;

    arg_number(L, 2, &off);
    p = (float *)(arg_handle(L, 1) + (DWORD)off);
    if (!readable(p, 4))
        return 0;
    lua_pushnumber(L, *p);
    return 1;
}

static int d_read_string(lua_State *L)
{
    lua_Number off = 0;
    const char *p;
    int n = 0;

    arg_number(L, 2, &off);
    p = (const char *)(arg_handle(L, 1) + (DWORD)off);
    while (n < 256 && readable(p + n, 1) && p[n] >= 0x20 && p[n] < 0x7F)
        n++;
    if (n == 0 || n == 256 || !readable(p + n, 1) || p[n] != '\0')
        return 0;
    lua_pushlstring(L, p, n);
    return 1;
}

/* Engine.Debug.Handle(userdata) -> the C++ object behind an engine userdata */
static int d_handle(lua_State *L)
{
    DWORD u = arg_handle(L, 1);

    if (u == 0)
        return 0;
    push_handle(L, u);
    return 1;
}

static int d_find_dword(lua_State *L)
{
    DWORD value = arg_handle(L, 1);
    lua_Number maxn = 64;
    MEMORY_BASIC_INFORMATION mbi;
    char *p = (char *)0x10000;
    int found = 0;
    DWORD top = top_address();

    arg_number(L, 2, &maxn);
    lua_createtable(L, 0, 0);
    while ((DWORD)p < top && (DWORD)p >= 0x10000 && VirtualQuery(p, &mbi, sizeof(mbi)) != 0) {
        char *start = (char *)mbi.BaseAddress, *end = start + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))
            && !(mbi.Protect & PAGE_GUARD)) {
            DWORD *q;
            for (q = (DWORD *)start; (char *)(q + 1) <= end; q++)
                if (*q == value) {
                    push_handle(L, (DWORD)q);
                    lua_rawseti(L, -2, ++found);
                    if (found >= (int)maxn)
                        return 1;
                }
        }
        p = end;
    }
    return 1;
}

/* ---------------------------------------------------------------- losable battles
 * The hero's death ends a battle in the per-turn battle update (0x50B9A0): when the hero
 * is no longer alive it calls Defeat(scene, "scrYouAreDead.tga") at 0x50BA2D - Lua
 * TacticLevel:EndBattle(), the "You are Dead!" scene, game over. The battle-won path
 * (0x5118FA) runs the same preparation and then calls
 *   EndBattleWon(scene, controller + 0x160, controller + 0x188)   0x51CEB0, thiscall, ret 8
 * (Lua TacticLevel:EndBattle(), the TacticalEndVictory scene, back to the world).
 * Engine.LosableBattle(true) points the call at 0x50BA2D at defeat_thunk: while armed, a
 * hero death takes the battle-won path instead (esi = the controller in both places) and
 * disarms itself; otherwise the thunk jumps straight to Defeat. Companion deaths
 * (Eneya/Rafael/Frederic, 0x51CAC0) still lose the game.
 * The update can call Defeat again in the same battle (seen when several enemies struck an
 * already dead hero in one turn): the second call would run vanilla Defeat on a battle that
 * already ended - Lua EndBattle twice, then a crash (0x50CD4F). So while the last armed battle
 * has ended through the thunk (engine_lost, cleared by Engine.LosableDone() once the world is
 * back) a Defeat call on a scene whose state (ecx + 0x110: 2 lost, 3 won - set by Defeat
 * 0x519ABB / EndBattleWon 0x51CFE2) says "ended" is dropped. */
/* ------------------------------------------- the battle grid and the cursor (engine 8)
   Found 2026-11-01 (exe disassembly + live reads):
   - Hex grid: columns -C..C, rows -R..R; [0x6CA8BC] = C, [0x6CA8C0] = R,
     [0x6CA8C4] = 2C + 1, [0x6CA8C8] = table of 16-byte entries (float x, float z, ...);
     index = (R - row) * (2C + 1) + C + col (code at 0x4FCAD2).
   - The battle controller (vtable 0x6139C0, at +0xBC of the Lua battle scene object)
     intersects the mouse ray with the ground every frame: x at +0xA0, z at +0xA8
     (0x51017C). The point is not snapped to a hex.
   - [0x6CA8F0] draws the acting troop's movement area (green), [0x6CA8F4] the hovered
     troop's area (and Teleport's range); same layout:
     +0x40 effect, +0x64 array of 1600 hex tiles, +0x70 tiles used, +0x74 troop shown,
     +0x78 / +0x7C the red (enemy) / green (friend) effects. A tile: +0x58 x, +0x60 z,
     +0x64 byte shown (0x4FD920 fills it like this from a troop's area). The controller's
     +0x2A0 holds the Teleport effect.
   Every site is compared with the code before anything is read or written. */
#define GRID_COLS    0x6CA8BC
#define GRID_ROWS    0x6CA8C0
#define GRID_STRIDE  0x6CA8C4
#define GRID_TABLE   0x6CA8C8
#define AREA_OBJ     0x6CA8F4
#define MOVE_OBJ     0x6CA8F0
#define MOVE_VT      0x61479C
#define MOVE_MESH_VT 0x6147A4
#define AREA_VT      0x6148A8
#define AREA_MESH_VT 0x61483C
#define AREA_TILES   1600
#define CTRL_VT      0x6139C0
#define SCENE_CTRL   0x0BC

static int grid_state = -1;           /* -1 unchecked, 0 bad, 1 good */

static int grid_code_ok(void)
{
    static const short fill[] = { 0x8B, 0x15, 0xC8, 0xA8, 0x6C, 0x00 };            /* 0x4FD949 */
    static const short tx[] = { 0xF3, 0x0F, 0x11, 0x42, 0x58 };                     /* 0x4FD97E */
    static const short tz[] = { 0xF3, 0x0F, 0x11, 0x42, 0x60 };                     /* 0x4FD989 */
    static const short ton[] = { 0x88, 0x5A, 0x64 };                                /* 0x4FD98E */
    static const short idx[] = { 0x0F, 0xAF, 0x0D, 0xC4, 0xA8, 0x6C, 0x00,
                                 0x03, 0x0D, 0xBC, 0xA8, 0x6C, 0x00 };              /* 0x4FCADC */
    static const short rows[] = { 0x8B, 0x0D, 0xC0, 0xA8, 0x6C, 0x00 };             /* 0x4FCAD2 */
    static const short cz[] = { 0x8D, 0xAE, 0xA8, 0x00, 0x00, 0x00, 0x55,
                                0x8D, 0x9E, 0xA0, 0x00, 0x00, 0x00 };               /* 0x51017C */
    static const short area[] = { 0x8B, 0x0D, 0xF4, 0xA8, 0x6C, 0x00, 0x53, 0xE8 }; /* 0x511797 */
    static const short fx[] = { 0x8B, 0x4E, 0x78, 0x89, 0x46, 0x74, 0x83, 0xC0, 0x60,
                                0x89, 0x4E, 0x40 };                                 /* 0x523215 */
    static const short tp[] = { 0x8B, 0x86, 0xA0, 0x02, 0x00, 0x00 };               /* 0x50F215 */

    if (grid_state < 0)
        grid_state = match(0x4FD949, fill, 6) && match(0x4FD97E, tx, 5) && match(0x4FD989, tz, 5) &&
                     match(0x4FD98E, ton, 3) && match(0x4FCADC, idx, 13) && match(0x4FCAD2, rows, 6) &&
                     match(0x51017C, cz, 13) && match(0x511797, area, 8) && match(0x523215, fx, 12) &&
                     match(0x50F215, tp, 6);
    return grid_state;
}

/* number of hexes, and the table of their centres - 0 / NULL outside a battle */
static int grid_count(const float **table)
{
    int c, r, s;

    if (!grid_code_ok())
        return 0;
    c = *(int *)GRID_COLS;
    r = *(int *)GRID_ROWS;
    s = *(int *)GRID_STRIDE;
    if (c < 1 || c > 100 || r < 1 || r > 100 || s != 2 * c + 1 || s * (2 * r + 1) > AREA_TILES)
        return 0;
    *table = *(const float **)GRID_TABLE;
    if (!readable(*table, (size_t)s * (2 * r + 1) * 16))
        return 0;
    return s * (2 * r + 1);
}

/* Engine.HexCount() -> the number of hexes of the battle grid (nothing outside a battle).
   Hex indices are 0 .. count - 1. */
static int e_hex_count(lua_State *L)
{
    const float *t;
    int n = grid_count(&t);

    if (n == 0)
        return 0;
    lua_pushnumber(L, (lua_Number)n);
    return 1;
}

/* Engine.HexCenter(i) -> x, z of hex i */
static int e_hex_center(lua_State *L)
{
    const float *t;
    int n = grid_count(&t);
    lua_Number i;

    if (n == 0 || !arg_number(L, 1, &i) || i < 0 || (int)i >= n)
        return 0;
    lua_pushnumber(L, t[(int)i * 4]);
    lua_pushnumber(L, t[(int)i * 4 + 1]);
    return 2;
}

/* Engine.HexesNear(x, z, r) -> table { i, i, ... } of the hexes whose centre is within r,
   nearest first. Engine.HexesNear(x, z, 0) -> the one hex containing the point. */
static int e_hexes_near(lua_State *L)
{
    static int idx[AREA_TILES];
    static float dist[AREA_TILES];
    const float *t;
    int n = grid_count(&t), k = 0, i, j;
    lua_Number x, z, r;

    if (n == 0 || !arg_number(L, 1, &x) || !arg_number(L, 2, &z) || !arg_number(L, 3, &r) || r < 0)
        return 0;
    for (i = 0; i < n; i++) {
        float dx = t[i * 4] - x, dz = t[i * 4 + 1] - z, d = dx * dx + dz * dz;
        if (r == 0) {                                  /* the nearest centre = the hex hit */
            if (k == 0 || d < dist[0]) { idx[0] = i; dist[0] = d; k = 1; }
            continue;
        }
        if (d > r * r)
            continue;
        for (j = k; j > 0 && dist[j - 1] > d; j--) {   /* insertion by distance */
            idx[j] = idx[j - 1];
            dist[j] = dist[j - 1];
        }
        idx[j] = i;
        dist[j] = d;
        k++;
    }
    lua_createtable(L, k, 0);
    for (i = 0; i < k; i++) {
        lua_pushnumber(L, (lua_Number)idx[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static DWORD battle_ctrl(lua_State *L, int idx)
{
    DWORD scene = arg_handle(L, idx), c;

    if (!grid_code_ok() || scene == 0 || !readable((void *)(scene + SCENE_CTRL), 4))
        return 0;
    c = *(DWORD *)(scene + SCENE_CTRL);
    if (!readable((void *)c, 0x2A4) || *(DWORD *)c != CTRL_VT)
        return 0;
    return c;
}

/* Engine.BattleCursor(scene) -> x, z: where the mouse ray meets the ground this frame.
   scene = GUIScene.TacticLevel.scene. */
static int e_battle_cursor(lua_State *L)
{
    DWORD c = battle_ctrl(L, 1);

    if (c == 0)
        return 0;
    lua_pushnumber(L, *(float *)(c + 0xA0));
    lua_pushnumber(L, *(float *)(c + 0xA8));
    return 2;
}

/* The two area displays: [0x6CA8F0] draws the acting troop's movement area (green) and
   is drawn in every cursor mode; [0x6CA8F4] (the hovered troop's area) is neither updated
   nor drawn while a spell is armed, so the mod's hexes go into the first one. */
static DWORD area_obj(DWORD global, DWORD vt, DWORD mesh_vt)
{
    DWORD a = *(DWORD *)global, tiles;

    if (!readable((void *)a, 0x80) || *(DWORD *)a != vt || *(DWORD *)(a + 4) != mesh_vt)
        return 0;
    tiles = *(DWORD *)(a + 0x64);
    if (!readable((void *)tiles, AREA_TILES * 4))
        return 0;
    return a;
}

/* puts tile k of display a on hex h; 0 if the tile is not writable */
static int put_tile(DWORD a, int k, const float *t, int h)
{
    DWORD tile = ((DWORD *)*(DWORD *)(a + 0x64))[k];
    MEMORY_BASIC_INFORMATION mbi;

    if (!readable((void *)tile, 0x68) || VirtualQuery((void *)tile, &mbi, sizeof(mbi)) == 0 ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return 0;
    *(float *)(tile + 0x58) = t[h * 4];
    *(float *)(tile + 0x60) = t[h * 4 + 1];
    *(unsigned char *)(tile + 0x64) = 1;           /* "moved": the tile rebuilds itself */
    return 1;
}

/* Engine.ShowHexes(scene, style, i, i, ...) -> number shown. Draws the given hexes in place
   of the acting troop's green movement area: style 0 = red (an enemy's area), 1 = green,
   2 = Teleport's; the hovered troop's area is hidden meanwhile. Engine.ShowHexes(scene, -1)
   puts the troop's own movement area and the hover display back (as the engine draws them). The engine itself redraws the area when the next troop acts. */
static int e_show_hexes(lua_State *L)
{
    static const short refill[] = { 0x8B, 0x46, 0x64, 0x8B, 0x0D, 0xF0, 0xA8, 0x6C, 0x00,
                                    0x83, 0xC0, 0x60, 0x50, 0x83, 0xC1, 0x04, 0xE8 };  /* 0x508415 */
    const float *t;
    int n = grid_count(&t), top = lua_gettop(L), k = 0, i;
    DWORD c = battle_ctrl(L, 1), move, hover, fx;
    lua_Number style;

    if (n == 0 || c == 0 || !arg_number(L, 2, &style) || style < -1 || style > 2 || !match(0x508415, refill, 17))
        return 0;
    move = area_obj(MOVE_OBJ, MOVE_VT, MOVE_MESH_VT);
    hover = area_obj(AREA_OBJ, AREA_VT, AREA_MESH_VT);
    if (move == 0 || hover == 0)
        return 0;
    fx = style == 0 ? *(DWORD *)(hover + 0x78) : style == 2 ? *(DWORD *)(c + 0x2A0) : *(DWORD *)(hover + 0x7C);
    if (fx == 0 || !readable((void *)fx, 4))
        return 0;
    if (style < 0) {                               /* the current troop's own area */
        DWORD troop = *(DWORD *)(c + 0x64);
        const unsigned char *flags;
        int size;
        if (!readable((void *)troop, 0x6C))
            return 0;
        flags = *(const unsigned char **)(troop + 0x64);
        size = *(int *)(troop + 0x68);
        if (size < 0 || size > n || (size > 0 && !readable(flags, size)))
            return 0;
        for (i = 0; i < size && k < AREA_TILES; i++)
            if (flags[i] && put_tile(move, k, t, i))
                k++;
    } else {
        for (i = 3; i <= top && k < AREA_TILES; i++) {
            lua_Number h;
            if (!arg_number(L, i, &h) || h < 0 || (int)h >= n)
                continue;
            if (!put_tile(move, k, t, (int)h))
                break;
            k++;
        }
    }
    /* while the mod's hexes are up, the hovered troop's area (drawn over them in the same
       colours) is switched off with its own "shown" flag (+0x5C = its mesh's +0x58) */
    if (*(unsigned char *)(hover + 0x5C) <= 1)
        *(unsigned char *)(hover + 0x5C) = style < 0;
    *(DWORD *)(move + 0x40) = fx;
    *(int *)(move + 0x70) = k;
    lua_pushnumber(L, (lua_Number)k);
    return 1;
}

/* ------------------------------------- ground spells may be clicked on a unit (engine 8)
   The spell object's CanTargetUnit (vtable 0x61D37C slot +0x68 = 0x573400) refuses every
   unit for Type 4 (a point on the ground), so the player had to find bare ground. The per-frame
   cursor code (0x510282) asks it for the unit under the cursor; a yes gives the "valid target"
   cursor (state 4, the same as a valid ground point) and the click casts. The replacement says
   yes for Type 4 and stores the unit's position (unit +0x64 x, +0x6C z - the live position, as
   in the hero object) as the spell's point (+0x6C/+0x74, which the cast 0x5737A0 turns into
   SpellGroundInfo). Every other Type goes to the original. */
#define SPELL_VT_SLOT  0x61D3E4
#define CAN_TARGET_FN  0x573400

void unit_ground_thunk(void);
__asm__(
    ".text\n"
    ".globl _unit_ground_thunk\n"
    "_unit_ground_thunk:\n"
    "    cmpl $4, 0x64(%ecx)\n"
    "    jne 1f\n"
    "    movl 4(%esp), %eax\n"            /* the unit */
    "    testl %eax, %eax\n"
    "    jz 2f\n"
    "    movl %eax, 0x68(%ecx)\n"
    "    movl 0x64(%eax), %edx\n"
    "    movl %edx, 0x6C(%ecx)\n"
    "    movl $0, 0x70(%ecx)\n"
    "    movl 0x6C(%eax), %edx\n"
    "    movl %edx, 0x74(%ecx)\n"
    "    movl $1, %eax\n"
    "    ret $4\n"
    "2:  xorl %eax, %eax\n"
    "    ret $4\n"
    "1:  movl $0x573400, %eax\n"
    "    jmp *%eax\n");

static int unit_ground_patched = 0;

/* Engine.GroundSpellsOnUnits() -> 1 when installed (once per process; the DLL is pinned) */
static int e_ground_spells_on_units(lua_State *L)
{
    static const short orig[] = { 0x56, 0x8B, 0xF1, 0x6A, 0x00, 0x68, 0x60, 0xD3, 0x61, 0x00 };
    DWORD *slot = (DWORD *)SPELL_VT_SLOT, old;
    HMODULE self;

    if (!unit_ground_patched && readable(slot, 4) && *slot == CAN_TARGET_FN &&
        match(CAN_TARGET_FN, orig, 10) && match(0x51017C, (const short[]){ 0x8D, 0xAE, 0xA8 }, 3) &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCSTR)&e_ground_spells_on_units, &self) &&
        VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        *slot = (DWORD)&unit_ground_thunk;
        VirtualProtect(slot, 4, old, &old);
        unit_ground_patched = 1;
    }
    lua_pushnumber(L, (lua_Number)unit_ground_patched);
    return 1;
}

/* Engine.GroundSpellPoint(scene) -> x, z where the armed ground spell (Type 4) would land if
   the player clicked now, or nothing when no ground spell is armed. The controller keeps the
   armed spell at +0x1C0 (0x5104CE); the spell object (vtable 0x61D37C) stores the point it
   accepted at +0x6C/+0x74 - the ground under the cursor every frame (0x5733C0), or with
   GroundSpellsOnUnits the hovered unit's position. */
static int e_ground_spell_point(lua_State *L)
{
    static const short armed[] = { 0x8B, 0x8E, 0xC0, 0x01, 0x00, 0x00 };          /* 0x5104CE */
    static const short type4[] = { 0x83, 0x7E, 0x64, 0x04 };                      /* 0x5733D6 */
    static const short store[] = { 0x89, 0x4E, 0x6C };                            /* 0x5733E2 */
    DWORD c = battle_ctrl(L, 1), sp;

    if (c == 0 || !match(0x5104CE, armed, 6) || !match(0x5733D6, type4, 4) || !match(0x5733E2, store, 3))
        return 0;
    sp = *(DWORD *)(c + 0x1C0);
    if (sp == 0 || !readable((void *)sp, 0x7C) || *(DWORD *)sp != 0x61D37C || *(int *)(sp + 0x64) != 4)
        return 0;
    lua_pushnumber(L, *(float *)(sp + 0x6C));
    lua_pushnumber(L, *(float *)(sp + 0x74));
    return 2;
}

/* --------------------------------------------------------- swapping two units (engine 8)
   0x511B30 = the engine's own "put this unit on hex N" (thiscall, ret 4; Teleport's move ends
   in it): if the unit's hex (unit +0x2BC) still names it as occupant (hex entry +0xC), that hex
   is emptied (+0x9 = 1 free); then unit +0x2BC = N, hex N's occupant = the unit, +0x9 = 0, and
   the unit's position is set to the hex centre (0x4AC940). Teleport's animated move (0x511BA0 ->
   0x514650) picks the nearest FREE hex when the destination is taken, so a swap uses this one:
   B onto A's hex first (A's hex now names B, so moving A does not empty it), then A onto B's. */
typedef void (__attribute__((thiscall)) *place_fn)(void *unit, int hex);
#define PLACE_AT_HEX 0x511B30
#define U_HEX        0x2BC

/* Engine.SwapUnits(a, b) -> true when the two units have traded hexes */
static int e_swap_units(lua_State *L)
{
    static const short place[] = { 0xA1, 0xC8, 0xA8, 0x6C, 0x00, 0x56, 0x8B, 0xB1, 0xBC, 0x02, 0x00, 0x00,
                                   0xC1, 0xE6, 0x04, 0x33, 0xD2, 0x39, 0x4C, 0x06, 0x0C };
    static const short occupy[] = { 0x89, 0x48, 0x0C };                           /* 0x511B84 */
    DWORD a = arg_handle(L, 1), b = arg_handle(L, 2);
    const float *t;
    const DWORD *grid;
    int n = grid_count(&t), ha, hb;

    if (n == 0 || a == b || !valid_unit(a) || !valid_unit(b) ||
        !match(PLACE_AT_HEX, place, 21) || !match(0x511B84, occupy, 3))
        return 0;
    ha = *(int *)(a + U_HEX);
    hb = *(int *)(b + U_HEX);
    grid = (const DWORD *)t;                       /* same 16-byte entries; occupant at +0xC */
    if (ha < 0 || ha >= n || hb < 0 || hb >= n || ha == hb ||
        grid[ha * 4 + 3] != a || grid[hb * 4 + 3] != b)
        return 0;
    ((place_fn)PLACE_AT_HEX)((void *)b, ha);
    ((place_fn)PLACE_AT_HEX)((void *)a, hb);
    lua_pushboolean(L, 1);
    return 1;
}

#define DEFEAT_SITE 0x50BA2D
#define DEFEAT_FN   0x519990
#define WON_FN      0x51CEB0

int engine_losable = 0;               /* armed: the next hero death ends the battle as won */
int engine_lost = 0;                  /* the last armed battle ended through the thunk */
void defeat_thunk(void);
__asm__(
    ".text\n"
    ".globl _defeat_thunk\n"
    "_defeat_thunk:\n"
    "    cmpl $0, _engine_losable\n"
    "    jne 1f\n"
    "    cmpl $0, _engine_lost\n"         /* not armed: the battle the thunk ended, again? */
    "    je 2f\n"
    "    cmpl $2, 0x110(%ecx)\n"
    "    jl 2f\n"
    "    ret $4\n"                        /* already over: drop the call (and its argument) */
    "2:  movl $0x519990, %eax\n"          /* otherwise Defeat as before */
    "    jmp *%eax\n"
    "1:  movl $0, _engine_losable\n"
    "    movl $1, _engine_lost\n"
    "    popl %eax\n"                     /* return address */
    "    addl $4, %esp\n"                 /* Defeat's one argument (the picture name) */
    "    leal 0x188(%esi), %edx\n"
    "    pushl %edx\n"
    "    leal 0x160(%esi), %edx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    movl $0x51CEB0, %eax\n"          /* EndBattleWon pops both arguments */
    "    jmp *%eax\n");

static int defeat_patched = 0;

static int patch_defeat(void)
{
    unsigned char *site = (unsigned char *)DEFEAT_SITE;
    HMODULE self;
    DWORD old;
    LONG rel;

    if (defeat_patched)
        return 1;
    if (!readable(site, 5) || site[0] != 0xE8 ||
        (DWORD)(site + 5) + *(LONG *)(site + 1) != DEFEAT_FN)
        return 0;
    if (!readable((void *)WON_FN, 2) || ((unsigned char *)WON_FN)[0] != 0x6A || ((unsigned char *)WON_FN)[1] != 0xFF)
        return 0;
    /* the patched call must never point into an unloaded DLL (Lua unloads C libraries
       when the game replaces its state) */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)&patch_defeat, &self))
        return 0;
    rel = (LONG)((unsigned char *)&defeat_thunk - (site + 5));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(LONG *)(site + 1) = rel;
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    defeat_patched = 1;
    return 1;
}

/* Engine.LosableBattle(1) arms the next hero death (installs the patch the first time),
   Engine.LosableBattle(0) disarms. Returns 1 when armed, 0 when not (or the code at the
   call site is not what this exe build should have - then nothing is changed). */
static int e_losable_battle(lua_State *L)
{
    lua_Number on = 0;

    if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TNUMBER)
        on = lua_tonumber(L, 1);
    if (on != 0 && patch_defeat()) {
        engine_losable = 1;
        engine_lost = 0;
    } else {
        engine_losable = 0;
    }
    lua_pushnumber(L, (lua_Number)engine_losable);
    return 1;
}

/* Engine.LostBattle() -> 1 if the last armed battle ended with the hero's death, else 0 */
static int e_lost_battle(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)engine_lost);
    return 1;
}

/* Engine.LosableDone() - the battle is over and the world is back: forget the lost battle
   (from now on every Defeat call is a real one again) */
static int e_losable_done(lua_State *L)
{
    engine_lost = 0;
    lua_pushnumber(L, 1);
    return 1;
}

/* ---------------------------------------------------------------- shields (engine 9/10)
 * Every hit - blow, arrow, spell, poison - is stored on the unit as PENDING damage (+0x3DC) by
 * one of two setters, and applied later when the unit's reaction plays:
 *   0x511E10 (thiscall, ret 8) - the engine's own blows (called at 0x4FB9A5 with the result of
 *            the Lua CalcAndRetAttack; a negative result = a critical);
 *   0x512A10 - the Lua method SetSubHealth (spells, ModAPI.Battle.Hit).
 * Both clamp it to the unit's health. The reaction then takes one of two paths (PlayDefense
 * 0x513360: a delayed one through state 5 / 0x516CAC, or at once at 0x51347E when the delay is
 * shorter than the hit animation's lead) - engine 9 hooked only the delayed one, so spells
 * (SetSubHealth + PlayDefense) went past the shield. engine 10 hooks both SETTERS instead: the
 * moment the amount is known, the unit's damage factor (Curse of Frailty) multiplies it and its
 * shield takes what it can, so the animation, the number and the health lost all agree.
 * Healing (negative pending) is left alone. A pending hit replaced by a later one has still used
 * the shield (rare). */
#define PENDING_SET_A 0x511E34          /* ret 8 + int3 padding at the end of 0x511E10 */
#define PENDING_SET_B 0x512A8E          /* pop edi/esi, xor eax, pop ebx, ret 4 of 0x512A10 */
#define U_PENDING    0x3DC
#define MAX_SHIELDS  128

/* engine 13: two more factors per unit for item effects (tiered_items.lua), multiplied with
   factor so no mod overwrites another's: guard = every hit, spell_guard = only hits set through
   the Lua SetSubHealth path (spells, ModAPI.Battle.Hit) - the engine's own blows never go there. */
static struct { DWORD unit; int amount; float factor, guard, spell_guard; } shields[MAX_SHIELDS];   /* factors 0 = 1 */
static int shield_absorbed = 0;                  /* absorbed since Engine.ShieldAbsorbed() */
static int shield_patched = 0;

/* engine 10: a unit's entry may also carry a damage factor (Curse of Frailty): the hit is
   multiplied first (at most the unit's health), then the shield takes what it can. */
static float factor_of(float f) { return f > 0 ? f : 1.0f; }

void __cdecl shield_absorb_how(DWORD u, int spell)
{
    int i, pending, a, hp;
    float f;

    if (!readable((void *)(u + U_PENDING), 4))
        return;
    pending = *(int *)(u + U_PENDING);
    if (pending <= 0)
        return;
    for (i = 0; i < MAX_SHIELDS; i++) {
        if (shields[i].unit != u)
            continue;
        f = factor_of(shields[i].factor) * factor_of(shields[i].guard) *
            (spell ? factor_of(shields[i].spell_guard) : 1.0f);
        if (f != 1.0f) {
            pending = (int)ceilf((float)pending * f);
            hp = *(int *)(u + U_HEALTH);
            if (pending > hp) pending = hp;
            if (pending < 0) pending = 0;
            *(int *)(u + U_PENDING) = pending;
        }
        if (shields[i].amount > 0) {
            a = shields[i].amount < pending ? shields[i].amount : pending;
            *(int *)(u + U_PENDING) = pending - a;
            shields[i].amount -= a;
            shield_absorbed += a;
        }
        return;
    }
}

void __cdecl shield_absorb(DWORD u) { shield_absorb_how(u, 0); }
void __cdecl shield_absorb_spell(DWORD u) { shield_absorb_how(u, 1); }

/* the unit's entry, made if needed (-1 when the table is full) */
static int shield_slot(DWORD u)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_SHIELDS; i++) {
        if (shields[i].unit == u)
            return i;
        if (free_slot < 0 && shields[i].unit == 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        shields[free_slot].unit = u;
        shields[free_slot].amount = 0;
        shields[free_slot].factor = 0;
        shields[free_slot].guard = 0;
        shields[free_slot].spell_guard = 0;
    }
    return free_slot;
}

/* an entry with no shield and no factor is free again */
static void shield_tidy(int i)
{
    if (shields[i].amount <= 0 && factor_of(shields[i].factor) == 1.0f && factor_of(shields[i].guard) == 1.0f &&
        factor_of(shields[i].spell_guard) == 1.0f)
        shields[i].unit = 0;
}

/* engine 11: the last blow the engine dealt (0x511E10 = the engine's own blows only, never a
   spell or ModAPI.Battle.Hit): its target, the amount set and whether it was a critical (the
   setter's 2nd argument, also kept at unit +0x3E0). Engine.LastBlow() reads it, so a mod can see
   "a blow landed" with one call instead of watching damage calculations (CLEAVE). */
static volatile DWORD blow_serial = 0, blow_unit = 0;
static volatile int blow_amount = 0, blow_critical = 0;

void __cdecl blow_record(DWORD u)
{
    if (!readable((void *)(u + U_PENDING), 8))
        return;
    blow_unit = u;
    blow_amount = *(int *)(u + U_PENDING);
    blow_critical = *(unsigned char *)(u + 0x3E0) != 0;
    blow_serial++;
}

void pending_thunk_a(void);
void pending_thunk_b(void);
__asm__(
    ".text\n"
    ".globl _pending_thunk_a\n"
    "_pending_thunk_a:\n"                 /* ecx = the unit (thiscall, unchanged) */
    "    pushal\n"
    "    pushl %ecx\n"
    "    call _blow_record\n"
    "    call _shield_absorb\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    ret $8\n"
    ".globl _pending_thunk_b\n"
    "_pending_thunk_b:\n"                 /* esi = the unit until the epilogue below */
    "    pushal\n"
    "    pushl %esi\n"
    "    call _shield_absorb_spell\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popl %edi\n"                     /* the epilogue the jump replaced */
    "    popl %esi\n"
    "    xorl %eax, %eax\n"
    "    popl %ebx\n"
    "    ret $4\n");

static int write_jump(unsigned char *site, void *to, int len)
{
    DWORD old;
    int i;

    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    site[0] = 0xE9;
    *(LONG *)(site + 1) = (LONG)((unsigned char *)to - (site + 5));
    for (i = 5; i < len; i++)
        site[i] = 0x90;
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

static int patch_shield(void)
{
    static const unsigned char want_a[5] = { 0xC2, 0x08, 0x00, 0xCC, 0xCC };
    static const unsigned char want_b[8] = { 0x5F, 0x5E, 0x33, 0xC0, 0x5B, 0xC2, 0x04, 0x00 };
    unsigned char *a = (unsigned char *)PENDING_SET_A, *b = (unsigned char *)PENDING_SET_B;
    HMODULE self;

    if (shield_patched)
        return 1;
    /* each site checked, with the instruction that stores the pending damage before it */
    if (!readable(a - 0x0E, 0x0E + 5) || memcmp(a, want_a, 5) != 0 ||
        a[-0x0E] != 0x89 || a[-0x0D] != 0x91 || a[-0x0C] != 0xDC || a[-0x0B] != 0x03)
        return 0;
    if (!readable(b - 0x14, 0x14 + 8) || memcmp(b, want_b, 8) != 0 ||
        b[-0x14] != 0x89 || b[-0x13] != 0x86 || b[-0x12] != 0xDC || b[-0x11] != 0x03)
        return 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)&patch_shield, &self))
        return 0;
    if (!write_jump(a, (void *)&pending_thunk_a, 5) || !write_jump(b, (void *)&pending_thunk_b, 8))
        return 0;
    shield_patched = 1;
    return 1;
}

/* Engine.SetShield(u, amount) -> amount kept (nothing if not a unit or the code at the
   patch site is not this build's). The next hits on u lose up to amount between them.
   0 removes the shield. */
static int e_set_shield(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number amount;
    int i, n;

    if (!valid_unit(u) || !arg_number(L, 2, &amount) || !patch_shield())
        return 0;
    n = (int)amount;
    if (n < 0) n = 0;
    if (n > 1000000) n = 1000000;
    i = shield_slot(u);
    if (i < 0)
        return 0;
    shields[i].amount = n;
    shield_tidy(i);
    lua_pushnumber(L, (lua_Number)n);
    return 1;
}

/* Engine.SetDamageFactor(u, f) -> f kept: every later hit on u is multiplied by f (0.01..10,
   at most the unit's health), before its shield. 1 removes it. */
static int e_set_damage_factor(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number f;
    int i;

    if (!valid_unit(u) || !arg_number(L, 2, &f) || !patch_shield())
        return 0;
    if (f < 0.01f) f = 0.01f;
    if (f > 10.0f) f = 10.0f;
    i = shield_slot(u);
    if (i < 0)
        return 0;
    shields[i].factor = (float)f;
    shield_tidy(i);
    lua_pushnumber(L, f);
    return 1;
}

/* engine 13: Engine.SetGuard(u, all, spells) -> true: item factors on every later hit on u (all)
   and on spell hits only (spells), each 0.01..10, multiplied with SetDamageFactor's; 1 = none. */
static int e_set_guard(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    lua_Number all, spells;
    int i;

    if (!valid_unit(u) || !arg_number(L, 2, &all) || !arg_number(L, 3, &spells) || !patch_shield())
        return 0;
    if (all < 0.01f) all = 0.01f;
    if (all > 10.0f) all = 10.0f;
    if (spells < 0.01f) spells = 0.01f;
    if (spells > 10.0f) spells = 10.0f;
    i = shield_slot(u);
    if (i < 0)
        return 0;
    shields[i].guard = (float)all;
    shields[i].spell_guard = (float)spells;
    shield_tidy(i);
    lua_pushboolean(L, 1);
    return 1;
}

/* Engine.Shield(u) -> what is left of u's shield (0 if none) */
static int e_shield(lua_State *L)
{
    DWORD u = arg_handle(L, 1);
    int i;

    if (u == 0)
        return 0;
    for (i = 0; i < MAX_SHIELDS; i++)
        if (shields[i].unit == u) {
            lua_pushnumber(L, (lua_Number)(shields[i].amount > 0 ? shields[i].amount : 0));
            return 1;
        }   /* (an entry may hold only a damage factor) */
    lua_pushnumber(L, 0);
    return 1;
}

/* Engine.ClearShields() - a new battle: every shield goes */
static int e_clear_shields(lua_State *L)
{
    memset(shields, 0, sizeof(shields));
    shield_absorbed = 0;
    lua_pushnumber(L, 1);
    return 1;
}

/* Engine.LastBlow() -> serial, unit, amount, critical (serial 0 = no blow yet). The serial
   grows with every blow the engine deals; the first call puts the hook in place. */
static int e_last_blow(lua_State *L)
{
    if (!patch_shield())
        return 0;
    lua_pushnumber(L, (lua_Number)(blow_serial & 0xFFFFF));   /* small: Lua numbers are floats */
    if (blow_unit == 0)
        return 1;
    push_handle(L, blow_unit);
    lua_pushnumber(L, (lua_Number)blow_amount);
    lua_pushboolean(L, blow_critical);
    return 4;
}

/* Engine.ShieldAbsorbed() -> damage the shields took since the last call */
static int e_shield_absorbed(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)shield_absorbed);
    shield_absorbed = 0;
    return 1;
}

/* ------------------------------------------------- spell classes (engine 12)
   The battle makes the C++ object of an armed spell through a registry: 0x575400 (thiscall,
   arg std::string name) looks name.substr(0, 5) up in a std::map<string, factory> at registry
   +4. The registry's constructor 0x5754B0 fills it, one block per vanilla prefix:
   push <"spW03">, ..., call 0x5542E0 (map::operator[], thiscall, arg string*, returns the value
   slot), ..., mov dword [eax], <factory>. A prefix not in the map gets the instant-spell factory
   0x574ED0 - so a mod spell slot (spX15_ModSpell, ...) never lasts: no BeginUnitMain turns, no
   QnTurn countdown, no Delete. Poison, Hermes, Armor ... share the lasting factory 0x574FF0.
   The registry is [0x6CA52C], made on demand by 0x4C73E0 (no arguments, returns it) and freed
   when the battle ends (0x50ABAC), so an added prefix has to be added in every battle. */
#define REG_PTR      0x6CA52C
#define REG_GET      0x4C73E0
#define REG_VTABLE   0x61D724
#define MAP_INDEX    0x5542E0
#define REG_CTOR_LO  0x5754F0
#define REG_CTOR_HI  0x575A10

typedef struct {                /* MSVC 8 std::string (as in resources.c) */
    DWORD alval;
    union { char buf[16]; const char *ptr; } bx;
    DWORD size;
    DWORD res;
} MsString;

typedef DWORD (*reg_get_fn)(void);
typedef DWORD *(__attribute__((thiscall)) *map_index_fn)(void *map, const MsString *key);

/* the factory the registry's constructor files a vanilla prefix ("spW03") under, or 0 */
static DWORD class_factory(const char *like)
{
    DWORD a, b;

    if (!readable((void *)REG_CTOR_LO, REG_CTOR_HI - REG_CTOR_LO))
        return 0;
    for (a = REG_CTOR_LO; a + 0x40 < REG_CTOR_HI; a++) {
        const unsigned char *p = (const unsigned char *)a;
        const char *s;

        if (p[0] != 0x68)
            continue;
        s = (const char *)*(const DWORD *)(p + 1);
        if (!readable(s, 6) || memcmp(s, like, 5) != 0 || s[5] != 0)
            continue;
        for (b = a + 5; b < a + 0x30; b++) {
            const unsigned char *q = (const unsigned char *)b;
            DWORD f;

            if (q[0] != 0xE8 || b + 5 + *(const int *)(q + 1) != MAP_INDEX)
                continue;
            for (q += 5; q < (const unsigned char *)b + 20; q++)
                if (q[0] == 0xC7 && q[1] == 0x00) {
                    f = *(const DWORD *)(q + 2);
                    return is_code(f) ? f : 0;
                }
            return 0;
        }
    }
    return 0;
}

/* Engine.SpellClass(name, like) -> true: in this battle a spell whose name starts with the
   5 letters of name is made like the vanilla spell starting with like ("spW03" = Poison:
   lasting, BeginUnitMain every turn, QnTurn, Delete). Only during a battle. */
static int e_spell_class(lua_State *L)
{
    static const short getter[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, ANY, ANY, ANY, ANY,
                                    0x50, 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA1, 0x2C, 0xA5, 0x6C, 0x00 };
    static const short index[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, ANY, ANY, ANY, ANY,
                                   0x50, 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x28 };
    const char *name = lua_tolstring(L, 1, NULL), *like = lua_tolstring(L, 2, NULL);
    DWORD factory, reg, *slot;
    MsString key;

    if (name == NULL || like == NULL || strlen(name) < 5 || strlen(like) < 5 ||
        !match(REG_GET, getter, sizeof(getter) / sizeof(getter[0])) ||
        !match(MAP_INDEX, index, sizeof(index) / sizeof(index[0])))
        return 0;
    factory = class_factory(like);
    if (factory == 0)
        return 0;
    reg = ((reg_get_fn)REG_GET)();
    if (reg == 0 || !readable((void *)reg, 0x14) || *(DWORD *)reg != REG_VTABLE)
        return 0;
    memset(&key, 0, sizeof(key));
    memcpy(key.bx.buf, name, 5);
    key.size = 5;
    key.res = 15;
    slot = ((map_index_fn)MAP_INDEX)((void *)(reg + 4), &key);
    if (slot == NULL || !readable(slot, 4))
        return 0;
    *slot = factory;
    lua_pushboolean(L, 1);
    return 1;
}

typedef struct { const char *name; lua_CFunction fn; } Reg;

static const Reg FUNCS[] = {
    { "Version", e_version },
    { "Attach", e_attach },
    { "Units", e_units },
    { "UnitsNear", e_units_near },
    { "UnitName", e_unit_name },
    { "UnitHealth", e_unit_health },
    { "UnitSide", e_unit_side },
    { "UnitPosition", e_unit_position },
    { "UnitStats", e_unit_stats },
    { "DamageUnit", e_damage_unit },
    { "HitUnit", e_hit_unit },
    { "PreviewDamage", e_preview_damage },
    { "HealthBarShown", e_health_bar_shown },
    { "ShowHealthBar", e_show_health_bar },
    { "LosableBattle", e_losable_battle },
    { "LostBattle", e_lost_battle },
    { "LosableDone", e_losable_done },
    { "HexCount", e_hex_count },
    { "HexCenter", e_hex_center },
    { "HexesNear", e_hexes_near },
    { "BattleCursor", e_battle_cursor },
    { "ShowHexes", e_show_hexes },
    { "GroundSpellsOnUnits", e_ground_spells_on_units },
    { "GroundSpellPoint", e_ground_spell_point },
    { "SwapUnits", e_swap_units },
    { "SetShield", e_set_shield },
    { "Shield", e_shield },
    { "ClearShields", e_clear_shields },
    { "ShieldAbsorbed", e_shield_absorbed },
    { "SetDamageFactor", e_set_damage_factor },
    { "SetGuard", e_set_guard },
    { "LastBlow", e_last_blow },
    { "SpellClass", e_spell_class },
};

static const Reg DEBUG_FUNCS[] = {
    { "ReadU32", d_read_u32 },
    { "ReadInt", d_read_int },
    { "ReadFloat", d_read_float },
    { "ReadString", d_read_string },
    { "Handle", d_handle },
    { "FindDword", d_find_dword },
};

static void register_all(lua_State *L, const Reg *r, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        lua_pushcclosure(L, r[i].fn, 0);
        lua_setfield(L, -2, r[i].name);
    }
}

/* Returns the Engine table, or nothing if this exe is not recognised. */
__declspec(dllexport) int engine_open(lua_State *L)
{
    if (!bind_api())
        return 0;
    lua_createtable(L, 0, 12);
    register_all(L, FUNCS, sizeof(FUNCS) / sizeof(FUNCS[0]));
    lua_createtable(L, 0, 6);
    register_all(L, DEBUG_FUNCS, sizeof(DEBUG_FUNCS) / sizeof(DEBUG_FUNCS[0]));
    lua_setfield(L, -2, "Debug");
    return 1;
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

/* Why engine_open returned nothing (a plain C string; for the log). Safe to
   call without the Lua API: returns nothing, writes the reason to a file. */
__declspec(dllexport) int engine_why(lua_State *L)
{
    FILE *f;

    char path[MAX_PATH];

    (void)L;
    bind_api();
    beside_dll(path, "engine_status.txt");
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "%s\n", api_state == 1 ? "ok" : api_error);
        fclose(f);
    }
    return 0;
}
