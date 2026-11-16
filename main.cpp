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
  #define OFS_FRAME_PARENT 8
#else
  #define OFS_ENT_CLUMP   0x18  // CEntity::m_pRwObject
  #define OFS_ATOM_PARENT 4     // RwObject::parent (frame milik atomic)
  #define OFS_ATOM_GEOM   20    // RpAtomic::geometry
  #define OFS_GEOM_NVERT  20    // RpGeometry::numVertices
  #define OFS_GEOM_MATS   32    // RpGeometry::matList.materials
  #define OFS_GEOM_NMAT   36    // RpGeometry::matList.numMaterials
  #define OFS_MAT_COLOR   4     // RpMaterial::color (RGBA)
  #define OFS_FRAME_PARENT 4    // RwFrame::object.parent (frame induk)
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
        else if (sscanf(line, "case=%d", &a) == 1) g_set.caseFollow = a != 0;
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

    const char* keys[] = { "CCoronas", "RegisterCorona", "Corona" };
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
        for (size_t i = 0; i < count && shown < 900; ++i)
        {
            if (syms[i].st_shndx == SHN_UNDEF) continue;
            if (syms[i].st_name >= strraw.size()) continue;
            const char* name = strraw.data() + syms[i].st_name;
            // pastikan string berakhir di dalam buffer
            size_t maxLen = strraw.size() - syms[i].st_name;
            if (strnlen(name, maxLen) == maxLen) continue;
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
    char libPath[512];
    if (FindLibPath(libPath, sizeof(libPath))) ScanExports(libPath);
    else Log("path libGTASA tidak ketemu di /proc/self/maps");
}
