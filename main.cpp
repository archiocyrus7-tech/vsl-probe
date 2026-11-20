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
#include <elf.h>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "vsl_data.h"
#include "vsl_shadow.h"

MYMOD(vsl.huntley.led, VSL Huntley LED, 0.2, VSL port)

// ---------- offset struktur (GTA SA 32-bit / RenderWare) ----------
#ifdef HOST_TEST
  #define OFS_ENT_CLUMP   0x18
  #define OFS_ATOM_PARENT 8
  #define OFS_ATOM_GEOM   24
  #define OFS_GEOM_NVERT  20
  #define OFS_GEOM_MATS   32
  #define OFS_GEOM_NMAT   40
  #define OFS_MAT_COLOR   8
  #define OFS_FRAME_PARENT 8
  #define OFS_FRAME_MAT    16
#else
  #define OFS_ENT_CLUMP   0x18  // CEntity::m_pRwObject
  #define OFS_ATOM_PARENT 4     // RwObject::parent (frame milik atomic)
  #define OFS_ATOM_GEOM   20    // RpAtomic::geometry
  #define OFS_GEOM_NVERT  20    // RpGeometry::numVertices
  #define OFS_GEOM_MATS   32    // RpGeometry::matList.materials
  #define OFS_GEOM_NMAT   36    // RpGeometry::matList.numMaterials
  #define OFS_MAT_COLOR   4     // RpMaterial::color (RGBA)
  #define OFS_FRAME_PARENT 4    // RwFrame::object.parent (frame induk)
  #define OFS_FRAME_MAT    16   // RwFrame::modelling (RwMatrix 64 byte: right, up, at, pos)
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
    int test;
    bool caseFollow;
    bool corona;
    bool cull;
    bool all;
    bool shOn, shFlip;
    float shInt, shScale, shAng, shVA;
    bool shTint;
    int shMode, shType;
    int los;
    float bias;
    float scale;
    float gsize[NGROUPS];
    uint8_t gcol[NGROUPS][3];
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
    g_set.test = 0;
    g_set.caseFollow = false;   // bawaan PC: bingkai hitam terus
    g_set.corona = true;
    g_set.cull = true;
    g_set.all = false;
    g_set.shOn = true; g_set.shFlip = false;
    g_set.shInt = 1.0f; g_set.shScale = 0.5f; g_set.shAng = 0.0f; g_set.shVA = 90.0f;
    g_set.shTint = true;
    g_set.shMode = 0; g_set.shType = 1;
    g_set.los = 0;
    g_set.bias = -0.40f;
    g_set.scale = 1.0f;
    for (int g = 0; g < NGROUPS; ++g)
    {
        g_set.gsize[g] = GROUPS[g].size;
        for (int k = 0; k < 3; ++k) g_set.gcol[g][k] = GROUPS[g].c1[k];
    }
    for (int g = 0; g < NGROUPS; ++g) { g_set.on[g] = true; g_set.ovr[g] = 0; }
}

static void ReadCfg()
{
    FILE* f = fopen(g_cfgPath, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        int a = 0, b = 0, gi = 0, c3 = 0;
        float fv = 0.f;
        if (sscanf(line, "enabled=%d", &a) == 1) g_set.enabled = a != 0;
        else if (sscanf(line, "speed=%f", &fv) == 1) { if (fv > 0.05f && fv < 10.f) g_set.speed = fv; }
        else if (sscanf(line, "wrap=%d", &a) == 1) g_set.wrap = a != 0;
        else if (sscanf(line, "case=%d", &a) == 1) g_set.caseFollow = a != 0;
        else if (sscanf(line, "corona=%d", &a) == 1) g_set.corona = a != 0;
        else if (sscanf(line, "cull=%d", &a) == 1) g_set.cull = a != 0;
        else if (sscanf(line, "all=%d", &a) == 1) g_set.all = a != 0;
        else if (sscanf(line, "los=%d", &a) == 1) g_set.los = a != 0 ? 1 : 0;
        else if (sscanf(line, "bias=%f", &fv) == 1) { if (fv > -1.f && fv < 1.f) g_set.bias = fv; }
        else if (sscanf(line, "scale=%f", &fv) == 1) { if (fv > 0.05f && fv < 10.f) g_set.scale = fv; }
        else if (sscanf(line, "shadow=%d", &a) == 1) g_set.shOn = a != 0;
        else if (sscanf(line, "shflip=%d", &a) == 1) g_set.shFlip = a != 0;
        else if (sscanf(line, "shint=%f", &fv) == 1) { if (fv >= 0.f && fv < 5.f) g_set.shInt = fv; }
        else if (sscanf(line, "shscale=%f", &fv) == 1) { if (fv > 0.05f && fv < 10.f) g_set.shScale = fv; }
        else if (sscanf(line, "shang=%f", &fv) == 1) { if (fv > -360.f && fv < 360.f) g_set.shAng = fv; }
        else if (sscanf(line, "shva=%f", &fv) == 1) { if (fv > 0.f && fv < 1000.f) g_set.shVA = fv; }
        else if (sscanf(line, "shtint=%d", &a) == 1) g_set.shTint = a != 0;
        else if (sscanf(line, "shmode=%d", &a) == 1) g_set.shMode = (a >= 0 && a <= 2) ? a : 0;
        else if (sscanf(line, "shtype=%d", &a) == 1) g_set.shType = (a >= 1 && a <= 3) ? a : 1;
        else if (sscanf(line, "gs%d=%f", &gi, &fv) == 2) { if (gi >= 1 && gi <= NGROUPS && fv > 0.f && fv < 5.f) g_set.gsize[gi - 1] = fv; }
        else if (sscanf(line, "gc%d=%d,%d,%d", &gi, &a, &b, &c3) == 4)
        {
            if (gi >= 1 && gi <= NGROUPS)
            {
                g_set.gcol[gi - 1][0] = (uint8_t)a; g_set.gcol[gi - 1][1] = (uint8_t)b; g_set.gcol[gi - 1][2] = (uint8_t)c3;
            }
        }
        else if (sscanf(line, "test=%d", &a) == 1) g_set.test = (a >= 0 && a <= 5) ? a : 0;
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
    static int lastE = -1, lastT = -1;
    if (lastE != (int)g_set.enabled || lastT != g_set.test)
    {
        lastE = (int)g_set.enabled; lastT = g_set.test;
        Log("cfg dibaca: enabled=%d test=%d speed=%.2f", (int)g_set.enabled, g_set.test, g_set.speed);
    }
}

// ---------- mesin pola ----------
static uint8_t g_lit[NGROUPS][MAXPTS];
static uint8_t g_litAny[NGROUPS];
static uint32_t g_litStamp = 0;
static bool g_litValid = false;

static void ComputeLit(uint32_t t)
{
    double now = (double)t * g_set.speed;
    for (int g = 0; g < NGROUPS; ++g)
    {
        memset(g_lit[g], 0, MAXPTS);
        g_litAny[g] = 0;
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
            if (g_lit[g][i]) g_litAny[g] = 1;
        }
    }
    g_litStamp = t;
    g_litValid = true;
}

