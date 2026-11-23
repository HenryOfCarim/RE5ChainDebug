// re5_chaindbg — in-game debug draw for RE5 chain (cloth/hair) simulation and
// its CCL collision primitives.
//
// v2: the retail Steam exe is Enigma-packed (8.2 MB on disk, imports and
// vtables are built at runtime), so no fixed address from an unpacked build can
// be used.  Everything is now discovered at runtime:
//   * Direct3DCreate9 is hooked in d3d9.dll itself (not in the game's IAT),
//     by a 5-byte patch that is restored around the call to the original;
//   * uChain is found by scanning memory for the loaded rChain resource
//     (magic 'CHN\0' + version 0x80618 at +0x60/+0x64), then for pointers to
//     it (uChain.mpChain at +0x140), then validated through the CCL resource
//     ('CCL\0' at +0x60 of uChain.mpCollision at +0x218).
//
// Drawn over the frame:
//   green points  - uChain joints (JointWork.mWorldMat translation)
//   green lines   - links from the .chn graph (JointInfo.node_joint_no)
//   red circles   - CCL spheres, yellow - capsules, violet - aabb/obb
//   top-left 20x20 square: red = chain not found yet, green = found
//
// Keys: F7 toggle.  Log: re5_chaindbg.log next to the exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <io.h>
#include "rest_links.h"
#include <stdio.h>
#include <stdint.h>
#include <math.h>

typedef struct { float x, y, z, rhw; DWORD color; } VERT;
#define FVF_LINE (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

#define OFF_MPCHAIN    0x140
#define OFF_JOINTWORK  0x14C
#define OFF_COLLISION  0x218
#define OFF_COLINFONUM 0x21C
#define OFF_COLJWMAT   0x250
#define OFF_COLTAG     0x258

#define JW_WORLDMAT    0x010
#define JW_REFJOINT    0x1C4

#define RCHAIN_MAGIC   0x004E4843u   // 'CHN\0'
#define RCCOL_MAGIC    0x004C4343u   // 'CCL\0'
#define RES_VERSION    0x00080618u

#define CHAIN_VTABLE_PC  0x010B789Cu   // uChain
#define CHAIN_VTABLE_EX  0x000F6DEECu   // uChainEx
#define RCHAIN_MPINFO  0x070
#define RCCOL_MPINFO   0x070
#define COLINFO_SIZE   0x40

typedef HRESULT(__stdcall* PFN_EndScene)(IDirect3DDevice9*);
typedef HRESULT(__stdcall* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* PFN_SetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(__stdcall* PFN_CreateDevice)(IDirect3D9*, UINT, DWORD, HWND, DWORD, void*, IDirect3DDevice9**);
typedef IDirect3D9* (__stdcall* PFN_D3DCreate9)(UINT);

static PFN_EndScene    g_origEndScene = NULL;
static PFN_Present     g_origPresent = NULL;
typedef HRESULT(__stdcall* PFN_DrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static PFN_DrawIndexedPrimitive g_origDrawIP = NULL;
typedef HRESULT(__stdcall* PFN_Reset)(IDirect3DDevice9*, void*);
static PFN_Reset       g_origReset = NULL;
static PFN_SetVSConstF g_origSetVSConstF = NULL;
static PFN_CreateDevice g_origCreateDevice = NULL;
static PFN_D3DCreate9  g_origD3DCreate9 = NULL;
static void* g_d3d9Export = NULL;
static BYTE  g_d3d9Orig[5];
static int   g_patched = 0;

static FILE* g_log = NULL;
static double g_drawMs = 0.0;
static uintptr_t g_imgDelta = 0;   // module base - 0x400000
static int g_offJW = 0x14C;        // uChain -> JointWork** (auto-detected)
static int g_offCJ = 0x250;        // uChain -> MtMatrix* col joint world mats
static volatile LONG g_frame = 0;
static volatile int g_show = 0;
static volatile int g_strandDbg = 0;   // F8
static volatile int g_wantScan = 1;
static volatile LONG g_scanRunning = 0;

#define NBLOCKS 24
static float g_blocks[NBLOCKS][4];
static volatile LONG g_camOk = 0;
// Matrices snapshotted inside DrawIndexedPrimitive: at that moment the shader
// constants that were uploaded last are exactly the ones this draw uses, so the
// main pass can be identified instead of guessing from the end of the frame.
static float g_use[NBLOCKS][4];
static volatile LONG g_useValid = 0;
static volatile int  g_captureEnabled = 0;   // F9: draw-time capture (hook not installed)
static volatile LONG g_frameCaptured = 0;
static float g_lastCam[3] = { 0, 0, 0 };
static volatile LONG g_lastCamValid = 0;
static float g_useCamDist = 1e9f;
static float g_frameBest[NBLOCKS][4];
static float g_frameBestCam[3];
static float g_frameBestScore = 1e9f;
static volatile LONG g_frameHave = 0;
static float g_view[12];      // validated view rows (xyz + translation in .w)
static volatile LONG g_viewOk = 0;
static volatile LONG g_viewHits = 0;
static float g_fx = 1.0f, g_fy = 1.0f;
static int g_viewReg = 4;      // which constant block starts the view matrix
static int g_viewMode = 0;     // 0: rows, 1: columns (transposed)
static int g_wSign = 1;        // +1: w = +z, -1: w = -z
static int g_calibScore = -1;  // joints that landed on screen with the choice
static float g_calibSpread = 0.0f;

static uint8_t* g_chain = NULL;
static uint8_t* g_cands[8];
static volatile LONG g_forceCache = 0;
static int g_candN = 0;
static int g_chainJoints = 0;
static int g_chainPrims = 0;


// ---------------------------------------------------------------- crash probe
// Logs the faulting address of a fatal exception so a crash can be attributed to
// our module, the game or something else, without an external debugger.
static HMODULE g_selfModule = NULL;
static uintptr_t g_selfBase = 0, g_selfEnd = 0;
static uintptr_t g_gameBase = 0, g_gameEnd = 0;

static void LogRaw(const char* text)
{
    if (!g_log) return;
    fputs(text, g_log);
    fflush(g_log);
}

static LONG CALLBACK CrashProbe(PEXCEPTION_POINTERS info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;

    // first-chance: our module raised an AV but the game may swallow it.  Those
    // are the early warning of heap/stack damage, so record them once per site.
    static uintptr_t seen[16];
    static int seenN = 0;
    if (addr >= g_selfBase && addr < g_selfEnd) {
        int known = 0;
        for (int i = 0; i < seenN; ++i) if (seen[i] == addr) known = 1;
        if (!known) {
            if (seenN < 16) seen[seenN++] = addr;
            char first[160];
            ULONG_PTR fa = 0;
            if (info->ExceptionRecord->NumberParameters > 1)
                fa = info->ExceptionRecord->ExceptionInformation[1];
            _snprintf(first, sizeof(first), "FIRSTCHANCE code=%08lX addr=asi+0x%X arg=%p\n",
                      code, (unsigned)(addr - g_selfBase), (void*)fa);
            LogRaw(first);
        }
    }
    char line[220];
    const char* where = "other";
    uintptr_t off = addr;
    if (addr >= g_selfBase && addr < g_selfEnd) { where = "re5_chaindbg.asi"; off = addr - g_selfBase; }
    else if (addr >= g_gameBase && addr < g_gameEnd) { where = "re5dx9.exe"; off = addr - g_gameBase; }

    ULONG_PTR arg0 = 0;
    if (info->ExceptionRecord->NumberParameters > 1)
        arg0 = info->ExceptionRecord->ExceptionInformation[1];

    _snprintf(line, sizeof(line), "CRASH code=%08lX addr=%p (%s+0x%X) arg=%p\n",
              code, (void*)addr, where, (unsigned)off, (void*)arg0);
    LogRaw(line);

    // walk the frame chain and report every return address with its module, so a
    // crash can be attributed to a function instead of a single offset
    CONTEXT* ctx = info->ContextRecord;
    uint32_t ebp = (uint32_t)ctx->Ebp;
    for (int depth = 0; depth < 24 && ebp > 0x10000 && ebp < 0x7FFF0000; ++depth) {
        uint32_t frame[2] = { 0, 0 };
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)ebp, frame, sizeof(frame), &got) ||
            got != sizeof(frame)) break;
        uint32_t ret = frame[1];
        if (!ret) break;
        const char* m = "other";
        uint32_t o = ret;
        if (ret >= g_selfBase && ret < g_selfEnd) { m = "asi"; o = ret - g_selfBase; }
        else if (ret >= g_gameBase && ret < g_gameEnd) { m = "game"; o = ret - g_gameBase; }
        _snprintf(line, sizeof(line), "  frame %02d ret=%p (%s+0x%X)\n", depth, (void*)ret, m, o);
        LogRaw(line);
        if (frame[0] <= ebp) break;
        ebp = frame[0];
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InitCrashProbe(HMODULE self)
{
    g_selfModule = self;
    g_selfBase = (uintptr_t)self;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)self;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_selfBase + dos->e_lfanew);
    g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;

    HMODULE game = GetModuleHandleA(NULL);
    g_gameBase = (uintptr_t)game;
    dos = (IMAGE_DOS_HEADER*)game;
    nt = (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
    g_gameEnd = g_gameBase + nt->OptionalHeader.SizeOfImage;

    AddVectoredExceptionHandler(1, CrashProbe);
}

// ------------------------------------------------------------------ logging
static CRITICAL_SECTION g_logCs;
static volatile LONG g_logCsInit = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    if (!g_logCsInit) return;                        // initialised in DllMain
    if (!TryEnterCriticalSection(&g_logCs)) return;   // never stall a frame
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static FILE* OpenLog(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash) return NULL;
    slash[1] = 0;
    lstrcatA(path, "re5_chaindbg.log");
    FILE* f = fopen(path, "w");
    if (!f) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        lstrcatA(tmp, "re5_chaindbg.log");
        f = fopen(tmp, "w");
    }
    return f;
}

// ------------------------------------------------------------------ memory
// Reads through ReadProcessMemory: a pointer into a freed level returns 0
// instead of raising an access violation.
static int SRead(uint32_t va, void* dst, size_t n)
{
    if (!va) return 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, dst, n, &got)) return 0;
    return got == n;
}

static uint32_t SRd32(uint32_t va)
{
    uint32_t v = 0;
    return SRead(va, &v, 4) ? v : 0;
}

static int Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return 0;
    return ((const BYTE*)p + n) <= ((const BYTE*)mbi.BaseAddress + mbi.RegionSize);
}

static uint32_t Rd32(const void* p) { uint32_t v = 0; memcpy(&v, p, 4); return v; }
static float    Rdf(const void* p) { float v = 0; memcpy(&v, p, 4); return v; }

// Memory is read through ReadProcessMemory on this very process: a page that
// disappears mid-scan (the engine frees and reallocates a lot during boot) then
// fails gracefully instead of faulting the game.
#define SCAN_CHUNK 0x10000

typedef int (*RegionCb)(uint8_t* buf, size_t len, uintptr_t addr, void* ctx);

static int ScanRegions(RegionCb cb, void* ctx)
{
    static uint8_t buf[SCAN_CHUNK];
    ULONG_PTR addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    HANDLE self = GetCurrentProcess();
    int done = 0;
    while (addr < 0x7FFF0000) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
        ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress;
        ULONG_PTR size = mbi.RegionSize;
        DWORD prot = mbi.Protect & 0xFF;
        int readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                        prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && readable &&
            size <= 0x04000000 && base >= 0x02000000 && mbi.Type == MEM_PRIVATE) {
            // (the module image is below 0x02000000 and holds no heap objects)
            for (ULONG_PTR off = 0; off < size; off += SCAN_CHUNK) {
                SIZE_T want = (SIZE_T)((size - off) < SCAN_CHUNK ? (size - off) : SCAN_CHUNK);
                SIZE_T got = 0;
                if (ReadProcessMemory(self, (LPCVOID)(base + off), buf, want, &got) && got >= 4) {
                    if (cb(buf, (size_t)got, (uintptr_t)(base + off), ctx)) { done = 1; break; }
                }
            }
            if (done) break;
        }
        if (!size) break;
        addr = base + size;
    }
    return done;
}

