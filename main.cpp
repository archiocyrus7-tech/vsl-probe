// VSL Probe 0.1 - plugin AML pengintai (TIDAK mengubah apa pun di game).
// Tugas: cek fungsi RenderWare / GTA yang tersedia di libGTASA.so, lalu tulis ke vsl_probe.log
#include <mod/amlmod.h>
#include <android/log.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <vector>

MYMOD(vsl.huntley.probe, VSL Probe, 0.1, VSL port)

static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "VSLProbe", "%s", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

static void OpenLog()
{
    const char* paths[] = {
        "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/vsl_probe.log",
        "/sdcard/Android/data/com.rockstargames.gtasa/files/vsl_probe.log",
        "/data/data/com.rockstargames.gtasa/files/vsl_probe.log",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        g_log = fopen(paths[i], "w");
        if (g_log) { Log("log file: %s", paths[i]); return; }
    }
}

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

    const char* keys[] = {
        "RwFrame", "RpClump", "RpGeometry", "RpMaterial", "RpAtomic", "RwMatrix",
        "NodeName", "CVehicleModelInfo", "CModelInfo", "CPools", "CVisibilityPlugins",
        "m_snTimeInMilliseconds", "CAutomobile", "CVehicle",
    };
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

extern "C" void OnModLoad()
{
    OpenLog();
    Log("== VSL Probe 0.1 mulai ==");

    uintptr_t base = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    Log("libGTASA base=0x%x handle=%p", (unsigned)base, h);
    if (!h) { Log("handle libGTASA null, berhenti"); return; }

    // nama yang ingin dicek langsung lewat AML
    const char* syms[] = {
        "RwFrameForAllChildren", "RwFrameForAllObjects", "RwFrameUpdateObjects", "RwFrameGetLTM",
        "RpClumpForAllAtomics", "RpClumpClone", "RpGeometryForAllMaterials",
        "RpMaterialSetColor", "RpMaterialGetColor", "RpAtomicGetFrame", "RpAtomicSetFrame",
        "GetFrameNodeName", "_Z17GetFrameNodeNameP7RwFrame",
        "_ZN10CModelInfo16ms_modelInfoPtrsE",
        "_ZN6CTimer22m_snTimeInMillisecondsE",
        "_ZN6CPools13ms_pVehiclePoolE",
    };
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); ++i)
    {
        void* p = aml->GetSym(h, syms[i]);
        Log("%s : %s (%p)", syms[i], p ? "ADA" : "tidak ada", p);
    }

    char path[512];
    if (FindLibPath(path, sizeof(path)))
    {
        Log("path libGTASA: %s", path);
        ScanExports(path);
    }
    else Log("path libGTASA tidak ketemu di /proc/self/maps");

    Log("== VSL Probe selesai ==");
}