// ---------- cache bohlam LED per kendaraan ----------
struct Lamp { int n; void* m[4]; void* atom; int cn; void* cm[2]; };
struct Cache
{
    void* self;
    void* clump;
    void* probe;
    bool valid;
    bool hidden;
    int natoms;
    void* atoms[512];
    int nAll;
    void* allMat[640];
    int nCase;
    void* caseMat[256];
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

// ---------- baca memori dengan aman (tidak crash kalau alamat tidak valid) ----------
static bool SafeRead(const void* src, void* dst, size_t n)
{
    static int pfd[2] = {-1, -1};
    if (!src || n == 0 || n > 4096) return false;
    if (pfd[0] < 0)
    {
        if (pipe(pfd) != 0) { pfd[0] = pfd[1] = -1; return false; }
    }
    ssize_t w = write(pfd[1], src, n);
    if (w < 0) return false;
    if ((size_t)w != n)
    {
        char tmp[4096];
        if (read(pfd[0], tmp, (size_t)w) < 0) {}
        return false;
    }
    return read(pfd[0], dst, n) == (ssize_t)n;
}

static bool ReadPtr(const void* base, int ofs, void** out)
{
    return SafeRead((const char*)base + ofs, out, sizeof(void*));
}
static bool ReadInt(const void* base, int ofs, int* out)
{
    return SafeRead((const char*)base + ofs, out, sizeof(int));
}

// letak field struktur RenderWare (bisa dikoreksi otomatis kalau tebakan awal salah)
struct Layout { int atomGeom, geomNV, geomMats, geomNMat, matColor; };
static Layout g_lay = { OFS_ATOM_GEOM, OFS_GEOM_NVERT, OFS_GEOM_MATS, OFS_GEOM_NMAT, OFS_MAT_COLOR };
static bool g_layChecked = false;

// cek geometry lewat layout tertentu; kalau valid isi jumlah & daftar material
static bool GeomOk(void* geom, const Layout& L, int* nmOut, void*** matsOut)
{
    if (!geom || ((uintptr_t)geom & 3)) return false;
    int nv = 0, nm = 0;
    void** mats = nullptr;
    if (!ReadInt(geom, L.geomNV, &nv) || nv < 1 || nv > 200000) return false;
    if (!ReadInt(geom, L.geomNMat, &nm) || nm < 1 || nm > 32) return false;
    if (!ReadPtr(geom, L.geomMats, (void**)&mats) || !mats || ((uintptr_t)mats & 3)) return false;
    for (int i = 0; i < nm; ++i)
    {
        void* m = nullptr;
        char probe[16];
        if (!ReadPtr(mats, i * (int)sizeof(void*), &m) || !m || ((uintptr_t)m & 3)) return false;
        if (!SafeRead(m, probe, sizeof(probe))) return false;
    }
    *nmOut = nm;
    *matsOut = mats;
    return true;
}

// log mentah beberapa kata memori supaya struktur bisa dianalisis
static void DumpWords(const char* label, const void* p, int bytes)
{
    unsigned char buf[256];
    if (bytes > 256) bytes = 256;
    if (!p || !SafeRead(p, buf, (size_t)bytes)) { Log("%s: tidak terbaca (%p)", label, p); return; }
    char line[600]; int n = 0;
    for (int i = 0; i + 3 < bytes; i += 4)
    {
        uint32_t w; memcpy(&w, buf + i, 4);
        n += snprintf(line + n, sizeof(line) - n, "%08x ", (unsigned)w);
        if (n > (int)sizeof(line) - 12) break;
    }
    Log("%s: %s", label, line);
}

static bool AutoDetectLayout(void* atom, void* geomHint)
{
    static const int aoffs[] = { 20, 16, 24, 28, 32, 36, 40, 12, 44 };
    static const int moffs[] = { 32, 36, 40, 44, 28, 48 };
    static const int voffs[] = { 20, 16, 24, 28, 32 };
    for (size_t ai = 0; ai < sizeof(aoffs) / sizeof(aoffs[0]); ++ai)
    {
        void* g = nullptr;
        if (!ReadPtr(atom, aoffs[ai], &g) || !g) continue;
        for (size_t mi = 0; mi < sizeof(moffs) / sizeof(moffs[0]); ++mi)
            for (size_t vi = 0; vi < sizeof(voffs) / sizeof(voffs[0]); ++vi)
            {
                Layout L = g_lay;
                L.atomGeom = aoffs[ai];
                L.geomMats = moffs[mi];
                L.geomNMat = moffs[mi] + (int)sizeof(void*);
                L.geomNV = voffs[vi];
                int nm; void** mats;
                if (GeomOk(g, L, &nm, &mats))
                {
                    g_lay = L;
                    Log("layout otomatis: atomGeom=%d nverts=%d mats=%d nmat=%d", L.atomGeom, L.geomNV, L.geomMats, L.geomNMat);
                    return true;
                }
            }
    }
    (void)geomHint;
    return false;
}

static void FixLedGeomFlags(Cache* c);

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

    // atomic LED pertama dipakai untuk memeriksa / menebak letak struktur
    if (!g_layChecked)
    {
        g_layChecked = true;
        void* fr = GetFrameFromName(clump, "lightbar-led-1");
        void* atom = nullptr;
        for (int a = 0; a < col.n; ++a) if (col.frame[a] == fr) { atom = col.atom[a]; break; }
        if (!atom) { Log("atomic untuk lightbar-led-1 tidak ketemu"); g_layChecked = false; c->valid = false; return c; }
        Log("atomic LED = %p", atom);
        DumpWords("  atomic (16 kata)", atom, 64);
        void* g0 = nullptr;
        if (ReadPtr(atom, g_lay.atomGeom, &g0)) DumpWords("  geometry (24 kata)", g0, 96);
        int nm = 0; void** mats = nullptr;
        bool ok = (((uint8_t*)atom)[0] == 1) && GeomOk(g0, g_lay, &nm, &mats);
        Log("layout standar %s", ok ? "COCOK" : "tidak cocok");
        if (!ok) ok = AutoDetectLayout(atom, g0);
        if (ok)
        {
            void* g1 = nullptr;
            ReadPtr(atom, g_lay.atomGeom, &g1);
            if (GeomOk(g1, g_lay, &nm, &mats))
            {
                void* m0 = nullptr;
                ReadPtr(mats, 0, &m0);
                DumpWords("  material pertama (8 kata)", m0, 32);
            }
        }
        else
        {
            Log("layout tidak ketemu, plugin dimatikan demi keamanan");
            g_ok = false;
            c->valid = false;
            return c;
        }
    }

    // semua material semua atomic (untuk mode tes 2)
    for (int a = 0; a < col.n; ++a)
    {
        void* geom = nullptr;
        int nm = 0; void** mats = nullptr;
        if (!ReadPtr(col.atom[a], g_lay.atomGeom, &geom) || !GeomOk(geom, g_lay, &nm, &mats)) continue;
        for (int m = 0; m < nm && c->nAll < 640; ++m)
        {
            void* mp = nullptr;
            if (ReadPtr(mats, m * (int)sizeof(void*), &mp) && mp) c->allMat[c->nAll++] = mp;
        }
    }