// There can be several loaded chain resources and several uChain instances
// (one per unit); the first hit is often an inactive template.  Collect them all
// and pick the instance that actually looks alive: non-null collision matrices
// and the most initialised joints.
#define MAXRES 8
#define MAXCAND 8

typedef struct {
    uint8_t* res[MAXRES];
    int n;
    long magicHits;
    long versionHits;
    unsigned long long bytes;
} FindCtx;

static int CbFindResource(uint8_t* buf, size_t len, uintptr_t addr, void* ctx)
{
    FindCtx* c = (FindCtx*)ctx;
    c->bytes += len;
    for (size_t i = 0; i + 0x80 < len; i += 4) {
        uint32_t v;
        memcpy(&v, buf + i, 4);
        int kind;
        if (v == RCHAIN_MAGIC) kind = 0;
        else if (v == RCCOL_MAGIC) kind = 1;
        else continue;
        c->magicHits++;
        uint32_t ver = Rd32(buf + i + 4);
        if ((ver & 0xFFFF0000u) != 0x00080000u) continue;
        c->versionHits++;
        uint32_t dataSize = Rd32(buf + i + 8);
        if (dataSize == 0 || dataSize > 0x100000) continue;
        if (kind == 0) {
            uint8_t nch = buf[i + 0x0C];
            if (nch == 0 || nch > 8) continue;
        }
        if (kind != 0) continue;                 // start from .chn only
        if (c->n >= MAXRES) return 1;
        uint8_t* r = (uint8_t*)(addr + i) - 0x60;
        int dup = 0;
        for (int k = 0; k < c->n; ++k) if (c->res[k] == r) dup = 1;
        if (!dup) {
            c->res[c->n++] = r;
            Log("rChain resource #%d at %p (data_size=%u)", c->n - 1, (void*)r, dataSize);
        }
    }
    return 0;
}

typedef struct {
    uint8_t* res;
    uint8_t* cand[MAXCAND];
    int n;
} PtrCtx;

static int CbFindPointer(uint8_t* buf, size_t len, uintptr_t addr, void* ctx)
{
    PtrCtx* c = (PtrCtx*)ctx;
    uint32_t want = (uint32_t)(uintptr_t)c->res;
    for (size_t i = 0; i + 4 < len; i += 4) {
        uint32_t v;
        memcpy(&v, buf + i, 4);
        if (v != want) continue;
        uint8_t* chain = (uint8_t*)(addr + i) - OFF_MPCHAIN;
        if (!Readable(chain, 0x260)) continue;
        uint32_t vt = SRd32((uint32_t)(uintptr_t)chain);
        if (vt < 0x00400000u || vt > 0x04000000u) continue;
        if (c->n < MAXCAND) c->cand[c->n++] = chain;
    }
    return 0;
}

static int InitJoints(uint8_t* c)
{
    uint8_t* base = (uint8_t*)SRd32((uint32_t)(uintptr_t)c + g_offJW);
    if (!base) return 0;
    int n = g_chainJoints < 16 ? g_chainJoints : 16;
    if (!Readable(base, n * 4)) n = 4;
    if (!Readable(base, n * 4)) return 0;
    int cnt = 0;
    for (int k = 0; k < n; ++k) {
        uint32_t v = Rd32(base + 4 * k);
        if (!v) continue;
        uint8_t* j = (uint8_t*)(uintptr_t)v;
        if (!Readable(j, 0x1D0)) continue;
        uint32_t vt = Rd32(j);
        if (vt < 0x00400000u || vt > 0x04000000u) continue;
        const float* m = (const float*)(j + JW_WORLDMAT);
        if (!(m[12] == m[12]) || (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f)) continue;
        ++cnt;
    }
    return cnt;
}

static int ScoreChain(uint8_t* c)
{
    uint32_t jmat = SRd32((uint32_t)(uintptr_t)c + g_offCJ);
    int score = (jmat && Readable((void*)(uintptr_t)jmat, 64)) ? 100 : 0;
    score += InitJoints(c) * 2;
    return score;
}

// The console symbol dump and the PC build agree on most of uChain, but not
// necessarily on where the runtime arrays hang.  Instead of trusting one
// offset, look for them structurally:
//   * JointWork** : a field whose array elements point at objects with a
//     module vtable and an MtMatrix at +0x10 (row 3 = 0,0,0,1);
//   * col world matrices : a field pointing straight at such a matrix.
static int LooksLikeMatrix(const void* p)
{
    if (!p || !Readable(p, 64)) return 0;
    const float* m = (const float*)p;
    for (int i = 0; i < 16; ++i) if (!(m[i] == m[i])) return 0;
    if (fabsf(m[3]) > 1e-3f || fabsf(m[7]) > 1e-3f || fabsf(m[11]) > 1e-3f) return 0;
    if (fabsf(m[12]) > 2e5f || fabsf(m[13]) > 2e5f || fabsf(m[14]) > 2e5f) return 0;
    if (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f) return 0;
    return 1;
}

// The col-joint world matrices are not at +0x250 in this build (the field is
// null while mCollisionEnable is 1).  Find the array structurally instead: a
// field anywhere in uChain that points at a run of matrix records.
static int g_offCJfound = 0;

static void FindColMatrices(void)
{
    if (g_offCJfound || !g_chain) return;
    int bestOff = -1, bestCnt = 0;
    for (int off = 0; off <= 0x2FC; off += 4) {
        uint32_t v = SRd32((uint32_t)(uintptr_t)g_chain + off);
        if (!v || !Readable((void*)(uintptr_t)v, 64 * 3)) continue;
        int cnt = 0;
        for (int k = 0; k < 8; ++k) {
            if (!LooksLikeMatrix((void*)(uintptr_t)(v + 64 * k))) break;
            ++cnt;
        }
        if (cnt > bestCnt) { bestCnt = cnt; bestOff = off; }
    }
    if (bestCnt >= 3) {
        g_offCJ = bestOff;
        g_offCJfound = 1;
        Log("ccl: matrix array found at +0x%X (%d records)", bestOff, bestCnt);
    } else {
        Log("ccl: no matrix array found (best +0x%X, %d)", bestOff, bestCnt);
        g_offCJfound = 2;   // don't spam
    }
}

static int CountJointWork(uint8_t* chain, int off)
{
    if (!Readable(chain + off, 4)) return 0;
    uint32_t jw = SRd32((uint32_t)(uintptr_t)chain + off);
    if (!jw || !Readable((void*)(uintptr_t)jw, 8)) return 0;
    int n = 0;
    for (int k = 0; k < 8; ++k) {
        if (!Readable((void*)(uintptr_t)(jw + 4 * k), 4)) break;
        uint32_t p = Rd32((void*)(uintptr_t)(jw + 4 * k));
        if (!p || !Readable((void*)(uintptr_t)p, 0x1D0)) continue;
        uint32_t vt = Rd32((void*)(uintptr_t)p);
        if (vt < 0x00400000u + g_imgDelta || vt > 0x04000000u + g_imgDelta) continue;
        if (!LooksLikeMatrix((void*)(uintptr_t)(p + JW_WORLDMAT))) continue;
        ++n;
    }
    return n;
}

static void AutoLayout(uint8_t* chain)
{
    if (!Readable(chain, 0x260)) return;
    int bestOff = g_offJW, bestCnt = 0;
    for (int off = 0x100; off <= 0x240; off += 4) {
        int c = CountJointWork(chain, off);
        if (c > bestCnt) { bestCnt = c; bestOff = off; }
    }
    if (bestCnt >= 2) {
        g_offJW = bestOff;
        Log("layout: JointWork** at +0x%X (%d objects)", g_offJW, bestCnt);
    } else {
        Log("layout: JointWork** not found (best +0x%X had %d)", bestOff, bestCnt);
    }

    int bestCj = g_offCJ, bestScore = 0;
    for (int off = 0x200; off <= 0x240; off += 4) {
        if (!Readable(chain + off, 4)) break;
        uint32_t v = SRd32((uint32_t)(uintptr_t)chain + off);
        if (!v || !LooksLikeMatrix((void*)(uintptr_t)v)) continue;
        int sc = 1 + (LooksLikeMatrix((void*)(uintptr_t)(v + 64)) ? 1 : 0);
        if (sc > bestScore) { bestScore = sc; bestCj = off; }
    }
    if (bestScore > 0) {
        g_offCJ = bestCj;
        Log("layout: col joint matrices at +0x%X (score %d)", g_offCJ, bestScore);
    } else {
        Log("layout: col joint matrices not found");
    }
}

static uint8_t* g_jblock;
static uint8_t* JointBase(void);
static int PosFromJoint(uint8_t* j, float* out);

static int DiscoverJoints(void)
{
    g_jblock = NULL;
    uint8_t* arr = JointBase();
    if (!arr || !Readable(arr, 4)) return 0;
    uint32_t block = SRd32((uint32_t)(uintptr_t)arr);
    if (!block || !Readable((void*)(uintptr_t)block, 0x1D0)) return 0;
    g_jblock = (uint8_t*)(uintptr_t)block;

    int count = 0, misses = 0;
    for (int i = 0; i < 128; ++i) {
        uint8_t* j = g_jblock + (size_t)i * 0x1D0;
        float pos[3];
        if (PosFromJoint(j, pos)) { ++count; misses = 0; }
        else if (++misses >= 3) break;
    }
    if (count == 0) { g_jblock = NULL; return 0; }
    return count;
}

static void ChainCounts(void)
{
    g_chainJoints = 0;
    g_chainPrims = 0;
    if (!g_chain) return;
    uint8_t* chainRes = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + OFF_MPCHAIN);
    if (!chainRes || !Readable(chainRes, 0x78)) {
        Log("counts: mpChain unreadable (%p)", (void*)chainRes);
        return;
    }
    uint8_t* info = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)chainRes + RCHAIN_MPINFO);
    uint8_t nch = *(uint8_t*)(chainRes + 0x6C);
    Log("counts: res=%p hdr=%08X %08X %08X %08X info=%p nch=%u",
        (void*)chainRes, SRd32((uint32_t)(uintptr_t)chainRes + 0x60), SRd32((uint32_t)(uintptr_t)chainRes + 0x64),
        SRd32((uint32_t)(uintptr_t)chainRes + 0x68), SRd32((uint32_t)(uintptr_t)chainRes + 0x6C), (void*)info, nch);

    // no bail-outs: a strange chain count must not wipe the primitive count
    int chains = (nch >= 1 && nch <= 8) ? nch : 1;
    if (Readable(info, 0x20)) {
        for (int i = 0; i < chains; ++i) {
            uint8_t num = info[i * 0x20];
            if (num == 0 || num > 64) continue;
            g_chainJoints += num;
        }
    }
    int blockJoints = DiscoverJoints();
    if (blockJoints > g_chainJoints) g_chainJoints = blockJoints;
    g_chainPrims = (int)SRd32((uint32_t)(uintptr_t)g_chain + OFF_COLINFONUM);
    if (g_chainPrims < 0 || g_chainPrims > 64) g_chainPrims = 0;
    Log("counts: joints=%d (chaininfo=%d block=%d) prims=%d",
        g_chainJoints, g_chainJoints, blockJoints, g_chainPrims);
}

// Preferred route on the 1.10 PC build: the uChain vtable address is fixed, so
// every live instance can be enumerated directly and scored.
typedef struct { uint8_t* cand[16]; int n; } VtCtx;

static int CbFindVtable(uint8_t* buf, size_t len, uintptr_t addr, void* ctx)
{
    VtCtx* c = (VtCtx*)ctx;
    for (size_t i = 0; i + 4 < len; i += 4) {
        uint32_t v;
        memcpy(&v, buf + i, 4);
        if (v != (CHAIN_VTABLE_PC + g_imgDelta) && v != (CHAIN_VTABLE_EX + g_imgDelta)) continue;
        uint8_t* chain = (uint8_t*)(addr + i);
        if (!Readable(chain, 0x260)) continue;
        if (c->n < 16) c->cand[c->n++] = chain;
    }
    return 0;
}

