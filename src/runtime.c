// Core runtime: guest memory, PE loading, dispatch, x87 helpers, HLE math.
#include "recomp.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#if defined(__GNUC__)
__attribute__((aligned(4096)))
#endif
uint8_t G[GUEST_SIZE];
Regs R;
uint32_t DF;
double FPR[8];
uint32_t FTOP, FSW_CC, FCW = 0x27f;

// ---------------------------------------------------------------- logging
static FILE *g_log;
void host_log(const char *fmt, ...) {
    va_list ap;
    if (!g_log) g_log = fopen("port.log", "w");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}

uint32_t mod_from_address(uint32_t a);
void host_backtrace(void) {
    // walk guest ebp chain for diagnostics
    uint32_t bp = R.ebp;
    host_log("  esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n",
             R.esp, R.ebp, R.eax, R.ecx, R.edx, R.ebx, R.esi, R.edi);
    for (int i = 0; i < 24 && bp > 0x10000 && bp < 0x380000; i++) {
        host_log("  frame %08x ret %08x\n", bp, R32(bp + 4));
        bp = R32(bp);
    }
    // also scan stack for code addresses
    host_log("  stack scan:");
    for (uint32_t p = R.esp, n = 0; p < 0x370000 && n < 24; p += 4) {
        uint32_t v = R32(p);
        if (v < MAGIC_BASE && mod_from_address(v)) { host_log(" %08x", v); n++; }
    }
    host_log("\n");
}

// the last shim calls, printed with a fatal error
#define NRECENT 48
static struct { const char *name; uint32_t a[4], from; } recent[NRECENT];
static unsigned recent_n;
void host_recent_dump(void) {
    unsigned n = recent_n < NRECENT ? recent_n : NRECENT;
    host_log("  last %u shim calls:\n", n);
    for (unsigned i = recent_n - n; i < recent_n; i++) {
        unsigned k = i % NRECENT;
        host_log("    %s(%08x, %08x, %08x, %08x) from %08x\n", recent[k].name, recent[k].a[0], recent[k].a[1], recent[k].a[2], recent[k].a[3], recent[k].from);
    }
}
void host_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    host_log("FATAL: %s\n", buf);
    host_recent_dump();
    host_backtrace();
    host_shutdown();
    exit(1);
}

// ---------------------------------------------------------------- traps
// rdtsc: a 1 GHz counter from the host clock. UE2 calibrates GSecondsPerCycle against it at start-up and times
// everything (appSeconds) with it.
uint64_t host_rdtsc(void) {
    extern uint64_t host_time_ns(void);
    return host_time_ns();
}
uint32_t guest_divzero(uint32_t addr) { host_log("divide by zero at %08x\n", addr); return 1; }
void guest_int(int n, uint32_t addr) { host_fatal("int %d at %08x", n, addr); }
void guest_unsupported(uint32_t addr) { host_fatal("unsupported instruction at %08x", addr); }
// What the game sees from CPUID: a Pentium Pro class CPU with an FPU, TSC, CX8 and CMOV but no MMX, SSE or
// 3DNow!, so D3DX, the CRT and the engine take their x87/integer code paths (the recompiler doesn't lift SIMD).
void host_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    (void)sub;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (leaf == 0) { out[0] = 1; memcpy(&out[1], "Genu", 4); memcpy(&out[3], "ineI", 4); memcpy(&out[2], "ntel", 4); }
    else if (leaf == 1) { out[0] = 0x0633; out[3] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); }
}
void guest_badaddr(uint32_t addr) { host_fatal("execution reached non-code address %08x", addr); }
void guest_badjump(uint32_t addr, uint32_t t) { host_fatal("bad switch jump at %08x -> %08x", addr, t); }
void guest_noreturn(uint32_t addr) { host_fatal("returned from noreturn call at %08x", addr); }
#ifdef MEMCHECK
void guest_memfault(uint32_t a, int n) { host_fatal("memory access out of range %08x (%d)", a, n); }
#endif

void guest_movs(uint32_t dst, uint32_t src, uint32_t count, uint32_t el) {
    uint32_t n = count * el;
    if (!n) return;
    if (dst > src && dst - src < n) {
        // forward overlapping copy: element-wise semantics
        for (uint32_t i = 0; i < n; i += el) (COW(dst + i, (uint32_t)(el)), memcpy(G + dst + i, G + src + i, el));
    } else {
        (COW(dst, (uint32_t)(n)), memmove(G + dst, G + src, n));
    }
}
void guest_stos8(uint32_t dst, uint32_t v, uint32_t count) { (COW(dst, (uint32_t)(count)), memset(G + dst, (int)(v & 0xff), count)); }
void guest_stos16(uint32_t dst, uint32_t v, uint32_t count) { for (uint32_t i = 0; i < count; i++) W16(dst + i * 2, v); }
void guest_stos32(uint32_t dst, uint32_t v, uint32_t count) {
    if (v == 0) { (COW(dst, (uint32_t)((size_t)count * 4)), memset(G + dst, 0, (size_t)count * 4)); return; }
    for (uint32_t i = 0; i < count; i++) W32(dst + i * 4, v);
}