    c->natoms = col.n;
    for (int a = 0; a < col.n; ++a) c->atoms[a] = col.atom[a];
    int found = 0, missing = 0, bad = 0, linked = 0;
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
            void* geom = nullptr;
            int nm = 0; void** mats = nullptr;
            if (!ReadPtr(atom, g_lay.atomGeom, &geom) || !GeomOk(geom, g_lay, &nm, &mats)) { ++bad; continue; }
            Lamp& L = c->lamp[g][i];
            // bingkai: atomic yang frame-nya adalah induk frame bohlam
            void* pf = nullptr;
            if (ReadPtr(fr, OFS_FRAME_PARENT, &pf) && pf)
            {
                for (int a2 = 0; a2 < col.n; ++a2)
                {
                    if (col.frame[a2] != pf) continue;
                    void* g2 = nullptr; int nv2 = 0;
                    if (!ReadPtr(col.atom[a2], g_lay.atomGeom, &g2) || !g2 || !ReadInt(g2, g_lay.geomNV, &nv2)) break;
                    if (nv2 != 2342 && nv2 != 2280) break;
                    int nm2 = 0; void** mats2 = nullptr;
                    if (!GeomOk(g2, g_lay, &nm2, &mats2)) break;
                    L.cn = nm2 > 2 ? 2 : nm2;
                    for (int m2 = 0; m2 < L.cn; ++m2) ReadPtr(mats2, m2 * (int)sizeof(void*), &L.cm[m2]);
                    ++linked;
                    break;
                }
            }
            L.atom = atom;
            L.n = nm > 4 ? 4 : nm;
            for (int m = 0; m < L.n; ++m) ReadPtr(mats, m * (int)sizeof(void*), &L.m[m]);
            ++found;
        }
    }
    Log("cache: kendaraan=%p atomic=%d material=%d  bohlam ketemu=%d hilang=%d tidak valid=%d", self, col.n, c->nAll, found, missing, bad);
    if (found > 0) FixLedGeomFlags(c);
    // bingkai bohlam ("led-case" = 2342 vertex, "led berjejer*" = 2280 vertex): nyalakan flag kalikan-warna agar tampil sesuai warna materialnya
    {
        int cases = 0;
        for (int a = 0; a < col.n; ++a)
        {
            void* geom = nullptr;
            int nv = 0;
            if (!ReadPtr(col.atom[a], g_lay.atomGeom, &geom) || !geom || !ReadInt(geom, g_lay.geomNV, &nv)) continue;
            if (nv != 2342 && nv != 2280) continue;
            int nm = 0; void** mats = nullptr;
            if (!GeomOk(geom, g_lay, &nm, &mats)) continue;
            uint32_t f;
            if (SafeRead((char*)geom + 8, &f, 4)) { f |= 0x40u; memcpy((char*)geom + 8, &f, 4); }
            ++cases;
            for (int m = 0; m < nm && c->nCase < 256; ++m)
            {
                void* mp = nullptr;
                if (ReadPtr(mats, m * (int)sizeof(void*), &mp) && mp) c->caseMat[c->nCase++] = mp;
            }
        }
        Log("bingkai LED: atomic=%d material=%d  terhubung ke bohlam=%d", cases, c->nCase, linked);
    }
    static bool diagDone = false;
    if (!diagDone && found > 0)
    {
        diagDone = true;
        void* ledAtom = c->lamp[0][0].atom;
        void* bodyFrame = GetFrameFromName(clump, "chassis");
        void* bodyAtom = nullptr;
        for (int a = 0; a < col.n; ++a) if (bodyFrame && col.frame[a] == bodyFrame) { bodyAtom = col.atom[a]; break; }
        if (!bodyAtom && col.n > 0) bodyAtom = col.atom[0];
        const char* nm2[2] = { "LED", "BODI" };
        void* at2[2] = { ledAtom, bodyAtom };
        for (int k = 0; k < 2; ++k)
        {
            char lab[64];
            snprintf(lab, sizeof(lab), "DIAG atomic %s (32 kata)", nm2[k]);
            DumpWords(lab, at2[k], 128);
            void* gg = nullptr; uint32_t fl = 0;
            if (at2[k] && ReadPtr(at2[k], g_lay.atomGeom, &gg) && gg && SafeRead((char*)gg + 8, &fl, 4))
            {
                Log("DIAG geometry %s: flags=%08x", nm2[k], (unsigned)fl);
                snprintf(lab, sizeof(lab), "DIAG geometry %s (16 kata)", nm2[k]);
                DumpWords(lab, gg, 64);
            }
        }
    }
    if (bad > 0)
    {
        Log("sebagian struktur tidak valid, plugin dimatikan demi keamanan");
        g_ok = false;
        c->valid = false;
        return c;
    }
    c->valid = found > 0;
    return c;
}

static bool g_haveLast = false;
static uint8_t g_lastCol[3] = {0, 0, 0};
static int g_prevTest = -1;

static void SetLedVisible(Cache* c, bool visible)
{
    for (int g = 0; g < NGROUPS; ++g)
        for (int i = 0; i < GROUPS[g].npts && i < MAXPTS; ++i)
        {
            void* atom = c->lamp[g][i].atom;
            if (!atom) continue;
            uint8_t* flags = (uint8_t*)atom + 2;      // RwObject::flags, bit 4 = rpATOMICRENDER
            if (visible) *flags |= 0x04; else *flags &= (uint8_t)~0x04;
        }
}

static void FixLedGeomFlags(Cache* c)
{
    for (int g = 0; g < NGROUPS; ++g)
        for (int i = 0; i < GROUPS[g].npts && i < MAXPTS; ++i)
        {
            void* atom = c->lamp[g][i].atom;
            void* geom = nullptr;
            if (!atom || !ReadPtr(atom, g_lay.atomGeom, &geom) || !geom) continue;
            uint32_t f;
            if (!SafeRead((char*)geom + 8, &f, 4)) continue;
            f |= 0x40u;      // rpGEOMETRYMODULATEMATERIALCOLOR
            f &= ~0x08u;     // tanpa prelit (warna vertex bawaan)
            memcpy((char*)geom + 8, &f, 4);
        }
}

static void PaintAllRed(Cache* c)
{
    for (int k = 0; k < c->nAll; ++k)
    {
        uint8_t* p = (uint8_t*)c->allMat[k] + g_lay.matColor;
        p[0] = 255; p[1] = 0; p[2] = 0;
    }
}

static void ApplyColors(Cache* c)
{
    if (g_prevTest != g_set.test) { Log("mode tes -> %d", g_set.test); g_prevTest = g_set.test; }

    // mode 2: sembunyikan atomic LED (cek apakah kotak yang kelihatan memang atomic LED)
    if (g_set.test == 2) { SetLedVisible(c, false); c->hidden = true; return; }
    if (c->hidden) { SetLedVisible(c, true); c->hidden = false; }
    // mode 3: cat SEMUA material kendaraan merah (cek apakah warna material berpengaruh sama sekali)
    if (g_set.test == 3) { PaintAllRed(c); return; }

    if (g_set.test == 4) FixLedGeomFlags(c);
    if (g_set.test == 5)
    {
        for (int k = 0; k < c->nCase; ++k)
        {
            uint8_t* p = (uint8_t*)c->caseMat[k] + g_lay.matColor;
            p[0] = 255; p[1] = 0; p[2] = 0;
        }
    }

    // diagnostik: apakah warna yang kita tulis frame lalu ditimpa pihak lain?
    const Lamp* first = nullptr;
    for (int g = 0; g < NGROUPS && !first; ++g)
        for (int i = 0; i < GROUPS[g].npts && i < MAXPTS; ++i)
            if (c->lamp[g][i].n > 0) { first = &c->lamp[g][i]; break; }
    static int overwrites = 0;
    if (first && g_haveLast)
    {
        const uint8_t* cur = (const uint8_t*)first->m[0] + g_lay.matColor;
        if (memcmp(cur, g_lastCol, 3) != 0 && ++overwrites <= 6)
            Log("warna ditimpa pihak lain: ditulis=%02x%02x%02x terbaca=%02x%02x%02x", g_lastCol[0], g_lastCol[1], g_lastCol[2], cur[0], cur[1], cur[2]);
    }

    static const uint8_t RED[3] = { 255, 0, 0 };
    for (int g = 0; g < NGROUPS; ++g)
    {
        const Group& G = GROUPS[g];
        for (int i = 0; i < G.npts && i < MAXPTS; ++i)
        {
            const Lamp& L = c->lamp[g][i];
            if (L.n <= 0) continue;
            const uint8_t* col = (g_set.test == 1 || g_set.test == 4) ? RED : (g_lit[g][i] ? G.onc : G.offc);
            for (int m = 0; m < L.n; ++m)
            {
                uint8_t* p = (uint8_t*)L.m[m] + g_lay.matColor;
                p[0] = col[0]; p[1] = col[1]; p[2] = col[2];   // alpha tidak diubah
            }
            if (&L == first) { memcpy(g_lastCol, col, 3); g_haveLast = true; }
            if (L.cn > 0 && g_set.test != 1 && g_set.test != 4)
            {
                static const uint8_t BLACK[3] = { 0, 0, 0 };    // warna asli bingkai di DFF
                bool shared = G.npts <= 4;                      // batang LED: satu bingkai untuk 4 bohlam
                bool lit = shared ? (g_litAny[g] != 0) : (g_lit[g][i] != 0);
                const uint8_t* cc = (g_set.caseFollow && lit) ? G.onc : BLACK;
                for (int m = 0; m < L.cn; ++m)
                {
                    uint8_t* p = (uint8_t*)L.cm[m] + g_lay.matColor;
                    p[0] = cc[0]; p[1] = cc[1]; p[2] = cc[2];
                }
            }
        }
    }
}