// counts for an arbitrary candidate chain (not necessarily the current one)
static void CountsOf(uint8_t* c, int* joints, int* prims, uint32_t* magic)
{
    *joints = 0; *prims = 0; *magic = 0;
    if (!c || !Readable(c, 0x260)) return;
    uint8_t* res = (uint8_t*)(uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)c + OFF_MPCHAIN);
    if (!res || !Readable(res, 0x78)) return;
    *magic = SRd32((uint32_t)(uintptr_t)res + 0x60);
    if (*magic != RCHAIN_MAGIC) return;
    uint8_t* info = (uint8_t*)SRd32((uint32_t)(uintptr_t)res + RCHAIN_MPINFO);
    uint8_t nch = *(uint8_t*)(res + 0x6C);
    int chains = (nch >= 1 && nch <= 8) ? nch : 1;
    if (Readable(info, 0x20)) {
        for (int i = 0; i < chains; ++i) {
            uint8_t num = info[i * 0x20];
            if (num == 0 || num > 96) continue;
            *joints += num;
        }
    }
    *prims = (int)SRd32((uint32_t)(uintptr_t)c + OFF_COLINFONUM);
    if (*prims < 0 || *prims > 64) *prims = 0;
}


// ---------------------------------------------------------------- sCamera
// The game's camera object carries BOTH the view matrix (object+0x100) and the
// projection (object+0x140), and it is the same object every frame.  Reading
// those is far more reliable than guessing which shader-constant block was the
// main pass.  Layout from re5_freecam: dword0 = exe vtable, +0x54 = uCamera*,
// viewport 0 at +0x50, mViewMat at +0x100, mProjMat at +0x140.
static int FilterOutliers(float (*world)[3], int n, float maxDist)
{
    if (n < 4) return n;
    float cx = 0, cy = 0, cz = 0;
    for (int i = 0; i < n; ++i) { cx += world[i][0]; cy += world[i][1]; cz += world[i][2]; }
    cx /= n; cy /= n; cz /= n;

    int out = 0;
    for (int i = 0; i < n; ++i) {
        float dx = world[i][0] - cx, dy = world[i][1] - cy, dz = world[i][2] - cz;
        if (sqrtf(dx * dx + dy * dy + dz * dz) <= maxDist) {
            if (out != i) {
                world[out][0] = world[i][0];
                world[out][1] = world[i][1];
                world[out][2] = world[i][2];
            }
            ++out;
        }
    }
    return out;
}

static void GatherJoints(float (*world)[3], int* count, int maxn);

static uint8_t* g_sc = NULL;
static volatile LONG g_scOk = 0;
static int g_scSign = 1;
static float g_scFx = 1.0f, g_scFy = 1.0f;

static int ScOrthonormal(void)
{
    if (!g_sc || !Readable(g_sc + 0x100, 48)) return 0;
    const float* m = (const float*)(g_sc + 0x100);
    for (int k = 0; k < 12; ++k) if (!(m[k] == m[k])) return 0;
    float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    float l1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
    float l2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
    if (fabsf(l0 - 1) > 0.1f || fabsf(l1 - 1) > 0.1f || fabsf(l2 - 1) > 0.1f) return 0;
    float d01 = m[0]*m[4] + m[1]*m[5] + m[2]*m[6];
    float d02 = m[0]*m[8] + m[1]*m[9] + m[2]*m[10];
    float d12 = m[4]*m[8] + m[5]*m[9] + m[6]*m[10];
    if (fabsf(d01) > 0.15f || fabsf(d02) > 0.15f || fabsf(d12) > 0.15f) return 0;
    return 1;
}

typedef struct { uint8_t* cand[8]; int n; } ScCtx;

static int CbFindSCamera(uint8_t* buf, size_t len, uintptr_t addr, void* ctx)
{
    ScCtx* c = (ScCtx*)ctx;
    if (len < 0x200) return 0;
    for (size_t i = 0; i + 0x1A0 < len; i += 4) {
        uint32_t vt;
        memcpy(&vt, buf + i, 4);
        if (vt < 0x00400000u + g_imgDelta || vt > 0x04000000u + g_imgDelta) continue;
        uint32_t cam;
        memcpy(&cam, buf + i + 0x54, 4);
        if (!cam || !Readable((void*)(uintptr_t)cam, 4)) continue;
        uint32_t cvt = *(uint32_t*)(uintptr_t)cam;
        if (cvt < 0x00400000u + g_imgDelta || cvt > 0x04000000u + g_imgDelta) continue;
        const float* m = (const float*)(buf + i + 0x100);
        int ok = 1;
        for (int k = 0; k < 12; ++k) if (!(m[k] == m[k])) { ok = 0; break; }
        if (!ok) continue;
        float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
        float l1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
        float l2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
        if (fabsf(l0 - 1) > 0.1f || fabsf(l1 - 1) > 0.1f || fabsf(l2 - 1) > 0.1f) continue;
        const float* pm = (const float*)(buf + i + 0x140);
        if (!(pm[0] > 0.05f) || !(pm[5] > 0.05f)) continue;
        if (!(pm[0] == pm[0]) || !(pm[5] == pm[5])) continue;
        if (c->n < 8) c->cand[c->n++] = (uint8_t*)(addr + i);
        continue;
    }
    return 0;
}

static void ScCamPos(float* out)
{
    const float* m = (const float*)(g_sc + 0x100);
    out[0] = -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]);
    out[1] = -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]);
    out[2] = -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11]);
}

static LONG g_scNextTry = 0;

static void FindSCamera(void)
{
    ScCtx c;
    ZeroMemory(&c, sizeof(c));
    ScanRegions(CbFindSCamera, &c);
    if (c.n == 0) {
        Log("sCamera: not found (%d candidates)", c.n);
        g_scOk = 0;
        g_scNextTry = g_frame + 1800;
        return;
    }

    float world[160][3];
    int n = 0;
    GatherJoints(world, &n, 160);
    float refPos[3] = { 0, 0, 0 };
    for (int i = 0; i < n; ++i) {
        refPos[0] += world[i][0]; refPos[1] += world[i][1]; refPos[2] += world[i][2];
    }
    if (n) { refPos[0] /= n; refPos[1] /= n; refPos[2] /= n; }

    uint8_t* best = NULL;
    float bestDist = 1e30f;
    float bestFx = 0, bestFy = 0;
    for (int k = 0; k < c.n; ++k) {
        uint8_t* q = c.cand[k];
        const float* m = (const float*)(q + 0x100);
        const float* pm = (const float*)(q + 0x140);
        float fx = pm[0], fy = pm[5];
        if (!(fx > 0.35f && fx < 6.0f)) continue;
        if (!(fy > 0.35f && fy < 6.0f)) continue;
        if (fy < fx * 0.4f || fy > fx * 2.5f) continue;      // aspect sanity
        if (fabsf(pm[1]) > 0.05f || fabsf(pm[4]) > 0.05f) continue;
        float cx = -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]);
        float cy = -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]);
        float cz = -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11]);
        float d = 1e30f;
        if (n) {
            d = sqrtf((cx - refPos[0]) * (cx - refPos[0]) + (cy - refPos[1]) * (cy - refPos[1]) +
                      (cz - refPos[2]) * (cz - refPos[2]));
        }
        if (d > 2000.0f) continue;                            // not this character's camera
        if (d < bestDist) { bestDist = d; best = q; bestFx = fx; bestFy = fy; }
    }
    if (!best) {
        Log("sCamera: %d candidates, none matched (aspect/position)", c.n);
        g_scOk = 0;
        g_scNextTry = g_frame + 1800;
        return;
    }

    g_sc = best;
    g_scFx = bestFx;
    g_scFy = bestFy;
    g_scSign = 1;
    {
        const float* m = (const float*)(g_sc + 0x100);
        float pz = 0;
        for (int i = 0; i < n; ++i)
            pz += world[i][0] * m[8] + world[i][1] * m[9] + world[i][2] * m[10] + m[11];
        if (pz < 0) g_scSign = -1;
    }
    g_scOk = 1;
    Log("sCamera at %p fx=%.3f fy=%.3f sign=%+d camDist=%.0f (%d candidates)",
        (void*)g_sc, g_scFx, g_scFy, g_scSign, bestDist, c.n);
}

static void CamPos(float* out)
{
    if (g_scOk) { ScCamPos(out); return; }
    const float* a = g_blocks[g_viewReg];
    const float* b = g_blocks[g_viewReg + 1];
    const float* c = g_blocks[g_viewReg + 2];
    out[0] = -(a[0] * a[3] + b[0] * b[3] + c[0] * c[3]);
    out[1] = -(a[1] * a[3] + b[1] * b[3] + c[1] * c[3]);
    out[2] = -(a[2] * a[3] + b[2] * b[3] + c[2] * c[3]);
}

// average of the first few JointWork translations of a candidate chain
static int ChainCentroid(uint8_t* c, float* out)
{
    if (!c || !Readable(c, 0x260)) return 0;
    uint8_t* arr = (uint8_t*)SRd32((uint32_t)(uintptr_t)c + OFF_JOINTWORK);
    if (!arr || !Readable(arr, 4)) return 0;
    uint32_t block = SRd32((uint32_t)(uintptr_t)arr);
    if (!block) return 0;
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t* j = (uint8_t*)(uintptr_t)block + (size_t)i * 0x1D0;
        if (!Readable(j + JW_WORLDMAT, 64)) break;
        const float* m = (const float*)(j + JW_WORLDMAT);
        if (!(m[12] == m[12]) || (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f)) continue;
        sx += m[12]; sy += m[13]; sz += m[14]; ++n;
    }
    if (!n) return 0;
    out[0] = sx / n; out[1] = sy / n; out[2] = sz / n;
    return 1;
}