// ---------------------------------------------------------------- x87
double F80(uint32_t a) {
    uint64_t m = R64(a);
    uint32_t se = R16(a + 8);
    int s = se >> 15, e = se & 0x7fff;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7fff) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return s ? -v : v;
}
void WF80(uint32_t a, double v) {
    uint64_t m = 0; uint32_t se = 0;
    if (signbit(v)) { se = 0x8000; v = -v; }
    if (v == 0.0) { m = 0; }
    else if (isinf(v)) { se |= 0x7fff; m = 0x8000000000000000ull; }
    else if (isnan(v)) { se |= 0x7fff; m = 0xC000000000000000ull; }
    else {
        int e; double f = frexp(v, &e);    // v = f * 2^e, f in [0.5,1)
        m = (uint64_t)ldexp(f, 64);
        se |= (uint32_t)(e - 1 + 16383);
    }
    W64(a, m); W16(a + 8, se);
}
void fpu_com(double a, double b) {
    FSW_CC &= ~0x4500u;
    if (isnan(a) || isnan(b)) FSW_CC |= 0x4500;
    else if (a < b) FSW_CC |= 0x100;
    else if (a == b) FSW_CC |= 0x4000;
}
uint32_t fpu_sw(void) { return (FSW_CC & 0x4700u) | ((FTOP & 7) << 11); }
static double fpu_rnd(double v) {
    switch ((FCW >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
double fpu_round(double v) { return fpu_rnd(v); }
int32_t fpu_toint32(double v) {
    v = fpu_rnd(v);
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return INT32_MIN;
    return (int32_t)v;
}
int16_t fpu_toint16(double v) {
    v = fpu_rnd(v);
    if (!(v >= -32768.0 && v <= 32767.0)) return INT16_MIN;
    return (int16_t)v;
}
int64_t fpu_toint64(double v) {
    v = fpu_rnd(v);
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)v;
}
void fpu_xam(void) {
    double v = ST0;
    FSW_CC &= ~0x4700u;
    if (signbit(v)) FSW_CC |= 0x200;
    if (isnan(v)) FSW_CC |= 0x100;
    else if (isinf(v)) FSW_CC |= 0x500;
    else if (v == 0.0) FSW_CC |= 0x4000;
    else FSW_CC |= 0x400;
}
void fpu_prem(int ieee) {
    double a = ST0, b = STI(1);
    ST0 = ieee ? remainder(a, b) : fmod(a, b);
    FSW_CC &= ~0x4700u;
}
void fpu_xtract(void) {
    double v = ST0; int e;
    double f = frexp(v, &e);
    ST0 = (double)(e - 1);
    FPUSH(f * 2.0);
}
void fpu_ldenv(uint32_t a) { FCW = R16(a); FSW_CC = R16(a + 4) & 0x4700; FTOP = (R16(a + 4) >> 11) & 7; }
void fpu_stenv(uint32_t a) { (COW(a, (uint32_t)(28)), memset(G + a, 0, 28)); W16(a, FCW); W16(a + 4, fpu_sw()); W16(a + 8, 0xffff); }
void fpu_save(uint32_t a) {
    fpu_stenv(a);
    for (int i = 0; i < 8; i++) WF80(a + 28 + i * 10, STI(i));
    fpu_init();
}
void fpu_rstor(uint32_t a) {
    fpu_ldenv(a);
    for (int i = 0; i < 8; i++) STI(i) = F80(a + 28 + i * 10);
}
void fpu_init(void) { FCW = 0x37f; FSW_CC = 0; FTOP = 0; }
uint64_t mmx_paddusb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += 8) {
        uint32_t s = (uint32_t)((a >> i) & 0xff) + (uint32_t)((b >> i) & 0xff);
        if (s > 255) s = 255;
        r |= (uint64_t)s << i;
    }
    return r;
}