static void RegisterLamps(void* self, Cache* c);
static void RegisterShadows(void* self, Cache* c);

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
    RegisterLamps(self, c);
    RegisterShadows(self, c);
}

DECL_HOOKv(CVehicle_PreRender, void* self)
{
    CVehicle_PreRender(self);
    if (g_ok) OnVehiclePreRender(self);
}

// ---------- corona dengan ID tetap (supaya tidak hilang saat mobil bergerak) ----------
// CCoronas::RegisterCorona (varian dengan coronaType) di libGTASA 32-bit:
//   (id, attachTo, r, g, b, a, CVector const& posn, radius, farClip, coronaType, flareType,
//    reflectionType, LOSCheck, unused, normalAngle, bNeonFade, pullTowardsCam, bFadeDestroy,
//    fadeSpeed, bOnlyFromBelow, bWhiteCore)
typedef void (*RegisterCorona_t)(unsigned int id, void* attachTo,
    unsigned char r, unsigned char g, unsigned char b, unsigned char a,
    const float* posn, float radius, float farClip,
    unsigned char coronaType, unsigned char flareType, unsigned char reflectionType,
    unsigned char LOSCheck, unsigned char unused, float normalAngle, bool bNeonFade,
    float pullTowardsCam, bool bFadeDestroy, float fadeSpeed, bool bOnlyFromBelow, bool bWhiteCore);
static RegisterCorona_t g_RegCorona = nullptr;
static int g_coronaCalls = 0;

// Dipanggil dari script Lua lewat FFI. Semua argumen bilangan bulat (milimeter) supaya
// tidak bergantung pada cara bilangan desimal dikirim antar-bahasa.
extern "C" __attribute__((visibility("default")))
void VSL_Corona(unsigned int id, int xmm, int ymm, int zmm, int rmm,
                int r, int g, int b, int type, int flare, int los)
{
    if (!g_RegCorona) return;
    float pos[3] = { xmm * 0.001f, ymm * 0.001f, zmm * 0.001f };
    g_RegCorona(id, nullptr, (unsigned char)r, (unsigned char)g, (unsigned char)b, 255,
                pos, rmm * 0.001f, 150.0f, (unsigned char)type, (unsigned char)flare, 0,
                (unsigned char)los, 0, 0.0f, false, 0.0f, false, 60.0f, false, false);
    if (g_coronaCalls < 3) { ++g_coronaCalls; Log("VSL_Corona dipanggil: id=%x pos=%.2f,%.2f,%.2f r=%.3f tipe=%d", id, pos[0], pos[1], pos[2], rmm * 0.001f, type); }
}

// ---------- corona didaftarkan dari plugin, tepat sebelum mobil digambar (tanpa selisih frame) ----------
static int g_camMm[3] = {0, 0, 0};
static bool g_camValid = false;
static uintptr_t g_ownPtr = 0;
static bool g_corOk = true;
static uint32_t g_regStamp = 0;
static int g_corLog = 0;

// matriks dunia kendaraan dari frame akar clump (RwMatrix: right, up(maju), at(atas), pos)
static bool GetWorldMatrix(void* clump, float* m)
{
    void* root = nullptr;
    if (!ReadPtr(clump, OFS_ATOM_PARENT, &root) || !root || ((uintptr_t)root & 3)) return false;
    if (!SafeRead((char*)root + OFS_FRAME_MAT, m, 64)) return false;
    for (int o = 0; o <= 8; o += 4)
    {
        float l = sqrtf(m[o] * m[o] + m[o + 1] * m[o + 1] + m[o + 2] * m[o + 2]);
        if (!(l > 0.9f && l < 1.1f)) return false;
    }
    float d = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
    if (!(fabsf(d) < 0.1f)) return false;
    for (int k = 12; k < 15; ++k) if (!(fabsf(m[k]) < 20000.f)) return false;
    return true;
}

static void RegisterLamps(void* self, Cache* c)
{
    if (!g_RegCorona || !g_corOk || !g_set.corona || !g_set.enabled) return;
    if (!g_set.all && (uintptr_t)self != g_ownPtr) return;
    float m[16];
    if (!GetWorldMatrix(c->clump, m))
    {
        if (g_corLog < 5) { ++g_corLog; Log("matriks dunia kendaraan tidak valid, corona plugin dimatikan"); }
        g_corOk = false;
        return;
    }
    const bool useCull = g_set.cull && g_camValid;
    const float cam[3] = { g_camMm[0] * 0.001f, g_camMm[1] * 0.001f, g_camMm[2] * 0.001f };
    const unsigned slot = (unsigned)(c - g_cache) & 15u;
    int registered = 0;
    for (int g = 0; g < NGROUPS; ++g)
    {
        const Group& G = GROUPS[g];
        const float nx = m[0] * G.nrm[0] + m[4] * G.nrm[1] + m[8] * G.nrm[2];
        const float ny = m[1] * G.nrm[0] + m[5] * G.nrm[1] + m[9] * G.nrm[2];
        const float nz = m[2] * G.nrm[0] + m[6] * G.nrm[1] + m[10] * G.nrm[2];
        const float rad = g_set.gsize[g] * g_set.scale;
        for (int i = 0; i < G.npts && i < MAXPTS; ++i)
        {
            const float x = G.pts[i][0], y = G.pts[i][1], z = G.pts[i][2];
            float p[3] = { m[12] + m[0] * x + m[4] * y + m[8] * z,
                           m[13] + m[1] * x + m[5] * y + m[9] * z,
                           m[14] + m[2] * x + m[6] * y + m[10] * z };
            bool show = true;
            if (useCull)
            {
                float dx = cam[0] - p[0], dy = cam[1] - p[1], dz = cam[2] - p[2];
                float d = sqrtf(dx * dx + dy * dy + dz * dz) + 1e-6f;
                show = (nx * dx + ny * dy + nz * dz) / d > g_set.bias;
            }
            const unsigned id = 0x56000000u | (slot << 10) | (unsigned)(g * 20 + i);
            if (g_lit[g][i] && show)
            {
                g_RegCorona(id, nullptr, g_set.gcol[g][0], g_set.gcol[g][1], g_set.gcol[g][2], 255, p, rad, 150.0f,
                            (unsigned char)G.ctype, (unsigned char)G.flare, 0, (unsigned char)g_set.los, 0, 0.0f, false, 0.0f, false, 60.0f, false, false);
                if (G.small)
                    g_RegCorona(id + 0x200u, nullptr, 255, 255, 255, 255, p, rad * G.smallScale, 150.0f,
                                (unsigned char)G.smallType, 0, 0, (unsigned char)g_set.los, 0, 0.0f, false, 0.0f, false, 60.0f, false, false);
            }
            else
            {
                // hitam = tidak terlihat, tapi tetap terdaftar dan ikut bergerak bersama mobil
                g_RegCorona(id, nullptr, 0, 0, 0, 255, p, rad, 150.0f,
                            (unsigned char)G.ctype, (unsigned char)G.flare, 0, 0, 0, 0.0f, false, 0.0f, false, 60.0f, false, false);
            }
            ++registered;
        }
    }
    if (g_pTime) g_regStamp = *g_pTime;
    if (g_corLog < 3 && registered > 0)
    {
        ++g_corLog;
        Log("corona plugin: kendaraan=%p terdaftar=%d pos=%.1f,%.1f,%.1f kamera=%s", self, registered, m[12], m[13], m[14], useCull ? "ya" : "tidak");
    }
}

