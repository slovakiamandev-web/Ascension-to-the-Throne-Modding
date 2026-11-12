/*
 * resources.dll - lets mods replace or edit any file the engine reads from the paks.
 *
 * The engine opens every resource through ResourceManager::Find(const std::string &name)
 * (0x4514B0, thiscall, ret 4). Find has its own disk path: a name containing ':' (an
 * absolute path) is opened from disk with an ifstream instead of the pak, with the file's
 * real size. A detour on Find swaps the name for the absolute path of a replacement file,
 * so the engine loads it in place of the pak's - nothing else in the engine changes.
 * A few loaders (forest .fst layouts) ask the pak manager directly instead (Info 0x576A90 +
 * Read 0x576820); both are detoured the same way (see info_hook / read_hook).
 *
 * Two kinds of replacement, both written by ModAPI (see ModAPI.ReplaceFile / ModAPI.Terrain):
 *  - Mods/resources_map.txt   "<resource name>|<path>"  - a mod's own file (any size;
 *                             relative paths are relative to the game folder).
 *  - Mods/terrain_edits.txt   terrain edits, built into a copy of the original file the
 *                             moment the engine asks for it (Mods/_cache/<name>):
 *       raise <continent> <x> <z> <radius> <height>
 *           smooth bump (negative = dip) on the vertices of Continent<N>_QuadTree.dat
 *           (the drawn ground: vertices, normals, node boxes); Continent<N>_HeightMap.nmph
 *           (walking height) gets the same vertex changes spread over the drawn triangles
 *       flat <continent> <height> [<walk height>]
 *           every drawn vertex at one height (normals up, node boxes); the walking height
 *           follows the vertex changes (the tesselation's fine detail stays); raises on top.
 *           With a walk height, every point of the walking grid is set to exactly that height
 *           instead - for a land whose real floor is a model (the drawn ground, with its few
 *           units of detail, then lies out of sight below it)
 *       paint <continent> <layer> <x> <z> <radius> <amount 0..1>
 *           blends the alpha of maskcont<N><layer>.dds (roads, sand, stone, pass)
 *           towards amount (0 erases)
 *       grid <continent> <file>
 *           our own height map for an area: every drawn vertex inside it is set to the grid's
 *           height there (bilinear), its normal to the grid's slope; the walking height follows
 *           over the drawn triangles, as for raise. Applied after flat (inside the grid it wins),
 *           raises on top. File (little-endian): "HGRD", float x0, z0, cell, u32 cols, rows,
 *           then rows x cols floats, row j at z0 + j * cell, column i at x0 + i * cell.
 *    The original is the mod replacement if there is one, else the pak's record.
 *
 *  - Mods/file_edits.txt      other files made from the game's own when the engine asks for them
 *                             (Mods/_cache/<name>), one edit per line, fields split by '|':
 *       from|<file>|<source>                   <file> is made from <source> (default: itself)
 *       set|<file>|<key>|<value>               text: the first line "<key> = ...;" gets <value>
 *       text|<file>|<old>|<new>                text: every <old> becomes <new>
 *       stage|<file>|<texture>|<field>|<value> effect (.ef): in each stage "Texture[k] = <texture>;"
 *                                              the first "<field>[k] = ...;" gets <value>
 *       clear|<file>|<x>|<z>|<radius>          forest (.fst): trees inside the circle left out
 *       speed|<file>|<factor>                  animation (.ska) or its root motion (.trk) played
 *                                              <factor> times faster (frames resampled)
 *       reverse|<file>                         animation / root motion played backwards
 *       smooth|<file>|<a>|<b>|<sway>|<surge>   root motion (.trk): the steps of frames a..b-1 pulled
 *                                              towards their average (keep sway / surge of the rest)
 *       gap|<file>|<contour>|<new>|<i1>|<t1>|<i2>|<t2>|<i3>|<t3>|<i4>|<t4>
 *       detour|<file>|<contour>|<after>|<at x>|<at z>|<x>|<z>|...
 *                                              collision model (.gdl): a walkable gap / pocket in
 *                                              the invisible walls (as tools/make_collision_edits.py)
 *    Built once per run, so no mod ships a changed game file and a changed mod is picked up the
 *    next time it loads.
 *
 *  - Mods/resources_alias.txt "<from> <to>" substrings (e.g. "continent4 continent3"): a name
 *                             no pak has is served from the pak record with <to> in its place;
 *                             aliased .ef files get the rules reversed inside. Also applied to
 *                             scripts: a detour on ScriptManager::RunScript (0x432620).
 * New continents (> 3) also need two engine name checks widened: the import slots of MSVCP80's
 * string operator!= / operator== are pointed at wrappers that only differ for the reflection
 * quadtree and "Continent3" literals (see str_ne_hook / str_eq_hook).
 *
 * Exports (no Lua arguments; call through package.loadlib):
 *   rs_install  check the code at Find, pin this DLL, install the detour (once per process)
 *   rs_reload   re-read both files (call after changing them)
 *   rs_list_mods  the folders in Mods/ -> mod_folders.txt next to this DLL (for ModAPI.LoadMods)
 * Status goes to Mods/resources.log; every requested name too while
 * Mods/resources_lognames.txt exists.
 *
 * Build (32-bit, like the game):  gcc -shared -O2 -Wall -static-libgcc -o resources.dll resources.c
 */
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

#define FIND_ADDR  0x4514B0
#define MAX_MAP    512
#define MAX_EDITS  256

/* MSVC 8 std::string: allocator, 16-byte buffer or pointer, size, capacity. */
typedef struct {
    DWORD alval;
    union { char buf[16]; const char *ptr; } bx;
    DWORD size;
    DWORD res;
} MsString;

typedef void *(__fastcall *FindFn)(void *self, void *edx, const MsString *name);

static const unsigned char FIND_PROLOGUE[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68 };
static unsigned char trampoline[16];
static FindFn original_find;

typedef struct { char name[128]; char path[MAX_PATH]; } MapEntry;
typedef struct {
    int paint;                  /* 0 = raise, 1 = paint, 2 = flat, 3 = grid */
    int continent;
    char layer[16];
    float x, z, r, v;           /* v = height (raise, flat) or amount (paint); flat: x = walk height
                                   when z is 1; grid: x, z = first point, r = cell */
    int cols, rows;             /* grid */
    float *g;                   /* grid heights (malloc'd, freed on reload) */
} Edit;

static MapEntry map[MAX_MAP];
static int nmap;
static Edit edits[MAX_EDITS];
static int nedits;
static CRITICAL_SECTION lock;
static int log_names;
static char folder[MAX_PATH];   /* the Mods folder, with a trailing backslash */

/* ------------------------------------------------------------------ helpers */

static void mods_path(char *out, const char *name)
{
    strcpy(out, folder);
    strcat(out, name);
}