// ---------------------------------------------------------------- HLE runtime functions
#define HRET() (R.esp += 4)
void hle_ftol(void) {
    double v = ST0; FPOP();
    int64_t r;
    v = trunc(v);
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) r = INT64_MIN; else r = (int64_t)v;
    R.eax = (uint32_t)r; R.edx = (uint32_t)((uint64_t)r >> 32);
    HRET();
}
// a probe answered "no" (Karma's SSE test executes xorps inside __try)
void hle_ret0(void) { R.eax = 0; HRET(); }
void hle_floor(void) { FPUSH(floor(F64(R.esp + 4))); HRET(); }
void hle_ceil(void) { FPUSH(ceil(F64(R.esp + 4))); HRET(); }
void hle_pow(void) { FPUSH(pow(F64(R.esp + 4), F64(R.esp + 12))); HRET(); }
void hle_CIpow(void) { double y = ST0; FPOP(); double x = ST0; ST0 = pow(x, y); HRET(); }
void hle_acos(void) { FPUSH(acos(F64(R.esp + 4))); HRET(); }
void hle_CIacos(void) { ST0 = acos(ST0); HRET(); }
void hle_asin(void) { FPUSH(asin(F64(R.esp + 4))); HRET(); }
void hle_CIasin(void) { ST0 = asin(ST0); HRET(); }
void hle_sin(void) { FPUSH(sin(F64(R.esp + 4))); HRET(); }
void hle_CIsin(void) { ST0 = sin(ST0); HRET(); }
void hle_cos(void) { FPUSH(cos(F64(R.esp + 4))); HRET(); }
void hle_CIcos(void) { ST0 = cos(ST0); HRET(); }
void hle_tan(void) { FPUSH(tan(F64(R.esp + 4))); HRET(); }
void hle_CItan(void) { ST0 = tan(ST0); HRET(); }
void hle_exp(void) { FPUSH(exp(F64(R.esp + 4))); HRET(); }
void hle_CIexp(void) { ST0 = exp(ST0); HRET(); }
void hle_sqrt(void) { FPUSH(sqrt(F64(R.esp + 4))); HRET(); }
void hle_CIsqrt(void) { ST0 = sqrt(ST0); HRET(); }
void hle_modf(void) {
    double ip, f = modf(F64(R.esp + 4), &ip);
    WF64(R32(R.esp + 12), ip);
    FPUSH(f); HRET();
}
void hle_ldexp(void) { FPUSH(ldexp(F64(R.esp + 4), (int32_t)R32(R.esp + 12))); HRET(); }
void hle_frexp(void) {
    int e; double f = frexp(F64(R.esp + 4), &e);
    W32(R32(R.esp + 12), (uint32_t)e);
    FPUSH(f); HRET();
}
void hle_memcpy(void) {
    uint32_t d = R32(R.esp + 4), s = R32(R.esp + 8), n = R32(R.esp + 12);
    if (n > GUEST_SIZE || d > GUEST_SIZE - n || s > GUEST_SIZE - n)
        host_fatal("memcpy(%08x, %08x, %u) out of range, caller %08x", d, s, n, R32(R.esp));
    if (n) (COW(d, (uint32_t)(n)), memmove(G + d, G + s, n));
    R.eax = d; HRET();
}
void hle_memset(void) {
    uint32_t d = R32(R.esp + 4), c = R32(R.esp + 8), n = R32(R.esp + 12);
    if (n) (COW(d, (uint32_t)(n)), memset(G + d, (int)(c & 0xff), n));
    R.eax = d; HRET();
}
void hle_strlen(void) { R.eax = (uint32_t)strlen((char *)G + R32(R.esp + 4)); HRET(); }
// setjmp/longjmp: the game's image decoders (libjpeg/libpng inside D3DX) use them to probe formats and report
// errors. The recompiler calls setjmp in the C frame of the function that calls _setjmp3; that frame stays alive
// for as long as the guest jump point does. One slot per guest jmp_buf address.
#define MAXJMP 64
static GJmp jmps[MAXJMP];
GJmp *hle_setjmp3_slot(void) {
    uint32_t gbuf = R32(R.esp + 4);
    int free_i = -1;
    for (int i = 0; i < MAXJMP; i++) {
        if (jmps[i].gbuf == gbuf) { free_i = i; break; }
        if (!jmps[i].gbuf && free_i < 0) free_i = i;
    }
    if (free_i < 0) { host_log("setjmp: too many jump points, reusing slot 0\n"); free_i = 0; }
    GJmp *j = &jmps[free_i];
    j->gbuf = gbuf;
    j->seh = R32(FS_BASE);
    W32(gbuf, 0x5E7D0000u | (uint32_t)free_i);   // mark the guest jmp_buf
    return j;
}
void hle_setjmp3(void) { R.eax = 0; HRET(); }
void hle_longjmp(void) {
    uint32_t gbuf = R32(R.esp + 4), val = R32(R.esp + 8);
    for (int i = 0; i < MAXJMP; i++) if (jmps[i].gbuf == gbuf) {
        jmps[i].r.eax = val ? val : 1;
        W32(FS_BASE, jmps[i].seh);
        longjmp(jmps[i].env, 1);
    }
    host_fatal("longjmp to an unknown jump buffer %08x", gbuf);
}

// ---------------------------------------------------------------- dispatch
// Guest code addresses of every module map to host functions through one table per module, found by a 64 KB page
// map. Imports the shims implement are "magic" addresses at MAGIC_BASE.
typedef struct {
    const RecModule *rm;
    char name[64];              // file name, e.g. "Core.dll"
    GFunc *disp;                // [text_hi - text_lo]
    int inited, loaded;
} Module;
static Module mods[64];
static int n_mods;
static uint8_t page_mod[0x10000];   // 64 KB page -> module index + 1

#define MAX_THUNKS 8192
static struct { GFunc fn; const char *name; } thunks[MAX_THUNKS];
static uint32_t thunk_calls[MAX_THUNKS];
static int n_thunks;
// how often each shim was called (printed by the watchdog)
void host_call_stats(void) {
    host_log("shim call counts:\n");
    for (int i = 0; i < n_thunks; i++) if (thunk_calls[i]) host_log("  %9u %s\n", thunk_calls[i], thunks[i].name);
}