static void PickNearestChain(void)
{
    if (g_candN <= 0) return;
    float cam[3];
    CamPos(cam);
    uint8_t* best = NULL;
    float bestDist = 1e30f;
    for (int i = 0; i < g_candN; ++i) {
        uint8_t* c = g_cands[i];
        if (!Readable(c, 0x260)) continue;
        int j = 0, pr = 0;
        uint32_t mg = 0;
        CountsOf(c, &j, &pr, &mg);
        if (mg != RCHAIN_MAGIC || j < 3) continue;
        float cen[3];
        if (!ChainCentroid(c, cen)) continue;
        float dx = cen[0] - cam[0], dy = cen[1] - cam[1], dz = cen[2] - cam[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    if (best && best != g_chain) {
        g_chain = best;
        g_jblock = NULL;
        g_forceCache = 1;
        ChainCounts();
        Log("switched to chain %p (camDist=%.0f joints=%d prims=%d)",
            (void*)best, bestDist, g_chainJoints, g_chainPrims);
    }
}

static void FindModelJoints(void);

static int TryVtableRoute(void)
{
    VtCtx vc;
    ZeroMemory(&vc, sizeof(vc));
    ScanRegions(CbFindVtable, &vc);
    if (vc.n == 0) { Log("vtable route: no uChain objects"); return 0; }
    Log("vtable route: %d candidate object(s)", vc.n);

    float cam[3];
    CamPos(cam);

    uint8_t* best = NULL;
    float bestDist = 1e30f;
    int bestScore = -1;
    for (int i = 0; i < vc.n; ++i) {
        uint8_t* c = vc.cand[i];
        uint8_t* res = (uint8_t*)(uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)c + OFF_MPCHAIN);
        int joints = 0, prims = 0;
        uint32_t resMagic = 0;
        CountsOf(c, &joints, &prims, &resMagic);
        float cen[3];
        float dist = 1e30f;
        if (resMagic == RCHAIN_MAGIC && ChainCentroid(c, cen)) {
            float dx = cen[0] - cam[0], dy = cen[1] - cam[1], dz = cen[2] - cam[2];
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        Log("  vtable cand %p res=%p magic=%08X joints=%d prims=%d camDist=%.0f",
            (void*)c, (void*)res, resMagic, joints, prims, dist);
        // the player's chain is the one nearest the camera
        if (resMagic != RCHAIN_MAGIC) continue;
        if (dist < bestDist - 1.0f) { bestDist = dist; best = c; bestScore = joints * 2 + prims * 4; }
    }
    if (!best || bestScore <= 0) return 0;
    g_candN = 0;
    for (int i = 0; i < vc.n && g_candN < 8; ++i) {
        int j = 0, pr = 0;
        uint32_t mg = 0;
        CountsOf(vc.cand[i], &j, &pr, &mg);
        if (mg == RCHAIN_MAGIC && j >= 3) g_cands[g_candN++] = vc.cand[i];
    }
    if (g_candN == 0) return 0;
    g_chain = g_cands[0];
    ChainCounts();
    FindModelJoints();      // background thread
    g_wantScan = 0;
    Log("uChain chosen %p score=%d joints=%d prims=%d", (void*)g_chain, bestScore,
        g_chainJoints, g_chainPrims);
    return 1;
}

static void FindModelJoints(void);

static DWORD WINAPI ScanThread(LPVOID)
{
    InterlockedExchange(&g_scanRunning, 1);
    Log("scan thread started");
    Sleep(3000);
    int attempt = 0;
    while (g_wantScan) {
        if (TryVtableRoute()) break;
        // the resource route costs several extra full-memory passes, so it is
        // only tried every third time; the vtable route is the primary one
        if ((attempt++ % 3) != 0) { Sleep(10000); continue; }
        FindCtx fc;
        ZeroMemory(&fc, sizeof(fc));
        ScanRegions(CbFindResource, &fc);
        Log("scan: %llu MB, magic=%ld versioned=%ld chn resources=%d",
            fc.bytes / (1024ull * 1024ull), fc.magicHits, fc.versionHits, fc.n);
        if (fc.n == 0) { Sleep(10000); continue; }

        uint8_t* best = NULL;
        int bestScore = -1;
        for (int r = 0; r < fc.n; ++r) {
            PtrCtx pc;
            ZeroMemory(&pc, sizeof(pc));
            pc.res = fc.res[r];
            ScanRegions(CbFindPointer, &pc);
            for (int k = 0; k < pc.n; ++k) {
                int sc = ScoreChain(pc.cand[k]);
                uint8_t* cch = pc.cand[k];
                Log("  cand res#%d chain=%p vt=%08X jw=%08X jmat=%08X score=%d",
                    r, (void*)cch, SRd32((uint32_t)(uintptr_t)cch), SRd32((uint32_t)(uintptr_t)cch + g_offJW),
                    SRd32((uint32_t)(uintptr_t)cch + g_offCJ), sc);
                if (sc > bestScore) { bestScore = sc; best = pc.cand[k]; }
            }
        }
        if (!best || bestScore <= 0) { Sleep(10000); continue; }

        g_chain = best;
        ChainCounts();
        FindModelJoints();          // background thread: never stalls a frame
        g_wantScan = 0;
        Log("uChain chosen %p score=%d joints=%d prims=%d", (void*)g_chain, bestScore,
            g_chainJoints, g_chainPrims);
        Log("scan thread done, exiting");
        break;
    }
    InterlockedExchange(&g_scanRunning, 0);
    return 0;
}

// ------------------------------------------------------------------ chain data
static void Xform(const float* m, const float* p, float* out)
{
    out[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    out[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    out[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}

// ------------------------------------------------------------------ projection
// The view matrix is uploaded as three consecutive vec4 constants, but which
// register that is depends on the build (the dated notes said c4; the packed
// retail build may differ).  So instead of trusting one register, every block
// is captured and the one that projects the chain onto the screen is picked
// automatically.  Both handednesses are tried as well.
static int ProjectWith(int reg, int mode, int sign, const float* world, int w, int h, float* sx, float* sy)
{
    const float* v0 = g_blocks[reg];
    const float* v1 = g_blocks[reg + 1];
    const float* v2 = g_blocks[reg + 2];
    float px, py, pz;
    if (mode == 0) {
        // rows as uploaded, row-vector convention: p_view = p * V
        px = world[0] * v0[0] + world[1] * v0[1] + world[2] * v0[2] + v0[3];
        py = world[0] * v1[0] + world[1] * v1[1] + world[2] * v1[2] + v1[3];
        pz = world[0] * v2[0] + world[1] * v2[1] + world[2] * v2[2] + v2[3];
    } else {
        // same numbers, column-vector convention: p_view = V^T * p
        px = world[0] * v0[0] + world[1] * v1[0] + world[2] * v2[0] + v0[3];
        py = world[0] * v0[1] + world[1] * v1[1] + world[2] * v2[1] + v1[3];
        pz = world[0] * v0[2] + world[1] * v1[2] + world[2] * v2[2] + v2[3];
    }
    float cw = pz * (float)sign;
    if (cw <= 0.05f) return 0;
    float ndcX = px * g_fx / cw;
    float ndcY = py * g_fy / cw;
    if (!(ndcX > -1.6f && ndcX < 1.6f && ndcY > -1.6f && ndcY < 1.6f)) return 0;
    *sx = (ndcX * 0.5f + 0.5f) * (float)w;
    *sy = (0.5f - ndcY * 0.5f) * (float)h;
    return 1;
}

static int Project(const float* world, int w, int h, float* sx, float* sy)
{
    if (g_useValid) {
        g_fx = g_use[7][0] > 0.05f ? g_use[7][0] : g_fx;
        g_fy = g_use[7][1] > 0.05f ? g_use[7][1] : g_fy;
        const float* v0 = g_use[g_viewReg];
        const float* v1 = g_use[g_viewReg + 1];
        const float* v2 = g_use[g_viewReg + 2];
        float px, py, pz;
        if (g_viewMode == 0) {
            px = world[0] * v0[0] + world[1] * v0[1] + world[2] * v0[2] + v0[3];
            py = world[0] * v1[0] + world[1] * v1[1] + world[2] * v1[2] + v1[3];
            pz = world[0] * v2[0] + world[1] * v2[1] + world[2] * v2[2] + v2[3];
        } else {
            px = world[0] * v0[0] + world[1] * v1[0] + world[2] * v2[0] + v0[3];
            py = world[0] * v0[1] + world[1] * v1[1] + world[2] * v2[1] + v1[3];
            pz = world[0] * v0[2] + world[1] * v1[2] + world[2] * v2[2] + v2[3];
        }
        float cw = pz * (float)g_wSign;
        if (cw <= 0.05f) return 0;
        float ndcX = px * g_fx / cw;
        float ndcY = py * g_fy / cw;
        if (!(ndcX > -1.6f && ndcX < 1.6f && ndcY > -1.6f && ndcY < 1.6f)) return 0;
        *sx = (ndcX * 0.5f + 0.5f) * (float)w;
        *sy = (0.5f - ndcY * 0.5f) * (float)h;
        return 1;
    }
    if (g_scOk) {
        const float* v0 = (const float*)(g_sc + 0x100);
        const float* v1 = (const float*)(g_sc + 0x104);
        const float* v2 = (const float*)(g_sc + 0x108);
        float px = world[0] * v0[0] + world[1] * v0[1] + world[2] * v0[2] + v0[3];
        float py = world[0] * v1[0] + world[1] * v1[1] + world[2] * v1[2] + v1[3];
        float pz = world[0] * v2[0] + world[1] * v2[1] + world[2] * v2[2] + v2[3];
        float cw = pz * (float)g_scSign;
        if (cw <= 0.05f) return 0;
        float ndcX = px * g_scFx / cw;
        float ndcY = py * g_scFy / cw;
        if (!(ndcX > -1.4f && ndcX < 1.4f && ndcY > -1.4f && ndcY < 1.4f)) return 0;
        *sx = (ndcX * 0.5f + 0.5f) * (float)w;
        *sy = (0.5f - ndcY * 0.5f) * (float)h;
        return 1;
    }
    if (!g_camOk) return 0;
    return ProjectWith(g_viewReg, g_viewMode, g_wSign, world, w, h, sx, sy);
}

// uChain::mpJointWork is documented as JointWork** (array of pointers) but the
// packed retail build stores the JointWork objects contiguously.  Decide once,
// from the data itself, which layout this build uses.
static int g_jointLayout = -1;   // -1 unknown, 0 = pointer array, 1 = contiguous

static uint8_t* JointBase(void)
{
    if (!g_chain) return NULL;
    return (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + g_offJW);
}

static int PosFromJoint(uint8_t* j, float* out)
{
    if (!j) return 0;
    float m16[16];
    if (!SRead((uint32_t)(uintptr_t)j + JW_WORLDMAT, m16, 64)) return 0;
    uint32_t vt = SRd32((uint32_t)(uintptr_t)j);
    if (vt < 0x00400000u + g_imgDelta || vt > 0x04000000u + g_imgDelta) return 0;
    float x = m16[12], y = m16[13], z = m16[14];
    if (!(x == x) || !(y == y) || !(z == z)) return 0;
    if (fabsf(x) > 200000.0f || fabsf(y) > 200000.0f || fabsf(z) > 200000.0f) return 0;
    if (x == 0.0f && y == 0.0f && z == 0.0f) return 0;
    out[0] = x; out[1] = y; out[2] = z;
    return 1;
}

// uChain::setup (0x612F80) walks the array like this:
//     edi = [esi+0x14C]          ; JointWork**
//     edi = [edi + chain*4]      ; block for that chain
//     ... edi += 0x1D0           ; contiguous JointWork records
// so +0x14C is indexed by CHAIN, not by joint, and every chain owns one
// contiguous run of JointWork objects.
static uint8_t* JointAt(int i)
{
    if (!g_chain) return NULL;
    if (g_jblock) {
        uint8_t* j = g_jblock + (size_t)i * 0x1D0;
        return Readable(j, 0x1D0) ? j : NULL;
    }
    uint8_t* arr = JointBase();
    uint8_t* chainRes = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + OFF_MPCHAIN);
    if (!arr || !chainRes || !Readable(chainRes, 0x78)) return NULL;
    uint8_t* info = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)chainRes + RCHAIN_MPINFO);
    uint8_t nch = *(uint8_t*)(chainRes + 0x6C);
    if (!Readable(info, 0x20)) return NULL;
    if (nch == 0 || nch > 8) nch = 1;

    int idx = i;
    for (int c = 0; c < nch; ++c) {
        int num = info[c * 0x20];
        if (idx < num) {
            if (!Readable(arr + 4 * c, 4)) return NULL;
            uint32_t block = SRd32((uint32_t)(uintptr_t)arr + 4 * c);
            if (!block) return NULL;
            uint8_t* j = (uint8_t*)(uintptr_t)block + (size_t)idx * 0x1D0;
            return Readable(j, 0x1D0) ? j : NULL;
        }
        idx -= num;
    }
    return NULL;
}

// Readable() calls VirtualQuery, which is far from free (tens of microseconds
// on a big VA space).  Doing it per joint per frame cost ~40 ms; the joint
// pointers are therefore validated once and then reused for a while.
static uint8_t* g_jcache[160];
static float g_centroid[3];
static int g_jcacheN = 0;
static volatile LONG g_jcacheFrame = -100000;

static void RefreshJointCache(void)
{
    g_jcacheN = 0;
    if (!g_chain) return;
    for (int i = 0; i < g_chainJoints && g_jcacheN < 160; ++i) {
        uint8_t* j = JointAt(i);
        float pos[3];
        if (!PosFromJoint(j, pos)) continue;
        g_jcache[g_jcacheN++] = j;
    }
    g_jcacheFrame = g_frame;
    float cx = 0, cy = 0, cz = 0;
    for (int i = 0; i < g_jcacheN; ++i) {
        const float* m = (const float*)(g_jcache[i] + JW_WORLDMAT);
        cx += m[12]; cy += m[13]; cz += m[14];
    }
    if (g_jcacheN) { g_centroid[0] = cx / g_jcacheN; g_centroid[1] = cy / g_jcacheN; g_centroid[2] = cz / g_jcacheN; }
    Log("joint cache: %d of %d", g_jcacheN, g_chainJoints);
}

static void GatherJoints(float (*world)[3], int* count, int maxn)
{
    *count = 0;
    if (!g_chain || g_chainJoints <= 0) return;
    if (g_forceCache || g_jcacheN == 0 || (g_frame - g_jcacheFrame) > 120) {
        g_forceCache = 0;
        RefreshJointCache();
    }
    for (int i = 0; i < g_jcacheN && *count < maxn; ++i) {
        const float* m = (const float*)(g_jcache[i] + JW_WORLDMAT);
        float x = m[12], y = m[13], z = m[14];
        if (!(x == x) || !(y == y) || !(z == z)) continue;
        if (x == 0.0f && y == 0.0f && z == 0.0f) continue;
        world[*count][0] = x; world[*count][1] = y; world[*count][2] = z;
        ++(*count);
    }
    *count = FilterOutliers(world, *count, 600.0f);
}

static float VLen3(const float* v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// A genuine view matrix is three orthonormal rows with the translation in .w of
// each row (row-vector convention: p_view = p * V).  Rejecting everything that
// is not a rotation is what stops a light/material constant block from being
// mistaken for the camera.
static int ViewPlausible(int reg, const float* meanPos, float* distOut)
{
    const float* v0 = g_blocks[reg];
    const float* v1 = g_blocks[reg + 1];
    const float* v2 = g_blocks[reg + 2];
    float l0 = VLen3(v0), l1 = VLen3(v1), l2 = VLen3(v2);
    if (fabsf(l0 - 1.0f) > 0.08f || fabsf(l1 - 1.0f) > 0.08f || fabsf(l2 - 1.0f) > 0.08f)
        return 0;
    float d01 = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
    float d02 = v0[0] * v2[0] + v0[1] * v2[1] + v0[2] * v2[2];
    float d12 = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
    if (fabsf(d01) > 0.10f || fabsf(d02) > 0.10f || fabsf(d12) > 0.10f)
        return 0;
    // camera position = -(R^T * t)
    float cx = -(v0[0] * v0[3] + v1[0] * v1[3] + v2[0] * v2[3]);
    float cy = -(v0[1] * v0[3] + v1[1] * v1[3] + v2[1] * v2[3]);
    float cz = -(v0[2] * v0[3] + v1[2] * v1[3] + v2[2] * v2[3]);
    if (!(cx == cx) || !(cy == cy) || !(cz == cz)) return 0;
    float dx = cx - meanPos[0], dy = cy - meanPos[1], dz = cz - meanPos[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 5.0f || dist > 4000.0f) return 0;
    *distOut = dist;
    return 1;
}

static int g_locked = 0;

static void CamPosOf(int reg, float* out)
{
    const float* a = g_blocks[reg];
    const float* b = g_blocks[reg + 1];
    const float* c = g_blocks[reg + 2];
    out[0] = -(a[0] * a[3] + b[0] * b[3] + c[0] * c[3]);
    out[1] = -(a[1] * a[3] + b[1] * b[3] + c[1] * c[3]);
    out[2] = -(a[2] * a[3] + b[2] * b[3] + c[2] * c[3]);
}

static void Calibrate(int w, int h)
{
    if (g_locked) return;
    if (g_useValid) {
        g_viewReg = 4;
        g_viewMode = 0;
        // handedness: pick the sign that keeps the cloud nearest the screen centre
        float world[160][3];
        int n = 0;
        GatherJoints(world, &n, 160);
        if (n > 0) {
            float cx = 0, cy = 0, cz = 0;
            for (int i = 0; i < n; ++i) { cx += world[i][0]; cy += world[i][1]; cz += world[i][2]; }
            cx /= n; cy /= n; cz /= n;
            float cen[3] = { cx, cy, cz };
            float bestD = 1e30f;
            int bestSign = -1;
            for (int sg = 1; sg >= -1; sg -= 2) {
                float sx, sy;
                int save = g_wSign;
                g_wSign = sg;
                int ok = Project(cen, w, h, &sx, &sy);
                g_wSign = save;
                if (!ok) continue;
                float dx = sx - w * 0.5f, dy = sy - h * 0.5f;
                float d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; bestSign = sg; }
            }
            g_wSign = bestSign;
        }
        g_locked = 1;
        Log("projection: draw-time capture (c4 rows, sign=%+d, fx=%.3f fy=%.3f)",
            g_wSign, g_fx, g_fy);
        return;
    }
    float world[160][3];
    int n = 0;
    GatherJoints(world, &n, 160);
    if (n < 3) return;

    float cen[3] = { 0, 0, 0 };
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            cen[k] += world[i][k];
            if (world[i][k] < mn[k]) mn[k] = world[i][k];
            if (world[i][k] > mx[k]) mx[k] = world[i][k];
        }
    }
    for (int k = 0; k < 3; ++k) cen[k] /= n;
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (diag < 1.0f) return;

    int bestReg = -1, bestMode = 0, bestSign = 1;
    float bestCost = 1e30f, bestSpread = 0, bestDist = 0;

    for (int reg = 0; reg + 2 < NBLOCKS; ++reg) {
        const float* a = g_blocks[reg];
        const float* b = g_blocks[reg + 1];
        const float* c = g_blocks[reg + 2];
        float cam[3];
        cam[0] = -(a[0] * a[3] + b[0] * b[3] + c[0] * c[3]);
        cam[1] = -(a[1] * a[3] + b[1] * b[3] + c[1] * c[3]);
        cam[2] = -(a[2] * a[3] + b[2] * b[3] + c[2] * c[3]);
        float camDist = sqrtf((cam[0]-cen[0])*(cam[0]-cen[0]) + (cam[1]-cen[1])*(cam[1]-cen[1]) +
                              (cam[2]-cen[2])*(cam[2]-cen[2]));
        if (!(camDist > 30.0f) || camDist > 12000.0f) continue;

        for (int mode = 0; mode < 2; ++mode) {
            for (int sign = 1; sign >= -1; sign -= 2) {
                float sx, sy;
                if (!ProjectWith(reg, mode, sign, cen, w, h, &sx, &sy)) continue;
                float cdx = sx - w * 0.5f, cdy = sy - h * 0.5f;
                float centre = sqrtf(cdx * cdx + cdy * cdy) / (0.5f * (float)w);
                if (centre > 1.0f) continue;

                int score = 0;
                float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
                for (int i = 0; i < n; ++i) {
                    float qx, qy;
                    if (!ProjectWith(reg, mode, sign, world[i], w, h, &qx, &qy)) continue;
                    ++score;
                    if (qx < minx) minx = qx;
                    if (qx > maxx) maxx = qx;
                    if (qy < miny) miny = qy;
                    if (qy > maxy) maxy = qy;
                }
                if (score < (n * 6) / 10) continue;
                float spread = (maxx - minx) + (maxy - miny);
                if (spread < 20.0f) continue;
                // a real camera gives a 2D blob, not a line or a point
                if ((maxx - minx) < 15.0f || (maxy - miny) < 15.0f) continue;
                float expected = diag * g_fx * (0.5f * (float)w) / camDist;
                if (expected < 8.0f) continue;
                float sizeErr = fabsf(spread - expected) / expected;
                float cost = centre + 2.0f * sizeErr;
                if (cost < bestCost) {
                    bestCost = cost; bestReg = reg; bestMode = mode; bestSign = sign;
                    bestSpread = spread; bestDist = camDist;
                }
            }
        }
    }
    if (bestReg < 0) {
        g_viewReg = 4; g_viewMode = 0; g_wSign = -1;
        g_locked = 1;
        Log("projection: fallback c4/-1 (no candidate matched)");
        return;
    }
    g_viewReg = bestReg;
    g_viewMode = bestMode;
    g_wSign = bestSign;
    g_locked = 1;
    Log("projection: view=c%d mode=%d sign=%+d cost=%.2f spread=%.0f expected~%.0f camDist=%.0f",
        g_viewReg, bestMode, g_wSign, bestCost, bestSpread,
        diag * g_fx * (0.5f * (float)w) / bestDist, bestDist);
}