static void logmsg(const char *fmt, ...)
{
    char path[MAX_PATH];
    FILE *f;
    va_list ap;

    mods_path(path, "resources.log");
    f = fopen(path, "a");
    if (f == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const char *ms_text(const MsString *s)
{
    return s->res >= 16 ? s->bx.ptr : s->bx.buf;
}

static unsigned char *read_whole(const char *path, DWORD *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc(n > 0 ? n : 1);
    if (data != NULL && fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (DWORD)n;
    return data;
}

/* A record of the game's paks (in the game folder), read straight from the pak. The game loads
 * Resource0.pak, Resource1.pak, ... until a number is missing, and a later pak wins - so they are
 * searched from the last one down (the Polish version has a Resource2.pak of its own). */
static unsigned char *read_from_paks(const char *name, DWORD *size)
{
    char pak[32];
    int k, last = -1;

    for (k = 0; k < 16; k++) {
        sprintf(pak, "Resource%d.pak", k);
        if (GetFileAttributesA(pak) == INVALID_FILE_ATTRIBUTES)
            break;
        last = k;
    }
    for (k = last; k >= 0; k--) {
        FILE *f;
        unsigned char head[12], rec[0x88];
        DWORD n, i;
        sprintf(pak, "Resource%d.pak", k);
        f = fopen(pak, "rb");
        if (f == NULL)
            continue;
        if (fread(head, 1, 12, f) != 12 || memcmp(head, "GRES", 4) != 0) {
            fclose(f);
            continue;
        }
        n = *(DWORD *)(head + 8);
        for (i = 0; i < n && fread(rec, 1, 0x88, f) == 0x88; i++) {
            rec[0x7F] = 0;
            if (_stricmp((char *)rec, name) == 0) {
                DWORD off = *(DWORD *)(rec + 0x80), len = *(DWORD *)(rec + 0x84);
                unsigned char *data = (unsigned char *)malloc(len ? len : 1);
                if (data != NULL && (fseek(f, (long)off, SEEK_SET) != 0 || fread(data, 1, len, f) != len)) {
                    free(data);
                    data = NULL;
                }
                fclose(f);
                *size = len;
                return data;
            }
        }
        fclose(f);
    }
    return NULL;
}

/* Alias rules (Mods/resources_alias.txt, "<from> <to>" per line): a requested name that
 * contains <from> (any case) and is not a mod replacement is served from the pak record
 * with <to> in its place - e.g. "continent4 continent3" + "cont4 cont3" makes a
 * continent 4 out of continent 3's files. */
#define MAX_ALIAS 16
static struct { char from[32], to[32]; } alias[MAX_ALIAS];
static int nalias;

/* The continent number in a rule's <from>: its first run of digits ("continent5", "cont5",
   "continent5reflection" -> "5"), "" if none. Static buffer - copy it before the next call. */
static const char *rule_number(const char *from)
{
    static char num[16];
    size_t n = 0;
    while (*from && !isdigit((unsigned char)*from))
        from++;
    while (isdigit((unsigned char)*from) && n < sizeof(num) - 1)
        num[n++] = *from++;
    num[n] = 0;
    return num;
}

static int matched_rule = -1;   /* the rule apply_alias used last */

/* name with the first matching alias applied, into out; 0 if no rule matches */
static int apply_alias(const char *name, char *out, size_t outsize)
{
    char low[160];
    int k, i;

    if (strlen(name) >= sizeof(low))
        return 0;
    for (i = 0; name[i]; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';
    for (k = 0; k < nalias; k++) {
        const char *hit = strstr(low, alias[k].from);
        size_t at, lf = strlen(alias[k].from);
        if (hit == NULL)
            continue;
        at = (size_t)(hit - low);
        if (at + strlen(alias[k].to) + strlen(name + at + lf) + 1 > outsize)
            return 0;
        memcpy(out, name, at);
        strcpy(out + at, alias[k].to);
        strcat(out, name + at + lf);
        matched_rule = k;
        return 1;
    }
    return 0;
}

/* The file a resource name currently stands for: a mod replacement, else the pak's,
 * else (alias rules) the pak record it is an alias of. */
static unsigned char *read_original(const char *name, DWORD *size)
{
    char other[160];
    unsigned char *data;
    int i;

    for (i = 0; i < nmap; i++)
        if (_stricmp(map[i].name, name) == 0)
            return read_whole(map[i].path, size);
    data = read_from_paks(name, size);
    if (data == NULL && apply_alias(name, other, sizeof(other)))
        data = read_from_paks(other, size);
    return data;
}

/* Effect files (.ef, text) name their continent's resources ("Continent3", "MaskCont3Roads",
 * "gContinent3") and the engine takes the continent's identity from them - an unchanged
 * copy made continent 4 call itself continent 3, and the game travelled back there. So an
 * aliased .ef gets every rule applied in reverse (<to> -> <from>, keeping the letter case);
 * the new names are then served through the same rules. Returns a new buffer.
 * Only the rules of the continent the file was asked for (number) are used: two added continents
 * on the same base (4 and 5 on 3) both have a "-> continent3" rule, and the first one used to win,
 * so continent 5's .ef named continent 4 and the engine loaded continent 4's ground. */
static unsigned char *rename_inside(const unsigned char *data, DWORD size, DWORD *outsize, const char *number)
{
    unsigned char *out = (unsigned char *)malloc(size * 2 + 64);
    DWORD i = 0, o = 0;
    int k;

    if (out == NULL)
        return NULL;
    while (i < size) {
        int done = 0;
        for (k = 0; k < nalias && !done; k++) {
            size_t lt = strlen(alias[k].to), lf = strlen(alias[k].from), c;
            if (number[0] && strcmp(rule_number(alias[k].from), number) != 0)
                continue;
            if (lt == 0 || i + lt > size || _strnicmp((const char *)data + i, alias[k].to, lt) != 0)
                continue;
            for (c = 0; c < lf; c++) {
                char ch = alias[k].from[c];
                /* the case of the text it replaces, letter by letter where they overlap */
                if (c < lt && isalpha(data[i + c]) && isalpha((unsigned char)ch))
                    ch = isupper(data[i + c]) ? (char)toupper((unsigned char)ch) : (char)tolower((unsigned char)ch);
                out[o++] = (unsigned char)ch;
            }
            i += (DWORD)lt;
            done = 1;
        }
        if (!done)
            out[o++] = data[i++];
        if (o + 64 > size * 2 + 64)
            break;
    }
    *outsize = o;
    return out;
}

/* A name only an alias rule can supply: copy the aliased pak record into Mods/_cache. */
static int build_alias(const char *name, char *out)
{
    char other[160], cache[MAX_PATH], dir[MAX_PATH];
    unsigned char *data;
    DWORD size;
    FILE *f;

    if (nalias == 0 || !apply_alias(name, other, sizeof(other)))
        return 0;
    mods_path(cache, "_cache\\");
    strcat(cache, name);
    {
        /* Mods/_cache/<name>.src = "<source>|<continent>" of the copy: the copies are kept from one
           game start to the next, and a changed rule (a continent added on another base, a second
           continent on the same base) must not reuse a copy made for the old one */
        char key[200], old[200] = "", src[MAX_PATH];
        FILE *sf;
        sprintf(key, "%s|%s", other, matched_rule >= 0 ? rule_number(alias[matched_rule].from) : "");
        strcpy(src, cache);
        strcat(src, ".src");
        sf = fopen(src, "rb");
        if (sf != NULL) {
            size_t got = fread(old, 1, sizeof(old) - 1, sf);
            old[got] = '\0';
            fclose(sf);
        }
        if (strcmp(old, key) != 0) {
            DeleteFileA(cache);
            sf = fopen(src, "wb");
            if (sf != NULL) {
                fwrite(key, 1, strlen(key), sf);
                fclose(sf);
            }
        }
    }
    if (GetFileAttributesA(cache) == INVALID_FILE_ATTRIBUTES) {
        if ((data = read_from_paks(name, &size)) != NULL) {      /* the pak has it after all */
            free(data);
            return 0;
        }
        data = read_from_paks(other, &size);
        if (data == NULL)
            return 0;
        if (strlen(name) > 3 && _stricmp(name + strlen(name) - 3, ".ef") == 0) {
            DWORD n;
            char number[16] = "";
            unsigned char *renamed;
            if (matched_rule >= 0 && matched_rule < nalias)
                strncpy(number, rule_number(alias[matched_rule].from), sizeof(number) - 1);
            renamed = rename_inside(data, size, &n, number);
            if (renamed != NULL) {
                free(data);
                data = renamed;
                size = n;
            }
        }
        mods_path(dir, "_cache");
        CreateDirectoryA(dir, NULL);
        f = fopen(cache, "wb");
        if (f == NULL || fwrite(data, 1, size, f) != size) {
            if (f) fclose(f);
            free(data);
            return 0;
        }
        fclose(f);
        free(data);
        logmsg("alias %s = %s", name, other);
    }
    strcpy(out, cache);
    return 1;
}

/* ------------------------------------------------------------- terrain edits */

/* Height added at (x, z) by raise edit e, and its gradient. */
static float bump(const Edit *e, float x, float z, float *gx, float *gz)
{
    float dx = x - e->x, dz = z - e->z, r = sqrtf(dx * dx + dz * dz), t, g;

    *gx = *gz = 0.0f;
    if (r >= e->r)
        return 0.0f;
    t = 3.14159265f * r / e->r;
    if (r > 1e-4f) {
        g = -e->v * 0.5f * sinf(t) * 3.14159265f / e->r;
        *gx = g * dx / r;
        *gz = g * dz / r;
    }
    return e->v * (cosf(t) * 0.5f + 0.5f);
}

static DWORD quadtree_vertices(const unsigned char *data, DWORD size, DWORD *count);

/* Height of the grid edits of the continent at (x, z) and its slope; 0 if (x, z) lies
   outside every grid (the last grid holding the point wins). */
static int grid_at(int continent, float x, float z, float *h, float *gx, float *gz)
{
    int k, found = 0;
    for (k = 0; k < nedits; k++) {
        const Edit *e = &edits[k];
        float fx, fz, tx, tz, h00, h10, h01, h11;
        int i, j;
        if (e->paint != 3 || e->continent != continent || e->g == NULL)
            continue;
        fx = (x - e->x) / e->r;
        fz = (z - e->z) / e->r;
        if (fx < 0.0f || fz < 0.0f || fx > (float)(e->cols - 1) || fz > (float)(e->rows - 1))
            continue;
        i = (int)fx;
        j = (int)fz;
        if (i >= e->cols - 1) i = e->cols - 2;
        if (j >= e->rows - 1) j = e->rows - 2;
        tx = fx - i;
        tz = fz - j;
        h00 = e->g[j * e->cols + i];
        h10 = e->g[j * e->cols + i + 1];
        h01 = e->g[(j + 1) * e->cols + i];
        h11 = e->g[(j + 1) * e->cols + i + 1];
        *h = (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
        *gx = ((h10 - h00) * (1 - tz) + (h11 - h01) * tz) / e->r;
        *gz = ((h01 - h00) * (1 - tx) + (h11 - h10) * tx) / e->r;
        found = 1;
    }
    return found;
}

/* The height a "flat" edit sets for the continent; 0 if there is none. */
static int flat_height(int continent, float *h)
{
    int k, found = 0;
    for (k = 0; k < nedits; k++)
        if (edits[k].paint == 2 && edits[k].continent == continent) {
            *h = edits[k].v;
            found = 1;
        }
    return found;
}

/* The exact walking height a "flat" edit sets for the continent; 0 if it sets none. */
static int flat_walk(int continent, float *walk)
{
    int k, found = 0;
    for (k = 0; k < nedits; k++)
        if (edits[k].paint == 2 && edits[k].continent == continent) {
            found = edits[k].z == 1.0f;
            *walk = edits[k].x;
        }
    return found;
}

/* Total height change of all raise edits at (x, z). */
static float raise_at(int continent, float x, float z)
{
    float h = 0.0f, gx, gz;
    int k;

    for (k = 0; k < nedits; k++)
        if (!edits[k].paint && edits[k].continent == continent)
            h += bump(&edits[k], x, z, &gx, &gz);
    return h;
}

/* The walking height must follow the DRAWN ground, and the drawn ground only has a
 * vertex every few hundred units: a vertex raised by a bump lifts every triangle
 * that touches it, far wider than the bump itself. So the heightmap gets the
 * vertex changes interpolated across the drawn triangles (barycentric in x/z),
 * not the bump - otherwise the hero sinks into or floats over the edited ground
 * (seen: waist-deep at the foot of a +60 hill). */
static int raise_heightmap(unsigned char *data, DWORD size, int continent)
{
    char qname[64];
    unsigned char *q;
    DWORD w, h, qsize, nv, p, m, t;
    const unsigned short *idx;
    float *dv, div;
    unsigned char *done;
    int ox, oz, count = 0;

    if (size < 24)
        return -1;
    w = *(DWORD *)data;
    h = *(DWORD *)(data + 4);
    ox = *(int *)(data + 8);
    oz = *(int *)(data + 12);
    if ((unsigned long long)w * h * 2 + 24 != size)
        return -1;
    div = *(float *)(data + 16 + 2 * w * h);

    sprintf(qname, "Continent%d_QuadTree.dat", continent);
    q = read_original(qname, &qsize);
    if (q == NULL)
        return -3;
    p = quadtree_vertices(q, qsize, &nv);
    if (p == 0) {
        free(q);
        return -3;
    }
    m = *(DWORD *)(q + p + 32 * nv);
    idx = (const unsigned short *)(q + p + 32 * nv + 4);
    dv = (float *)malloc(nv * sizeof(float));
    done = (unsigned char *)calloc(((size_t)w * h + 7) / 8, 1);    /* each grid point once */
    {
        /* flat: each vertex moves to the flat height; the walking height moves by the
           same change, so the fine detail the drawn ground keeps (the tesselation adds
           a few units between the sparse vertices - median +2.5 on continent 3) stays
           in both and they match. Setting the grid to one value left the hero ~3 units
           inside the drawn ground. */
        float fh, gh, gx, gz;
        int flat = flat_height(continent, &fh);
        for (t = 0; t < nv; t++) {
            const float *v = (const float *)(q + p + 32 * t);
            dv[t] = raise_at(continent, v[0], v[2]);
            if (grid_at(continent, v[0], v[2], &gh, &gx, &gz))
                dv[t] += gh - v[1];
            else if (flat)
                dv[t] += fh - v[1];
        }
    }
    for (t = 0; t + 2 < m; t += 3) {
        const float *a, *b, *c;
        float da, db, dc, den, x0, x1, z0, z1;
        int i, j, i0, i1, j0, j1;
        if (idx[t] >= nv || idx[t + 1] >= nv || idx[t + 2] >= nv)
            continue;
        da = dv[idx[t]];
        db = dv[idx[t + 1]];
        dc = dv[idx[t + 2]];
        if (da == 0.0f && db == 0.0f && dc == 0.0f)
            continue;
        a = (const float *)(q + p + 32 * idx[t]);
        b = (const float *)(q + p + 32 * idx[t + 1]);
        c = (const float *)(q + p + 32 * idx[t + 2]);
        den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (fabsf(den) < 1e-6f)
            continue;
        x0 = fminf(a[0], fminf(b[0], c[0])); x1 = fmaxf(a[0], fmaxf(b[0], c[0]));
        z0 = fminf(a[2], fminf(b[2], c[2])); z1 = fmaxf(a[2], fmaxf(b[2], c[2]));
        i0 = (int)ceilf((x0 - ox) / 5.0f); i1 = (int)floorf((x1 - ox) / 5.0f);
        j0 = (int)ceilf((z0 - oz) / 5.0f); j1 = (int)floorf((z1 - oz) / 5.0f);
        for (i = i0 < 0 ? 0 : i0; i <= i1 && i < (int)w; i++)
            for (j = j0 < 0 ? 0 : j0; j <= j1 && j < (int)h; j++) {
                float x = (float)(ox + 5 * i), z = (float)(oz + 5 * j), w1, w2, w3, v;
                unsigned short *cell;
                w1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den;
                w2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den;
                size_t at = (size_t)i * h + j;                   /* column-major */
                w3 = 1.0f - w1 - w2;
                if (w1 < -1e-4f || w2 < -1e-4f || w3 < -1e-4f || (done[at >> 3] & (1 << (at & 7))))
                    continue;
                done[at >> 3] |= (unsigned char)(1 << (at & 7));
                cell = (unsigned short *)(data + 16 + 2 * at);
                v = *cell + (w1 * da + w2 * db + w3 * dc) * div;
                *cell = (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5f);
                count++;
            }
    }
    free(done);
    free(dv);
    free(q);
    {
        /* a level floor: every grid point at exactly the walk height */
        float walk, off = *(float *)(data + 16 + 2 * w * h + 4), v;
        if (flat_walk(continent, &walk)) {
            unsigned short *cell = (unsigned short *)(data + 16), value;
            size_t at;
            v = (walk - off) * div;
            value = (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5f);
            for (at = 0; at < (size_t)w * h; at++)
                cell[at] = value;
            count = (int)(w * h);
        }
    }
    return count;
}

/* Quadtree: u32 0, u32 nodes, node area (8-corner boxes), u32 n + n vertices
 * (x y z nx ny nz u v), u32 m + m u16 indices. Returns the vertex block offset. */
static DWORD quadtree_vertices(const unsigned char *data, DWORD size, DWORD *count)
{
    DWORD p;

    for (p = 8; p + 4 <= size; p++) {
        DWORD n = *(DWORD *)(data + p - 4), e;
        if (n == 0 || n > 200000)
            continue;
        e = p + 32 * n;
        if (e + 4 <= size && e + 4 + 2 * (unsigned long long)*(DWORD *)(data + e) == size) {
            *count = n;
            return p;
        }
    }
    return 0;
}

static int is_box(const float *k)
{
    return k[0] == k[9] && k[9] == k[12] && k[12] == k[21] && k[3] == k[6] && k[6] == k[15] && k[15] == k[18] &&
           k[1] == k[4] && k[4] == k[7] && k[7] == k[10] && k[13] == k[16] && k[16] == k[19] && k[19] == k[22] &&
           k[2] == k[5] && k[5] == k[14] && k[14] == k[17] && k[8] == k[11] && k[11] == k[20] && k[20] == k[23] &&
           k[0] < k[3] && k[1] >= k[13] && k[8] < k[2];
}

static int raise_quadtree(unsigned char *data, DWORD size, int continent, int *boxes)
{
    DWORD n, p = quadtree_vertices(data, size, &n), i, o;
    int moved = 0, flattened = 0, gridded = 0, any = 0, k;
    float *mx = NULL, *mz = NULL, *my = NULL;

    *boxes = 0;
    if (p == 0)
        return -1;
    for (k = 0; k < nedits; k++)
        if (edits[k].paint != 1 && edits[k].continent == continent)
            any = 1;
    if (!any)
        return 0;
    {
        float fh;
        if (flat_height(continent, &fh)) {
            for (i = 0; i < n; i++) {
                float *v = (float *)(data + p + 32 * i);
                v[1] = fh;
                v[3] = 0.0f;
                v[4] = 1.0f;
                v[5] = 0.0f;
            }
            for (o = 8; o + 96 <= p - 4; o += 4) {
                float *b = (float *)(data + o);
                if (!is_box(b))
                    continue;
                b[1] = b[4] = b[7] = b[10] = fh + 1.0f;      /* top */
                b[13] = b[16] = b[19] = b[22] = fh - 1.0f;   /* bottom */
                (*boxes)++;
            }
            flattened = (int)n;
        }
    }
    mx = (float *)malloc(n * sizeof(float));
    mz = (float *)malloc(n * sizeof(float));
    my = (float *)malloc(n * sizeof(float));
    for (i = 0; i < n; i++) {
        /* grid: the vertex at our height, its normal from our slope */
        float *v = (float *)(data + p + 32 * i), gh, gx, gz, ln;
        if (!grid_at(continent, v[0], v[2], &gh, &gx, &gz))
            continue;
        ln = sqrtf(gx * gx + 1.0f + gz * gz);
        v[1] = gh;
        v[3] = -gx / ln;
        v[4] = 1.0f / ln;
        v[5] = -gz / ln;
        gridded++;
    }
    for (i = 0; i < n; i++) {
        float *v = (float *)(data + p + 32 * i), h = 0.0f, gx = 0.0f, gz = 0.0f, sx, sz, ln;
        for (k = 0; k < nedits; k++) {
            float ax, az;
            if (edits[k].paint || edits[k].continent != continent)
                continue;
            h += bump(&edits[k], v[0], v[2], &ax, &az);
            gx += ax;
            gz += az;
        }
        if (h == 0.0f && gx == 0.0f && gz == 0.0f) {
            float gh, ax, az;                   /* a gridded vertex still counts for the boxes */
            if (gridded && grid_at(continent, v[0], v[2], &gh, &ax, &az)) {
                mx[moved] = v[0];
                my[moved] = v[1];
                mz[moved] = v[2];
                moved++;
            }
            continue;
        }
        /* the original surface's slope plus the bump's, back to a unit normal */
        sx = (v[4] > 0.01f ? -v[3] / v[4] : 0.0f) + gx;
        sz = (v[4] > 0.01f ? -v[5] / v[4] : 0.0f) + gz;
        ln = sqrtf(sx * sx + 1.0f + sz * sz);
        v[1] += h;
        v[3] = -sx / ln;
        v[4] = 1.0f / ln;
        v[5] = -sz / ln;
        mx[moved] = v[0];
        my[moved] = v[1];
        mz[moved] = v[2];
        moved++;
    }
    /* node boxes: lift the top (and lower the bottom) of every box that holds a moved vertex
       outside it (the boxes are not tight around the vertices, so only moved ones count) */
    for (o = 8; o + 96 <= p - 4; o += 4) {
        float *b = (float *)(data + o), top, bottom;
        int m;
        if (!is_box(b))
            continue;
        top = b[1];
        bottom = b[13];
        for (m = 0; m < moved; m++)
            if (mx[m] >= b[0] && mx[m] <= b[3] && mz[m] >= b[8] && mz[m] <= b[2]) {
                if (my[m] > top) top = my[m];
                if (my[m] < bottom) bottom = my[m];
            }
        if (top > b[1] || bottom < b[13]) {
            b[1] = b[4] = b[7] = b[10] = top;
            b[13] = b[16] = b[19] = b[22] = bottom;
            (*boxes)++;
        }
    }
    free(mx);
    free(my);
    free(mz);
    if (gridded > moved)
        moved = gridded;
    return moved > flattened ? moved : flattened;
}

/* ------------------------------------------------------------- mask painting */

static void alpha_palette(int a0, int a1, int *pal)
{
    int k;
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1)
        for (k = 1; k < 7; k++)
            pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    else {
        for (k = 1; k < 5; k++)
            pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
}

static void decode_alpha(const unsigned char *b, int *out)
{
    int pal[8], k;
    unsigned long long bits = 0;

    alpha_palette(b[0], b[1], pal);
    for (k = 0; k < 6; k++)
        bits |= (unsigned long long)b[2 + k] << (8 * k);
    for (k = 0; k < 16; k++)
        out[k] = pal[(bits >> (3 * k)) & 7];
}

static void encode_alpha(const int *vals, unsigned char *b)
{
    int hi = 0, lo = 255, pal[8], k, m;
    unsigned long long bits = 0;

    for (k = 0; k < 16; k++) {
        if (vals[k] > hi) hi = vals[k];
        if (vals[k] < lo) lo = vals[k];
    }
    b[0] = (unsigned char)hi;
    b[1] = (unsigned char)lo;
    if (hi == lo) {
        memset(b + 2, 0, 6);
        return;
    }
    alpha_palette(hi, lo, pal);
    for (k = 0; k < 16; k++) {
        int best = 0;
        for (m = 1; m < 8; m++)
            if (abs(pal[m] - vals[k]) < abs(pal[best] - vals[k]))
                best = m;
        bits |= (unsigned long long)best << (3 * k);
    }
    for (k = 0; k < 6; k++)
        b[2 + k] = (unsigned char)(bits >> (8 * k));
}

/* World -> texture mapping, fitted (least squares) to the continent's drawn vertices:
 * u = au * x + bu, v = av * z + bv. */
static int uv_mapping(int continent, double *au, double *bu, double *av, double *bv)
{
    char name[64];
    DWORD size, n, p, i;
    unsigned char *q;
    double sx = 0, su = 0, sxx = 0, sxu = 0, sz = 0, sv = 0, szz = 0, szv = 0;

    sprintf(name, "Continent%d_QuadTree.dat", continent);
    q = read_from_paks(name, &size);
    if (q == NULL)
        return 0;
    p = quadtree_vertices(q, size, &n);
    if (p == 0) {
        free(q);
        return 0;
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(q + p + 32 * i);
        sx += v[0]; su += v[6]; sxx += (double)v[0] * v[0]; sxu += (double)v[0] * v[6];
        sz += v[2]; sv += v[7]; szz += (double)v[2] * v[2]; szv += (double)v[2] * v[7];
    }
    *au = (n * sxu - sx * su) / (n * sxx - sx * sx);
    *bu = (su - *au * sx) / n;
    *av = (n * szv - sz * sv) / (n * szz - sz * sz);
    *bv = (sv - *av * sz) / n;
    free(q);
    return 1;
}

static int paint_mask(unsigned char *data, DWORD size, int continent, const char *layer)
{
    DWORD width, height, mips, o = 128, w, h, mip;
    double au, bu, av, bv;
    int blocks = 0, k;

    if (size < 128 || memcmp(data, "DDS ", 4) != 0 || memcmp(data + 84, "DXT5", 4) != 0)
        return -1;
    if (!uv_mapping(continent, &au, &bu, &av, &bv))
        return -2;
    height = *(DWORD *)(data + 12);
    width = *(DWORD *)(data + 16);
    mips = *(DWORD *)(data + 28);
    if (mips == 0)
        mips = 1;
    w = width;
    h = height;
    for (mip = 0; mip < mips && o < size; mip++) {
        DWORD bw = w > 4 ? w / 4 : 1, bh = h > 4 ? h / 4 : 1, bx, by;
        if (o + bw * bh * 16 > size)
            break;
        for (k = 0; k < nedits; k++) {
            const Edit *e = &edits[k];
            double u0, u1, v0, v1;
            int x0, x1, y0, y1;
            if (e->paint != 1 || e->continent != continent || _stricmp(e->layer, layer) != 0)
                continue;
            /* texel range the circle covers (either sign of the mapping) */
            u0 = au * (e->x - e->r) + bu; u1 = au * (e->x + e->r) + bu;
            v0 = av * (e->z - e->r) + bv; v1 = av * (e->z + e->r) + bv;
            x0 = (int)floor((u0 < u1 ? u0 : u1) * w) / 4 - 1; x1 = (int)floor((u0 < u1 ? u1 : u0) * w) / 4 + 1;
            y0 = (int)floor((v0 < v1 ? v0 : v1) * h) / 4 - 1; y1 = (int)floor((v0 < v1 ? v1 : v0) * h) / 4 + 1;
            for (by = (DWORD)(y0 < 0 ? 0 : y0); (int)by <= y1 && by < bh; by++)
                for (bx = (DWORD)(x0 < 0 ? 0 : x0); (int)bx <= x1 && bx < bw; bx++) {
                    unsigned char *blk = data + o + 16 * (by * bw + bx);
                    int vals[16], changed = 0, t;
                    decode_alpha(blk, vals);
                    for (t = 0; t < 16; t++) {
                        double u = (bx * 4 + t % 4 + 0.5) / w, vv = (by * 4 + t / 4 + 0.5) / h;
                        double x = (u - bu) / au, z = (vv - bv) / av;
                        double d = sqrt((x - e->x) * (x - e->x) + (z - e->z) * (z - e->z)), wt;
                        int nv;
                        if (d >= e->r)
                            continue;
                        wt = d <= e->r * 0.7 ? 1.0 : (e->r - d) / (e->r * 0.3);
                        nv = (int)(vals[t] + (e->v * 255.0 - vals[t]) * wt + 0.5);
                        nv = nv < 0 ? 0 : nv > 255 ? 255 : nv;
                        if (nv != vals[t]) {
                            vals[t] = nv;
                            changed = 1;
                        }
                    }
                    if (changed) {
                        encode_alpha(vals, blk);
                        blocks++;
                    }
                }
        }
        o += bw * bh * 16;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return blocks;
}

/* Which terrain file a resource name is: 1 heightmap, 2 quadtree, 3 mask (+ layer); 0 none. */
static int terrain_kind(const char *name, int *continent, char *layer)
{
    char low[128];
    int i, n = 0;

    if (strlen(name) >= sizeof(low))
        return 0;
    for (i = 0; name[i]; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';
    if (sscanf(low, "continent%d%n", continent, &n) == 1 && n > 0) {
        if (strcmp(low + n, "_heightmap.nmph") == 0)
            return 1;
        if (strcmp(low + n, "_quadtree.dat") == 0)
            return 2;
        return 0;
    }
    if (sscanf(low, "maskcont%d%15[a-z]%n", continent, layer, &n) == 2 && strcmp(low + n, ".dds") == 0)
        return 3;
    return 0;
}

/* Edited files already built for the current edit list (reloading the edit list
 * clears it), so a file the engine asks for twice is written only once. */
static char built[64][128];
static int nbuilt;

/* If the requested name is a terrain file with edits, builds the edited copy in
 * Mods/_cache and puts its path in out. */
static int build_edited(const char *name, char *out)
{
    int continent = 0, n = -1, boxes = 0, kind, k, has = 0;
    char layer[16] = "", cache[MAX_PATH];
    unsigned char *data;
    DWORD size;
    FILE *f;

    kind = terrain_kind(name, &continent, layer);
    if (kind == 0)
        return 0;
    for (k = 0; k < nedits; k++)
        if (edits[k].continent == continent &&
            ((kind < 3 && edits[k].paint != 1) || (kind == 3 && edits[k].paint == 1 && _stricmp(edits[k].layer, layer) == 0)))
            has = 1;
    if (!has)
        return 0;
    mods_path(cache, "_cache\\");
    strcat(cache, name);
    for (k = 0; k < nbuilt; k++)
        if (_stricmp(built[k], name) == 0) {
            strcpy(out, cache);
            return 1;
        }
    data = read_original(name, &size);
    if (data == NULL) {
        logmsg("edit %s: original not found", name);
        return 0;
    }
    if (kind == 1)
        n = raise_heightmap(data, size, continent);
    else if (kind == 2)
        n = raise_quadtree(data, size, continent, &boxes);
    else
        n = paint_mask(data, size, continent, layer);
    if (n < 0) {
        logmsg("edit %s: unexpected file layout (%d) - left alone", name, n);
        free(data);
        return 0;
    }
    {
        char dir[MAX_PATH];
        mods_path(dir, "_cache");
        CreateDirectoryA(dir, NULL);
    }
    f = fopen(cache, "wb");
    if (f == NULL || fwrite(data, 1, size, f) != size) {
        if (f) fclose(f);
        free(data);
        logmsg("edit %s: cannot write %s", name, cache);
        return 0;
    }
    fclose(f);
    free(data);
    if (kind == 2)
        logmsg("edit %s: %d vertices, %d boxes", name, n, boxes);
    else
        logmsg("edit %s: %d %s", name, n, kind == 1 ? "grid points" : "mask blocks");
    if (nbuilt < 64)
        strcpy(built[nbuilt++], name);
    strcpy(out, cache);
    return 1;
}

/* ---------------------------------------------------------------- file edits */
/* Mods/file_edits.txt (ModAPI.EditFile / ModAPI.CollisionEdit): files made on the player's PC from
 * the game's own, so no mod ever ships a changed game file. One edit per line, fields separated
 * by '|', the second field is the file the edit makes (see the header for the kinds). The edits of
 * a file are applied in their order to the original the first time the engine asks for it in this
 * run, so a changed mod is picked up the next time it loads. */
#define MAX_FILE_EDITS 128
#define MAX_FIELDS 48
typedef struct { char line[700]; char *f[MAX_FIELDS]; int n; } FileEdit;
static FileEdit fedits[MAX_FILE_EDITS];
static int nfedits;

typedef struct { unsigned char *d; DWORD n; } Buf;   /* d[n] is always 0 */

/* replaces len bytes at `at` with rep (rlen bytes) */
static int splice(Buf *b, DWORD at, DWORD len, const char *rep, DWORD rlen)
{
    unsigned char *nd = (unsigned char *)malloc(b->n - len + rlen + 1);
    if (nd == NULL)
        return 0;
    memcpy(nd, b->d, at);
    memcpy(nd + at, rep, rlen);
    memcpy(nd + at + rlen, b->d + at + len, b->n - at - len);
    b->n = b->n - len + rlen;
    nd[b->n] = 0;
    free(b->d);
    b->d = nd;
    return 1;
}

static int is_blank(unsigned char c)
{
    return c == ' ' || c == '\t';
}

/* text: the first line "<key> = <value>;" (blanks anywhere around the =) gets value */
static int text_set(Buf *b, const char *key, const char *value)
{
    DWORD i = 0, kl = (DWORD)strlen(key);

    while (i < b->n) {
        DWORD j = i;
        unsigned char *nl;
        while (j < b->n && is_blank(b->d[j]))
            j++;
        if (j + kl <= b->n && memcmp(b->d + j, key, kl) == 0) {
            DWORD k = j + kl;
            while (k < b->n && is_blank(b->d[k]))
                k++;
            if (k < b->n && b->d[k] == '=') {
                unsigned char *semi;
                k++;
                while (k < b->n && is_blank(b->d[k]))
                    k++;
                semi = (unsigned char *)memchr(b->d + k, ';', b->n - k);
                if (semi != NULL)
                    return splice(b, k, (DWORD)(semi - (b->d + k)), value, (DWORD)strlen(value));
            }
        }
        nl = (unsigned char *)memchr(b->d + i, '\n', b->n - i);
        if (nl == NULL)
            break;
        i = (DWORD)(nl - b->d) + 1;
    }
    return 0;
}

/* text: every old becomes new; returns how many */
static int text_replace(Buf *b, const char *old, const char *rep)
{
    DWORD pos = 0, ol = (DWORD)strlen(old), rl = (DWORD)strlen(rep);
    int count = 0;

    if (ol == 0)
        return 0;
    while (pos < b->n) {
        char *hit = strstr((char *)b->d + pos, old);
        if (hit == NULL)
            break;
        pos = (DWORD)((unsigned char *)hit - b->d);
        if (!splice(b, pos, ol, rep, rl))
            break;
        pos += rl;
        count++;
    }
    return count;
}

/* effect (.ef): in every stage "Texture[k] = <texture>;", the first "<field>[k] = ...;" before the
   next stage gets value; returns how many */
static int text_stage(Buf *b, const char *texture, const char *field, const char *value)
{
    DWORD pos = 0;
    int count = 0;

    while (pos < b->n) {
        char *p = strstr((char *)b->d + pos, "Texture["), *q, *end, *hit, want[96], tex[80];
        int k;
        if (p == NULL)
            break;
        pos = (DWORD)((unsigned char *)p - b->d) + 8;
        q = p + 8;
        if (!isdigit((unsigned char)*q) || q[1] != ']')
            continue;
        k = *q - '0';
        q += 2;
        while (is_blank(*q))
            q++;
        if (*q != '=')
            continue;
        q++;
        while (is_blank(*q))
            q++;
        _snprintf(tex, sizeof(tex), "<%s>;", texture);
        tex[sizeof(tex) - 1] = 0;
        if (strncmp(q, tex, strlen(tex)) != 0)
            continue;
        q += strlen(tex);
        end = strstr(q, "Texture[");
        if (end == NULL)
            end = (char *)b->d + b->n;
        _snprintf(want, sizeof(want), "%s[%d]", field, k);
        want[sizeof(want) - 1] = 0;
        hit = strstr(q, want);
        if (hit == NULL || hit >= end)
            continue;
        hit += strlen(want);
        while (is_blank(*hit))
            hit++;
        if (*hit != '=')
            continue;
        hit++;
        while (is_blank(*hit))
            hit++;
        {
            char *semi = strchr(hit, ';');
            DWORD at = (DWORD)((unsigned char *)hit - b->d);
            if (semi == NULL || semi > end)
                continue;
            if (!splice(b, at, (DWORD)(semi - hit), value, (DWORD)strlen(value)))
                break;
            pos = at + (DWORD)strlen(value);
            count++;
        }
    }
    return count;
}

/* forest (.fst, 68-byte trees: u32 kind + a 4x4 float matrix, translation x at +52, z at +60):
   the trees inside the circle are left out; returns how many */
static int forest_clear(Buf *b, double cx, double cz, double r)
{
    DWORD i, kept = 0;
    int cut = 0;

    if (b->n % 68 != 0)
        return -1;
    for (i = 0; i < b->n / 68; i++) {
        double x = *(float *)(b->d + i * 68 + 52), z = *(float *)(b->d + i * 68 + 60);
        if (sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz)) < r) {
            cut++;
            continue;
        }
        if (kept != i)
            memmove(b->d + kept * 68, b->d + i * 68, 68);
        kept++;
    }
    b->n = kept * 68;
    b->d[b->n] = 0;
    return cut;
}

/* ---- animations. Same steps as tools/attt_models.py (speed_animation, reverse_animation,
   smooth_trk - the reference these are checked against).
   .ska: u32 blocks, u8 flag, then per block u32 bones, u32 frames and frames x bones x 6 floats
   [pitch, yaw, roll, x, y, z], frame-major. .trk (root motion, text): the frame count, then x y z
   per frame (y = forward, z = height). */
typedef struct { int n; double *p; } Trk;   /* p = n x 3 */

static int buf_add(Buf *b, const void *p, DWORD n);

static int has_ext(const char *name, const char *ext)
{
    size_t a = strlen(name), b = strlen(ext);
    return a >= b && _stricmp(name + a - b, ext) == 0;
}

/* Python's round(): halves to the even neighbour */
static double py_round(double x)
{
    double r = floor(x), d = x - r;
    if (d > 0.5 || (d == 0.5 && fmod(r, 2.0) != 0.0))
        r += 1.0;
    return r;
}

/* the start of each block's frames in a .ska; returns the block count, < 0 on a bad file */
static int ska_blocks(const Buf *b, DWORD *at, int max)
{
    DWORD o = 5, nb;
    int i;

    if (b->n < 5)
        return -1;
    nb = *(DWORD *)b->d;
    if (nb == 0 || nb > (DWORD)max)
        return -1;
    for (i = 0; i < (int)nb; i++) {
        DWORD bones, frames;
        if (o + 8 > b->n)
            return -1;
        bones = *(DWORD *)(b->d + o);
        frames = *(DWORD *)(b->d + o + 4);
        if (bones == 0 || frames == 0 || bones > 4096 || frames > 100000 ||
            (unsigned long long)bones * frames * 24 > b->n - o - 8)
            return -1;
        at[i] = o;
        o += 8 + bones * frames * 24;
    }
    return o == b->n ? (int)nb : -1;
}

static int trk_read(const Buf *b, Trk *t)
{
    char *p = (char *)b->d, *e;
    int i;

    t->n = (int)strtol(p, &e, 10);
    if (e == p || t->n < 1 || t->n > 100000)
        return 0;
    t->p = (double *)malloc((size_t)t->n * 3 * sizeof(double));
    if (t->p == NULL)
        return 0;
    for (i = 0; i < t->n * 3; i++) {
        p = e;
        t->p[i] = strtod(p, &e);
        if (e == p) {
            free(t->p);
            return 0;
        }
    }
    return 1;
}

/* written like tools/attt_models.py's write_trk */
static int trk_write(Buf *b, const Trk *t)
{
    Buf out = { NULL, 0 };
    char line[128];
    int i, ok;

    _snprintf(line, sizeof(line), "%d\r\n", t->n);
    ok = buf_add(&out, line, (DWORD)strlen(line));
    for (i = 0; ok && i < t->n; i++) {
        __mingw_snprintf(line, sizeof(line), "%.4f\t%.4f\t%.4f\r\n", t->p[i * 3], t->p[i * 3 + 1], t->p[i * 3 + 2]);
        ok = buf_add(&out, line, (DWORD)strlen(line));
    }
    if (!ok) {
        free(out.d);
        return 0;
    }
    free(b->d);
    *b = out;                            /* buf_add keeps the closing 0 */
    return 1;
}

/* played `factor` times faster: new frame k = old frame k x step, blended between the two nearest
   frames (angles the shorter way round); the root motion is resampled the same way */
static int anim_speed(Buf *b, const char *name, double factor)
{
    if (factor <= 0.0)
        return -2;
    if (has_ext(name, ".trk")) {
        Trk t, s;
        int k, c;
        double step;
        if (!trk_read(b, &t))
            return -1;
        s.n = (int)py_round((t.n - 1) / factor) + 1;
        if (s.n < 2)
            s.n = 2;
        step = (double)(t.n - 1) / (s.n - 1);
        s.p = (double *)malloc((size_t)s.n * 3 * sizeof(double));
        if (s.p == NULL) {
            free(t.p);
            return -3;
        }
        for (k = 0; k < s.n; k++) {
            double tt = k * step, f;
            int i = (int)tt, j;
            if (i > t.n - 1) i = t.n - 1;
            j = i + 1 > t.n - 1 ? t.n - 1 : i + 1;
            f = tt - i;
            for (c = 0; c < 3; c++)
                s.p[k * 3 + c] = t.p[i * 3 + c] + (t.p[j * 3 + c] - t.p[i * 3 + c]) * f;
        }
        free(t.p);
        k = trk_write(b, &s);
        free(s.p);
        return k ? 1 : -3;
    } else {
        DWORD at[256];
        int nb = ska_blocks(b, at, 256), i;
        Buf out = { NULL, 0 };
        if (nb < 0)
            return -1;
        if (!buf_add(&out, b->d, 5))
            return -3;
        for (i = 0; i < nb; i++) {
            DWORD bones = *(DWORD *)(b->d + at[i]), frames = *(DWORD *)(b->d + at[i] + 4), hdr[2];
            const float *v = (const float *)(b->d + at[i] + 8);
            DWORD fresh = (DWORD)py_round((frames - 1) / factor) + 1, k, x;
            double step;
            float *row;
            if (fresh < 2)
                fresh = 2;
            step = (double)(frames - 1) / (fresh - 1);
            hdr[0] = bones;
            hdr[1] = fresh;
            row = (float *)malloc(bones * 24);
            if (row == NULL || !buf_add(&out, hdr, 8)) {
                free(row);
                free(out.d);
                return -3;
            }
            for (k = 0; k < fresh; k++) {
                double tt = k * step, f;
                DWORD ii = (DWORD)tt, jj;
                if (ii > frames - 1) ii = frames - 1;
                jj = ii + 1 > frames - 1 ? frames - 1 : ii + 1;
                f = tt - ii;
                for (x = 0; x < bones * 6; x++) {
                    double a = v[ii * bones * 6 + x], c = v[jj * bones * 6 + x];
                    if (f == 0.0 || ii == jj)
                        row[x] = (float)a;
                    else if (x % 6 < 3) {             /* an angle: blend the shorter way round */
                        double d = fmod(c - a + M_PI, 2.0 * M_PI);
                        if (d < 0.0)
                            d += 2.0 * M_PI;
                        row[x] = (float)(a + (d - M_PI) * f);
                    } else
                        row[x] = (float)(a + (c - a) * f);
                }
                if (!buf_add(&out, row, bones * 24)) {
                    free(row);
                    free(out.d);
                    return -3;
                }
            }
            free(row);
        }
        free(b->d);
        *b = out;
        return 1;
    }
}

/* played backwards: the frames in reverse order; the root motion runs the other way (its
   positions reversed, shifted to start where the original did) */
static int anim_reverse(Buf *b, const char *name)
{
    if (has_ext(name, ".trk")) {
        Trk t, s;
        int k, c;
        if (!trk_read(b, &t))
            return -1;
        s.n = t.n;
        s.p = (double *)malloc((size_t)s.n * 3 * sizeof(double));
        if (s.p == NULL) {
            free(t.p);
            return -3;
        }
        for (k = 0; k < t.n; k++)
            for (c = 0; c < 3; c++)
                s.p[k * 3 + c] = t.p[(t.n - 1 - k) * 3 + c] + (t.p[c] - t.p[(t.n - 1) * 3 + c]);
        free(t.p);
        k = trk_write(b, &s);
        free(s.p);
        return k ? 1 : -3;
    } else {
        DWORD at[256], row, k;
        int nb = ska_blocks(b, at, 256), i;
        unsigned char *tmp;
        if (nb < 0)
            return -1;
        for (i = 0; i < nb; i++) {
            DWORD frames = *(DWORD *)(b->d + at[i] + 4);
            unsigned char *v = b->d + at[i] + 8;
            row = *(DWORD *)(b->d + at[i]) * 24;
            tmp = (unsigned char *)malloc(row);
            if (tmp == NULL)
                return -3;
            for (k = 0; k < frames / 2; k++) {
                memcpy(tmp, v + k * row, row);
                memcpy(v + k * row, v + (frames - 1 - k) * row, row);
                memcpy(v + (frames - 1 - k) * row, tmp, row);
            }
            free(tmp);
        }
        return 1;
    }
}

/* a steadier root motion (the camera follows it): inside the loop (steps a..b-1) each frame's
   sideways (x) and forward (y) step is pulled towards the loop's average step; sway / surge = how
   much of the difference is kept. Steps outside the loop and the height are left as they are. */
static int trk_smooth(Buf *b, const char *name, int a, int z, double sway, double surge)
{
    Trk t;
    double mean[2] = { 0.0, 0.0 }, keep[2], prev[2];
    int i, c, steps, r;

    if (!has_ext(name, ".trk"))
        return -2;
    if (!trk_read(b, &t))
        return -1;
    steps = t.n - 1;
    if (a < 0 || z <= a || a >= steps) {
        free(t.p);
        return -4;
    }
    for (i = a; i < z && i < steps; i++)
        for (c = 0; c < 2; c++)
            mean[c] += t.p[(i + 1) * 3 + c] - t.p[i * 3 + c];
    keep[0] = sway;
    keep[1] = surge;
    for (c = 0; c < 2; c++)
        mean[c] /= (z - a);
    prev[0] = t.p[0];
    prev[1] = t.p[1];
    for (i = 0; i < steps; i++) {
        for (c = 0; c < 2; c++) {
            double s = t.p[(i + 1) * 3 + c] - t.p[i * 3 + c];
            if (i >= a && i < z)
                s = mean[c] + keep[c] * (s - mean[c]);
            t.p[i * 3 + c] = prev[c];          /* point i is done; keep the original step's base */
            prev[c] = prev[c] + s;
        }
    }
    t.p[steps * 3] = prev[0];
    t.p[steps * 3 + 1] = prev[1];
    r = trk_write(b, &t);
    free(t.p);
    return r ? 1 : -3;
}

/* ---- collision models (.gdl, GMDL): see CLAUDE.md "The pyramid interior" for the layout. A
   continent's invisible walls are "Contour" records whose vertices are closed outlines. Same edits
   as tools/make_collision_edits.py (the reference these are checked against). */
typedef struct Rec Rec;
struct Rec {
    const unsigned char *name;
    DWORD nlen;
    const unsigned char *pivot, *faces, *mats;   /* pivot 16 bytes, faces nf x 6, mats mlen */
    DWORD a, nf, nv, mlen, nc, kcap;
    unsigned char *verts;                        /* nv x 32, malloc'd */
    Rec **kids;
};

static void rec_free(Rec *r)
{
    DWORD i;
    if (r == NULL)
        return;
    for (i = 0; i < r->nc; i++)
        rec_free(r->kids[i]);
    free(r->kids);
    free(r->verts);
    free(r);
}

static int rec_add(Rec *parent, Rec *kid)
{
    if (parent->nc == parent->kcap) {
        DWORD cap = parent->kcap ? parent->kcap * 2 : 8;
        Rec **k = (Rec **)realloc(parent->kids, cap * sizeof(Rec *));
        if (k == NULL)
            return 0;
        parent->kids = k;
        parent->kcap = cap;
    }
    parent->kids[parent->nc++] = kid;
    return 1;
}

#define NEED(n) do { if (*o + (n) > size) goto bad; } while (0)
static Rec *rec_parse(const unsigned char *d, DWORD size, DWORD *o, int depth)
{
    Rec *r = (Rec *)calloc(1, sizeof(Rec));
    DWORD i, nm, nc;

    if (r == NULL || depth > 16)
        goto bad;
    NEED(4);
    r->nlen = *(DWORD *)(d + *o);
    *o += 4;
    NEED(r->nlen + 16 + 8);
    r->name = d + *o;
    *o += r->nlen;
    r->pivot = d + *o;
    *o += 16;
    r->a = *(DWORD *)(d + *o);
    r->nf = *(DWORD *)(d + *o + 4);
    *o += 8;
    NEED(r->nf * 6 + 4);
    r->faces = d + *o;
    *o += r->nf * 6;
    r->nv = *(DWORD *)(d + *o);
    *o += 4;
    NEED(r->nv * 32 + 8);
    r->verts = (unsigned char *)malloc(r->nv * 32 + 1);
    if (r->verts == NULL)
        goto bad;
    memcpy(r->verts, d + *o, r->nv * 32);
    *o += r->nv * 32;
    r->mats = d + *o;
    nm = *(DWORD *)(d + *o + 4);
    *o += 8;
    for (i = 0; i < nm; i++) {
        NEED(68 + 4);
        *o += 68;
        NEED(4 + *(DWORD *)(d + *o) + 6);
        *o += 4 + *(DWORD *)(d + *o) + 6;
    }
    r->mlen = (DWORD)(d + *o - r->mats);
    NEED(4);
    nc = *(DWORD *)(d + *o);
    *o += 4;
    for (i = 0; i < nc; i++) {
        Rec *k = rec_parse(d, size, o, depth + 1);
        if (k == NULL || !rec_add(r, k)) {
            rec_free(k);
            goto bad;
        }
    }
    return r;
bad:
    rec_free(r);
    return NULL;
}
#undef NEED

static int buf_add(Buf *b, const void *p, DWORD n)
{
    unsigned char *nd = (unsigned char *)realloc(b->d, b->n + n + 1);
    if (nd == NULL)
        return 0;
    memcpy(nd + b->n, p, n);
    b->d = nd;
    b->n += n;
    b->d[b->n] = 0;
    return 1;
}

static int rec_write(Buf *b, const Rec *r)
{
    DWORD i;
    int ok = buf_add(b, &r->nlen, 4) && buf_add(b, r->name, r->nlen) && buf_add(b, r->pivot, 16) &&
             buf_add(b, &r->a, 4) && buf_add(b, &r->nf, 4) && buf_add(b, r->faces, r->nf * 6) &&
             buf_add(b, &r->nv, 4) && buf_add(b, r->verts, r->nv * 32) && buf_add(b, r->mats, r->mlen) &&
             buf_add(b, &r->nc, 4);
    for (i = 0; ok && i < r->nc; i++)
        ok = rec_write(b, r->kids[i]);
    return ok;
}

static Rec *rec_find(Rec *r, const char *name)
{
    DWORD i;
    if (r->nlen == strlen(name) && memcmp(r->name, name, r->nlen) == 0)
        return r;
    for (i = 0; i < r->nc; i++) {
        Rec *f = rec_find(r->kids[i], name);
        if (f != NULL)
            return f;
    }
    return NULL;
}

/* the point t of the way from vertex i to vertex i + 1 (outline points: x, y, z, then zeros).
   Every step is stored as a double, like the Python reference computes it */
static void point_on(const unsigned char *verts, DWORD nv, DWORD i, double t, float *out)
{
    const float *a = (const float *)(verts + i * 32), *b = (const float *)(verts + ((i + 1) % nv) * 32);
    int k;
    memset(out, 0, 32);
    for (k = 0; k < 3; k++) {
        volatile double da = a[k], diff = (double)b[k] - da, step = diff * t, sum = da + step;
        out[k] = (float)sum;
    }
}

/* gap: the band between corners G1, G2 (upper edge) and G3, G4 (lower edge) left out; the part
   after G2 keeps the record, the part after G4 becomes a new child record of the first root */
static int gdl_gap(Rec **roots, DWORD nroots, char **f)
{
    Rec *rec = NULL, *nr;
    DWORD i, k, n, idx[4], c1, c2;
    double t[4];
    float g[4][8];
    unsigned char *first, *second;

    for (i = 0; i < nroots && rec == NULL; i++)
        rec = rec_find(roots[i], f[2]);
    if (rec == NULL || rec->nv < 3)
        return -1;
    n = rec->nv;
    for (k = 0; k < 4; k++) {
        idx[k] = (DWORD)strtoul(f[4 + 2 * k], NULL, 10);
        t[k] = strtod(f[5 + 2 * k], NULL);
        if (idx[k] >= n)
            return -2;
        point_on(rec->verts, n, idx[k], t[k], g[k]);
    }
    /* first = G2, vertices idx2+1 .. idx3, G3; second = G4, vertices idx4+1 .. idx1+n, G1 */
    c1 = idx[2] > idx[1] ? idx[2] - idx[1] : 0;
    c2 = idx[0] + n - idx[3];
    first = (unsigned char *)malloc((c1 + 2) * 32);
    second = (unsigned char *)malloc((c2 + 2) * 32);
    nr = (Rec *)calloc(1, sizeof(Rec));
    if (first == NULL || second == NULL || nr == NULL) {
        free(first);
        free(second);
        free(nr);
        return -3;
    }
    memcpy(first, g[1], 32);
    for (k = 0; k < c1; k++)
        memcpy(first + (k + 1) * 32, rec->verts + ((idx[1] + 1 + k) % n) * 32, 32);
    memcpy(first + (c1 + 1) * 32, g[2], 32);
    memcpy(second, g[3], 32);
    for (k = 0; k < c2; k++)
        memcpy(second + (k + 1) * 32, rec->verts + ((idx[3] + 1 + k) % n) * 32, 32);
    memcpy(second + (c2 + 1) * 32, g[0], 32);
    *nr = *rec;
    nr->name = (const unsigned char *)f[3];
    nr->nlen = (DWORD)strlen(f[3]);
    nr->verts = second;
    nr->nv = c2 + 2;
    nr->nc = 0;
    nr->kcap = 0;
    nr->kids = NULL;
    free(rec->verts);
    rec->verts = first;
    rec->nv = c1 + 2;
    if (!rec_add(roots[0], nr)) {
        rec_free(nr);
        return -3;
    }
    return 1;
}

/* detour: the edge after vertex `after` goes through the given points instead ("at" = where that
   vertex is expected, checked to 2 units) */
static int gdl_detour(Rec **roots, DWORD nroots, char **f, int nf)
{
    Rec *rec = NULL;
    DWORD i, after, np;
    float *v;
    unsigned char *nv;
    double ax, az;

    for (i = 0; i < nroots && rec == NULL; i++)
        rec = rec_find(roots[i], f[2]);
    if (rec == NULL || nf < 8 || (nf - 6) % 2 != 0)
        return -1;
    after = (DWORD)strtoul(f[3], NULL, 10);
    ax = strtod(f[4], NULL);
    az = strtod(f[5], NULL);
    if (after >= rec->nv)
        return -2;
    v = (float *)(rec->verts + after * 32);
    if (fabs(v[0] - ax) > 2 || fabs(v[2] - az) > 2) {
        logmsg("file edit: %s vertex %lu is at (%.0f, %.0f), expected (%.0f, %.0f) - edit skipped",
               f[2], (unsigned long)after, v[0], v[2], ax, az);
        return -4;
    }
    np = (DWORD)(nf - 6) / 2;
    nv = (unsigned char *)malloc((rec->nv + np) * 32);
    if (nv == NULL)
        return -3;
    memcpy(nv, rec->verts, (after + 1) * 32);
    for (i = 0; i < np; i++) {
        float p[8] = { 0 };
        p[0] = (float)strtod(f[6 + 2 * i], NULL);
        p[2] = (float)strtod(f[7 + 2 * i], NULL);
        memcpy(nv + (after + 1 + i) * 32, p, 32);
    }
    memcpy(nv + (after + 1 + np) * 32, rec->verts + (after + 1) * 32, (rec->nv - after - 1) * 32);
    free(rec->verts);
    rec->verts = nv;
    rec->nv += np;
    return 1;
}

/* all the gap/detour edits of one model, in their order; returns how many applied, < 0 on a bad file */
static int edit_gdl(Buf *b, const char *name)
{
    Rec *roots[64];
    DWORD nroots, i, o = 20, tail;
    int k, applied = 0;
    Buf out = { NULL, 0 };

    if (b->n < 20 || memcmp(b->d, "GMDL", 4) != 0)
        return -1;
    nroots = *(DWORD *)(b->d + 16);
    if (nroots == 0 || nroots > 64)
        return -1;
    for (i = 0; i < nroots; i++) {
        roots[i] = rec_parse(b->d, b->n, &o, 0);
        if (roots[i] == NULL) {
            while (i > 0)
                rec_free(roots[--i]);
            return -1;
        }
    }
    tail = o;
    for (k = 0; k < nfedits; k++) {
        FileEdit *e = &fedits[k];
        int r = 0;
        if (_stricmp(e->f[1], name) != 0)
            continue;
        if (strcmp(e->f[0], "gap") == 0 && e->n == 12)
            r = gdl_gap(roots, nroots, e->f);
        else if (strcmp(e->f[0], "detour") == 0)
            r = gdl_detour(roots, nroots, e->f, e->n);
        else
            continue;
        if (r > 0)
            applied++;
        else if (r != -4)
            logmsg("file edit %s: %s %s failed (%d)", name, e->f[0], e->f[2], r);
    }
    if (buf_add(&out, b->d, 20)) {
        int ok = 1;
        for (i = 0; ok && i < nroots; i++)
            ok = rec_write(&out, roots[i]);
        if (ok && buf_add(&out, b->d + tail, b->n - tail)) {
            free(b->d);
            *b = out;
            out.d = NULL;
        } else
            applied = -3;
    }
    free(out.d);
    for (i = 0; i < nroots; i++)
        rec_free(roots[i]);
    return applied;
}

static int build_file_now(const char *name, char *out);

/* If the requested name has file edits, builds the edited copy in Mods/_cache from its source (the
 * "from" edit, else the file itself) and puts its path in out. Direct3D runs the game's FPU in single
 * precision; the edits are worked out in full double precision, so they come out exactly like the
 * tools/ reference scripts. */
static int build_file(const char *name, char *out)
{
    unsigned int old = _controlfp(0, 0);
    int r;

    _controlfp(_PC_53, _MCW_PC);
    r = build_file_now(name, out);
    _controlfp(old, _MCW_PC);
    return r;
}

static int build_file_now(const char *name, char *out)
{
    const char *source = name;
    char cache[MAX_PATH], dir[MAX_PATH];
    int k, has = 0, gdl = 0, done = 0;
    Buf b;
    DWORD size;
    FILE *f;

    for (k = 0; k < nfedits; k++)
        if (_stricmp(fedits[k].f[1], name) == 0) {
            has = 1;
            if (strcmp(fedits[k].f[0], "from") == 0 && fedits[k].n >= 3)
                source = fedits[k].f[2];
            if (strcmp(fedits[k].f[0], "gap") == 0 || strcmp(fedits[k].f[0], "detour") == 0)
                gdl = 1;
        }
    if (!has)
        return 0;
    mods_path(cache, "_cache\\");
    strcat(cache, name);
    for (k = 0; k < nbuilt; k++)
        if (_stricmp(built[k], name) == 0) {
            strcpy(out, cache);
            return 1;
        }
    b.d = read_original(source, &size);
    if (b.d == NULL) {
        logmsg("file edit %s: %s not found - left alone", name, source);
        return 0;
    }
    {
        unsigned char *t = (unsigned char *)realloc(b.d, size + 1);   /* room for the closing 0 */
        if (t == NULL) {
            free(b.d);
            return 0;
        }
        b.d = t;
        b.n = size;
        b.d[b.n] = 0;
    }
    for (k = 0; k < nfedits; k++) {
        FileEdit *e = &fedits[k];
        const char *kind = e->f[0];
        int r = -9;
        if (_stricmp(e->f[1], name) != 0 || strcmp(kind, "from") == 0 || strcmp(kind, "gap") == 0 ||
            strcmp(kind, "detour") == 0)
            continue;
        if (strcmp(kind, "set") == 0 && e->n == 4)
            r = text_set(&b, e->f[2], e->f[3]);
        else if (strcmp(kind, "text") == 0 && e->n == 4)
            r = text_replace(&b, e->f[2], e->f[3]);
        else if (strcmp(kind, "stage") == 0 && e->n == 5)
            r = text_stage(&b, e->f[2], e->f[3], e->f[4]);
        else if (strcmp(kind, "clear") == 0 && e->n == 5)
            r = forest_clear(&b, strtod(e->f[2], NULL), strtod(e->f[3], NULL), strtod(e->f[4], NULL));
        else if (strcmp(kind, "speed") == 0 && e->n == 3)
            r = anim_speed(&b, name, strtod(e->f[2], NULL));
        else if (strcmp(kind, "reverse") == 0 && e->n == 2)
            r = anim_reverse(&b, name);
        else if (strcmp(kind, "smooth") == 0 && e->n == 6)
            r = trk_smooth(&b, name, atoi(e->f[2]), atoi(e->f[3]), strtod(e->f[4], NULL), strtod(e->f[5], NULL));
        if (r <= 0 && strcmp(kind, "clear") != 0)
            logmsg("file edit %s: %s %s changed nothing (%d)", name, kind, e->n > 2 ? e->f[2] : "", r);
        else if (r < 0)
            logmsg("file edit %s: not a forest file - clear skipped", name);
        else
            done++;
    }
    if (gdl) {
        int r = edit_gdl(&b, name);
        if (r < 0)
            logmsg("file edit %s: not a usable collision model (%d) - walls left alone", name, r);
        else
            done += r;
    }
    mods_path(dir, "_cache");
    CreateDirectoryA(dir, NULL);
    f = fopen(cache, "wb");
    if (f == NULL || fwrite(b.d, 1, b.n, f) != b.n) {
        if (f) fclose(f);
        free(b.d);
        logmsg("file edit %s: cannot write %s", name, cache);
        return 0;
    }
    fclose(f);
    free(b.d);
    logmsg("file edit %s: from %s, %d edit(s) applied", name, source, done);
    if (nbuilt < 64)
        strcpy(built[nbuilt++], name);
    strcpy(out, cache);
    return 1;
}

/* -------------------------------------------------------------- the detour */

/* "grid <continent> <file>" -> e (see the header for the file layout) */
static int load_grid(Edit *e, const char *args)
{
    char file[MAX_PATH];
    unsigned char *d;
    DWORD size;
    int n = 0;

    if (sscanf(args, "%d %n", &e->continent, &n) != 1 || n == 0)
        return 0;
    strncpy(file, args + n, MAX_PATH - 1);
    file[MAX_PATH - 1] = '\0';
    file[strcspn(file, "\r\n")] = '\0';
    d = read_whole(file, &size);
    if (d == NULL || size < 24 || memcmp(d, "HGRD", 4) != 0) {
        logmsg("grid %s: not found or not a height grid - ignored", file);
        free(d);
        return 0;
    }
    e->x = *(float *)(d + 4);
    e->z = *(float *)(d + 8);
    e->r = *(float *)(d + 12);
    e->cols = (int)*(DWORD *)(d + 16);
    e->rows = (int)*(DWORD *)(d + 20);
    if (e->cols < 2 || e->rows < 2 || e->r <= 0.0f ||
        (unsigned long long)e->cols * e->rows * 4 + 24 != size) {
        logmsg("grid %s: bad size - ignored", file);
        free(d);
        return 0;
    }
    e->g = (float *)malloc((size_t)e->cols * e->rows * 4);
    memcpy(e->g, d + 24, (size_t)e->cols * e->rows * 4);
    free(d);
    e->paint = 3;
    logmsg("grid %s: %d x %d points, cell %.1f, from (%.0f, %.0f)", file, e->cols, e->rows, e->r, e->x, e->z);
    return 1;
}

static void load_files(void)
{
    char path[MAX_PATH], line[MAX_PATH + 160];
    FILE *f;

    EnterCriticalSection(&lock);
    nmap = 0;
    {
        int k;
        for (k = 0; k < nedits; k++)
            free(edits[k].g);
    }
    nedits = 0;
    nbuilt = 0;
    mods_path(path, "resources_map.txt");
    f = fopen(path, "r");
    if (f != NULL) {
        while (nmap < MAX_MAP && fgets(line, sizeof(line), f) != NULL) {
            char *bar = strchr(line, '|');
            if (bar == NULL || bar - line >= (int)sizeof(map[0].name))
                continue;
            *bar = '\0';
            bar[1 + strcspn(bar + 1, "\r\n")] = '\0';
            /* relative paths are relative to the game folder (the working directory);
               Find needs an absolute one - the ':' is what sends it to the disk */
            if (GetFullPathNameA(bar + 1, MAX_PATH, map[nmap].path, NULL) == 0 ||
                strchr(map[nmap].path, ':') == NULL ||
                GetFileAttributesA(map[nmap].path) == INVALID_FILE_ATTRIBUTES) {
                logmsg("map: %s -> %s: file not found, ignored", line, bar + 1);
                continue;
            }
            strcpy(map[nmap].name, line);
            nmap++;
        }
        fclose(f);
    }
    mods_path(path, "terrain_edits.txt");
    f = fopen(path, "r");
    if (f != NULL) {
        while (nedits < MAX_EDITS && fgets(line, sizeof(line), f) != NULL) {
            Edit *e = &edits[nedits];
            memset(e, 0, sizeof(*e));
            if (sscanf(line, "raise %d %f %f %f %f", &e->continent, &e->x, &e->z, &e->r, &e->v) == 5 && e->r > 0)
                nedits++;
            else if (sscanf(line, "flat %d %f %f", &e->continent, &e->v, &e->x) >= 2) {
                e->paint = 2;
                e->r = 1.0f;
                e->z = sscanf(line, "flat %d %f %f", &e->continent, &e->v, &e->x) == 3 ? 1.0f : 0.0f;
                nedits++;
            }
            else if (sscanf(line, "paint %d %15s %f %f %f %f", &e->continent, e->layer, &e->x, &e->z, &e->r,
                            &e->v) == 6 && e->r > 0) {
                e->paint = 1;
                nedits++;
            }
            else if (strncmp(line, "grid ", 5) == 0 && load_grid(e, line + 5))
                nedits++;
        }
        fclose(f);
    }
    nfedits = 0;
    mods_path(path, "file_edits.txt");
    f = fopen(path, "r");
    if (f != NULL) {
        while (nfedits < MAX_FILE_EDITS && fgets(fedits[nfedits].line, sizeof(fedits[0].line), f) != NULL) {
            FileEdit *e = &fedits[nfedits];
            char *p = e->line;
            e->line[strcspn(e->line, "\r\n")] = '\0';
            e->n = 0;
            while (e->n < MAX_FIELDS) {
                e->f[e->n++] = p;
                p = strchr(p, '|');
                if (p == NULL)
                    break;
                *p++ = '\0';
            }
            if (e->n >= 2 && e->f[0][0] && e->f[1][0])
                nfedits++;
        }
        fclose(f);
    }
    nalias = 0;
    mods_path(path, "resources_alias.txt");
    f = fopen(path, "r");
    if (f != NULL) {
        while (nalias < MAX_ALIAS && fgets(line, sizeof(line), f) != NULL) {
            int i;
            if (sscanf(line, "%31s %31s", alias[nalias].from, alias[nalias].to) != 2)
                continue;
            for (i = 0; alias[nalias].from[i]; i++)
                alias[nalias].from[i] = (char)tolower((unsigned char)alias[nalias].from[i]);
            nalias++;
        }
        fclose(f);
    }
    mods_path(path, "resources_lognames.txt");
    log_names = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    LeaveCriticalSection(&lock);
}

/* The disk path that replaces a pak name (an edited copy, a mod file or an alias), or
   "" to leave the name alone. */
static void resolve(const char *text, char *path)
{
    int i;

    path[0] = '\0';
    if (text == NULL || strchr(text, ':') != NULL)
        return;
    EnterCriticalSection(&lock);
    if (!build_edited(text, path) && !build_file(text, path)) {
        for (i = 0; i < nmap; i++)
            if (_stricmp(map[i].name, text) == 0) {
                strcpy(path, map[i].path);
                break;
            }
        if (path[0] == '\0')
            build_alias(text, path);
    }
    LeaveCriticalSection(&lock);
}

/* A std::string that only lives for one call (the engine reads it, copies what it keeps). */
static void ms_borrow(MsString *s, const char *text)
{
    memset(s, 0, sizeof(*s));
    s->bx.ptr = text;
    s->size = (DWORD)strlen(text);
    s->res = s->size < 16 ? 16 : s->size;
}

/* Same convention as Find: this in ecx, name on the stack, callee pops 4. */
static void *__fastcall find_hook(void *self, void *edx, const MsString *name)
{
    const char *text = name ? ms_text(name) : NULL;
    char path[MAX_PATH];

    resolve(text, path);
    if (text != NULL && strchr(text, ':') == NULL && log_names)
        logmsg("find %s%s%s", text, path[0] ? " -> " : "", path);
    if (path[0] != '\0') {
        MsString swapped;
        ms_borrow(&swapped, path);
        return original_find(self, edx, &swapped);
    }
    return original_find(self, edx, name);
}

/* ---- the pak manager's own file access, beside Find ----
 * Some loaders (Forest.cpp: the .fst tree layouts; found 2026-10-28) skip Find and ask the
 * pak manager ([0x6C88D0]) directly, in two steps:
 *   Info(out, const std::string &name)  0x576A90, thiscall, ret 8 - out = {offset, size, pak};
 *       a name containing ':' is a DISK file: {0, its size, 0}; a name in no pak: size 0,
 *       and the caller throws "Error opening file '<name>'"
 *   Read()                              0x576820, thiscall on the caller's request
 *       {size, offset, std::string name (+8), pak (+0x24)}; opens the disk when the name has ':'
 * The caller fills the request with the ORIGINAL name, so both steps are swapped: Info gets the
 * mod file's path (real size), Read runs on a copy of the request that carries that path. */
#define INFO_ADDR 0x576A90
#define READ_ADDR 0x576820
typedef DWORD *(__fastcall *InfoFn)(void *self, void *edx, DWORD *out, const MsString *name);
typedef void *(__fastcall *ReadFn)(void *request, void *edx);
static const unsigned char INFO_PROLOGUE[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68 };
static const unsigned char READ_PROLOGUE[] = { 0x6A, 0xFF, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x68 };
static unsigned char info_trampoline[16], read_trampoline[16];
static InfoFn original_info;
static ReadFn original_read;

static DWORD *__fastcall info_hook(void *self, void *edx, DWORD *out, const MsString *name)
{
    char path[MAX_PATH];

    resolve(name ? ms_text(name) : NULL, path);
    if (path[0] != '\0') {
        MsString swapped;
        if (log_names)
            logmsg("pak info %s -> %s", ms_text(name), path);
        ms_borrow(&swapped, path);
        return original_info(self, edx, out, &swapped);
    }
    return original_info(self, edx, out, name);
}

static void *__fastcall read_hook(void *request, void *edx)
{
    char path[MAX_PATH];
    const MsString *name = (const MsString *)((unsigned char *)request + 8);

    resolve(ms_text(name), path);
    if (path[0] != '\0') {
        unsigned char copy[0x28];
        memcpy(copy, request, sizeof(copy));
        ms_borrow((MsString *)(copy + 8), path);
        return original_read(copy, edx);
    }
    return original_read(request, edx);
}

/* ---- scripts: ScriptManager::RunScript(const char *name) (0x432620, thiscall, ret 4) ----
 * Runs "<name>.lua" from the paks for Lua's DoFile AND for the scripts the engine runs
 * itself (continent<N>Time, ...Towns, ...MusicManager, ...). A missing script throws
 * CGameException ("... - continent4npcs.lua") and the game dies. Alias rules apply
 * here too, for names that are not in the paks. */
#define RUN_ADDR 0x432620
typedef int (__fastcall *RunFn)(void *self, void *edx, const char *name);
static const unsigned char RUN_PROLOGUE[] = { 0x6A, 0xFF, 0x68, 0x5A, 0x63, 0x5E, 0x00, 0x64, 0xA1 };
static unsigned char run_trampoline[16];
static RunFn original_run;

static int __fastcall run_hook(void *self, void *edx, const char *name)
{
    char other[160], file[170];
    unsigned char *data;
    DWORD size;

    if (name != NULL && nalias > 0 && strlen(name) < 150) {
        int swap = 0;
        EnterCriticalSection(&lock);
        if (apply_alias(name, other, sizeof(other))) {
            sprintf(file, "%s.lua", name);
            data = read_from_paks(file, &size);
            if (data != NULL)
                free(data);                 /* the pak has it - leave it alone */
            else
                swap = 1;
        }
        LeaveCriticalSection(&lock);
        if (swap) {
            if (log_names)
                logmsg("script %s -> %s", name, other);
            return original_run(self, edx, other);
        }
    }
    return original_run(self, edx, name);
}

/* ---- continents beyond 3: two hard-coded name checks in the exe ----
 * Both compare through MSVCP80's string operators, imported at fixed slots:
 *   [0x607214] operator!=(const std::string &, const char *)
 *   [0x6071E4] operator==(const std::string &, const char *)
 * 1. Reflection quadtrees: a quadtree whose name is not one of the three literal
 *    "Continent<1-3>Reflection_QuadTree.dat" (0x60FEA0, 0x60FE7C, 0x60FE58) is treated as
 *    ground and asks for a .arr file reflections do not have (message box, load stuck).
 * 2. The world level only sets a continent up if its name is "Continent1/2/3"
 *    (0x51C073; literals 0x61418C, 0x614180, 0x614174).
 * The wrappers change the answer only for those literal addresses; every other comparison
 * in the game goes straight to the original. */
#define IAT_STR_NE 0x607214
#define IAT_STR_EQ 0x6071E4
typedef unsigned char (__cdecl *StrCmpFn)(const MsString *l, const char *r);
static StrCmpFn original_ne, original_eq;

static int ends_with_nocase(const char *s, const char *tail)
{
    size_t ls = strlen(s), lt = strlen(tail);
    return ls >= lt && _stricmp(s + ls - lt, tail) == 0;
}

/* "Continent<N>" with N >= 4 (any case) */
static int is_extra_continent(const char *s)
{
    int n = 0, used = 0;
    return _strnicmp(s, "continent", 9) == 0 && sscanf(s + 9, "%d%n", &n, &used) == 1 &&
           s[9 + used] == '\0' && n >= 4;
}

static unsigned char __cdecl str_ne_hook(const MsString *l, const char *r)
{
    if ((DWORD)r == 0x60FEA0 || (DWORD)r == 0x60FE7C || (DWORD)r == 0x60FE58) {
        const char *s = ms_text(l);
        if (ends_with_nocase(s, "Reflection_QuadTree.dat"))
            return 0;                                   /* "equal": it is a reflection */
    }
    return original_ne(l, r);
}

static unsigned char __cdecl str_eq_hook(const MsString *l, const char *r)
{
    if ((DWORD)r == 0x614174 && is_extra_continent(ms_text(l)))
        return 1;                                       /* counts as a continent */
    return original_eq(l, r);
}

/* Points an import slot of the exe at hook if it currently points into MSVCP80. */
static int patch_slot(DWORD slot, void *hook, void **original)
{
    HMODULE cpp = GetModuleHandleA("MSVCP80.dll");
    {
        void **p = (void **)slot;
        MEMORY_BASIC_INFORMATION mbi;
        DWORD old;
        if (cpp == NULL || VirtualQuery(*p, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase != (void *)cpp)
            return 0;
        if (!VirtualProtect(p, 4, PAGE_READWRITE, &old))
            return 0;
        *original = *p;
        *p = hook;
        VirtualProtect(p, 4, old, &old);
    }
    return 1;
}

/* Moves the first `len` bytes of target into tramp + jmp back, and points target at hook. */
static int detour(unsigned char *target, const unsigned char *expect, size_t expect_len, size_t len,
                  unsigned char *tramp, void *hook)
{
    DWORD old;

    if (target[0] == 0xE9) {
        /* Another mod hooked this function before us with a jump at its start (AttT-Fix's
         * dinput8.dll detours RunScript, 2026-11-09). Go in front of it: our hook runs first and
         * the trampoline continues into the other hook, which calls the original itself. */
        unsigned char *next = target + 5 + *(LONG *)(target + 1);
        tramp[0] = 0xE9;
        *(LONG *)(tramp + 1) = (LONG)(next - (tramp + 5));
        VirtualProtect(tramp, 5, PAGE_EXECUTE_READWRITE, &old);
        if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
            return 0;
        *(LONG *)(target + 1) = (LONG)((unsigned char *)hook - (target + 5));
        VirtualProtect(target, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
        logmsg("%08lX already hooked by another mod (jump to %08lX) - chained in front of it",
               (unsigned long)(DWORD_PTR)target, (unsigned long)(DWORD_PTR)next);
        return 1;
    }
    if (memcmp(target, expect, expect_len) != 0)
        return 0;
    memcpy(tramp, target, len);
    tramp[len] = 0xE9;
    *(LONG *)(tramp + len + 1) = (LONG)((target + len) - (tramp + len + 5));
    VirtualProtect(tramp, len + 5, PAGE_EXECUTE_READWRITE, &old);
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    *(LONG *)(target + 1) = (LONG)((unsigned char *)hook - (target + 5));
    memset(target + 5, 0x90, len - 5);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return 1;
}

static void init_folder(void)
{
    HMODULE self;
    char *slash;

    if (folder[0] != '\0')
        return;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&init_folder, &self);
    GetModuleFileNameA(self, folder, MAX_PATH);
    slash = strrchr(folder, '\\');
    if (slash != NULL)
        slash[1] = '\0';
    InitializeCriticalSection(&lock);
}

/* The mod loader (ModAPI.LoadMods) needs the folders in Mods/, which Lua cannot list without a console
   window flashing: writes their names, one per line, to mod_folders.txt next to this DLL. */
__declspec(dllexport) int rs_list_mods(void *L)
{
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILE *f;

    (void)L;
    init_folder();
    mods_path(path, "mod_folders.txt");
    f = fopen(path, "w");
    if (f == NULL)
        return 0;
    h = FindFirstFileA("Mods\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                fprintf(f, "%s\n", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fclose(f);
    return 0;
}

__declspec(dllexport) int rs_reload(void *L)
{
    (void)L;
    init_folder();
    load_files();
    return 0;
}

__declspec(dllexport) int rs_install(void *L)
{
    static int installed = 0;
    unsigned char *site = (unsigned char *)FIND_ADDR;
    HMODULE self;
    DWORD old;

    (void)L;
    init_folder();
    if (!installed) {                     /* a fresh log once per process */
        char path[MAX_PATH];
        mods_path(path, "resources.log");
        DeleteFileA(path);
    }
    load_files();
    if (installed) {
        logmsg("already installed: %d replacement(s), %d terrain edit(s)", nmap, nedits);
        return 0;
    }
    if (memcmp(site, FIND_PROLOGUE, sizeof(FIND_PROLOGUE)) != 0) {
        logmsg("unexpected code at ResourceManager::Find - not installed (other game build?)");
        return 0;
    }
    /* the game unloads Lua's C libraries when it replaces its Lua state; the
       detour must never point into an unloaded DLL */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)&find_hook, &self)) {
        logmsg("could not pin the DLL - not installed");
        return 0;
    }
    /* trampoline: the first 6 bytes (mov eax, fs:[0]) then jmp back to Find + 6 */
    memcpy(trampoline, site, 6);
    trampoline[6] = 0xE9;
    *(LONG *)(trampoline + 7) = (LONG)((site + 6) - (trampoline + 11));
    VirtualProtect(trampoline, sizeof(trampoline), PAGE_EXECUTE_READWRITE, &old);
    original_find = (FindFn)(void *)trampoline;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) {
        logmsg("VirtualProtect failed - not installed");
        return 0;
    }
    site[0] = 0xE9;
    *(LONG *)(site + 1) = (LONG)((unsigned char *)&find_hook - (site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    installed = 1;
    original_run = (RunFn)(void *)run_trampoline;
    if (!detour((unsigned char *)RUN_ADDR, RUN_PROLOGUE, sizeof(RUN_PROLOGUE), 7, run_trampoline, (void *)run_hook))
        logmsg("unexpected code at RunScript - script aliases off");
    /* both or neither: the second step must see the same swap as the first */
    original_info = (InfoFn)(void *)info_trampoline;
    original_read = (ReadFn)(void *)read_trampoline;
    if (memcmp((void *)INFO_ADDR, INFO_PROLOGUE, sizeof(INFO_PROLOGUE)) != 0 ||
        memcmp((void *)READ_ADDR, READ_PROLOGUE, sizeof(READ_PROLOGUE)) != 0 ||
        !detour((unsigned char *)READ_ADDR, READ_PROLOGUE, sizeof(READ_PROLOGUE), 8, read_trampoline, (void *)read_hook) ||
        !detour((unsigned char *)INFO_ADDR, INFO_PROLOGUE, sizeof(INFO_PROLOGUE), 6, info_trampoline, (void *)info_hook))
        logmsg("unexpected code at the pak manager's Info/Read - files read that way stay the paks' (forests)");
    if (!patch_slot(IAT_STR_NE, (void *)str_ne_hook, (void **)&original_ne) ||
        !patch_slot(IAT_STR_EQ, (void *)str_eq_hook, (void **)&original_eq))
        logmsg("string compare slots not as expected - continents beyond 3 unsupported");
    logmsg("installed: %d replacement(s), %d terrain edit(s), %d alias rule(s)", nmap, nedits, nalias);
    return 0;
}