// ---------- pendar di tanah (shadow additif bertekstur, seperti shadow VSL di PC) ----------
// CShadows::StoreCarLightShadow(kendaraan, id, tekstur, posisi, frontX, frontY, sideX, sideY, r, g, b, maxViewAngle)
typedef void (*StoreCarLightShadow_t)(void* veh, int id, void* tex, float* posn, float frontX, float frontY,
                                      float sideX, float sideY, unsigned char r, unsigned char g, unsigned char b, float maxViewAngle);
typedef void* (*RwRasterCreate_t)(int w, int h, int depth, int flags);
typedef uint8_t* (*RwRasterLock_t)(void* raster, unsigned char level, int mode);
typedef void* (*RwRasterUnlock_t)(void* raster);
typedef void* (*RwTextureCreate_t)(void* raster);
typedef void* (*RwTextureSetName_t)(void* tex, const char* name);
static StoreCarLightShadow_t g_StoreShadow = nullptr;
static RwRasterCreate_t g_RasterCreate = nullptr;
static RwRasterLock_t g_RasterLock = nullptr;
static RwRasterUnlock_t g_RasterUnlock = nullptr;
static RwTextureCreate_t g_TextureCreate = nullptr;
static RwTextureSetName_t g_TextureSetName = nullptr;
// CShadows::StoreShadowToBeRendered(tipe, tekstur, posisi, frontX, frontY, sideX, sideY, intensitas, r, g, b, zDist, airJuga, skala, shadowRealtime, gambarDiBangunan)
typedef void (*StoreShadowEx_t)(unsigned char type, void* tex, float* posn, float frontX, float frontY, float sideX, float sideY,
                                short intensity, unsigned char r, unsigned char g, unsigned char b,
                                float zDist, bool onWater, float scale, void* rts, bool onBuildings);
static StoreShadowEx_t g_StoreShadowEx = nullptr;
static bool g_shExLogged = false;
static int g_shRGB[3] = { 0, 0, 0 };
static void* g_shTex[2] = { nullptr, nullptr };
static void* g_shRaster[2] = { nullptr, nullptr };
static void* g_lastRaster = nullptr;
static int g_shState = 0;   // 0 = belum dicoba, 1 = siap, 2 = gagal (shadow mati, lampu & corona tetap jalan)
static int g_shLog = 0;

static void* MakeShadowTexture(const uint8_t* px, const char* name)
{
    void* r = g_RasterCreate(SH_TEX_SIZE, SH_TEX_SIZE, 32, 0x504);   // tipe tekstur + format 8888
    if (!r) { Log("shadow: RwRasterCreate gagal (%s)", name); return nullptr; }
    uint8_t* d = g_RasterLock(r, 0, 2);                              // 2 = tulis
    g_lastRaster = r;
    char probe[16];
    if (!d || !SafeRead(d, probe, sizeof(probe))) { Log("shadow: RwRasterLock gagal (%s) ptr=%p", name, (void*)d); return nullptr; }
    for (int k = 0; k < SH_TEX_SIZE * SH_TEX_SIZE; ++k)
    {
        d[k * 4 + 0] = px[k]; d[k * 4 + 1] = px[k]; d[k * 4 + 2] = px[k]; d[k * 4 + 3] = px[k];
    }
    g_RasterUnlock(r);
    void* t = g_TextureCreate(r);
    if (!t) { Log("shadow: RwTextureCreate gagal (%s)", name); return nullptr; }
    if (g_TextureSetName) g_TextureSetName(t, name);
    Log("shadow: tekstur %s dibuat (%p)", name, t);
    return t;
}

static bool EnsureShadowTex()
{
    if (g_shState == 1) return true;
    if (g_shState == 2) return false;
    g_shState = 2;
    if (!g_StoreShadow || !g_RasterCreate || !g_RasterLock || !g_RasterUnlock || !g_TextureCreate)
    {
        Log("shadow: fungsi game tidak lengkap, shadow dimatikan");
        return false;
    }
    g_shTex[0] = MakeShadowTexture(SH_TEX2, "vslsh2");
    g_shRaster[0] = g_lastRaster;
    g_shTex[1] = MakeShadowTexture(SH_TEX3, "vslsh3");
    g_shRaster[1] = g_lastRaster;
    if (!g_shTex[0] || !g_shTex[1]) return false;
    g_shState = 1;
    return true;
}

// tekstur shadow yang warnanya "ditanam" (dipakai kalau fungsi game mengabaikan warna kiriman)
struct TintTex { int tex, r, g, b; void* t; };
static TintTex g_tint[24];
static int g_nTint = 0;

static void* GetTintTexture(int ti, int r, int g, int b)
{
    if (!g_RasterCreate || !g_RasterLock || !g_RasterUnlock || !g_TextureCreate) return nullptr;
    r = (r < 0 ? 0 : (r > 255 ? 255 : r)); g = (g < 0 ? 0 : (g > 255 ? 255 : g)); b = (b < 0 ? 0 : (b > 255 ? 255 : b));
    r = ((r * 15 + 127) / 255) * 17; g = ((g * 15 + 127) / 255) * 17; b = ((b * 15 + 127) / 255) * 17;   // 16 tingkat per kanal
    for (int i = 0; i < g_nTint; ++i)
        if (g_tint[i].tex == ti && g_tint[i].r == r && g_tint[i].g == g && g_tint[i].b == b) return g_tint[i].t;
    if (g_nTint >= 24) return nullptr;
    TintTex& e = g_tint[g_nTint++];
    e.tex = ti; e.r = r; e.g = g; e.b = b; e.t = nullptr;
    const uint8_t* px = (ti == 0) ? SH_TEX2 : SH_TEX3;
    void* rs = g_RasterCreate(SH_TEX_SIZE, SH_TEX_SIZE, 32, 0x504);
    if (!rs) return nullptr;
    uint8_t* d = g_RasterLock(rs, 0, 2);
    char probe[16];
    if (!d || !SafeRead(d, probe, sizeof(probe))) return nullptr;
    for (int k = 0; k < SH_TEX_SIZE * SH_TEX_SIZE; ++k)
    {
        d[k * 4 + 0] = (uint8_t)(px[k] * r / 255);
        d[k * 4 + 1] = (uint8_t)(px[k] * g / 255);
        d[k * 4 + 2] = (uint8_t)(px[k] * b / 255);
        d[k * 4 + 3] = px[k];
    }
    g_RasterUnlock(rs);
    void* t = g_TextureCreate(rs);
    if (t && g_TextureSetName)
    {
        char nm[32];
        snprintf(nm, sizeof(nm), "vslt%d_%02x%02x%02x", ti, r, g, b);
        g_TextureSetName(t, nm);
    }
    e.t = t;
    Log("shadow: tekstur berwarna %d rgb=%d,%d,%d dibuat (%p)", ti, r, g, b, t);
    return t;
}