// ------------------------------------------------------------------ model joints
// CCL primitives are attached to the player model's bones.  The joint array is
// found structurally: a field of uModel pointing at 0x90-stride records whose
// world matrix (+0x50) sits near the chain we already track.  The function id
// used by ref_joint_no is the byte at +0x07 of such a record.
static uint8_t* g_jarr = NULL;
static int g_jstride = 0;
static int g_jmatoff = 0;

static int MatrixNearBuf(const uint8_t* buf, int moff, const float* refPos, float maxd)
{
    const float* m = (const float*)(buf + moff);
    for (int i = 0; i < 16; ++i) if (!(m[i] == m[i])) return 0;
    if (fabsf(m[12]) > 2e5f || fabsf(m[13]) > 2e5f || fabsf(m[14]) > 2e5f) return 0;
    if (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f) return 0;
    float dx = m[12] - refPos[0], dy = m[13] - refPos[1], dz = m[14] - refPos[2];
    return (dx * dx + dy * dy + dz * dz) < (maxd * maxd);
}

static uint8_t* g_jarrEnd = NULL;

static void FindModelJoints(void)
{
    if (g_jarr || !g_chain) return;
    float world[160][3];
    int n = 0;
    GatherJoints(world, &n, 160);
    if (n < 3) return;
    float refPos[3] = { 0, 0, 0 };
    for (int i = 0; i < n; ++i) {
        refPos[0] += world[i][0]; refPos[1] += world[i][1]; refPos[2] += world[i][2];
    }
    refPos[0] /= n; refPos[1] /= n; refPos[2] /= n;

    uint8_t* model = (uint8_t*)Rd32(g_chain + 0x210);
    if (!model || !Readable(model, 0x400)) return;

    static const int strides[] = { 0x90, 0xA0, 0xB0 };
    static const int matoffs[] = { 0x40, 0x48, 0x50, 0x58 };
    int bestOff = -1, bestStride = 0, bestMat = 0, bestCnt = 0;

    for (int off = 0; off <= 0x3FC; off += 4) {
        uint32_t v = Rd32(model + off);
        if (!v) continue;
        // one validation for the whole run we are going to read
        size_t span = (size_t)8 * 0xB0 + 0x60;
        if (!Readable((void*)(uintptr_t)v, span)) continue;
        for (int si = 0; si < 3; ++si) {
            for (int mi = 0; mi < 4; ++mi) {
                int cnt = 0;
                for (int k = 0; k < 8; ++k) {
                    uint8_t rec[0x90 + 0x60];
                    if (!SRead((uint32_t)(uintptr_t)v + (uint32_t)((size_t)k * strides[si]),
                               rec, 0x90)) break;
                    if (!MatrixNearBuf(rec, matoffs[mi], refPos, 150.0f)) break;
                    ++cnt;
                }
                if (cnt > bestCnt) { bestCnt = cnt; bestOff = off; bestStride = strides[si]; bestMat = matoffs[mi]; }
            }
        }
    }
    if (bestCnt >= 6) {
        g_jarr = (uint8_t*)(uintptr_t)Rd32(model + bestOff);
        g_jstride = bestStride;
        g_jmatoff = bestMat;
        g_jarrEnd = g_jarr + (size_t)512 * g_jstride;
        Log("model: joints at model+0x%X stride=0x%X mat=+0x%X (%d records near cloud)",
            bestOff, bestStride, bestMat, bestCnt);
    } else {
        Log("model: joints not found (best +0x%X stride=0x%X mat=+0x%X cnt=%d)",
            bestOff, bestStride, bestMat, bestCnt);
    }
}