uint32_t host_thunk(GFunc fn, const char *name) {
    for (int i = 0; i < n_thunks; i++)
        if (thunks[i].fn == fn && fn != NULL) return MAGIC_BASE + i * 4;
    if (n_thunks >= MAX_THUNKS) host_fatal("too many thunks");
    thunks[n_thunks].fn = fn; thunks[n_thunks].name = name;
    return MAGIC_BASE + (n_thunks++) * 4;
}

static inline GFunc *disp_slot(uint32_t a) {
    unsigned m = page_mod[a >> 16];
    if (!m) return NULL;
    Module *md = &mods[m - 1];
    if (a < md->rm->text_lo || a >= md->rm->text_hi) return NULL;
    return &md->disp[a - md->rm->text_lo];
}

int g_trace_calls;     // log every shim call
uint32_t g_unwind, g_unwind_target, g_unwind_ebp;
static Regs g_unwind_regs;
void guest_unwind_jump(uint32_t target) {
    if (getenv("PORT_SEH")) host_log("continuation jump to %08x, ebp %08x esp %08x\n", target, R.ebp, R.esp);
    g_unwind = 1; g_unwind_target = target; g_unwind_ebp = R.ebp; g_unwind_regs = R;
}
void guest_unwind_done(void) { g_unwind = 0; R = g_unwind_regs; }

void call_ind(uint32_t a) {
    if (a >= MAGIC_BASE) {
        int i = (a - MAGIC_BASE) >> 2;
        if (i < n_thunks && thunks[i].fn) {
            unsigned k = recent_n++ % NRECENT;
            recent[k].name = thunks[i].name; recent[k].from = R32(R.esp);
            for (int j = 0; j < 4; j++) recent[k].a[j] = R32(R.esp + 4 + 4 * j);
            if (g_trace_calls) host_log("  call %s(%08x, %08x, %08x, %08x) from %08x\n", thunks[i].name, R32(R.esp + 4), R32(R.esp + 8), R32(R.esp + 12), R32(R.esp + 16), R32(R.esp));
            thunk_calls[i]++;
            thunks[i].fn(); return;
        }
        if (i < n_thunks) host_fatal("unimplemented import %s", thunks[i].name);
    } else {
        GFunc *s = disp_slot(a);
        if (s && *s) { (*s)(); return; }
    }
    host_fatal("indirect call to unknown address %08x", a);
}

// replace the code at a function address for every indirect call (vtables, function pointers); returns the
// original, which the replacement may call. Direct calls in the recompiled code are not affected.
GFunc host_override(uint32_t addr, GFunc fn) {
    GFunc *s = disp_slot(addr);
    if (!s) return NULL;
    GFunc old = *s;
    *s = fn;
    return old;
}

// call a guest __thiscall method (this in ecx, args on stack); returns eax. While an exception unwind passes
// through (g_unwind), the registers stay as the unwind left them.
uint32_t guest_call_this(uint32_t fn, uint32_t self, int nargs, ...) {
    uint32_t args[16];
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    Regs save = R;
    for (int i = nargs - 1; i >= 0; i--) { R.esp -= 4; W32(R.esp, args[i]); }
    R.esp -= 4; W32(R.esp, RET_MAGIC);
    R.ecx = self;
    call_ind(fn);
    if (g_unwind) return 0;
    uint32_t ret = R.eax;
    R.esp = save.esp; R.ebx = save.ebx; R.esi = save.esi; R.edi = save.edi; R.ebp = save.ebp;
    return ret;
}

// call guest function (stdcall or cdecl) from host; returns eax
uint32_t guest_call(uint32_t fn, int nargs, ...) {
    uint32_t args[16];
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    Regs save = R;
    for (int i = nargs - 1; i >= 0; i--) { R.esp -= 4; W32(R.esp, args[i]); }
    R.esp -= 4; W32(R.esp, RET_MAGIC);
    call_ind(fn);
    if (g_unwind) return 0;
    uint32_t ret = R.eax;
    R.esp = save.esp; R.ebx = save.ebx; R.esi = save.esi; R.edi = save.edi; R.ebp = save.ebp;
    return ret;
}
int mod_count(void);
uint32_t mod_base_at(int i);
uint32_t mod_size_at(int i);

// ---------------------------------------------------------------- guest heap
// Boundary-tag allocator inside guest memory.  Block header (8 bytes) precedes
// payload: [size|inuse][prevsize].  Payload is 16-byte aligned.
#ifndef HEAP_BASE
#define HEAP_BASE 0x00600000u
#endif
#define HEAP_START HEAP_BASE
#define HDR 8u
#define NBINS 128
static uint32_t heap_top;      // address of next unallocated block header
static uint32_t bins[NBINS];   // free list heads (block addresses)
static uint32_t heap_used, heap_peak;