// ---------- gambar sendiri: kerucut shadow VSL digambar langsung ke tanah (warna bebas) ----------
typedef void* (*Im3DTransform_t)(void* verts, unsigned int n, void* ltm, unsigned int flags);
typedef int (*Im3DRender_t)(int primType, unsigned short* idx, int n);
typedef int (*Im3DEnd_t)(void);
typedef int (*RwRSSet_t)(int state, void* val);
typedef int (*RwRSGet_t)(int state, void* val);
typedef float (*FindGroundZ_t)(float x, float y, float z, bool* found, void** ent);
static Im3DTransform_t g_Im3DTransform = nullptr;
static Im3DRender_t g_Im3DRender = nullptr;
static Im3DEnd_t g_Im3DEnd = nullptr;
static RwRSSet_t g_RSSet = nullptr;
static RwRSGet_t g_RSGet = nullptr;
static FindGroundZ_t g_FindGroundZ = nullptr;
static bool g_imOk = false;

struct Im3DVert { float x, y, z, nx, ny, nz; uint32_t col; float u, v; };
struct GQuad { float p[4][3]; uint32_t col; int tex; };
#define MAXQ 96
static GQuad g_quad[MAXQ];
static int g_nQuad = 0;
static int g_imLog = 0;

static void ImMarkerPath(char* out, size_t n)
{
    snprintf(out, n, "%s", g_cfgPath);
    char* p = strstr(out, "vsl_cfg.txt");
    if (p && (size_t)(p - out) + 20 < n) strcpy(p, "vsl_im3d_try.txt");
}
static int g_imFrames = 0;
static bool g_imMarked = false;

static void DrawVslGroundGlow()
{
    if (!g_imOk || g_nQuad <= 0) { g_nQuad = 0; return; }
    const bool dbg = (g_imLog < 3);
    if (dbg) ++g_imLog;
    if (!g_imMarked)
    {
        // penanda: kalau game crash saat menggambar, sesi berikutnya mode ini otomatis dimatikan
        g_imMarked = true;
        char mp[420];
        ImMarkerPath(mp, sizeof(mp));
        FILE* mf = fopen(mp, "w");
        if (mf) { fputs("1", mf); fclose(mf); }
    }
    const int RS_RASTER = 1, RS_ZTEST = 6, RS_ZWRITE = 8, RS_SRC = 10, RS_DST = 11, RS_VALPHA = 12;
    const int ids[6] = { RS_ZTEST, RS_ZWRITE, RS_SRC, RS_DST, RS_VALPHA, RS_RASTER };
    const intptr_t defs[6] = { 1, 1, 5, 6, 0, 0 };   // nilai aman kalau Get tidak mengisi hasil
    intptr_t old[6];
    bool have[6];
    if (dbg) Log("im3d: 1 n=%d simpan state", g_nQuad);
    {
        void* const SENT = (void*)0x7ffffff1;
        for (int k = 0; k < 6; ++k)
        {
            void* v = SENT;
            if (g_RSGet) g_RSGet(ids[k], &v);
            have[k] = (v != SENT);
            old[k] = (intptr_t)v;
            if (have[k] && (k == 2 || k == 3) && (old[k] < 1 || old[k] > 11)) have[k] = false;   // blend tidak valid
            if (have[k] && k <= 1 && (old[k] != 0 && old[k] != 1)) have[k] = false;
            if (!have[k]) old[k] = defs[k];
        }
    }
    if (dbg) Log("im3d: 2 set state");
    g_RSSet(RS_ZTEST, (void*)1);
    g_RSSet(RS_ZWRITE, (void*)0);
    g_RSSet(RS_SRC, (void*)2);      // ONE
    g_RSSet(RS_DST, (void*)2);      // ONE  -> cahaya ditambahkan ke tanah
    g_RSSet(RS_VALPHA, (void*)1);
    static unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
    static const float U[4] = { 0.f, 1.f, 1.f, 0.f }, V[4] = { 0.f, 0.f, 1.f, 1.f };
    for (int q = 0; q < g_nQuad; ++q)
    {
        void* ras = g_shRaster[g_quad[q].tex];
        if (!ras) continue;
        if (dbg && q == 0) Log("im3d: 3 raster=%p", ras);
        g_RSSet(RS_RASTER, ras);
        Im3DVert vs[4];
        for (int k = 0; k < 4; ++k)
        {
            vs[k].x = g_quad[q].p[k][0]; vs[k].y = g_quad[q].p[k][1]; vs[k].z = g_quad[q].p[k][2];
            vs[k].nx = 0.f; vs[k].ny = 0.f; vs[k].nz = 1.f;
            vs[k].col = g_quad[q].col; vs[k].u = U[k]; vs[k].v = V[k];
        }
        if (dbg && q == 0) Log("im3d: 4 Transform pos=%.1f,%.1f,%.1f", vs[0].x, vs[0].y, vs[0].z);
        void* tr = g_Im3DTransform(vs, 4, nullptr, 1);   // 1 = rwIM3D_VERTEXUV
        if (dbg && q == 0) Log("im3d: 5 Transform hasil=%p", tr);
        if (tr)
        {
            g_Im3DRender(3, idx, 6);               // 3 = daftar segitiga
            if (dbg && q == 0) Log("im3d: 6 Render selesai");
            g_Im3DEnd();
            if (dbg && q == 0) Log("im3d: 7 End selesai");
        }
    }
    if (dbg) Log("im3d: 8 kembalikan state");
    for (int k = 0; k < 6; ++k) g_RSSet(ids[k], (void*)old[k]);
    if (dbg) Log("im3d: 9 frame pertama aman");
    g_nQuad = 0;
    if (++g_imFrames == 120)
    {
        char mp[420];
        ImMarkerPath(mp, sizeof(mp));
        remove(mp);   // 120 frame aman: penanda dihapus
    }
}

DECL_HOOKv(CShadows_RenderStored, bool b)
{
    CShadows_RenderStored(b);
    DrawVslGroundGlow();
}