static int ModelJointMatrix2(uint16_t ref, float* out)
{
    if (!g_jarr || !g_jstride) return 0;
    float best[16];
    float bestD = 1e30f;
    int found = 0;
    for (int k = 0; k < 256; ++k) {
        const uint8_t* rec = g_jarr + (size_t)k * g_jstride;
        if (g_jarrEnd && rec + g_jstride > g_jarrEnd) break;
        uint8_t hdr[0x08];
        if (!SRead((uint32_t)(uintptr_t)rec, hdr, sizeof(hdr))) break;
        if (hdr[0x07] != (uint8_t)(ref & 0xFF)) continue;
        float m16[16];
        if (!SRead((uint32_t)(uintptr_t)rec + g_jmatoff, m16, 64)) continue;
        const float* m = m16;
        int ok = 1;
        for (int i = 0; i < 16; ++i) if (!(m[i] == m[i])) { ok = 0; break; }
        if (!ok) continue;
        float dx = m[12] - g_centroid[0], dy = m[13] - g_centroid[1], dz = m[14] - g_centroid[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; memcpy(best, m, 64); found = 1; }
    }
    if (!found || bestD > 2000.0f * 2000.0f) return 0;
    memcpy(out, best, 64);
    return 1;
}

static int ModelJointMatrix(uint16_t ref, float* out)
{
    // legacy path: uModel+0x294/0x298/0x29C (correct on the reference dump)
    if (!g_chain) return 0;
    uint8_t* model = (uint8_t*)Rd32(g_chain + 0x210);
    if (!model || !Readable(model, 0x2A0)) return 0;
    uint32_t jn = Rd32(model + 0x294);
    uint32_t jarr = Rd32(model + 0x298);
    uint32_t tab = Rd32(model + 0x29C);
    if (jn < 8 || jn > 1024 || !jarr || !tab) return 0;
    if (!Readable((void*)(uintptr_t)tab, 256)) return 0;
    uint8_t idx = *(uint8_t*)(uintptr_t)(tab + ref);
    if (idx == 0xFF || idx >= jn) return 0;
    uint8_t* j = (uint8_t*)(uintptr_t)jarr + (size_t)idx * 0x90;
    if (!Readable(j + 0x50, 64)) return 0;
    memcpy(out, j + 0x50, 64);
    return 1;
}

static void LogModelDiag(void)
{
    static int done = 0;
    if (done || !g_chain) return;
    done = 1;
    uint8_t* model = (uint8_t*)Rd32(g_chain + 0x210);
    Log("model: mpColModel=%p readable=%d", (void*)model, model && Readable(model, 0x2A0));
    for (int ref = 0; ref < 6; ++ref) {
        float m[16];
        int ok2 = ModelJointMatrix2((uint16_t)ref, m);
        int ok1 = ok2 ? 0 : ModelJointMatrix((uint16_t)ref, m);
        if (ok2 || ok1)
            Log("model: ref %d -> (%.1f %.1f %.1f)%s", ref, m[12], m[13], m[14], ok2 ? "" : " [legacy]");
    }
}

static void HexDump(const char* tag, const uint8_t* q, int len)
{
    if (!q || !Readable(q, len)) { Log("%s: unreadable", tag); return; }
    for (int off = 0; off < len; off += 16) {
        char line[160];
        int n = 0;
        n += snprintf(line + n, sizeof(line) - n, "%s+%03X:", tag, off);
        for (int i = 0; i < 16 && off + i < len; ++i)
            n += snprintf(line + n, sizeof(line) - n, " %02X", q[off + i]);
        Log("%s", line);
    }
}

static void DumpChainOnce(void)
{
    static int done = 0;
    if (done || !g_chain) return;
    done = 1;
    HexDump("chain", g_chain, 0x120);
}

static void ResetChainState(void)
{
    g_chain = NULL;
    g_locked = 0;              // a new level may have a different camera
    g_useValid = 0;
    g_jblock = NULL;
    g_chainJoints = 0;
    g_chainPrims = 0;
    g_candN = 0;
    g_jarr = NULL;
    g_jstride = 0;
    g_forceCache = 1;
    g_wantScan = 1;
}

static int ChainStillAlive(void)
{
    if (!g_chain) return 0;
    if (!Readable(g_chain, 0x260)) return 0;
    uint32_t vt = SRd32((uint32_t)(uintptr_t)g_chain);
    if (vt != (CHAIN_VTABLE_PC + g_imgDelta) && vt != (CHAIN_VTABLE_EX + g_imgDelta)) return 0;
    uint32_t res = SRd32((uint32_t)(uintptr_t)g_chain + OFF_MPCHAIN);
    if (!res || !Readable((void*)(uintptr_t)res, 0x78)) return 0;
    if (SRd32((uint32_t)(uintptr_t)res + 0x60) != RCHAIN_MAGIC) return 0;
    return 1;
}

static void ReportPenetration(void)
{
    if (!g_chain || g_chainJoints <= 0) return;
    uint8_t* chainRes = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + OFF_MPCHAIN);
    uint8_t* col = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + OFF_COLLISION);
    uint8_t* cinfo = col ? (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)col + RCCOL_MPINFO) : NULL;
    if (!cinfo || g_chainPrims <= 0 || !Readable(cinfo, g_chainPrims * COLINFO_SIZE)) {
        Log("PEN: no ccl data");
        return;
    }
    if (g_jcacheN == 0) return;

    float pc[16][3];
    float pr[16];
    int pn = 0;
    for (int i = 0; i < g_chainPrims && pn < 16; ++i) {
        uint8_t* ci = cinfo + i * COLINFO_SIZE;
        uint16_t ref = *(uint16_t*)(ci + 4);
        float m[16];
        if (!(ModelJointMatrix2(ref, m) || ModelJointMatrix(ref, m))) continue;
        Xform(m, (const float*)(ci + 0x10), pc[pn]);
        pr[pn] = Rdf(ci + 0x2C);
        ++pn;
    }
    if (pn == 0) { Log("PEN: no usable primitives"); return; }

    int inside = 0, considered = 0;
    for (int i = 0; i < g_jcacheN; ++i) {
        float m16[16];
        if (!SRead((uint32_t)(uintptr_t)g_jcache[i] + JW_WORLDMAT, m16, 64)) continue;
        float pw[3] = { m16[12], m16[13], m16[14] };
        uint16_t ref = (uint16_t)SRd32((uint32_t)(uintptr_t)g_jcache[i] + JW_REFJOINT);
        float best = -1e9f;
        for (int k = 0; k < pn; ++k) {
            float dx = pw[0] - pc[k][0], dy = pw[1] - pc[k][1], dz = pw[2] - pc[k][2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            float pen = pr[k] + 1.0f - d;
            if (pen > best) best = pen;
        }
        ++considered;
        if (best > 0) ++inside;
        if (best > -3.0f)
            Log("PEN j%02d ref=%u pen=%+.2f %s", i, ref, best, best > 0 ? "INSIDE" : "");
    }
    Log("PEN summary: %d joints, %d inside", considered, inside);
}

// ------------------------------------------------------------------ drawing
#define MAXV 16384

static void BeginLines(IDirect3DDevice9* dev)
{
    dev->SetFVF(FVF_LINE);
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

static void AddCircle(VERT* out, int* n, int maxv, float cx, float cy, float r, DWORD color)
{
    const int seg = 16;
    if (r < 1.0f) r = 1.0f;
    if (r > 400.0f) r = 400.0f;
    for (int i = 0; i < seg; ++i) {
        if (*n + 2 > maxv) return;
        float a0 = (float)i / seg * 6.2831853f;
        float a1 = (float)(i + 1) / seg * 6.2831853f;
        out[*n].x = cx + cosf(a0) * r; out[*n].y = cy + sinf(a0) * r;
        out[*n].z = 0; out[*n].rhw = 1; out[*n].color = color; ++*n;
        out[*n].x = cx + cosf(a1) * r; out[*n].y = cy + sinf(a1) * r;
        out[*n].z = 0; out[*n].rhw = 1; out[*n].color = color; ++*n;
    }
}

static void AddLine(VERT* out, int* n, int maxv, float x0, float y0, float x1, float y1, DWORD color)
{
    if (*n + 2 > maxv) return;
    out[*n].x = x0; out[*n].y = y0; out[*n].z = 0; out[*n].rhw = 1; out[*n].color = color; ++*n;
    out[*n].x = x1; out[*n].y = y1; out[*n].z = 0; out[*n].rhw = 1; out[*n].color = color; ++*n;
}

static void DrawAlive(IDirect3DDevice9* dev)
{
    VERT q[6];
    float pts[6][2] = { {8,8}, {28,8}, {8,28}, {28,8}, {28,28}, {8,28} };
    int found = (g_chain != NULL && g_chainJoints > 0);
    for (int i = 0; i < 6; ++i) {
        q[i].x = pts[i][0]; q[i].y = pts[i][1]; q[i].z = 0; q[i].rhw = 1;
        q[i].color = found ? 0xFF00FF00u : 0xFFFF0000u;
    }
    dev->SetFVF(FVF_LINE);
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(VERT));
}

static float ViewDepth(const float* world)
{
    const float* v0 = g_blocks[g_viewReg];
    const float* v1 = g_blocks[g_viewReg + 1];
    const float* v2 = g_blocks[g_viewReg + 2];
    float pz;
    if (g_viewMode == 0)
        pz = world[0] * v2[0] + world[1] * v2[1] + world[2] * v2[2] + v2[3];
    else
        pz = world[0] * v0[2] + world[1] * v1[2] + world[2] * v2[2] + v2[3];
    return fabsf(pz);
}

// Guard against a collapsed projection: if the current choice maps the cloud
// into a line or a dot, try the other convention / handedness for this frame.
static void EnsureProjectionSane(int W, int H)
{
    static LONG lastCheck = -1000;
    if (g_scOk || g_useValid || g_locked) return;   // locked = trust the calibration
    if (g_frame - lastCheck < 30) return;
    lastCheck = g_frame;

    float world[160][3];
    int n = 0;
    GatherJoints(world, &n, 160);
    if (n < 3) return;


    float bestMin = -1.0f;
    int bestMode = g_viewMode, bestSign = g_wSign;
    int modes[2] = { g_viewMode, 1 - g_viewMode };
    for (int mi = 0; mi < 2; ++mi) {
        for (int sg = 1; sg >= -1; sg -= 2) {
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                float sx, sy;
                if (!ProjectWith(g_viewReg, modes[mi], sg, world[i], W, H, &sx, &sy)) continue;
                ++ok;
                if (sx < minx) minx = sx;
                if (sx > maxx) maxx = sx;
                if (sy < miny) miny = sy;
                if (sy > maxy) maxy = sy;
            }
            if (ok < (n * 6) / 10) continue;
            float wpx = maxx - minx, hpx = maxy - miny;
            float mn = wpx < hpx ? wpx : hpx;
            if (mn > bestMin) { bestMin = mn; bestMode = modes[mi]; bestSign = sg; }
        }
    }
    float curMin = (bestMode == g_viewMode && bestSign == g_wSign) ? bestMin : 0.0f;
    if (bestMin > 25.0f && (bestMode != g_viewMode || bestSign != g_wSign) &&
        bestMin > curMin * 1.4f) {
        Log("projection: switching to mode=%d sign=%+d (min axis %.0f px)",
            bestMode, bestSign, bestMin);
        g_viewMode = bestMode;
        g_wSign = bestSign;
    }
}

static float RestLinkLen(uint16_t a, uint16_t b)
{
    if (a > b) { uint16_t t = a; a = b; b = t; }
    for (size_t i = 0; i < sizeof(g_restLinks) / sizeof(g_restLinks[0]); ++i)
        if (g_restLinks[i].a == a && g_restLinks[i].b == b) return g_restLinks[i].rest;
    return 0.0f;
}