// small sizes (<2048) use exact 16-byte classes; large sizes use log2/4 classes
static uint32_t lbins[64];
static uint32_t *binp(uint32_t sz) {
    if (sz < 2048) return &bins[sz >> 4];
    int lg = 31 - __builtin_clz(sz);
    int b = (lg - 11) * 4 + ((sz >> (lg - 2)) & 3);
    if (b > 63) b = 63;
    return &lbins[b];
}
#define BSIZE(b) (R32(b) & ~15u)
#define BUSED(b) (R32(b) & 1u)
#define BPREV(b) R32((b) + 4)
#define FNEXT(b) R32((b) + 8)
#define FPREV(b) R32((b) + 12)

static void fl_insert(uint32_t b) {
    uint32_t *h = binp(BSIZE(b));
    W32(b + 8, *h); W32(b + 12, 0);
    if (*h) W32(*h + 12, b);
    *h = b;
}
static void fl_remove(uint32_t b) {
    uint32_t n = FNEXT(b), p = FPREV(b);
    if (p) W32(p + 8, n); else *binp(BSIZE(b)) = n;
    if (n) W32(n + 12, p);
}

// the allocator's host-side state, saved and restored around an undoable pass (netplay.c)
typedef struct { uint32_t top, used, peak, bins[NBINS], lbins[64]; } HeapState;
static HeapState heap_saved;
void heap_state_save(void) {
    heap_saved.top = heap_top; heap_saved.used = heap_used; heap_saved.peak = heap_peak;
    memcpy(heap_saved.bins, bins, sizeof bins); memcpy(heap_saved.lbins, lbins, sizeof lbins);
}
void heap_state_restore(void) {
    heap_top = heap_saved.top; heap_used = heap_saved.used; heap_peak = heap_saved.peak;
    memcpy(bins, heap_saved.bins, sizeof bins); memcpy(lbins, heap_saved.lbins, sizeof lbins);
}

void heap_init(void) {
    heap_top = HEAP_START - HDR;   // so payload of first block = HEAP_START (16 aligned)
    memset(bins, 0, sizeof bins); memset(lbins, 0, sizeof lbins);
    W32(heap_top + 4, 0);
}

static uint32_t take_block(uint32_t b, uint32_t need) {
    uint32_t bs = BSIZE(b);
    if (bs - need >= 64) {
        uint32_t r = b + need;
        W32(r, bs - need); W32(r + 4, need);
        uint32_t nx = b + bs;
        W32(nx + 4, bs - need);
        fl_insert(r);
        bs = need;
    }
    W32(b, bs | 1);
    heap_used += bs; if (heap_used > heap_peak) heap_peak = heap_used;
    return b + HDR;
}

uint32_t heap_alloc(uint32_t n) {
    uint32_t need = (n + HDR + 15) & ~15u;
    if (need < 32) need = 32;
    if (need < 2048) {
        for (uint32_t s = need; s < 2048; s += 16) {
            uint32_t b = bins[s >> 4];
            if (b) { fl_remove(b); return take_block(b, need); }
        }
    }
    int lg = 31 - __builtin_clz(need < 2048 ? 2048 : need);
    int start = need < 2048 ? 0 : (lg - 11) * 4 + (int)((need >> (lg - 2)) & 3);
    if (start > 63) start = 63;
    for (int i = start; i < 64; i++) {
        for (uint32_t b = lbins[i]; b; b = FNEXT(b)) {
            if (BSIZE(b) >= need) { fl_remove(b); return take_block(b, need); }
        }
    }
    // a module image (the exe at its fixed base) above the heap start: cover it with a block that is never freed
    for (int i = 0; i < mod_count(); i++) {
        uint32_t lo = mod_base_at(i), hi = lo + mod_size_at(i);
        if (lo >= HEAP_START && heap_top < hi && (uint64_t)heap_top + need + 2 * HDR > lo) {
            uint32_t nt = ((hi + 0xffff) & ~0xffffu) + 0x10000 - HDR;
            W32(heap_top, (nt - heap_top) | 1);
            W32(nt + 4, nt - heap_top);
            heap_top = nt;
        }
    }
    if ((uint64_t)heap_top + need + 2 * HDR > GUEST_SIZE) {
        host_log("guest heap exhausted (request %u, used %u peak %u)\n", n, heap_used, heap_peak);
        return 0;
    }
    uint32_t b = heap_top;
    W32(b, need | 1);
    heap_top += need;
    W32(heap_top + 4, need);
    heap_used += need; if (heap_used > heap_peak) heap_peak = heap_used;
    return b + HDR;
}

void heap_free(uint32_t p) {
    if (!p) return;
    uint32_t b = p - HDR;
    if (b < HEAP_START - HDR || b >= heap_top || !BUSED(b)) { host_log("heap_free: bad pointer %08x\n", p); return; }
    uint32_t sz = BSIZE(b);
    heap_used -= sz;
    // coalesce with next
    uint32_t nx = b + sz;
    if (nx < heap_top && !BUSED(nx)) { fl_remove(nx); sz += BSIZE(nx); }
    // coalesce with previous
    uint32_t ps = BPREV(b);
    if (ps && b - ps >= HEAP_START - HDR) {
        uint32_t pb = b - ps;
        if (!BUSED(pb)) { fl_remove(pb); sz += BSIZE(pb); b = pb; }
    }
    if (b + sz == heap_top) {
        // merge into top
        heap_top = b;
        return;
    }
    W32(b, sz);
    W32(b + sz + 4, sz);
    fl_insert(b);
}

