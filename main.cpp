// VSL Huntley LED 0.1 - plugin AML: mewarnai bohlam LED lightbar Huntley (579) mengikuti pola VSL.
// Menu (script Lua vsl_huntley.lua) menulis pengaturan ke vsl_cfg.txt, plugin membacanya tiap ~1 detik.
// Semua struktur memori divalidasi dulu; kalau tidak cocok plugin mematikan dirinya (tidak crash).
#include <mod/amlmod.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include "vsl_data.h"

MYMOD(vsl.huntley.led, VSL Huntley LED, 0.1, VSL port)

// ---------- offset struktur (GTA SA 32-bit / RenderWare) ----------
#ifdef HOST_TEST
  #define OFS_ENT_CLUMP   0x18
  #define OFS_ATOM_PARENT 8
  #define OFS_ATOM_GEOM   24
  #define OFS_GEOM_NVERT  20
  #define OFS_GEOM_MATS   32
  #define OFS_GEOM_NMAT   40
  #define OFS_MAT_COLOR   8
#else
  #define OFS_ENT_CLUMP   0x18  // CEntity::m_pRwObject
  #define OFS_ATOM_PARENT 4     // RwObject::parent (frame milik atomic)
  #define OFS_ATOM_GEOM   20    // RpAtomic::geometry
  #define OFS_GEOM_NVERT  20    // RpGeometry::numVertices
  #define OFS_GEOM_MATS   32    // RpGeometry::matList.materials
  #define OFS_GEOM_NMAT   36    // RpGeometry::matList.numMaterials
  #define OFS_MAT_COLOR   4     // RpMaterial::color (RGBA)
#endif

// ---------- log ----------
static FILE* g_log = nullptr;
static char g_pkg[200] = "com.rockstargames.gtasa";
static char g_cfgPath[400] = "";