// ---- tiny 7-segment number drawing (no font, no D3DX) -------------------
static const unsigned char kSeg[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void DrawSegDigit(VERT* v, int* n, int maxv, int digit, float x, float y,
                         float w, float h, DWORD col)
{
    if (digit < 0 || digit > 9) return;
    unsigned char m = kSeg[digit];
    float t = 1.0f;
    float x1 = x + w, y1 = y, y2 = y + h * 0.5f, y3 = y + h;
    if (m & 0x01) AddLine(v, n, maxv, x, y1, x1, y1, col);          // a
    if (m & 0x02) AddLine(v, n, maxv, x1, y1, x1, y2, col);          // b
    if (m & 0x04) AddLine(v, n, maxv, x1, y2, x1, y3, col);          // c
    if (m & 0x08) AddLine(v, n, maxv, x, y3, x1, y3, col);           // d
    if (m & 0x10) AddLine(v, n, maxv, x, y2, x, y3, col);            // e
    if (m & 0x20) AddLine(v, n, maxv, x, y1, x, y2, col);            // f
    if (m & 0x40) AddLine(v, n, maxv, x, y2, x1, y2, col);           // g
}

static void DrawSegNumber(VERT* v, int* n, int maxv, int value, float x, float y,
                          float w, float h, DWORD col)
{
    char buf[8];
    int len = 0;
    if (value == 0) buf[len++] = '0';
    while (value > 0 && len < 7) { buf[len++] = (char)('0' + value % 10); value /= 10; }
    float cx = x;
    for (int i = len - 1; i >= 0; --i) {
        DrawSegDigit(v, n, maxv, buf[i] - '0', cx, y, w, h, col);
        cx += w + 2.5f;
    }
}

static void LogChainDiagnostics(void)
{
    static uint8_t* logged = NULL;
    if (!g_chain || logged == g_chain) return;
    logged = g_chain;

    int resolved = 0, total = 0;
    for (int i = 0; i < g_jcacheN && i < 160; ++i) {
        uint16_t ref = (uint16_t)SRd32((uint32_t)(uintptr_t)g_jcache[i] + JW_REFJOINT);
        float m[16];
        ++total;
        if (ModelJointMatrix2(ref, m) || ModelJointMatrix(ref, m)) ++resolved;
    }
    Log("diag: chain=%p joints=%d prims=%d refs resolved %d/%d modelJoints=%s",
        (void*)g_chain, g_chainJoints, g_chainPrims, resolved, total, g_jarr ? "yes" : "no");
}

static void DrawDebug(IDirect3DDevice9* dev)
{
    if (!g_show || !g_chain || !g_camOk) return;

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    int W = (int)vp.Width, H = (int)vp.Height;

    static VERT verts[MAXV];
    int n = 0;

    Calibrate(W, H);
    EnsureProjectionSane(W, H);
    LogChainDiagnostics();

    uint8_t* chainRes = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)g_chain + OFF_MPCHAIN);
    uint8_t* info = (uint8_t*)(uintptr_t)SRd32((uint32_t)(uintptr_t)chainRes + RCHAIN_MPINFO);
    int joints = g_chainJoints;
    if (joints > MAXV) joints = MAXV;
    if (!JointBase() || !Readable(info, 0x40)) return;

    float world[MAXV][3];
    uint16_t refs[MAXV];
    int count = 0;
    if (g_forceCache || g_jcacheN == 0 || (g_frame - g_jcacheFrame) > 120) { g_forceCache = 0; RefreshJointCache(); }
    for (int i = 0; i < g_jcacheN && i < joints; ++i) {
        const float* m = (const float*)(g_jcache[i] + JW_WORLDMAT);
        float x = m[12], y = m[13], z = m[14];
        if (!(x == x) || !(y == y) || !(z == z)) continue;
        if (x == 0.0f && y == 0.0f && z == 0.0f) continue;
        world[count][0] = x; world[count][1] = y; world[count][2] = z;
        refs[count] = (uint16_t)Rd32(g_jcache[i] + JW_REFJOINT);
        ++count;
    }

    if (count > 0) {
        float cx = 0, cy = 0, cz = 0;
        int used = 0;
        for (int i = 0; i < count; ++i) {
            float d = fabsf(world[i][0]) + fabsf(world[i][1]) + fabsf(world[i][2]);
            if (d < 3.0f) continue;                 // stuck at the origin
            cx += world[i][0]; cy += world[i][1]; cz += world[i][2];
            ++used;
        }
        if (used) {
            g_centroid[0] = cx / used;
            g_centroid[1] = cy / used;
            g_centroid[2] = cz / used;
        }
    }

    float devs[MAXV];
    for (int i = 0; i < count; ++i) devs[i] = -1.0f;

    static unsigned char numDrawn[MAXV];
    memset(numDrawn, 0, sizeof(numDrawn));
    int labelled = 0;

    for (int i = 0; i < count; ++i) {
        float sx, sy;
        if (!Project(world[i], W, H, &sx, &sy)) continue;

        DWORD col = 0xFF00FF00;                 // green: close to its bone
        float m16[16];
        if (ModelJointMatrix2(refs[i], m16) || ModelJointMatrix(refs[i], m16)) {
            float dx = world[i][0] - m16[12];
            float dy = world[i][1] - m16[13];
            float dz = world[i][2] - m16[14];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            devs[i] = d;
            if (g_strandDbg) {
                if (d > 25.0f)      col = 0xFFFF0000;      // red
                else if (d > 12.0f) col = 0xFFFFFF00;      // yellow
                else if (d > 5.0f)  col = 0xFFFFA500;      // orange
            }
        }
        float manhattan = fabsf(world[i][0]) + fabsf(world[i][1]) + fabsf(world[i][2]);
        if (manhattan < 3.0f) {
            col = 0xFFFFFFFF;                        // white: position is the origin
            static int originLogged = 0;
            if (originLogged < 3) {
                originLogged++;
                Log("joint ref=%u sits at the world origin (bone missing in the model?)", refs[i]);
            }
        }
        AddCircle(verts, &n, MAXV, sx, sy, 4.5f, col);

    }

    int nch = *(uint8_t*)(chainRes + 0x6C);
    for (int c = 0; c < nch && c < 8; ++c) {
        uint8_t* ci = info + c * 0x20;
        uint8_t num = ci[0];
        uint32_t off = Rd32(ci + 4);
        uint8_t* jbase = ci + off;
        if (!Readable(jbase, num * 0x60)) continue;
        for (int k = 0; k < num; ++k) {
            uint8_t* jinfo = jbase + k * 0x60;
            uint16_t self = *(uint16_t*)(jinfo + 4);
            uint8_t nn = jinfo[6];
            if (nn > 8) nn = 8;
            int ia = -1, ib;
            for (int q = 0; q < count; ++q) if (refs[q] == self) { ia = q; break; }
            if (ia < 0) continue;
            float ax, ay;
            if (!Project(world[ia], W, H, &ax, &ay)) continue;
            for (int nb = 0; nb < nn; ++nb) {
                uint16_t other = jinfo[0x40 + nb];
                ib = -1;
                for (int q = 0; q < count; ++q) if (refs[q] == other) { ib = q; break; }
                if (ib < 0) continue;
                float bx, by;
                if (!Project(world[ib], W, H, &bx, &by)) continue;
                float dx = world[ia][0] - world[ib][0];
                float dy = world[ia][1] - world[ib][1];
                float dz = world[ia][2] - world[ib][2];
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                DWORD lc = 0x8000FF00;
                float rest = RestLinkLen(self, other);
                float ratio = rest > 0.5f ? len / rest : 0.0f;
                if (g_strandDbg) {
                    if (ratio > 1.8f)       lc = 0xFFFF00FF;   // magenta: stretched
                    else if (ratio > 1.4f)  lc = 0xFFFFFF00;   // yellow: stretching
                    else if (ratio > 1.15f) lc = 0xFFFFA500;   // orange: slight
                }
                AddLine(verts, &n, MAXV, ax, ay, bx, by, lc);

                if (g_strandDbg && ratio > 1.5f && labelled < 24) {
                    if (!numDrawn[ia]) {
                        numDrawn[ia] = 1; ++labelled;
                        DrawSegNumber(verts, &n, MAXV, self, ax + 7.0f, ay - 9.0f, 4.0f, 9.0f, 0xFFFF00FF);
                    }
                    if (!numDrawn[ib]) {
                        numDrawn[ib] = 1; ++labelled;
                        DrawSegNumber(verts, &n, MAXV, other, bx + 7.0f, by - 9.0f, 4.0f, 9.0f, 0xFFFF00FF);
                    }
                }
                if (g_strandDbg && (g_frame % 120) == 0 && ratio > 1.15f) {
                    // distance between the two bones themselves: if the joint
                    // distance matches it, nothing is stretched - the link is
                    // simply long in the skeleton
                    float ma[16], mb[16];
                    float boneLen = -1.0f;
                    if (ModelJointMatrix2(self, ma) || ModelJointMatrix(self, ma)) {
                        if (ModelJointMatrix2(other, mb) || ModelJointMatrix(other, mb)) {
                            float bx = ma[12] - mb[12], by = ma[13] - mb[13], bz = ma[14] - mb[14];
                            boneLen = sqrtf(bx * bx + by * by + bz * bz);
                        }
                    }
                    Log("LINK %u-%u len=%.1f bone=%.1f rest=%.1f x=%.2f",
                        self, other, len, boneLen, rest, ratio);
                }
            }
        }
    }

    if (g_strandDbg && (g_frame % 120) == 0) {
        for (int i = 0; i < count; ++i) {
            if (devs[i] > 5.0f)
                Log("DEV ref=%u dev=%.1f pos=(%.0f %.0f %.0f)",
                    refs[i], devs[i], world[i][0], world[i][1], world[i][2]);
        }
    }

    int cclUnresolved = 0;
    if (g_chainPrims > 0) {
        uint32_t col = SRd32((uint32_t)(uintptr_t)g_chain + OFF_COLLISION);
        uint32_t cinfo = col ? SRd32(col + RCCOL_MPINFO) : 0;
        if (cinfo) {
            for (int i = 0; i < g_chainPrims; ++i) {
                uint8_t rec[COLINFO_SIZE];
                if (!SRead(cinfo + i * COLINFO_SIZE, rec, sizeof(rec))) break;
                uint8_t* ci = rec;
                uint16_t ref = *(uint16_t*)(ci + 4);
                uint16_t pair = *(uint16_t*)(ci + 6);
                uint8_t shape = ci[8];
                if (ref > 255 || pair > 255) continue;
                const float* lp = (const float*)(ci + 0x10);
                const float* le = (const float*)(ci + 0x20);
                float radius = Rdf(ci + 0x2C);

                float m0[16];
                if (!(ModelJointMatrix2(ref, m0) || ModelJointMatrix(ref, m0))) {
                    ++cclUnresolved;
                    continue;
                }
                float c0[3];
                Xform(m0, lp, c0);

                float sx, sy;
                if (!Project(c0, W, H, &sx, &sy)) continue;

                float depth = ViewDepth(c0);
                float rpx = (depth > 1.0f) ? radius * g_fx * (0.5f * (float)W) / depth : 6.0f;
                if (rpx < 2.0f) rpx = 2.0f;
                if (rpx > 500.0f) rpx = 500.0f;

                if (shape == 0) {
                    AddCircle(verts, &n, MAXV, sx, sy, rpx, 0xFFFF3030);
                } else if (shape == 1) {
                    float m1[16];
                    float c1[3];
                    if (!(ModelJointMatrix2(pair, m1) || ModelJointMatrix(pair, m1))) continue;
                    Xform(m1, le, c1);
                    float ex, ey;
                    if (Project(c1, W, H, &ex, &ey)) {
                        AddLine(verts, &n, MAXV, sx, sy, ex, ey, 0xFFFFD000);
                        AddCircle(verts, &n, MAXV, ex, ey, rpx, 0xFFFFD000);
                    }
                    AddCircle(verts, &n, MAXV, sx, sy, rpx, 0xFFFFD000);
                } else {
                    AddCircle(verts, &n, MAXV, sx, sy, rpx, 0xFFB060FF);
                }
            }
        }
    }

    if (count > 0 && n < 2) {
        static int warn = 0;
        if (++warn < 5) Log("draw: %d joints but no geometry on screen (invalid sim values?)", count);
    }

    if (cclUnresolved && (g_frame % 300) == 0)
        Log("ccl: %d of %d primitives unresolved (missing bone for their ref?)",
            cclUnresolved, g_chainPrims);

    if (n >= 2) {
        BeginLines(dev);
        dev->DrawPrimitiveUP(D3DPT_LINELIST, n / 2, verts, sizeof(VERT));
    }
}

// ------------------------------------------------------------------ d3d9 hook
static void* PatchVtable(void* obj, int index, void* hook)
{
    void** vt = *(void***)obj;
    DWORD old = 0;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) return NULL;
    void* orig = vt[index];
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return orig;
}