uint32_t heap_size(uint32_t p) {
    if (!p) return 0;
    return BSIZE(p - HDR) - HDR;
}

uint32_t heap_realloc(uint32_t p, uint32_t n) {
    if (!p) return heap_alloc(n);
    uint32_t old = heap_size(p);
    if (n <= old) return p;
    uint32_t q = heap_alloc(n);
    if (!q) return 0;
    (COW(q, (uint32_t)(old)), memcpy(G + q, G + p, old));
    heap_free(p);
    return q;
}

uint32_t heap_peak_bytes(void) { return heap_peak; }
uint32_t heap_used_bytes(void) { return heap_used; }

// simple bump region for host-made guest structures (COM objects, strings) that live forever
static uint32_t static_top = 0x00381000u;
uint32_t guest_static_alloc(uint32_t n) {
    n = (n + 15) & ~15u;
    if (static_top + n > 0x00400000u) return heap_alloc(n);
    uint32_t r = static_top; static_top += n;
    (COW(r, (uint32_t)(n)), memset(G + r, 0, n));
    return r;
}
uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = heap_alloc(n);
    (COW(a, (uint32_t)(n)), memcpy(G + a, s, n));
    return a;
}

// ---------------------------------------------------------------- PE loader and linker
// Every recompiled module is loaded from the game's System folder at the base recomp.py rebased it to (applying
// its .reloc table), then the import tables are bound: imports of another module go to its export, imports the
// host implements (shims, HLE'd CRT routines) to a thunk. DllMain runs when the module is first needed: the exe's
// static dependencies before the exe's entry point, the others on LoadLibrary (UE2 loads D3DDrv, WinDrv, IpDrv,
// ALAudio, Fire and GUI by name).
#define PEH(b)      ((b) + R32((b) + 0x3c))
#define DDIR(b, i)  (PEH(b) + 0x78 + 8 * (i))

extern GFunc shim_lookup(const char *dll, const char *name);
extern GFunc import_hle_lookup(const char *dll, const char *name);   // gen/import_hle.c
extern const char *ordinal_name(const char *dll, uint32_t ord);      // wsock.c

static void dll_base_name(const char *in, char *out, int outsz) {
    const char *p = in;
    for (const char *q = in; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') p = q + 1;
    snprintf(out, outsz, "%s", p);
    char *dot = strrchr(out, '.');
    if (dot && !strcasecmp(dot, ".dll")) *dot = 0;
    else if (dot && !strcasecmp(dot, ".exe")) *dot = 0;
}

static Module *mod_by_name(const char *name) {
    char want[64], have[64];
    dll_base_name(name, want, sizeof want);
    for (int i = 0; i < n_mods; i++) {
        dll_base_name(mods[i].name, have, sizeof have);
        if (!strcasecmp(want, have)) return &mods[i];
    }
    return NULL;
}
static Module *mod_by_base(uint32_t b) {
    for (int i = 0; i < n_mods; i++) if (mods[i].rm->base == b) return &mods[i];
    return NULL;
}

// export lookup in a loaded image: by name (binary search, names are sorted) or by ordinal
static uint32_t pe_export(uint32_t base, const char *name, uint32_t ordinal) {
    uint32_t ed = R32(DDIR(base, 0)), esz = R32(DDIR(base, 0) + 4);
    if (!ed) return 0;
    ed += base;
    uint32_t obase = R32(ed + 16), nfun = R32(ed + 20), nname = R32(ed + 24);
    uint32_t afun = base + R32(ed + 28), aname = base + R32(ed + 32), aord = base + R32(ed + 36);
    uint32_t idx = 0xffffffffu;
    if (name) {
        int lo = 0, hi = (int)nname - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = strcmp(name, (const char *)G + base + R32(aname + 4 * mid));
            if (!c) { idx = R16(aord + 2 * mid); break; }
            if (c < 0) hi = mid - 1; else lo = mid + 1;
        }
    } else {
        idx = ordinal - obase;
    }
    if (idx >= nfun) return 0;
    uint32_t rva = R32(afun + 4 * idx);
    if (!rva) return 0;
    if (rva >= ed - base && rva < ed - base + esz) {
        host_log("loader: forwarded export %s ignored\n", (const char *)G + base + rva);
        return 0;
    }
    return base + rva;
}