static void RegisterShadows(void* self, Cache* c)
{
    if (!g_set.shOn || !g_set.enabled || g_shState == 2) return;
    if (!g_set.all && (uintptr_t)self != g_ownPtr) return;
    if (!EnsureShadowTex()) return;
    int mode = g_set.shMode;
    if (mode == 2 && !g_imOk) mode = 0;
    float m[16];
    if (!GetWorldMatrix(c->clump, m)) return;
    const unsigned slot = (unsigned)(c - g_cache) & 15u;
    int stored = 0;
    for (int k = 0; k < NSHADOWS; ++k)
    {
        const ShadowPt& S = SHADOWS[k];
        if (!g_lit[S.g][S.i]) continue;
        const Group& G = GROUPS[S.g];
        const float lx = G.pts[S.i][0] + S.ox, ly = G.pts[S.i][1] + S.oy, lz = G.pts[S.i][2];
        float p[3] = { m[12] + m[0] * lx + m[4] * ly + m[8] * lz,
                       m[13] + m[1] * lx + m[5] * ly + m[9] * lz,
                       m[14] + m[2] * lx + m[6] * ly + m[10] * lz };
        const float a = (S.ang + g_set.shAng) * 0.0174532925f;
        const float fl0 = -sinf(a), fl1 = cosf(a);      // arah "depan" shadow (lokal kendaraan)
        const float sl0 = cosf(a), sl1 = sinf(a);       // arah "samping" shadow (lokal kendaraan)
        const float fs = (g_set.shFlip ? -1.f : 1.f) * S.h * g_set.shScale;
        const float ss = S.w * g_set.shScale;
        const float fx = (m[0] * fl0 + m[4] * fl1) * fs, fy = (m[1] * fl0 + m[5] * fl1) * fs;
        const float sx = (m[0] * sl0 + m[4] * sl1) * ss, sy = (m[1] * sl0 + m[5] * sl1) * ss;
        float k2 = S.inten * g_set.shInt;
        int r = (int)(g_set.gcol[S.g][0] * k2), gg = (int)(g_set.gcol[S.g][1] * k2), b = (int)(g_set.gcol[S.g][2] * k2);
        if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
        const int id = (int)(((uintptr_t)self & 0x0FFFFFFFu) + 0x100u * slot + (unsigned)k);
        if (mode == 2)
        {
            if (g_nQuad < MAXQ)
            {
                float gz = m[14] - 0.6f;
                if (g_FindGroundZ)
                {
                    bool found = false; void* ent = nullptr;
                    float z2 = g_FindGroundZ(p[0], p[1], p[2] + 1.0f, &found, &ent);
                    if (found && z2 > -50.f && z2 < p[2] + 1.5f) gz = z2;
                }
                gz += 0.04f;
                GQuad& Q = g_quad[g_nQuad++];
                const float sgn[4][2] = { {-1.f, 1.f}, {1.f, 1.f}, {1.f, -1.f}, {-1.f, -1.f} };   // (samping, depan)
                for (int k = 0; k < 4; ++k)
                {
                    Q.p[k][0] = p[0] + sgn[k][0] * sx + sgn[k][1] * fx;
                    Q.p[k][1] = p[1] + sgn[k][0] * sy + sgn[k][1] * fy;
                    Q.p[k][2] = gz;
                }
                int cr = (int)(g_set.gcol[S.g][0] * k2), cg = (int)(g_set.gcol[S.g][1] * k2), cb = (int)(g_set.gcol[S.g][2] * k2);
                if (cr > 255) cr = 255; if (cg > 255) cg = 255; if (cb > 255) cb = 255;
                if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cb < 0) cb = 0;
                Q.col = 0xFF000000u | ((uint32_t)cb << 16) | ((uint32_t)cg << 8) | (uint32_t)cr;
                Q.tex = (S.tex == 2) ? 0 : 1;
                g_shRGB[0] = cr; g_shRGB[1] = cg; g_shRGB[2] = cb;
                ++stored;
            }
            continue;
        }
        void* tx = g_shTex[S.tex == 2 ? 0 : 1];
        int sr = r, sg = gg, sb = b;
        if (g_set.shTint)
        {
            void* tt = GetTintTexture(S.tex == 2 ? 0 : 1, (int)g_set.gcol[S.g][0], (int)g_set.gcol[S.g][1], (int)g_set.gcol[S.g][2]);
            if (tt)
            {
                tx = tt;
                int v = (int)(255.f * k2);
                if (v > 255) v = 255;
                if (v < 0) v = 0;
                sr = sg = sb = v;
            }
        }
        if (mode == 1 && g_StoreShadowEx)
        {
            const bool tinted = (tx != g_shTex[S.tex == 2 ? 0 : 1]);
            int inten = (int)(255.f * k2);
            if (inten > 255) inten = 255;
            if (inten < 0) inten = 0;
            int dr = tinted ? 255 : (int)g_set.gcol[S.g][0];
            int dg = tinted ? 255 : (int)g_set.gcol[S.g][1];
            int db = tinted ? 255 : (int)g_set.gcol[S.g][2];
            if (dr < 0) dr = 0; if (dr > 255) dr = 255;
            if (dg < 0) dg = 0; if (dg > 255) dg = 255;
            if (db < 0) db = 0; if (db > 255) db = 255;
            if (!g_shExLogged) { g_shExLogged = true; Log("shadow: memanggil StoreShadowToBeRendered tipe=%d (kalau crash, baris ini terakhir)", g_set.shType); }
            g_StoreShadowEx((unsigned char)g_set.shType, tx, p, fx, fy, sx, sy, (short)inten,
                            (unsigned char)dr, (unsigned char)dg, (unsigned char)db, 15.0f, false, 1.0f, nullptr, false);
            sr = dr; sg = dg; sb = db;
        }
        else
        {
            g_StoreShadow(self, id, tx, p, fx, fy, sx, sy, (unsigned char)sr, (unsigned char)sg, (unsigned char)sb, g_set.shVA);
        }
        g_shRGB[0] = sr; g_shRGB[1] = sg; g_shRGB[2] = sb;
        ++stored;
    }
    if (g_shLog < 3 && stored > 0)
    {
        ++g_shLog;
        Log("shadow: mode=%d(aktif %d) tipe=%d tint=%d kendaraan=%p dikirim=%d rgb=%d,%d,%d", g_set.shMode, mode, g_set.shType, g_set.shTint ? 1 : 0, self, stored, g_shRGB[0], g_shRGB[1], g_shRGB[2]);
    }
}

// Dipanggil script Lua sekali per frame: posisi kamera (mm), penunjuk kendaraan aktif, bendera (bit0 = kamera valid).
// Mengembalikan 1 kalau plugin yang menggambar corona (script tidak perlu menggambar sendiri).
extern "C" __attribute__((visibility("default")))
int VSL_Frame(int camx, int camy, int camz, unsigned int ownPtr, int flags)
{
    g_camMm[0] = camx; g_camMm[1] = camy; g_camMm[2] = camz;
    g_camValid = (flags & 1) != 0;
    g_ownPtr = ownPtr;
    if (!g_RegCorona || !g_corOk || !g_set.corona || !g_set.enabled || !g_pTime) return 0;
    return (*g_pTime - g_regStamp) < 250u ? 1 : 0;
}

static void PublishApi()
{
    char p[400];
    snprintf(p, sizeof(p), "/storage/emulated/0/Android/data/%s/files/vsl_api.txt", g_pkg);
    FILE* f = fopen(p, "w");
    if (!f) { Log("tidak bisa menulis %s", p); return; }
    fprintf(f, "%lu\n%lu\n", (unsigned long)(uintptr_t)&VSL_Corona, (unsigned long)(uintptr_t)&VSL_Frame);
    fclose(f);
    Log("alamat VSL_Corona dipublikasikan ke %s (%p)", p, (void*)&VSL_Corona);
}

// ---------- cari fungsi corona game (untuk perbaikan corona yang hilang saat bergerak) ----------
// cari path file libGTASA.so dari /proc/self/maps
static bool FindLibPath(char* out, size_t outSize)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    bool ok = false;
    while (fgets(line, sizeof(line), f))
    {
        if (!strstr(line, "libGTASA.so")) continue;
        char* p = strchr(line, '/');
        if (!p) continue;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
        snprintf(out, outSize, "%s", p);
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}

static bool ReadBlock(FILE* f, long fileSize, uint32_t off, uint32_t size, std::vector<char>& out)
{
    if (size == 0 || size > 80u * 1024u * 1024u) return false;
    if ((long)off < 0 || (long)off + (long)size > fileSize) return false;
    out.resize(size);
    if (fseek(f, (long)off, SEEK_SET) != 0) return false;
    return fread(out.data(), 1, size, f) == size;
}