static void WriteJmp(void* target, void* hook, BYTE* saved)
{
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(saved, target, 5);
    BYTE* p = (BYTE*)target;
    p[0] = 0xE9;
    int32_t rel = (int32_t)((BYTE*)hook - (p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
}

static void RestoreJmp(void* target, const BYTE* saved)
{
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(target, saved, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
}


static void CamFromRows(const float (*m)[4], float* out)
{
    out[0] = -(m[0][0] * m[0][3] + m[1][0] * m[1][3] + m[2][0] * m[2][3]);
    out[1] = -(m[0][1] * m[0][3] + m[1][1] * m[1][3] + m[2][1] * m[2][3]);
    out[2] = -(m[0][2] * m[0][3] + m[1][2] * m[1][3] + m[2][2] * m[2][3]);
}

static HRESULT __stdcall Hook_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                                   INT baseVertex, UINT minIndex, UINT numVerts,
                                                   UINT startIndex, UINT primCount)
{
    // Score every draw of this frame and keep the one whose implied camera looks
    // like the third-person camera: a few metres behind the character and moving
    // smoothly from the previous frame.  Shadow/reflection passes are far away and
    // jump between frames, so they lose.
    float cam[3];
    CamFromRows((const float (*)[4])g_blocks, cam);
    int finite = 1;
    for (int i = 0; i < 3; ++i) if (!(cam[i] == cam[i]) || fabsf(cam[i]) > 1e6f) finite = 0;
    float fx = g_blocks[7][0], fy = g_blocks[7][1];

    if (finite && fx > 0.05f && fy > 0.05f && g_chain) {
        // never touch chain memory from inside a draw hook: use the cached
        // centroid only (it may be gone while the level is unloading)
        if (g_jcacheN > 0) {
            const float* cen = g_centroid;
            float dx = cam[0] - cen[0], dy = cam[1] - cen[1], dz = cam[2] - cen[2];
            float camDist = sqrtf(dx * dx + dy * dy + dz * dz);
            float score = fabsf(camDist - 350.0f);      // typical over-the-shoulder range
            if (g_lastCamValid) {
                float jx = cam[0] - g_lastCam[0], jy = cam[1] - g_lastCam[1], jz = cam[2] - g_lastCam[2];
                float jump = sqrtf(jx * jx + jy * jy + jz * jz);
                if (jump > 150.0f) score += 1e4f;       // another pass, not this frame's camera
                score += jump * 0.5f;                   // prefer the smoother candidate
            }
            if (score < g_frameBestScore) {
                g_frameBestScore = score;
                memcpy(g_frameBest, g_blocks, sizeof(g_frameBest));
                memcpy(g_frameBestCam, cam, sizeof(g_frameBestCam));
                g_frameHave = 1;
            }
        }
    }

    if (!g_origDrawIP) return D3DERR_INVALIDCALL;
    return g_origDrawIP(dev, type, baseVertex, minIndex, numVerts, startIndex, primCount);
}

static HRESULT __stdcall Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                     HWND wnd, const RGNDATA* dirty)
{
    g_frameCaptured = 0;   // draws of the next frame may capture

    if (!g_captureEnabled) g_useValid = 0;   // F9 off: use the constant-block path

    if (g_captureEnabled && g_frameHave) {
        memcpy(g_use, g_frameBest, sizeof(g_use));
        memcpy(g_lastCam, g_frameBestCam, sizeof(g_lastCam));
        g_lastCamValid = 1;
        g_useValid = 1;
        float dx = g_lastCam[0] - g_centroid[0];
        float dy = g_lastCam[1] - g_centroid[1];
        float dz = g_lastCam[2] - g_centroid[2];
        g_useCamDist = sqrtf(dx * dx + dy * dy + dz * dz);
        g_frameHave = 0;
        g_frameBestScore = 1e9f;
    }

    g_frameCaptured = 0;   // draws of the next frame may capture once
    InterlockedIncrement(&g_frame);
    static LARGE_INTEGER freq = { 0 }, prev = { 0 };
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static double acc = 0.0;
    static int accN = 0;
    if (prev.QuadPart) {
        acc += (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        ++accN;
    }
    prev = now;
    if (accN >= 120) {
        Log("perf: %.1f fps (%.1f ms/frame), show=%d draw=%.2f ms", accN / acc,
            1000.0 * acc / accN, g_show, g_drawMs / accN);
        acc = 0.0; accN = 0; g_drawMs = 0.0;
    }

    static int prevF7 = 0;
    int f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !prevF7) { g_show = !g_show; Log("show=%d", g_show); }
    prevF7 = f7;

    static int prevF8 = 0;
    int f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !prevF8) { g_strandDbg = !g_strandDbg; Log("strand debug=%d", g_strandDbg); }
    prevF8 = f8;

    static int prevF9 = 0;
    int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !prevF9) {
        g_captureEnabled = !g_captureEnabled;
        g_useValid = 0;
        g_locked = 0;
        g_useCamDist = 1e9f;
        g_lastCamValid = 0;
        Log("draw-time capture=%d (takes effect on the next device reset)", g_captureEnabled);
    }
    prevF9 = f9;

    if (g_candN > 0 && (g_frame % 120) == 0) PickNearestChain();
    {
        static int badFrames = 0;
        if (g_chain && !ChainStillAlive()) {
            if (++badFrames > 120) {
                Log("chain gone for 120 frames, resetting");
                badFrames = 0;
                ResetChainState();
                g_lastCamValid = 0;
                g_useValid = 0;
                g_useCamDist = 1e9f;
                if (!g_scanRunning) CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
            }
        } else {
            badFrames = 0;
        }
    }
    if (dev->TestCooperativeLevel() != D3D_OK) {
        return g_origPresent(dev, src, dst, wnd, dirty);
    }
    DrawAlive(dev);
    // (object dump removed: races with level teardown)
    if (g_chain && g_chainPrims > 0 && (g_frame % (g_strandDbg ? 120 : 1200)) == 0)
        ReportPenetration();
    if (g_chain && (g_frame % 300) == 0) {
        LogModelDiag();
        uint8_t tail[0x10];
        int tailOk = SRead((uint32_t)(uintptr_t)g_chain + 0x220, tail, sizeof(tail));
        Log("ccl: jmat=%08X ncol=%u enable=%u work=%08X",
            SRd32((uint32_t)(uintptr_t)g_chain + g_offCJ),
            SRd32((uint32_t)(uintptr_t)g_chain + OFF_COLINFONUM),
            tailOk ? (unsigned)tail[0x08] : 0u,
            SRd32((uint32_t)(uintptr_t)g_chain + 0x224));
    }
    {
        LARGE_INTEGER f2, a, b;
        QueryPerformanceCounter(&a);
        QueryPerformanceFrequency(&f2);
        if (g_show && g_chain && ChainStillAlive()) DrawDebug(dev);
        QueryPerformanceCounter(&b);
        g_drawMs += 1000.0 * (double)(b.QuadPart - a.QuadPart) / (double)f2.QuadPart;
    }
    return g_origPresent(dev, src, dst, wnd, dirty);
}

static HRESULT __stdcall Hook_EndScene(IDirect3DDevice9* dev)
{
    return g_origEndScene(dev);
}

static HRESULT __stdcall Hook_Reset(IDirect3DDevice9* dev, void* params)
{
    Log("device reset - dropping cached chain");
    g_chain = NULL;
    g_useValid = 0;
    g_frameHave = 0;
    g_frameBestScore = 1e9f;
    g_lastCamValid = 0;
    g_jcacheN = 0;
    g_jcacheFrame = -100000;
    g_jarr = NULL;
    g_jstride = 0;
    g_wantScan = 1;
    HRESULT hr = g_origReset(dev, params);
    return hr;
}

static HRESULT __stdcall Hook_SetVSConstF(IDirect3DDevice9* dev, UINT reg, const float* data, UINT count)
{
    if (data && count) {
        for (UINT k = 0; k < count && reg + k < NBLOCKS; ++k)
            memcpy(g_blocks[reg + k], data + 4 * k, 4 * sizeof(float));
        if (reg == 4 && count >= 3) {
            const float* a = data;
            const float* b = data + 4;
            const float* c = data + 8;
            float la = a[0]*a[0] + a[1]*a[1] + a[2]*a[2];
            float lb = b[0]*b[0] + b[1]*b[1] + b[2]*b[2];
            float lc = c[0]*c[0] + c[1]*c[1] + c[2]*c[2];
            float dab = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
            float dac = a[0]*c[0] + a[1]*c[1] + a[2]*c[2];
            float dbc = b[0]*c[0] + b[1]*c[1] + b[2]*c[2];
            if (fabsf(la - 1) < 0.05f && fabsf(lb - 1) < 0.05f && fabsf(lc - 1) < 0.05f &&
                fabsf(dab) < 0.05f && fabsf(dac) < 0.05f && fabsf(dbc) < 0.05f) {
                memcpy(g_view, data, 12 * sizeof(float));
                g_viewOk = 1;
                ++g_viewHits;
            }
        }
        if (reg == 7 && count == 3) {
            if (data[0] > 0.01f && data[5] > 0.01f) { g_fx = data[0]; g_fy = data[5]; }
            g_camOk = 1;
        }
    }
    return g_origSetVSConstF(dev, reg, data, count);
}

static HRESULT __stdcall Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, DWORD type,
                                           HWND hwnd, DWORD flags, void* pp, IDirect3DDevice9** out)
{
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DDevice9* dev = *out;
        g_origEndScene = (PFN_EndScene)PatchVtable(dev, 42, (void*)Hook_EndScene);
        g_origPresent = (PFN_Present)PatchVtable(dev, 17, (void*)Hook_Present);
        g_origReset = (PFN_Reset)PatchVtable(dev, 16, (void*)Hook_Reset);
        // draw-time capture is disabled: installing DrawIndexedPrimitive paired with
        // level unload reproduced a crash.  The code is kept for future work.
        (void)Hook_DrawIndexedPrimitive;
        g_origSetVSConstF = (PFN_SetVSConstF)PatchVtable(dev, 94, (void*)Hook_SetVSConstF);
        Log("device %p hooked EndScene=%p SetVSConstF=%p", (void*)dev,
            (void*)g_origEndScene, (void*)g_origSetVSConstF);
        if (g_wantScan && !g_scanRunning) CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
    }
    return hr;
}

static IDirect3D9* __stdcall Hook_D3DCreate9(UINT sdk)
{
    RestoreJmp(g_d3d9Export, g_d3d9Orig);
    IDirect3D9* d3d = g_origD3DCreate9(sdk);
    WriteJmp(g_d3d9Export, (void*)Hook_D3DCreate9, g_d3d9Orig);
    Log("Direct3DCreate9 intercepted, d3d=%p", (void*)d3d);
    if (d3d) g_origCreateDevice = (PFN_CreateDevice)PatchVtable(d3d, 16, (void*)Hook_CreateDevice);
    return d3d;
}

static DWORD WINAPI Install(LPVOID)
{
    Sleep(100);
    Log("installer started");
    HMODULE h = LoadLibraryA("d3d9.dll");
    if (!h) { Log("LoadLibrary(d3d9) failed %lu", GetLastError()); return 0; }
    g_d3d9Export = (void*)GetProcAddress(h, "Direct3DCreate9");
    if (!g_d3d9Export) { Log("Direct3DCreate9 not found"); return 0; }
    g_origD3DCreate9 = (PFN_D3DCreate9)g_d3d9Export;
    Log("d3d9=%p export=%p", (void*)h, g_d3d9Export);
    WriteJmp(g_d3d9Export, (void*)Hook_D3DCreate9, g_d3d9Orig);
    g_patched = 1;
    Log("d3d9 Direct3DCreate9 patched");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitCrashProbe(hmod);
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = 1;
        DisableThreadLibraryCalls(hmod);
        g_log = OpenLog();
        Log("re5_chaindbg v2 loaded");
        CreateThread(NULL, 0, Install, NULL, 0, NULL);
    }
    return TRUE;
}