static int load_module(const RecModule *rm, const char *sysdir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", sysdir, rm->file);
    FILE *f = fopen(path, "rb");
    if (!f) { host_log("loader: cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return 0; }
    fclose(f);
    uint32_t pe = *(uint32_t *)(buf + 0x3c);
    if (sz < 0x400 || pe > (uint32_t)sz - 0x100 || buf[pe] != 'P' || buf[pe + 1] != 'E' || *(uint16_t *)(buf + pe + 4) != 0x14c) {
        host_log("loader: %s is not a 32-bit x86 PE image\n", path);
        free(buf);
        return 0;
    }
    uint16_t nsec = *(uint16_t *)(buf + pe + 6);
    uint16_t optsz = *(uint16_t *)(buf + pe + 20);
    uint32_t pref = *(uint32_t *)(buf + pe + 24 + 28);
    uint32_t imgsz = *(uint32_t *)(buf + pe + 24 + 56);
    uint32_t hdrsz = *(uint32_t *)(buf + pe + 24 + 60);
    uint32_t base = rm->base;
    if (imgsz != rm->size) host_log("loader: %s size %x differs from the generated code's %x (another game version?)\n", rm->file, imgsz, rm->size);
    if ((uint64_t)base + imgsz > GUEST_SIZE) host_fatal("%s does not fit in guest memory", rm->file);
    memset(G + base, 0, imgsz);
    memcpy(G + base, buf, hdrsz);
    uint8_t *sec = buf + pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = *(uint32_t *)(sec + 8), va = *(uint32_t *)(sec + 12);
        uint32_t rsz = *(uint32_t *)(sec + 16), rofs = *(uint32_t *)(sec + 20);
        uint32_t n = rsz < vsz ? rsz : vsz;
        if (vsz == 0) n = rsz;
        if (n && rofs + n <= (uint32_t)sz) memcpy(G + base + va, buf + rofs, n);
    }
    free(buf);
    // relocations (type 3, HIGHLOW)
    int32_t delta = (int32_t)(base - pref);
    if (delta) {
        uint32_t rd = R32(DDIR(base, 5)), rsz = R32(DDIR(base, 5) + 4);
        if (!rd) host_fatal("%s has no relocations and cannot move from %08x to %08x", rm->file, pref, base);
        for (uint32_t p = base + rd, e = base + rd + rsz; p + 8 <= e;) {
            uint32_t page = R32(p), bsz = R32(p + 4);
            if (bsz < 8) break;
            for (uint32_t k = 8; k < bsz; k += 2) {
                uint32_t ent = R16(p + k);
                if ((ent >> 12) == 3) { uint32_t a = base + page + (ent & 0xfff); W32(a, R32(a) + (uint32_t)delta); }
                else if ((ent >> 12) != 0) host_log("loader: %s relocation type %u ignored\n", rm->file, ent >> 12);
            }
            p += bsz;
        }
        W32(PEH(base) + 0x34, base);    // ImageBase as the module sees it in its own headers
    }
    Module *md = &mods[n_mods++];
    md->rm = rm;
    snprintf(md->name, sizeof md->name, "%s", rm->file);
    md->loaded = 1;
    md->disp = calloc(rm->text_hi - rm->text_lo, sizeof(GFunc));
    for (int i = 0; i < rm->nfuncs; i++) {
        uint32_t a = rm->funcs[i].addr;
        if (a >= rm->text_lo && a < rm->text_hi) md->disp[a - rm->text_lo] = rm->funcs[i].fn;
    }
    for (uint32_t p = base >> 16; p <= (base + imgsz - 1) >> 16; p++) page_mod[p] = (uint8_t)n_mods;
    host_log("loader: %s at %08x (%u functions)\n", rm->file, base, rm->nfuncs);
    return 1;
}

uint32_t host_resolve_import(const char *dll, const char *name) {
    GFunc fn = import_hle_lookup(dll, name);
    if (!fn) fn = shim_lookup(dll, name);
    char *full = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(full, "%s!%s", dll, name);
    if (!fn) host_log("note: import %s not implemented\n", full);
    if (fn) {
        for (int i = 0; i < n_thunks; i++) if (thunks[i].fn == fn) { free(full); return MAGIC_BASE + i * 4; }
    }
    if (n_thunks >= MAX_THUNKS) host_fatal("too many thunks");
    thunks[n_thunks].fn = fn; thunks[n_thunks].name = full;
    return MAGIC_BASE + (n_thunks++) * 4;
}

// bind one module's import table
static void link_module(Module *md) {
    uint32_t base = md->rm->base;
    uint32_t id = R32(DDIR(base, 1));
    if (!id) return;
    for (uint32_t d = base + id; R32(d + 12); d += 20) {
        const char *dll = (const char *)G + base + R32(d + 12);
        uint32_t ilt = R32(d) ? base + R32(d) : base + R32(d + 16);
        uint32_t iat = base + R32(d + 16);
        Module *src = mod_by_name(dll);
        char dname[64];
        dll_base_name(dll, dname, sizeof dname);
        for (int k = 0;; k++) {
            uint32_t e = R32(ilt + 4 * k);
            if (!e) break;
            char ordname[16];
            const char *nm;
            uint32_t ord = 0;
            if (e & 0x80000000u) {
                ord = e & 0xffff; snprintf(ordname, sizeof ordname, "#%u", ord); nm = ordname;
                if (!src) { const char *on = ordinal_name(dname, ord); if (on) { nm = on; ord = 0; } }
            }
            else nm = (const char *)G + base + e + 2;
            uint32_t v = 0;
            GFunc h = import_hle_lookup(dname, nm);
            if (h) v = host_thunk(h, nm);
            else if (src) {
                v = pe_export(src->rm->base, ord ? NULL : nm, ord);
                if (!v) host_log("loader: %s imports %s!%s, which it does not export\n", md->name, dll, nm);
            } else {
                v = host_resolve_import(dname, nm);
            }
            W32(iat + 4 * k, v);
        }
    }
}

// run DllMain(DLL_PROCESS_ATTACH) after the module's own imports are initialised
static void init_module(Module *md, int is_static) {
    if (md->inited) return;
    md->inited = 1;
    uint32_t base = md->rm->base;
    uint32_t id = R32(DDIR(base, 1));
    for (uint32_t d = id ? base + id : 0; d && R32(d + 12); d += 20) {
        Module *dep = mod_by_name((const char *)G + base + R32(d + 12));
        if (dep && dep != md) init_module(dep, is_static);
    }
    if (md->rm->entry && (R16(PEH(base) + 0x16) & 0x2000)) {
        host_log("loader: DllMain %s\n", md->name);
        uint32_t r = guest_call(md->rm->entry, 3, base, 1, is_static ? 1 : 0);
        if (g_unwind) host_fatal("exception escaped DllMain of %s", md->name);
        if (!r) host_log("loader: DllMain of %s returned FALSE\n", md->name);
    }
}

// ---- module API for the kernel32 shims
// Modules the host implements entirely (OpenAL, loaded by name by ALAudio) get pseudo handles; GetProcAddress
// on them looks up the shims under that DLL name.
static const char *host_dlls[] = { "OpenAL32", "DefOpenAL32", NULL };
#define HOST_DLL_BASE 0x7ff00000u
uint32_t mod_load_library(const char *name) {
    Module *md = mod_by_name(name);
    if (md) { init_module(md, 0); return md->rm->base; }
    char b[64]; dll_base_name(name, b, sizeof b);
    for (int i = 0; host_dlls[i]; i++) if (!strcasecmp(b, host_dlls[i])) return HOST_DLL_BASE + i * 0x10000;
    host_log("LoadLibrary(%s): no such module\n", name);
    return 0;
}
uint32_t mod_get_handle(const char *name) {
    if (!name) return mods[0].rm->base;
    Module *md = mod_by_name(name);
    return md ? md->rm->base : 0;
}
uint32_t mod_get_proc(uint32_t h, const char *name, uint32_t ordinal) {
    if (h >= HOST_DLL_BASE) {
        int i = (h - HOST_DLL_BASE) >> 16;
        if (!name) return 0;
        GFunc fn = shim_lookup("OpenAL32", name);   // both names are OpenAL
        if (!fn) { host_log("GetProcAddress(%s, %s): not implemented\n", host_dlls[i], name); return 0; }
        return host_thunk(fn, name);
    }
    Module *md = mod_by_base(h);
    if (!md) return 0;
    uint32_t v = pe_export(h, name, ordinal);
    if (!v) host_log("GetProcAddress(%s, %s): not found\n", md->name, name ? name : "#ord");
    return v;
}
const char *mod_file_name(uint32_t h) {
    if (!h) return mods[0].name;
    Module *md = mod_by_base(h);
    return md ? md->name : NULL;
}
uint32_t mod_from_address(uint32_t a) {
    unsigned m = page_mod[a >> 16];
    return m ? mods[m - 1].rm->base : 0;
}
int mod_count(void) { return n_mods; }
uint32_t mod_base_at(int i) { return mods[i].rm->base; }
uint32_t mod_size_at(int i) { return mods[i].rm->size; }

int runtime_init(const char *sysdir) {
    memset(G, 0, 0x01000000u);
    // the exe is rec_modules[0] (layout.py); it becomes mods[0]
    for (int i = 0; i < rec_module_count; i++)
        if (!load_module(rec_modules[i], sysdir)) return 0;
    for (int i = 0; i < n_mods; i++) link_module(&mods[i]);
    heap_init();
    // TEB
    W32(FS_BASE + 0, 0xffffffffu);
    W32(FS_BASE + 4, 0x00370000u);
    W32(FS_BASE + 8, 0x00010000u);
    W32(FS_BASE + 0x18, FS_BASE);
    fpu_init(); FCW = 0x27f;
    R.esp = 0x0036fff0u;
    return 1;
}

uint32_t runtime_exe_entry(void) { return mods[0].rm->entry; }

void runtime_run_entry(uint32_t entry) {
    // static dependencies of the exe first, as the Windows loader does
    init_module(&mods[0], 1);
    R.esp -= 4; W32(R.esp, RET_MAGIC);
    call_ind(entry);
    if (g_unwind) host_fatal("uncaught exception unwind to %08x", g_unwind_target);
}