// baca tabel simbol ekspor (.dynsym) dari file .so dan log nama yang relevan
// nama simbol RenderWare/game yang dicari lewat pencocokan sebagian nama (supaya tidak bergantung tebakan nama mangled)
static const char* NEED[] = { "RwIm3DTransform", "RwIm3DRenderIndexedPrimitive", "RwIm3DEnd", "RwRenderStateSet",
                              "RwRenderStateGet", "CWorld21FindGroundZFor3DCoord", "CShadows19RenderStoredShadowsEb" };
#define NNEED 7
static char g_needName[NNEED][200];

static void ScanExports(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { Log("tidak bisa buka %s", path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS32)
    {
        Log("bukan ELF 32-bit: %s", path);
        fclose(f);
        return;
    }
    if (eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize != sizeof(Elf32_Shdr))
    {
        Log("file tidak punya section header, scan ekspor dilewati");
        fclose(f);
        return;
    }
    std::vector<char> shraw;
    if (!ReadBlock(f, sz, eh.e_shoff, (uint32_t)eh.e_shnum * sizeof(Elf32_Shdr), shraw))
    {
        Log("gagal baca section header");
        fclose(f);
        return;
    }
    const Elf32_Shdr* sh = (const Elf32_Shdr*)shraw.data();

    const char* keys[] = { "RwIm3D", "RwRenderState" };
    const int nKeys = (int)(sizeof(keys) / sizeof(keys[0]));

    for (int s = 0; s < eh.e_shnum; ++s)
    {
        if (sh[s].sh_type != SHT_DYNSYM) continue;
        if (sh[s].sh_link >= eh.e_shnum) continue;
        const Elf32_Shdr& str = sh[sh[s].sh_link];
        std::vector<char> symraw, strraw;
        if (!ReadBlock(f, sz, sh[s].sh_offset, sh[s].sh_size, symraw)) { Log("gagal baca dynsym"); continue; }
        if (!ReadBlock(f, sz, str.sh_offset, str.sh_size, strraw)) { Log("gagal baca dynstr"); continue; }
        const Elf32_Sym* syms = (const Elf32_Sym*)symraw.data();
        size_t count = symraw.size() / sizeof(Elf32_Sym);
        Log("dynsym: %u simbol total", (unsigned)count);
        int shown = 0;
        for (size_t i = 0; i < count && shown < 700; ++i)
        {
            if (syms[i].st_shndx == SHN_UNDEF) continue;
            if (syms[i].st_name >= strraw.size()) continue;
            const char* name = strraw.data() + syms[i].st_name;
            // pastikan string berakhir di dalam buffer
            size_t maxLen = strraw.size() - syms[i].st_name;
            if (strnlen(name, maxLen) == maxLen) continue;
            for (int n = 0; n < NNEED; ++n)
            {
                if (!g_needName[n][0] && name[0] == '_' && name[1] == 'Z' && strstr(name, NEED[n]) && strlen(name) < sizeof(g_needName[n]))
                {
                    strcpy(g_needName[n], name);
                    Log("EXPORT dicari: %s", name);
                }
            }
            for (int k = 0; k < nKeys; ++k)
            {
                if (strstr(name, keys[k]))
                {
                    Log("EXPORT %s", name);
                    ++shown;
                    break;
                }
            }
        }
        Log("EXPORT dicatat: %d", shown);
    }
    fclose(f);
}


ON_MOD_LOAD()
{
    InitPaths();
    Log("== VSL Huntley LED 0.1 mulai (shadow) ==");
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
    g_RegCorona = (RegisterCorona_t)aml->GetSym(h, "_ZN8CCoronas14RegisterCoronaEjP7CEntityhhhhRK7CVectorffhhhhhfbfbfbb");
    Log("CCoronas::RegisterCorona = %p", (void*)g_RegCorona);
    if (g_RegCorona) PublishApi();
    g_StoreShadow = (StoreCarLightShadow_t)aml->GetSym(h, "_ZN8CShadows19StoreCarLightShadowEP8CVehicleiP9RwTextureP7CVectorffffhhhf");
    g_RasterCreate = (RwRasterCreate_t)aml->GetSym(h, "_Z14RwRasterCreateiiii");
    g_RasterLock = (RwRasterLock_t)aml->GetSym(h, "_Z12RwRasterLockP8RwRasterhi");
    g_RasterUnlock = (RwRasterUnlock_t)aml->GetSym(h, "_Z14RwRasterUnlockP8RwRaster");
    g_TextureCreate = (RwTextureCreate_t)aml->GetSym(h, "_Z15RwTextureCreateP8RwRaster");
    g_TextureSetName = (RwTextureSetName_t)aml->GetSym(h, "_Z16RwTextureSetNameP9RwTexturePKc");
    g_StoreShadowEx = (StoreShadowEx_t)aml->GetSym(h, "_ZN8CShadows23StoreShadowToBeRenderedEhP9RwTextureP7CVectorffffshhhfbfP15CRealTimeShadowb");
    Log("shadow: StoreShadowToBeRendered(tekstur)=%p", (void*)g_StoreShadowEx);
    Log("shadow: StoreCarLightShadow=%p RasterCreate=%p Lock=%p Unlock=%p TextureCreate=%p SetName=%p",
        (void*)g_StoreShadow, (void*)g_RasterCreate, (void*)g_RasterLock, (void*)g_RasterUnlock, (void*)g_TextureCreate, (void*)g_TextureSetName);
    char libPath[512];
    if (FindLibPath(libPath, sizeof(libPath))) ScanExports(libPath);
    else Log("path libGTASA tidak ketemu di /proc/self/maps");

    // simbol untuk mode "gambar sendiri" (dicari lewat nama hasil scan)
    auto res = [&](int n) -> uintptr_t { return g_needName[n][0] ? aml->GetSym(h, g_needName[n]) : 0; };
    g_Im3DTransform = (Im3DTransform_t)res(0);
    g_Im3DRender = (Im3DRender_t)res(1);
    g_Im3DEnd = (Im3DEnd_t)res(2);
    g_RSSet = (RwRSSet_t)res(3);
    g_RSGet = (RwRSGet_t)res(4);
    g_FindGroundZ = (FindGroundZ_t)res(5);
    uintptr_t rss = res(6);
    Log("im3d: Transform=%p Render=%p End=%p RSSet=%p RSGet=%p GroundZ=%p RenderStoredShadows=0x%x",
        (void*)g_Im3DTransform, (void*)g_Im3DRender, (void*)g_Im3DEnd, (void*)g_RSSet, (void*)g_RSGet, (void*)g_FindGroundZ, (unsigned)rss);
    char imMark[420];
    ImMarkerPath(imMark, sizeof(imMark));
    FILE* imf = fopen(imMark, "r");
    const bool imCrashed = (imf != nullptr);
    if (imf) fclose(imf);
    if (imCrashed) Log("im3d: sesi sebelumnya crash saat menggambar, mode gambar sendiri dimatikan (hapus file vsl_im3d_try.txt untuk mencoba lagi)");
    if (!imCrashed && g_Im3DTransform && g_Im3DRender && g_Im3DEnd && g_RSSet && rss)
    {
        HOOKSYM(CShadows_RenderStored, h, g_needName[6]);
        g_imOk = true;
        Log("im3d: hook RenderStoredShadows terpasang, mode gambar sendiri siap");
    }
    else Log("im3d: simbol tidak lengkap, mode gambar sendiri dimatikan");
}