static void Log(const char* fmt, ...)
{
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "VSLHuntley", "%s", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

static void InitPaths()
{
    FILE* c = fopen("/proc/self/cmdline", "r");
    if (c)
    {
        char tmp[200] = {0};
        size_t n = fread(tmp, 1, sizeof(tmp) - 1, c);
        fclose(c);
        if (n > 0 && tmp[0]) snprintf(g_pkg, sizeof(g_pkg), "%s", tmp);
    }
    char p[400];
    snprintf(p, sizeof(p), "/storage/emulated/0/Android/data/%s/files/vsl_led.log", g_pkg);
    g_log = fopen(p, "w");
    if (!g_log)
    {
        snprintf(p, sizeof(p), "/storage/emulated/0/Download/vsl_led.log");
        g_log = fopen(p, "w");
    }
    snprintf(g_cfgPath, sizeof(g_cfgPath), "/storage/emulated/0/Android/data/%s/files/vsl_cfg.txt", g_pkg);
    Log("paket: %s", g_pkg);
    Log("cfg  : %s", g_cfgPath);
}

// ---------- fungsi game ----------
typedef void* (*GetFrameFromName_t)(void* clump, const char* name);
typedef void* (*ForAllAtomics_t)(void* clump, void* (*cb)(void*, void*), void* data);
static GetFrameFromName_t GetFrameFromName = nullptr;
static ForAllAtomics_t ForAllAtomics = nullptr;
static volatile uint32_t* g_pTime = nullptr;
static bool g_ok = false;

// ---------- pengaturan dari menu ----------
struct Settings
{
    bool enabled;
    double speed;
    bool wrap;
    bool on[NGROUPS];
    int ovr[NGROUPS];
};
static Settings g_set;
static uint32_t g_lastCfgRead = 0;
static bool g_cfgReadOnce = false;

static void DefaultSettings()
{
    g_set.enabled = true;
    g_set.speed = 1.0;
    g_set.wrap = true;
    for (int g = 0; g < NGROUPS; ++g) { g_set.on[g] = true; g_set.ovr[g] = 0; }
}

static void ReadCfg()
{
    FILE* f = fopen(g_cfgPath, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        int a = 0, b = 0, gi = 0;
        float fv = 0.f;
        if (sscanf(line, "enabled=%d", &a) == 1) g_set.enabled = a != 0;
        else if (sscanf(line, "speed=%f", &fv) == 1) { if (fv > 0.05f && fv < 10.f) g_set.speed = fv; }
        else if (sscanf(line, "wrap=%d", &a) == 1) g_set.wrap = a != 0;
        else if (sscanf(line, "g%d=%d,%d", &gi, &a, &b) == 3)
        {
            if (gi >= 1 && gi <= NGROUPS)
            {
                g_set.on[gi - 1] = a != 0;
                g_set.ovr[gi - 1] = (b >= 0 && b <= NPATTERNS) ? b : 0;
            }
        }
    }
    fclose(f);
}

// ---------- mesin pola ----------
static uint8_t g_lit[NGROUPS][MAXPTS];
static uint32_t g_litStamp = 0;
static bool g_litValid = false;

static void ComputeLit(uint32_t t)
{
    double now = (double)t * g_set.speed;
    for (int g = 0; g < NGROUPS; ++g)
    {
        memset(g_lit[g], 0, MAXPTS);
        if (!g_set.enabled || !g_set.on[g]) continue;
        const Group& G = GROUPS[g];
        int pat = -1;
        if (g_set.ovr[g] > 0) pat = g_set.ovr[g] - 1;
        else
        {
            double tt = fmod(now, (double)G.total);
            for (int c = 0; c < G.ncyc; ++c)
            {
                if (tt < G.cyc[c].dur) { pat = G.cyc[c].pat; break; }
                tt -= G.cyc[c].dur;
            }
            if (pat < 0) pat = G.cyc[0].pat;
        }
        const Pattern& P = PATTERNS[pat];
        double pt = fmod(now, (double)P.total);
        const char* row = P.steps[P.n - 1].bits;
        for (int s = 0; s < P.n; ++s)
        {
            if (pt < P.steps[s].dur) { row = P.steps[s].bits; break; }
            pt -= P.steps[s].dur;
        }
        int n = (int)strlen(row);
        if (n <= 0) continue;
        for (int i = 0; i < G.npts && i < MAXPTS; ++i)
        {
            int idx = i;
            if (idx >= n)
            {
                if (!g_set.wrap) continue;
                idx = i % n;
            }
            g_lit[g][i] = (row[idx] != '0') ? 1 : 0;
        }
    }
    g_litStamp = t;
    g_litValid = true;
}

// ---------- cache bohlam LED per kendaraan ----------
struct Lamp { int n; void* m[4]; };
struct Cache
{
    void* self;
    void* clump;
    void* probe;
    bool valid;
    Lamp lamp[NGROUPS][MAXPTS];
};
static Cache g_cache[16];
static int g_cacheNext = 0;

struct Collect { void* atom[512]; void* frame[512]; int n; };
static void* CollectCB(void* atomic, void* data)
{
    Collect* c = (Collect*)data;
    if (c->n < 512)
    {
        c->atom[c->n] = atomic;
        c->frame[c->n] = *(void**)((char*)atomic + OFS_ATOM_PARENT);
        c->n++;
    }
    return atomic;
}

static bool GeomLooksValid(void* geom)
{
    if (!geom || ((uintptr_t)geom & 3)) return false;
    int nv = *(int*)((char*)geom + OFS_GEOM_NVERT);
    int nm = *(int*)((char*)geom + OFS_GEOM_NMAT);
    void** mats = *(void***)((char*)geom + OFS_GEOM_MATS);
    if (nv < 1 || nv > 200000) return false;
    if (nm < 1 || nm > 32) return false;
    if (!mats || ((uintptr_t)mats & 3)) return false;
    for (int i = 0; i < nm; ++i)
        if (!mats[i] || ((uintptr_t)mats[i] & 3)) return false;
    return true;
}

static Cache* BuildCache(void* self, void* clump, void* probe)
{
    Cache* c = nullptr;
    for (int i = 0; i < 16; ++i) if (g_cache[i].self == self) { c = &g_cache[i]; break; }
    if (!c) { c = &g_cache[g_cacheNext]; g_cacheNext = (g_cacheNext + 1) % 16; }
    memset(c, 0, sizeof(*c));
    c->self = self; c->clump = clump; c->probe = probe;

    static Collect col;
    col.n = 0;
    ForAllAtomics(clump, CollectCB, &col);

    int found = 0, missing = 0, bad = 0;
    for (int g = 0; g < NGROUPS; ++g)
    {
        const Group& G = GROUPS[g];
        for (int i = 0; i < G.npts && i < MAXPTS; ++i)
        {
            char name[96];
            snprintf(name, sizeof(name), "%s%d", G.prefix, i + 1);
            void* fr = GetFrameFromName(clump, name);
            int k = -1;
            if (fr) for (int a = 0; a < col.n; ++a) if (col.frame[a] == fr) { k = a; break; }
            if (k < 0) { ++missing; continue; }
            void* atom = col.atom[k];
            if (((uint8_t*)atom)[0] != 1) { ++bad; continue; }   // bukan rpATOMIC
            void* geom = *(void**)((char*)atom + OFS_ATOM_GEOM);
            if (!GeomLooksValid(geom)) { ++bad; continue; }
            int nm = *(int*)((char*)geom + OFS_GEOM_NMAT);
            void** mats = *(void***)((char*)geom + OFS_GEOM_MATS);
            Lamp& L = c->lamp[g][i];
            L.n = nm > 4 ? 4 : nm;
            for (int m = 0; m < L.n; ++m) L.m[m] = mats[m];
            ++found;
        }
    }
    Log("cache: kendaraan=%p clump=%p atomic=%d  bohlam ketemu=%d hilang=%d tidak valid=%d", self, clump, col.n, found, missing, bad);
    if (bad > 0)
    {
        Log("struktur memori tidak cocok, plugin dimatikan demi keamanan");
        g_ok = false;
        c->valid = false;
        return c;
    }
    c->valid = found > 0;
    return c;
}

static void ApplyColors(Cache* c)
{
    for (int g = 0; g < NGROUPS; ++g)
    {
        const Group& G = GROUPS[g];
        for (int i = 0; i < G.npts && i < MAXPTS; ++i)
        {
            const Lamp& L = c->lamp[g][i];
            if (L.n <= 0) continue;
            const uint8_t* col = g_lit[g][i] ? G.onc : G.offc;
            for (int m = 0; m < L.n; ++m)
            {
                uint8_t* p = (uint8_t*)L.m[m] + OFS_MAT_COLOR;
                p[0] = col[0]; p[1] = col[1]; p[2] = col[2];   // alpha tidak diubah
            }
        }
    }
}

static int g_logCount = 0;

static void OnVehiclePreRender(void* self)
{
    uint32_t now = *g_pTime;
    if (!g_cfgReadOnce || now - g_lastCfgRead > 1000u || now < g_lastCfgRead)
    {
        ReadCfg();
        g_cfgReadOnce = true;
        g_lastCfgRead = now;
    }

    void* clump = *(void**)((char*)self + OFS_ENT_CLUMP);
    if (!clump || ((uintptr_t)clump & 3)) return;
    if (((uint8_t*)clump)[0] != 2) return;                 // bukan rpCLUMP

    void* probe = GetFrameFromName(clump, "lightbar-led-1");
    if (!probe) return;                                    // bukan Huntley ber-lightbar

    Cache* c = nullptr;
    for (int i = 0; i < 16; ++i) if (g_cache[i].self == self) { c = &g_cache[i]; break; }
    if (!c || c->clump != clump || c->probe != probe)
    {
        if (g_logCount < 20) { ++g_logCount; c = BuildCache(self, clump, probe); }
        else c = BuildCache(self, clump, probe);
        if (!g_ok) return;
    }
    if (!c->valid) return;

    if (!g_litValid || g_litStamp != now) ComputeLit(now);
    ApplyColors(c);
}

DECL_HOOKv(CVehicle_PreRender, void* self)
{
    CVehicle_PreRender(self);
    if (g_ok) OnVehiclePreRender(self);
}

ON_MOD_LOAD()
{
    InitPaths();
    Log("== VSL Huntley LED 0.1 mulai ==");
    DefaultSettings();

    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { Log("libGTASA handle null, berhenti"); return; }

    GetFrameFromName = (GetFrameFromName_t)aml->GetSym(h, "_ZN15CClumpModelInfo16GetFrameFromNameEP7RpClumpPKc");
    ForAllAtomics = (ForAllAtomics_t)aml->GetSym(h, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
    g_pTime = (volatile uint32_t*)aml->GetSym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    uintptr_t pre = aml->GetSym(h, "_ZN8CVehicle9PreRenderEv");
    Log("GetFrameFromName=%p ForAllAtomics=%p timer=%p PreRender=0x%x", (void*)GetFrameFromName, (void*)ForAllAtomics, (void*)g_pTime, (unsigned)pre);
    if (!GetFrameFromName || !ForAllAtomics || !g_pTime || !pre) { Log("ada simbol yang hilang, berhenti"); return; }

    g_ok = true;
    HOOKSYM(CVehicle_PreRender, h, "_ZN8CVehicle9PreRenderEv");
    Log("hook CVehicle::PreRender terpasang");
}
