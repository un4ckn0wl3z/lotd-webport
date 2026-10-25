// KERNEL32 shims for Unreal Engine 2 (Unicode build): files, paths, modules, memory, time, locale, threads and
// the SEH dispatch C++ exceptions need.
//
// The guest sees the game installed at C:\LOTD, started from C:\LOTD\System (its working directory). Paths
// resolve case-insensitively against the save overlay first (everything the game writes: ini files, logs, saves)
// and then the game folder (read-only).
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <setjmp.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0777)
#endif

char g_game_dir[512] = ".";
char g_save_dir[512] = "save";
char g_cmdline_extra[512];      // main.c: extra command line arguments for the game (a map URL, -log ...)
extern int g_fs_dirty;
static uint32_t last_error;
#define GROOT "C:\\LOTD"

uint32_t mod_load_library(const char *name);
uint32_t mod_get_handle(const char *name);
uint32_t mod_get_proc(uint32_t h, const char *name, uint32_t ordinal);
const char *mod_file_name(uint32_t h);
void game_background_tick(void);       // game.c: the work of the game's helper threads

// ---------------------------------------------------------------- strings
// guest UTF-16 -> host UTF-8 (BMP only)
static void wstr(uint32_t a, char *out, int cap) {
    int k = 0;
    for (uint32_t i = 0; a; i++) {
        uint32_t c = R16(a + 2 * i);
        if (!c) break;
        if (c < 0x80) { if (k + 1 >= cap) break; out[k++] = (char)c; }
        else if (c < 0x800) { if (k + 2 >= cap) break; out[k++] = (char)(0xc0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 63)); }
        else { if (k + 3 >= cap) break; out[k++] = (char)(0xe0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 63)); out[k++] = (char)(0x80 | (c & 63)); }
    }
    out[k] = 0;
}
// host UTF-8 -> guest UTF-16 buffer of cap characters; returns the length written (without the terminator)
static uint32_t put_wstr(uint32_t a, uint32_t cap, const char *s) {
    uint32_t k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && k + 1 < cap) {
        uint32_t c = *p++;
        if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xc0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p++; }
        W16(a + 2 * k++, c);
    }
    if (cap) W16(a + 2 * k, 0);
    return k;
}
static uint32_t wlen(uint32_t a) { uint32_t n = 0; while (R16(a + 2 * n)) n++; return n; }
// A or W string argument as UTF-8
#define SARG(i, wide, buf) ((wide) ? (wstr(ARG(i), buf, sizeof buf), buf) : (snprintf(buf, sizeof buf, "%s", GSTR(ARG(i))), buf))
// copy a host string into the caller's A or W buffer (cap in characters); returns the API's usual length result
static uint32_t put_str(uint32_t buf, uint32_t cap, const char *s, int wide) {
    uint32_t n = (uint32_t)strlen(s);
    if (cap <= n) return n + 1;             // too small: required size including the terminator
    if (wide) return put_wstr(buf, cap, s);
    memcpy(GSTR(buf), s, n + 1);
    return n;
}

// ---------------------------------------------------------------- paths
static char g_cwd[260] = "System";      // working directory, relative to the game root

static int find_ci(const char *dir, const char *name, char *out, int outsz) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) { snprintf(out, outsz, "%s", e->d_name); found = 1; break; }
    }
    closedir(d);
    return found;
}

// guest path -> components relative to the game root; returns -1 for a path outside the game
static int split_path(const char *gp, char comps[][128], int max) {
    char tmp[1024]; int n = 0;
    snprintf(tmp, sizeof tmp, "%s", gp);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    char *s = tmp;
    char all[1100];
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        s += 2;
        while (*s == '\\') s++;
        if (strncasecmp(s, "LOTD", 4) || (s[4] && s[4] != '\\')) return -1;
        s += 4;
        snprintf(all, sizeof all, "%s", s);
    } else if (s[0] == '\\') {
        while (*s == '\\') s++;
        if (strncasecmp(s, "LOTD", 4) || (s[4] && s[4] != '\\')) return -1;
        snprintf(all, sizeof all, "%s", s + 4);
    } else {
        snprintf(all, sizeof all, "%s\\%s", g_cwd, s);
    }
    char *tok = strtok(all, "\\");
    while (tok) {
        if (!strcmp(tok, ".") || !tok[0]) {}
        else if (!strcmp(tok, "..")) { if (n) n--; }
        else if (n < max) { snprintf(comps[n++], 128, "%s", tok); }
        tok = strtok(NULL, "\\");
    }
    return n;
}

static int resolve_in(const char *root, char comps[][128], int n, char *out, int outsz, int create_dirs) {
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    for (int i = 0; i < n; i++) {
        char real[256];
        if (find_ci(cur, comps[i], real, sizeof real)) {
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", real);
        } else {
            if (!create_dirs) return 0;
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", comps[i]);
            if (i < n - 1) mkdir_p(cur);
        }
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}

// for_write: the path in the save overlay (directories created). Otherwise the overlay's file if there is one,
// else the game's. Returns 0 if nothing exists there.
int fs_resolve(const char *gp, char *out, int outsz, int for_write) {
    char comps[32][128];
    int n = split_path(gp, comps, 32);
    if (n < 0) { snprintf(out, outsz, "%s/.outside", g_save_dir); return 0; }
    if (for_write) {
        mkdir_p(g_save_dir);
        return resolve_in(g_save_dir, comps, n, out, outsz, 1);
    }
    if (resolve_in(g_save_dir, comps, n, out, outsz, 0)) return 1;
    if (resolve_in(g_game_dir, comps, n, out, outsz, 0)) return 1;
    return 0;
}
static int logfiles = -1;
#define LOGF(...) do { if (logfiles < 0) logfiles = getenv("PORT_LOGFILES") != NULL; if (logfiles) host_log(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------- file handles
#define MAXF 256
static struct { FILE *f; char path[512]; int used; } files[MAXF];
#define FH_BASE 0x1000u
#define STD_IN 0xF00u
#define STD_OUT 0xF04u
#define STD_ERR 0xF08u

static FILE *fh(uint32_t h) {
    if (h < FH_BASE) return NULL;
    uint32_t i = (h - FH_BASE) / 4;
    if (i >= MAXF || !files[i].used) return NULL;
    return files[i].f;
}

static void copy_file(const char *from, const char *to) {
    FILE *src = fopen(from, "rb"), *dst = fopen(to, "wb");
    if (src && dst) { char b[16384]; size_t k; while ((k = fread(b, 1, sizeof b, src)) > 0) fwrite(b, 1, k, dst); }
    if (src) fclose(src);
    if (dst) fclose(dst);
}

static void create_file(int wide) {
    char name[1024]; SARG(0, wide, name);
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    char path[1024];
    FILE *f = NULL;
    int exists = fs_resolve(name, path, sizeof path, 0);
    struct stat st;
    if (exists && !stat(path, &st) && S_ISDIR(st.st_mode)) { last_error = 5; SRET(0xffffffffu, 7); }
    if (!write) {
        if (disp == 1 || disp == 2) { // CREATE_NEW / CREATE_ALWAYS for reading: make an empty file
            fs_resolve(name, path, sizeof path, 1); f = fopen(path, "w+b");
        } else if (exists) f = fopen(path, "rb");
    } else {
        char rpath[1024];
        snprintf(rpath, sizeof rpath, "%s", path);
        fs_resolve(name, path, sizeof path, 1);
        switch (disp) {
        case 1: if (exists) { last_error = 80; SRET(0xffffffffu, 7); } f = fopen(path, "w+b"); break;
        case 2: f = fopen(path, "w+b"); break;
        case 3: case 4: case 5:
            if (exists && strcmp(rpath, path)) copy_file(rpath, path);   // copy-on-write into the overlay
            if (disp == 5) f = fopen(path, "w+b");
            else { f = fopen(path, "r+b"); if (!f && disp == 4) f = fopen(path, "w+b"); }
            break;
        }
        if (f) g_fs_dirty = 1;
    }
    LOGF("CreateFile %s %s -> %s\n", write ? "w" : "r", name, f ? path : "NOT FOUND");
    if (!f) { last_error = 2; SRET(0xffffffffu, 7); }
    for (int i = 0; i < MAXF; i++) if (!files[i].used) {
        files[i].used = 1; files[i].f = f; snprintf(files[i].path, sizeof files[i].path, "%s", path);
        last_error = (exists && (disp == 2 || disp == 4)) ? 183 : 0;
        SRET(FH_BASE + i * 4, 7);
    }
    fclose(f);
    last_error = 4; SRET(0xffffffffu, 7);
}
static void k_CreateFileA(void) { create_file(0); }
static void k_CreateFileW(void) { create_file(1); }

static void k_ReadFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread = ARG(3);
    FILE *f = fh(h);
    if (!f) { if (pread) W32(pread, 0); last_error = 6; SRET(0, 5); }
    COW(buf, n);
    size_t got = fread(G + buf, 1, n, f);
    if (pread) W32(pread, (uint32_t)got);
    SRET(1, 5);
}

static void k_WriteFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h == STD_OUT || h == STD_ERR) {
        host_log("%.*s", (int)n, GSTR(buf));
        if (pw) W32(pw, n);
        SRET(1, 5);
    }
    FILE *f = fh(h);
    if (!f) { last_error = 6; SRET(0, 5); }
    size_t put = fwrite(G + buf, 1, n, f);
    if (pw) W32(pw, (uint32_t)put);
    g_fs_dirty = 1;
    SRET(1, 5);
}

static void k_SetFilePointer(void) {
    uint32_t h = ARG(0), mv = ARG(3);
    int32_t dist = (int32_t)ARG(1);
    FILE *f = fh(h);
    if (!f) { last_error = 6; SRET(0xffffffffu, 4); }
    if (ARG(2)) W32(ARG(2), 0);
    fseek(f, dist, mv == 0 ? SEEK_SET : mv == 1 ? SEEK_CUR : SEEK_END);
    SRET((uint32_t)ftell(f), 4);
}

static void k_SetEndOfFile(void) {
    FILE *f = fh(ARG(0));
    if (f) {
        fflush(f);
        long pos = ftell(f);
#ifdef _WIN32
        _chsize(_fileno(f), pos);
#else
        if (ftruncate(fileno(f), pos)) {}
#endif
    }
    SRET(1, 1);
}

static long file_size(FILE *f) {
    long cur = ftell(f); fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, cur, SEEK_SET);
    return sz;
}
static void k_GetFileSize(void) {
    FILE *f = fh(ARG(0));
    if (!f) { last_error = 6; SRET(0xffffffffu, 2); }
    if (ARG(1)) W32(ARG(1), 0);
    SRET((uint32_t)file_size(f), 2);
}

#define MAXTHREADS 32
static void close_thread(uint32_t h);
static void k_CloseHandle(void) {
    uint32_t h = ARG(0);
    if (h >= FH_BASE && h < FH_BASE + MAXF * 4) {
        uint32_t i = (h - FH_BASE) / 4;
        if (files[i].used) { fclose(files[i].f); files[i].used = 0; }
    }
    close_thread(h);
    SRET(1, 1);
}
static void k_FlushFileBuffers(void) { FILE *f = fh(ARG(0)); if (f) fflush(f); SRET(1, 1); }
static void k_GetFileType(void) { uint32_t h = ARG(0); SRET(h >= FH_BASE ? 1 : 2, 1); }
static void k_LockFile(void) { SRET(1, 5); }

// ---------------------------------------------------------------- time
static void put_filetime(uint32_t a, time_t t) {
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(a, (uint32_t)ft); W32(a + 4, (uint32_t)(ft >> 32));
}
static void put_systemtime(uint32_t a, struct tm *tm, int ms) {
    W16(a + 0, tm->tm_year + 1900); W16(a + 2, tm->tm_mon + 1); W16(a + 4, tm->tm_wday);
    W16(a + 6, tm->tm_mday); W16(a + 8, tm->tm_hour); W16(a + 10, tm->tm_min);
    W16(a + 12, tm->tm_sec); W16(a + 14, ms);
}
static void k_GetFileTime(void) {
    uint32_t h = ARG(0);
    time_t t = time(NULL);
    uint32_t hi = (h - FH_BASE) / 4;
    if (h >= FH_BASE && hi < MAXF && files[hi].used) { struct stat st; if (!stat(files[hi].path, &st)) t = st.st_mtime; }
    for (int i = 1; i <= 3; i++) if (ARG(i)) put_filetime(ARG(i), t);
    SRET(1, 4);
}
static void k_SetFileTime(void) { SRET(1, 4); }
static void k_FileTimeToLocalFileTime(void) { W64(ARG(1), R64(ARG(0))); SRET(1, 2); }
static void k_LocalFileTimeToFileTime(void) { W64(ARG(1), R64(ARG(0))); SRET(1, 2); }
static void k_FileTimeToSystemTime(void) {
    uint64_t ft = R64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm *tm = gmtime(&t);
    if (!tm) SRET(0, 2);
    put_systemtime(ARG(1), tm, (int)(ft / 10000 % 1000));
    SRET(1, 2);
}
static void k_SystemTimeToFileTime(void) {
    uint32_t s = ARG(0);
    struct tm tm = {0};
    tm.tm_year = (int)R16(s) - 1900; tm.tm_mon = (int)R16(s + 2) - 1; tm.tm_mday = (int)R16(s + 6);
    tm.tm_hour = (int)R16(s + 8); tm.tm_min = (int)R16(s + 10); tm.tm_sec = (int)R16(s + 12);
#ifdef _WIN32
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    put_filetime(ARG(1), t);
    SRET(1, 2);
}
static void k_GetLocalTime(void) {
    time_t t = time(NULL);
    put_systemtime(ARG(0), localtime(&t), (int)(host_ticks() % 1000));
    SRET(0, 1);
}
static void k_SetLocalTime(void) { SRET(0, 1); }
static void k_GetSystemTimeAsFileTime(void) { put_filetime(ARG(0), time(NULL)); SRET(0, 1); }
static void k_GetTimeZoneInformation(void) { COW(ARG(0), 172); memset(G + ARG(0), 0, 172); SRET(0, 1); }
static void k_GetTickCount(void) { SRET(host_ticks(), 0); }
extern uint64_t host_time_ns(void);
static void k_QueryPerformanceCounter(void) {
    uint64_t t = host_time_ns() / 1000;
    W32(ARG(0), (uint32_t)t); W32(ARG(0) + 4, (uint32_t)(t >> 32));
    SRET(1, 1);
}
static void k_QueryPerformanceFrequency(void) { W32(ARG(0), 1000000); W32(ARG(0) + 4, 0); SRET(1, 1); }
static void k_Sleep(void) {
    game_background_tick();
    host_sleep(ARG(0));
    SRET(0, 1);
}

// ---------------------------------------------------------------- attributes, directories
static int stat_guest(const char *name, struct stat *st, char *path, int psz) {
    if (!fs_resolve(name, path, psz, 0)) return -1;
    return stat(path, st);
}
static void get_attrs(int wide) {
    char name[1024], path[1024]; SARG(0, wide, name);
    struct stat st;
    if (stat_guest(name, &st, path, sizeof path)) { last_error = 2; SRET(0xffffffffu, 1); }
    SRET(S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
static void k_GetFileAttributesA(void) { get_attrs(0); }
static void k_GetFileAttributesW(void) { get_attrs(1); }
static void k_GetFileAttributesExW(void) {
    char name[1024], path[1024]; wstr(ARG(0), name, sizeof name);
    uint32_t p = ARG(2);
    struct stat st;
    if (stat_guest(name, &st, path, sizeof path)) { last_error = 2; SRET(0, 3); }
    COW(p, 36); memset(G + p, 0, 36);
    W32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    put_filetime(p + 4, st.st_mtime); put_filetime(p + 12, st.st_mtime); put_filetime(p + 20, st.st_mtime);
    W32(p + 32, (uint32_t)st.st_size);
    SRET(1, 3);
}
static void k_GetFileInformationByHandle(void) {
    FILE *f = fh(ARG(0));
    uint32_t p = ARG(1);
    if (!f) { last_error = 6; SRET(0, 2); }
    COW(p, 52); memset(G + p, 0, 52);
    W32(p, 0x80); W32(p + 36, (uint32_t)file_size(f)); W32(p + 40, 1);
    SRET(1, 2);
}
static void k_SetFileAttributes(void) { SRET(1, 2); }
static void delete_file(int wide) {
    char name[1024], path[1024]; SARG(0, wide, name);
    fs_resolve(name, path, sizeof path, 1);       // only files in the save overlay can be deleted
    int ok = remove(path) == 0;
    if (!ok) last_error = 2;
    g_fs_dirty = 1;
    SRET(ok, 1);
}
static void k_DeleteFileA(void) { delete_file(0); }
static void k_DeleteFileW(void) { delete_file(1); }
static void move_file(int wide) {
    char from[1024], to[1024], a[1024], b[1024], c[1024];
    SARG(0, wide, from); SARG(1, wide, to);
    int ex = fs_resolve(from, c, sizeof c, 0);
    fs_resolve(from, a, sizeof a, 1);
    fs_resolve(to, b, sizeof b, 1);
    g_fs_dirty = 1;
    if (ex && strcmp(a, c)) { copy_file(c, b); SRET(1, 2); }
    remove(b);
    SRET(rename(a, b) == 0, 2);
}
static void k_MoveFileA(void) { move_file(0); }
static void k_MoveFileW(void) { move_file(1); }
static void copy_file_api(int wide) {
    char from[1024], to[1024], a[1024], b[1024];
    SARG(0, wide, from); SARG(1, wide, to);
    if (!fs_resolve(from, a, sizeof a, 0)) { last_error = 2; SRET(0, 3); }
    fs_resolve(to, b, sizeof b, 1);
    copy_file(a, b);
    g_fs_dirty = 1;
    SRET(1, 3);
}
static void k_CopyFileA(void) { copy_file_api(0); }
static void k_CopyFileW(void) { copy_file_api(1); }
static void create_dir(int wide) {
    char name[1024], path[1024]; SARG(0, wide, name);
    if (fs_resolve(name, path, sizeof path, 0)) { last_error = 183; SRET(0, 2); }
    fs_resolve(name, path, sizeof path, 1);
    mkdir_p(path);
    g_fs_dirty = 1;
    SRET(1, 2);
}
static void k_CreateDirectoryA(void) { create_dir(0); }
static void k_CreateDirectoryW(void) { create_dir(1); }
static void k_RemoveDirectory(void) { SRET(0, 1); }

static void get_cwd(int wide) {
    char d[512]; snprintf(d, sizeof d, GROOT "\\%s", g_cwd);
    if (!g_cwd[0]) snprintf(d, sizeof d, GROOT);
    SRET(put_str(ARG(1), ARG(0), d, wide), 2);
}
static void k_GetCurrentDirectoryA(void) { get_cwd(0); }
static void k_GetCurrentDirectoryW(void) { get_cwd(1); }
static void set_cwd(int wide) {
    char name[1024]; SARG(0, wide, name);
    char comps[32][128];
    int n = split_path(name, comps, 32);
    if (n < 0) { last_error = 3; SRET(0, 1); }
    g_cwd[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(g_cwd);
        snprintf(g_cwd + l, sizeof g_cwd - l, "%s%s", i ? "\\" : "", comps[i]);
    }
    SRET(1, 1);
}
static void k_SetCurrentDirectoryA(void) { set_cwd(0); }
static void k_SetCurrentDirectoryW(void) { set_cwd(1); }
static void full_path(int wide) {
    char name[1024]; SARG(0, wide, name);
    char comps[32][128], out[1024];
    int n = split_path(name, comps, 32);
    if (n < 0) snprintf(out, sizeof out, "%s", name);
    else {
        snprintf(out, sizeof out, GROOT);
        for (int i = 0; i < n; i++) { size_t l = strlen(out); snprintf(out + l, sizeof out - l, "\\%s", comps[i]); }
    }
    uint32_t r = put_str(ARG(2), ARG(1), out, wide);
    if (ARG(3)) {
        const char *bs = strrchr(out, '\\');
        uint32_t off = bs ? (uint32_t)(bs + 1 - out) : 0;
        W32(ARG(3), ARG(2) + off * (wide ? 2 : 1));
    }
    SRET(r, 4);
}
static void k_GetFullPathNameA(void) { full_path(0); }
static void k_GetFullPathNameW(void) { full_path(1); }

// directory listing: the save overlay and the game folder, case-insensitive wildcards
#define MAXFIND 16
#define MAXFOUND 2048
static struct { int used; char (*names)[260]; int *isdir; uint32_t *sizes; int n, pos; } finds[MAXFIND];
static int wild_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;; s++) { if (wild_match(p + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return 0;
    if (*p == '.' && p[1] == '*' && !p[2] && !*s) return 1;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    return wild_match(p + 1, s + 1);
}
static void find_scan(int fi, const char *dir, const char *pat) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && finds[fi].n < MAXFOUND) {
        if (!wild_match(pat, e->d_name)) continue;
        int dup = 0;
        for (int k = 0; k < finds[fi].n; k++) if (!strcasecmp(finds[fi].names[k], e->d_name)) { dup = 1; break; }
        if (dup) continue;
        char full[1024]; snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st; int isdir = 0; uint32_t size = 0;
        if (!stat(full, &st)) { isdir = S_ISDIR(st.st_mode); size = (uint32_t)st.st_size; }
        snprintf(finds[fi].names[finds[fi].n], 260, "%s", e->d_name);
        finds[fi].isdir[finds[fi].n] = isdir; finds[fi].sizes[finds[fi].n] = size;
        finds[fi].n++;
    }
    closedir(d);
}
static void find_fill(int fi, uint32_t data, int wide) {
    int k = finds[fi].pos++;
    uint32_t sz = wide ? 592 : 320;
    COW(data, sz); memset(G + data, 0, sz);
    W32(data, finds[fi].isdir[k] ? 0x10 : 0x80);
    W32(data + 32, finds[fi].sizes[k]);
    if (wide) put_wstr(data + 44, 260, finds[fi].names[k]);
    else snprintf(GSTR(data + 44), 260, "%s", finds[fi].names[k]);
}
static void find_first(int wide) {
    char gp[1024]; SARG(0, wide, gp);
    for (char *p = gp; *p; p++) if (*p == '/') *p = '\\';
    char *sl = strrchr(gp, '\\');
    const char *pat = sl ? sl + 1 : gp;
    char dirpart[1024]; snprintf(dirpart, sizeof dirpart, "%.*s", sl ? (int)(sl - gp) : 0, gp);
    if (!sl) snprintf(dirpart, sizeof dirpart, ".");
    int fi = -1;
    for (int i = 0; i < MAXFIND; i++) if (!finds[i].used) { fi = i; break; }
    if (fi < 0) { last_error = 4; SRET(0xffffffffu, 2); }
    if (!finds[fi].names) {
        finds[fi].names = malloc(MAXFOUND * 260); finds[fi].isdir = malloc(MAXFOUND * sizeof(int));
        finds[fi].sizes = malloc(MAXFOUND * sizeof(uint32_t));
    }
    finds[fi].used = 1; finds[fi].n = 0; finds[fi].pos = 0;
    char comps[32][128];
    int n = split_path(dirpart, comps, 32);
    char real[1024];
    if (n >= 0) {
        if (resolve_in(g_save_dir, comps, n, real, sizeof real, 0)) find_scan(fi, real, pat);
        if (resolve_in(g_game_dir, comps, n, real, sizeof real, 0)) find_scan(fi, real, pat);
    }
    LOGF("FindFirstFile %s: %d entries\n", gp, finds[fi].n);
    if (!finds[fi].n) { finds[fi].used = 0; last_error = 2; SRET(0xffffffffu, 2); }
    find_fill(fi, ARG(1), wide);
    SRET(0x3000u + fi * 4, 2);
}
static void find_next(int wide) {
    uint32_t i = (ARG(0) - 0x3000u) / 4;
    if (i >= MAXFIND || !finds[i].used || finds[i].pos >= finds[i].n) { last_error = 18; SRET(0, 2); }
    find_fill((int)i, ARG(1), wide);
    SRET(1, 2);
}
static void k_FindFirstFileA(void) { find_first(0); }
static void k_FindFirstFileW(void) { find_first(1); }
static void k_FindNextFileA(void) { find_next(0); }
static void k_FindNextFileW(void) { find_next(1); }
static void k_FindClose(void) {
    uint32_t i = (ARG(0) - 0x3000u) / 4;
    if (i < MAXFIND) finds[i].used = 0;
    SRET(1, 1);
}
static void k_GetDriveType(void) { SRET(3, 1); }     // DRIVE_FIXED
static void k_GetLogicalDrives(void) { SRET(1u << 2, 0); }
static void k_GetDiskFreeSpaceA(void) {
    if (ARG(1)) W32(ARG(1), 8); if (ARG(2)) W32(ARG(2), 512);
    if (ARG(3)) W32(ARG(3), 0x100000); if (ARG(4)) W32(ARG(4), 0x200000);
    SRET(1, 5);
}

// ---------------------------------------------------------------- modules
static void module_file(uint32_t h, char *out, int cap) {
    const char *f = mod_file_name(h);
    snprintf(out, cap, GROOT "\\System\\%s", f ? f : "LOTD.exe");
}
static void k_GetModuleFileNameA(void) { char p[512]; module_file(ARG(0), p, sizeof p); uint32_t r = put_str(ARG(1), ARG(2), p, 0); SRET(r > ARG(2) ? ARG(2) : r, 3); }
static void k_GetModuleFileNameW(void) { char p[512]; module_file(ARG(0), p, sizeof p); uint32_t r = put_str(ARG(1), ARG(2), p, 1); SRET(r > ARG(2) ? ARG(2) : r, 3); }
static void k_GetModuleHandleA(void) { SRET(mod_get_handle(ARG(0) ? GSTR(ARG(0)) : NULL), 1); }
static void k_GetModuleHandleW(void) { char n[260]; SRET(mod_get_handle(ARG(0) ? (wstr(ARG(0), n, sizeof n), n) : NULL), 1); }
static void k_LoadLibraryA(void) { SRET(mod_load_library(GSTR(ARG(0))), 1); }
static void k_LoadLibraryW(void) { char n[260]; wstr(ARG(0), n, sizeof n); SRET(mod_load_library(n), 1); }
static void k_LoadLibraryExW(void) { char n[260]; wstr(ARG(0), n, sizeof n); SRET(mod_load_library(n), 3); }
static void k_FreeLibrary(void) { SRET(1, 1); }
static void k_GetProcAddress(void) {
    uint32_t h = ARG(0), np = ARG(1);
    if (!h) h = mod_get_handle(NULL);
    if (np < 0x10000) SRET(mod_get_proc(h, NULL, np), 2);
    SRET(mod_get_proc(h, GSTR(np), 0), 2);
}
static void k_DisableThreadLibraryCalls(void) { SRET(1, 1); }

// resources: walk the module's resource directory
static uint32_t rsrc_base(uint32_t mod) {
    uint32_t rva = R32(mod + R32(mod + 0x3c) + 0x78 + 2 * 8);
    return rva ? mod + rva : 0;
}
static int rsrc_name_eq(uint32_t base, uint32_t entry_name, uint32_t want, int wide) {
    if (!(entry_name & 0x80000000u)) return want < 0x10000 && entry_name == want;
    if (want < 0x10000) return 0;
    uint32_t s = base + (entry_name & 0x7fffffffu);
    uint32_t n = R16(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = R16(s + 2 + i * 2);
        uint32_t w = wide ? R16(want + i * 2) : R8(want + i);
        if (toupper((int)c) != toupper((int)w)) return 0;
    }
    return (wide ? R16(want + n * 2) : R8(want + n)) == 0;
}
static uint32_t rsrc_find_in(uint32_t base, uint32_t dir, uint32_t want, int wide, int any) {
    uint32_t nn = R16(dir + 12), ni = R16(dir + 14);
    for (uint32_t i = 0; i < nn + ni; i++) {
        uint32_t e = dir + 16 + i * 8;
        if (any || rsrc_name_eq(base, R32(e), want, wide)) return R32(e + 4);
    }
    return 0;
}
static uint32_t rsrc_find(uint32_t mod, uint32_t name, uint32_t type, int wide) {
    uint32_t base = rsrc_base(mod);
    if (!base) return 0;
    uint32_t t = rsrc_find_in(base, base, type, wide, 0);
    if (!(t & 0x80000000u)) return 0;
    uint32_t n = rsrc_find_in(base, base + (t & 0x7fffffffu), name, wide, 0);
    if (!(n & 0x80000000u)) return 0;
    uint32_t l = rsrc_find_in(base, base + (n & 0x7fffffffu), 0, 0, 1);
    if (!l || (l & 0x80000000u)) return 0;
    return base + l;     // IMAGE_RESOURCE_DATA_ENTRY
}
static uint32_t res_module(uint32_t h) { return h ? h : mod_get_handle(NULL); }
uint32_t k_find_resource(uint32_t mod, uint32_t name, uint32_t type, int wide) { return rsrc_find(res_module(mod), name, type, wide); }
static void k_FindResourceA(void) {
    uint32_t r = rsrc_find(res_module(ARG(0)), ARG(1), ARG(2), 0);
    if (!r) last_error = 1814;
    SRET(r, 3);
}
static void k_LoadResource(void) { uint32_t e = ARG(1); SRET(e ? res_module(ARG(0)) + R32(e) : 0, 2); }
static void k_LockResource(void) { SRET(ARG(0), 1); }
static void k_SizeofResource(void) { uint32_t e = ARG(1); SRET(e ? R32(e + 4) : 0, 2); }

// ---------------------------------------------------------------- process, environment, version
static uint32_t cmdline_a, cmdline_w;
static void make_cmdline(void) {
    if (cmdline_a) return;
    char s[1024];
    snprintf(s, sizeof s, "\"" GROOT "\\System\\LOTD.exe\"%s%s", g_cmdline_extra[0] ? " " : "", g_cmdline_extra);
    cmdline_a = guest_strdup(s);
    cmdline_w = heap_alloc((uint32_t)(strlen(s) + 1) * 2);
    put_wstr(cmdline_w, (uint32_t)strlen(s) + 1, s);
}
static void k_GetCommandLineA(void) { make_cmdline(); SRET(cmdline_a, 0); }
static void k_GetCommandLineW(void) { make_cmdline(); SRET(cmdline_w, 0); }
static void k_GetEnvironmentStrings(void) {
    static uint32_t env;
    if (!env) { env = heap_alloc(4); W32(env, 0); }
    SRET(env, 0);
}
static void k_FreeEnvironmentStrings(void) { SRET(1, 1); }
static void k_GetEnvironmentVariableA(void) { last_error = 203; SRET(0, 3); }
static void k_SetEnvironmentVariable(void) { SRET(1, 2); }
static void k_GetStartupInfoA(void) { uint32_t p = ARG(0); COW(p, 68); memset(G + p, 0, 68); W32(p, 68); SRET(0, 1); }
static void k_GetStdHandle(void) {
    uint32_t n = ARG(0);
    SRET(n == 0xfffffff6u ? STD_IN : n == 0xfffffff5u ? STD_OUT : STD_ERR, 1);
}
static void k_SetStdHandle(void) { SRET(1, 2); }
static void k_SetHandleCount(void) { SRET(ARG(0), 1); }
static void k_GetVersion(void) { SRET(0x0A280105u, 0); }
static void version_ex(int wide) {
    uint32_t p = ARG(0);
    W32(p + 4, 5); W32(p + 8, 1); W32(p + 12, 2600); W32(p + 16, 2);   // Windows XP
    if (wide) put_wstr(p + 20, 128, "Service Pack 2"); else snprintf(GSTR(p + 20), 128, "Service Pack 2");
    SRET(1, 1);
}
static void k_GetVersionExA(void) { version_ex(0); }
static void k_GetVersionExW(void) { version_ex(1); }
static void k_GetComputerNameA(void) { uint32_t r = put_str(ARG(0), R32(ARG(1)), "BROWSER", 0); W32(ARG(1), r); SRET(1, 2); }
static void k_GetComputerNameW(void) { uint32_t r = put_str(ARG(0), R32(ARG(1)), "BROWSER", 1); W32(ARG(1), r); SRET(1, 2); }
static void k_GetSystemDirectoryA(void) { SRET(put_str(ARG(0), ARG(1), "C:\\WINDOWS\\system32", 0), 2); }
static void k_GetSystemDirectoryW(void) { SRET(put_str(ARG(0), ARG(1), "C:\\WINDOWS\\system32", 1), 2); }
static void k_GetWindowsDirectoryA(void) { SRET(put_str(ARG(0), ARG(1), "C:\\WINDOWS", 0), 2); }
static void k_GetWindowsDirectoryW(void) { SRET(put_str(ARG(0), ARG(1), "C:\\WINDOWS", 1), 2); }
static void k_GetLastError(void) { SRET(last_error, 0); }
static void k_SetLastError(void) { last_error = ARG(0); SRET(0, 1); }
static void k_SetErrorMode(void) { SRET(0, 1); }
static void k_ExitProcess(void) {
    host_log("ExitProcess(%d)\n", ARG(0));
    host_exit_game((int)ARG(0));
}
static void k_TerminateProcess(void) { host_log("TerminateProcess\n"); host_exit_game((int)ARG(1)); }
static void k_GetCurrentProcess(void) { SRET(0xffffffffu, 0); }
static void k_GetCurrentProcessId(void) { SRET(0x1000, 0); }
static void k_GetCurrentThread(void) { SRET(0xfffffffeu, 0); }
static void k_GetCurrentThreadId(void) { SRET(0x1004, 0); }
static void k_GetProcessWorkingSetSize(void) { W32(ARG(1), 32u << 20); W32(ARG(2), 160u << 20); SRET(1, 3); }
static void k_CreateProcess(void) { last_error = 2; SRET(0, 10); }
static void k_GetExitCodeProcess(void) { W32(ARG(1), 0); SRET(1, 2); }
static void k_CreatePipe(void) { SRET(0, 4); }
static void k_PeekNamedPipe(void) { SRET(0, 6); }
static void k_DuplicateHandle(void) { if (ARG(3)) W32(ARG(3), ARG(1)); SRET(1, 7); }
static void k_CreateMutex(void) { last_error = 0; SRET(0x5000, 3); }
static void k_OutputDebugStringA(void) { host_log("debug: %s", GSTR(ARG(0))); SRET(0, 1); }
static void k_OutputDebugStringW(void) { char s[2048]; wstr(ARG(0), s, sizeof s); host_log("debug: %s", s); SRET(0, 1); }
static void k_DebugBreak(void) { host_log("DebugBreak\n"); SRET(0, 0); }
static void k_Beep(void) { SRET(1, 2); }
static void k_FormatMessage(void) { if (ARG(4) && ARG(5)) W16(ARG(4), 0); SRET(0, 7); }

// ---------------------------------------------------------------- memory
static void k_HeapCreate(void) { SRET(0x7F000001u, 3); }
static void k_HeapDestroy(void) { SRET(1, 1); }
static void k_GetProcessHeap(void) { SRET(0x7F000001u, 0); }
static void k_HeapAlloc(void) {
    uint32_t flags = ARG(1), n = ARG(2);
    uint32_t p = heap_alloc(n ? n : 1);
    if (p && (flags & 8)) { COW(p, n); memset(G + p, 0, n); }
    SRET(p, 3);
}
static void k_HeapFree(void) { heap_free(ARG(2)); SRET(1, 3); }
static void k_HeapReAlloc(void) {
    uint32_t flags = ARG(1), p = ARG(2), n = ARG(3);
    uint32_t old = heap_size(p);
    if (n <= old) SRET(p, 4);
    if (flags & 0x10) SRET(0, 4);    // HEAP_REALLOC_IN_PLACE_ONLY
    uint32_t q = heap_realloc(p, n);
    if (q && (flags & 8)) { COW(q + old, n - old); memset(G + q + old, 0, n - old); }
    SRET(q, 4);
}
static void k_HeapSize(void) { SRET(heap_size(ARG(2)), 3); }
static void k_HeapValidate(void) { SRET(1, 3); }
static void k_HeapCompact(void) { SRET(0, 2); }
static void k_HeapWalk(void) { last_error = 259; SRET(0, 2); }
static void k_GlobalAlloc(void) {
    uint32_t p = heap_alloc(ARG(1) ? ARG(1) : 1);
    if (p && (ARG(0) & 0x40)) { COW(p, ARG(1)); memset(G + p, 0, ARG(1)); }
    SRET(p, 2);
}
static void k_GlobalLock(void) { SRET(ARG(0), 1); }
static void k_GlobalUnlock(void) { SRET(1, 1); }
static void k_GlobalMemoryStatus(void) {
    uint32_t p = ARG(0);
    COW(p, 32); memset(G + p, 0, 32);
    W32(p, 32); W32(p + 4, 30); W32(p + 8, 512u << 20); W32(p + 12, 384u << 20);
    W32(p + 16, 1024u << 20); W32(p + 20, 768u << 20); W32(p + 24, 0x7ffe0000u); W32(p + 28, 0x60000000u);
    SRET(0, 1);
}
// VirtualAlloc: reservations are carved out of the guest heap, 64K aligned
#define MAXV 1024
static struct { uint32_t base, size, block; } vregions[MAXV];
static void k_VirtualAlloc(void) {
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr) {
        for (int i = 0; i < MAXV; i++)
            if (vregions[i].size && addr >= vregions[i].base && addr < vregions[i].base + vregions[i].size)
                SRET(addr & ~0xfffu, 4);
        host_log("VirtualAlloc: commit outside reservation %08x\n", addr);
        SRET(0, 4);
    }
    size = (size + 0xfff) & ~0xfffu;
    uint32_t blk = heap_alloc(size + 0x10000);
    if (!blk) SRET(0, 4);
    uint32_t base = (blk + 0xffff) & ~0xffffu;
    COW(base, size); memset(G + base, 0, size);
    for (int i = 0; i < MAXV; i++) if (!vregions[i].size) {
        vregions[i].base = base; vregions[i].size = size; vregions[i].block = blk;
        SRET(base, 4);
    }
    host_fatal("too many VirtualAlloc regions");
}
static void k_VirtualFree(void) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    for (int i = 0; i < MAXV; i++) {
        if (vregions[i].size && addr >= vregions[i].base && addr < vregions[i].base + vregions[i].size) {
            if (type & 0x8000) {   // MEM_RELEASE
                heap_free(vregions[i].block);
                vregions[i].size = 0;
            } else if (type & 0x4000) {  // MEM_DECOMMIT
                uint32_t end = addr + size;
                if (end > vregions[i].base + vregions[i].size) end = vregions[i].base + vregions[i].size;
                COW(addr, end - addr); memset(G + addr, 0, end - addr);
            }
            SRET(1, 3);
        }
    }
    SRET(0, 3);
}
static void k_VirtualProtect(void) { if (ARG(3)) W32(ARG(3), 4); SRET(1, 4); }
static void k_VirtualQuery(void) {
    uint32_t a = ARG(0), p = ARG(1);
    COW(p, 28); memset(G + p, 0, 28);
    uint32_t base = a & ~0xfffu, region = 0x1000;
    if (a >= 0x10000 && a < 0x370000) { base = 0x10000; region = 0x360000; }   // the guest stack
    W32(p, base); W32(p + 4, base); W32(p + 8, 4); W32(p + 12, region); W32(p + 16, 0x1000); W32(p + 20, 4); W32(p + 24, 0x20000);
    SRET(28, 3);
}
static void k_IsBad2(void) { SRET(0, 2); }
static void k_IsBad1(void) { SRET(0, 1); }
static void k_GetSystemInfo(void) {
    uint32_t p = ARG(0);
    COW(p, 36); memset(G + p, 0, 36);
    W16(p, 0); W32(p + 4, 4096); W32(p + 8, 0x10000); W32(p + 12, 0x7ffeffff); W32(p + 16, 1);
    W32(p + 20, 1); W32(p + 24, 586); W32(p + 28, 0x10000); W16(p + 32, 6); W16(p + 34, 0x0603);
    SRET(0, 1);
}
static void k_IsProcessorFeaturePresent(void) { SRET(0, 1); }   // no SSE/SSE2 (see host_cpuid)

// ---------------------------------------------------------------- sync, TLS, threads
static void k_CriticalSection(void) { SRET(0, 1); }
static void k_InterlockedIncrement(void) { uint32_t v = R32(ARG(0)) + 1; W32(ARG(0), v); SRET(v, 1); }
static void k_InterlockedDecrement(void) { uint32_t v = R32(ARG(0)) - 1; W32(ARG(0), v); SRET(v, 1); }
static void k_InterlockedExchange(void) { uint32_t v = R32(ARG(0)); W32(ARG(0), ARG(1)); SRET(v, 2); }
static uint32_t tls[64]; static int tls_used[64];
static void k_TlsAlloc(void) { for (int i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; tls[i] = 0; SRET(i, 0); } SRET(0xffffffffu, 0); }
static void k_TlsFree(void) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; SRET(1, 1); }
static void k_TlsGetValue(void) { last_error = 0; SRET(ARG(0) < 64 ? tls[ARG(0)] : 0, 1); }
static void k_TlsSetValue(void) { if (ARG(0) < 64) tls[ARG(0)] = ARG(1); SRET(1, 2); }

// Threads. The guest is single-threaded here. A thread that does a job and ends (IpDrv's host name lookup) runs to
// completion inside CreateThread on its own guest stack. Threads that loop forever are told apart by their start
// address (game.c: game_thread_kind) and their work is done from the frame loop instead (game_background_tick).
static struct { uint32_t h, start; int done; } threads[MAXTHREADS];
static jmp_buf *thread_exit_jmp;
int game_thread_kind(uint32_t start);    // game.c: 0 run now, 1 polled by the host (never run)
static void close_thread(uint32_t h) {
    for (int i = 0; i < MAXTHREADS; i++) if (threads[i].h == h) threads[i].h = 0;
}
static void k_CreateThread(void) {
    uint32_t start = ARG(2), param = ARG(3), pid = ARG(5);
    int slot = -1;
    for (int i = 0; i < MAXTHREADS; i++) if (!threads[i].h) { slot = i; break; }
    if (slot < 0) host_fatal("too many threads");
    uint32_t h = 0x6000u + slot * 4;
    threads[slot].h = h; threads[slot].start = start; threads[slot].done = 0;
    if (pid) W32(pid, 0x2000u + slot);
    int kind = game_thread_kind(start);
    host_log("CreateThread(%08x, %08x): %s\n", start, param, kind ? "work done by the host" : "run to completion");
    if (kind == 0) {
        Regs save = R;
        uint32_t stack = heap_alloc(256 * 1024);
        R.esp = stack + 256 * 1024 - 16;
        jmp_buf jb, *outer = thread_exit_jmp;
        thread_exit_jmp = &jb;
        if (!setjmp(jb)) guest_call(start, 1, param);
        thread_exit_jmp = outer;
        if (g_unwind) host_fatal("exception escaped thread %08x", start);
        R = save;
        heap_free(stack);
        threads[slot].done = 1;
    }
    SRET(h, 6);
}
static void k_ExitThread(void) {
    if (thread_exit_jmp) longjmp(*thread_exit_jmp, 1);
    host_fatal("ExitThread outside a thread");
}
static void k_ResumeThread(void) { SRET(0, 1); }
static void k_SetThreadPriority(void) { SRET(1, 2); }
static void k_WaitForSingleObject(void) { game_background_tick(); SRET(0, 2); }

// ---------------------------------------------------------------- SEH dispatch
// RaiseException walks the fs:[0] chain and calls each handler like Windows does. A C++ catch ends in
// _JumpToContinuation, which sets g_unwind (runtime.c): the handler call returns at once and so does this shim,
// leaving the registers as the unwind set them.
#define EXCEPTION_UNWINDING 2
static int seh_trace = -1;
#define SEH_TRACE_INIT() do { if (seh_trace < 0) seh_trace = getenv("PORT_SEH") != NULL; } while (0)
static void log_cxx_exception(uint32_t rec) {
    // MSVC C++ exception: params[1] = thrown object, params[2] = ThrowInfo. UE2 throws a TCHAR* message.
    uint32_t obj = R32(rec + 20 + 4), ti = R32(rec + 20 + 8);
    if (!ti) return;
    uint32_t cta = R32(ti + 12);
    uint32_t ct = cta ? R32(cta + 4) : 0;
    uint32_t td = ct ? R32(ct + 4) : 0;
    const char *tname = td ? GSTR(td + 8) : "?";
    host_log("  C++ exception of type %s\n", tname);
    if (strstr(tname, "PBG") || strstr(tname, "PAG")) {     // unsigned short const *
        char s[1024]; wstr(R32(obj), s, sizeof s);
        host_log("  message: %s\n", s);
    }
}
// The dispatcher's own registration, pushed around each handler call like ntdll's ExecuteHandler2 does: an
// exception raised while a handler runs (a throw inside a catch block) reaches it and gets told to skip ahead to
// the frame that handler belongs to (ExceptionNestedException). MSVC's _UnwindNestedFrames relinks this record
// after its RtlUnwind, so it must exist.
static void seh_nested_handler(void) {
    uint32_t rec = ARG(0), frame = ARG(1), disp = ARG(3);
    if (R32(rec + 4) & 6) { R.eax = 1; R.esp += 4; return; }      // unwinding: ExceptionContinueSearch
    W32(disp, R32(frame + 8));
    R.eax = 2; R.esp += 4;                                          // ExceptionNestedException
}
static void k_RaiseException(void) {
    SEH_TRACE_INIT();
    uint32_t code = ARG(0), flags = ARG(1), nargs = ARG(2), args = ARG(3);
    uint32_t ret = R32(R.esp);
    uint32_t sp = R.esp - 0x400;
    uint32_t rec = sp, ctx = sp + 0x80, disp = sp + 0x380;
    memset(G + sp, 0, 0x400);
    W32(rec, code); W32(rec + 4, flags & 1); W32(rec + 8, 0); W32(rec + 12, ret);
    if (nargs > 15) nargs = 15;
    W32(rec + 16, nargs);
    for (uint32_t i = 0; i < nargs; i++) W32(rec + 20 + 4 * i, R32(args + 4 * i));
    W32(ctx, 0x10007);
    W32(ctx + 0xb0, R.eax); W32(ctx + 0xac, R.ecx); W32(ctx + 0xa8, R.edx); W32(ctx + 0xa4, R.ebx);
    W32(ctx + 0xa0, R.esi); W32(ctx + 0x9c, R.edi); W32(ctx + 0xb4, R.ebp); W32(ctx + 0xb8, ret); W32(ctx + 0xc4, R.esp);
    Regs save = R;
    R.esp = sp - 16;
    if (code == 0xE06D7363u) {
        host_log("RaiseException: C++ throw from %08x\n", ret); log_cxx_exception(rec);
        if (seh_trace) { Regs keep = R; R = save; host_backtrace(); R = keep; }
    }
    else host_log("RaiseException %08x from %08x\n", code, ret);
    static uint32_t nested_thunk;
    if (!nested_thunk) nested_thunk = host_thunk(seh_nested_handler, "SEH nested-exception frame");
    uint32_t nest = sp + 0x3c0, nested_frame = 0;
    for (uint32_t reg = R32(FS_BASE); reg != 0xffffffffu && reg; reg = R32(reg)) {
        uint32_t handler = R32(reg + 4);
        W32(nest, R32(FS_BASE)); W32(nest + 4, nested_thunk); W32(nest + 8, reg);
        W32(FS_BASE, nest);
        uint32_t r = guest_call(handler, 4, rec, reg, ctx, disp);
        if (seh_trace) host_log("  SEH frame %08x handler %08x -> %u%s\n", reg, handler, r, g_unwind ? " (resumed)" : "");
        if (g_unwind) return;                          // a catch block resumed somewhere up the stack
        W32(FS_BASE, R32(nest));
        if (reg == nested_frame) { W32(rec + 4, R32(rec + 4) & ~0x10u); nested_frame = 0; }
        if (r == 0) { R = save; SRET(0, 4); }          // ExceptionContinueExecution
        if (r == 2) {                                  // ExceptionNestedException: skip to the frame being handled
            W32(rec + 4, R32(rec + 4) | 0x10u);
            if (R32(disp) > nested_frame) nested_frame = R32(disp);
        } else if (r != 1) host_log("RaiseException: handler %08x returned %u\n", handler, r);
    }
    R = save;
    host_fatal("unhandled exception %08x", code);
}
// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): call the handlers of the frames above the target
// with the unwinding flag and unlink them.
static void k_RtlUnwind(void) {
    SEH_TRACE_INIT();
    uint32_t target = ARG(0), rec = ARG(2), rv = ARG(3);
    if (seh_trace) host_log("RtlUnwind to %08x (head %08x)\n", target, R32(FS_BASE));
    Regs save = R;
    uint32_t sp = R.esp - 0x400;
    if (!rec) { rec = sp; memset(G + rec, 0, 80); W32(rec, 0xC0000027u); }
    W32(rec + 4, R32(rec + 4) | EXCEPTION_UNWINDING | (target ? 0 : 4));
    uint32_t ctx = sp + 0x80, disp = sp + 0x380;
    R.esp = sp - 16;
    for (uint32_t reg = R32(FS_BASE); reg != 0xffffffffu && reg && reg != target; ) {
        uint32_t handler = R32(reg + 4), next = R32(reg);
        guest_call(handler, 4, rec, reg, ctx, disp);
        if (seh_trace) host_log("  unwind frame %08x handler %08x\n", reg, handler);
        if (g_unwind) return;
        W32(FS_BASE, next);
        reg = next;
    }
    R = save;
    SRET(rv, 4);
}
static void k_SetUnhandledExceptionFilter(void) { SRET(0, 1); }
static void k_UnhandledExceptionFilter(void) { SRET(0, 1); }
static void k_SetConsoleCtrlHandler(void) { SRET(1, 2); }

// ---------------------------------------------------------------- locale and code pages (ANSI = 1252)
static void k_GetACP(void) { SRET(1252, 0); }
static void k_GetOEMCP(void) { SRET(437, 0); }
static void k_GetCPInfo(void) {
    uint32_t p = ARG(1);
    COW(p, 20); memset(G + p, 0, 20);
    W32(p, 1); W8(p + 4, '?');
    SRET(1, 2);
}
static void k_IsDBCSLeadByteEx(void) { SRET(0, 2); }
static void k_MultiByteToWideChar(void) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (cap == 0) SRET(n, 6);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) W16(dst + i * 2, R8(src + i));
    SRET(k, 6);
}
static void k_WideCharToMultiByte(void) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)wlen(src) + 1;
    if (cap == 0) SRET(n, 8);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) { uint32_t c = R16(src + i * 2); W8(dst + i, c < 256 ? c : '?'); }
    SRET(k, 8);
}
static void k_LCMapStringA(void) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (cap == 0) SRET(n, 6);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) {
        int c = R8(src + i);
        if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c);
        W8(dst + i, c);
    }
    SRET(k, 6);
}
static void k_LCMapStringW(void) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)wlen(src) + 1;
    if (cap == 0) SRET(n, 6);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) {
        uint32_t c = R16(src + 2 * i);
        if (c < 128) { if (flags & 0x100) c = tolower((int)c); else if (flags & 0x200) c = toupper((int)c); }
        W16(dst + 2 * i, c);
    }
    SRET(k, 6);
}
static uint32_t ctype1(uint32_t c) {
    uint32_t t = 0;
    if (c < 128) {
        if (isupper((int)c)) t |= 1; if (islower((int)c)) t |= 2; if (isdigit((int)c)) t |= 4; if (isspace((int)c)) t |= 8;
        if (ispunct((int)c)) t |= 0x10; if (iscntrl((int)c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40;
        if (isxdigit((int)c)) t |= 0x80; if (isalpha((int)c)) t |= 0x100;
    } else t = 0x100 | 0x2;
    return t;
}
static void k_GetStringTypeA(void) {
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int i = 0; i < n; i++) W16(out + i * 2, ctype1(R8(src + i)));
    SRET(1, 5);
}
static void k_GetStringTypeW(void) {
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) n = (int32_t)wlen(src) + 1;
    for (int i = 0; i < n; i++) W16(out + i * 2, ctype1(R16(src + 2 * i)));
    SRET(1, 4);
}
static void compare_string(int wide) {
    uint32_t flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0) n1 = wide ? (int32_t)wlen(s1) : (int32_t)strlen(GSTR(s1));
    if (n2 < 0) n2 = wide ? (int32_t)wlen(s2) : (int32_t)strlen(GSTR(s2));
    int n = n1 < n2 ? n1 : n2, r = 0;
    for (int i = 0; i < n && !r; i++) {
        int a = wide ? (int)R16(s1 + 2 * i) : (int)R8(s1 + i), b = wide ? (int)R16(s2 + 2 * i) : (int)R8(s2 + i);
        if ((flags & 1) && a < 128 && b < 128) { a = tolower(a); b = tolower(b); }
        r = a - b;
    }
    if (!r) r = n1 - n2;
    SRET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
static void k_CompareStringA(void) { compare_string(0); }
static void k_CompareStringW(void) { compare_string(1); }
static void k_GetDateFormatA(void) {
    uint32_t buf = ARG(4); int32_t cap = (int32_t)ARG(5);
    time_t t = time(NULL); char s[64];
    strftime(s, sizeof s, "%m/%d/%Y", localtime(&t));
    if (cap == 0) SRET(strlen(s) + 1, 6);
    COW(buf, (uint32_t)cap); snprintf(GSTR(buf), cap, "%s", s);
    SRET(strlen(GSTR(buf)) + 1, 6);
}
static void k_GetTimeFormatA(void) {
    uint32_t buf = ARG(4); int32_t cap = (int32_t)ARG(5);
    time_t t = time(NULL); char s[64];
    strftime(s, sizeof s, "%H:%M:%S", localtime(&t));
    if (cap == 0) SRET(strlen(s) + 1, 6);
    COW(buf, (uint32_t)cap); snprintf(GSTR(buf), cap, "%s", s);
    SRET(strlen(GSTR(buf)) + 1, 6);
}
static void k_EnumSystemLocalesA(void) { SRET(1, 2); }
static void k_IsValidCodePage(void) { SRET(1, 1); }
static void k_IsValidLocale(void) { SRET(1, 2); }
static void k_GetUserDefaultLCID(void) { SRET(0x409, 0); }
static void k_GetThreadLocale(void) { SRET(0x409, 0); }
static void k_GetSystemDefaultLangID(void) { SRET(0x409, 0); }
static const char *locale_value(uint32_t type) {
    switch (type & 0xffff) {
    case 0x1004: return "1252";    // LOCALE_IDEFAULTANSICODEPAGE
    case 0x000B: return "437";     // OEM code page
    case 0x0001: return "0409";    // LCID
    case 0x0003: case 0x0059: return "ENU";
    case 0x0007: case 0x005A: return "USA";
    case 0x000E: return ".";
    case 0x000F: return ",";
    case 0x1001: return "English";
    case 0x1002: return "United States";
    default: return "";
    }
}
static void k_GetLocaleInfoA(void) {
    const char *v = locale_value(ARG(1));
    int32_t cap = (int32_t)ARG(3);
    int n = (int)strlen(v) + 1;
    if (cap == 0) SRET(n, 4);
    if (cap < n) { last_error = 122; SRET(0, 4); }
    COW(ARG(2), n); memcpy(GSTR(ARG(2)), v, n);
    SRET(n, 4);
}
static void k_GetLocaleInfoW(void) {
    const char *v = locale_value(ARG(1));
    int32_t cap = (int32_t)ARG(3);
    int n = (int)strlen(v) + 1;
    if (cap == 0) SRET(n, 4);
    if (cap < n) { last_error = 122; SRET(0, 4); }
    put_wstr(ARG(2), cap, v);
    SRET(n, 4);
}
static int natoms;
static void k_GlobalAddAtomW(void) { if (natoms < 256) natoms++; SRET(0xC000 + natoms, 1); }
static void k_GlobalDeleteAtom(void) { SRET(0, 1); }

// ---------------------------------------------------------------- console (none)
static void k_ret0_0(void) { SRET(0, 0); }
static void k_ret0_2(void) { SRET(0, 2); }
static void k_ret0_4(void) { SRET(0, 4); }
static void k_ret0_5(void) { SRET(0, 5); }
static void k_WriteConsoleA(void) { host_log("%.*s", (int)ARG(2), GSTR(ARG(1))); if (ARG(3)) W32(ARG(3), ARG(2)); SRET(1, 5); }
static void k_WriteConsoleW(void) { if (ARG(3)) W32(ARG(3), ARG(2)); SRET(1, 5); }

#define K(n, f) {"KERNEL32", n, f}
const ShimDef shims_kernel32[] = {
    K("CreateFileA", k_CreateFileA), K("CreateFileW", k_CreateFileW), K("ReadFile", k_ReadFile), K("WriteFile", k_WriteFile),
    K("SetFilePointer", k_SetFilePointer), K("SetEndOfFile", k_SetEndOfFile), K("GetFileSize", k_GetFileSize),
    K("CloseHandle", k_CloseHandle), K("FlushFileBuffers", k_FlushFileBuffers), K("GetFileType", k_GetFileType),
    K("LockFile", k_LockFile), K("UnlockFile", k_LockFile),
    K("GetFileTime", k_GetFileTime), K("SetFileTime", k_SetFileTime), K("FileTimeToLocalFileTime", k_FileTimeToLocalFileTime),
    K("LocalFileTimeToFileTime", k_LocalFileTimeToFileTime), K("FileTimeToSystemTime", k_FileTimeToSystemTime),
    K("SystemTimeToFileTime", k_SystemTimeToFileTime), K("GetLocalTime", k_GetLocalTime), K("SetLocalTime", k_SetLocalTime),
    K("GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime), K("GetTimeZoneInformation", k_GetTimeZoneInformation),
    K("GetTickCount", k_GetTickCount), K("QueryPerformanceCounter", k_QueryPerformanceCounter),
    K("QueryPerformanceFrequency", k_QueryPerformanceFrequency), K("Sleep", k_Sleep),
    K("GetFileAttributesA", k_GetFileAttributesA), K("GetFileAttributesW", k_GetFileAttributesW),
    K("GetFileAttributesExW", k_GetFileAttributesExW), K("GetFileInformationByHandle", k_GetFileInformationByHandle),
    K("SetFileAttributesA", k_SetFileAttributes), K("SetFileAttributesW", k_SetFileAttributes),
    K("DeleteFileA", k_DeleteFileA), K("DeleteFileW", k_DeleteFileW), K("MoveFileA", k_MoveFileA), K("MoveFileW", k_MoveFileW),
    K("CopyFileA", k_CopyFileA), K("CopyFileW", k_CopyFileW), K("CreateDirectoryA", k_CreateDirectoryA),
    K("CreateDirectoryW", k_CreateDirectoryW), K("RemoveDirectoryA", k_RemoveDirectory), K("RemoveDirectoryW", k_RemoveDirectory),
    K("GetCurrentDirectoryA", k_GetCurrentDirectoryA), K("GetCurrentDirectoryW", k_GetCurrentDirectoryW),
    K("SetCurrentDirectoryA", k_SetCurrentDirectoryA), K("SetCurrentDirectoryW", k_SetCurrentDirectoryW),
    K("GetFullPathNameA", k_GetFullPathNameA), K("GetFullPathNameW", k_GetFullPathNameW),
    K("FindFirstFileA", k_FindFirstFileA), K("FindFirstFileW", k_FindFirstFileW), K("FindNextFileA", k_FindNextFileA),
    K("FindNextFileW", k_FindNextFileW), K("FindClose", k_FindClose), K("GetDriveTypeA", k_GetDriveType),
    K("GetDriveTypeW", k_GetDriveType), K("GetLogicalDrives", k_GetLogicalDrives), K("GetDiskFreeSpaceA", k_GetDiskFreeSpaceA),
    K("GetModuleFileNameA", k_GetModuleFileNameA), K("GetModuleFileNameW", k_GetModuleFileNameW),
    K("GetModuleHandleA", k_GetModuleHandleA), K("GetModuleHandleW", k_GetModuleHandleW), K("LoadLibraryA", k_LoadLibraryA),
    K("LoadLibraryW", k_LoadLibraryW), K("LoadLibraryExW", k_LoadLibraryExW), K("FreeLibrary", k_FreeLibrary),
    K("GetProcAddress", k_GetProcAddress), K("DisableThreadLibraryCalls", k_DisableThreadLibraryCalls),
    K("FindResourceA", k_FindResourceA), K("LoadResource", k_LoadResource), K("LockResource", k_LockResource),
    K("SizeofResource", k_SizeofResource),
    K("GetCommandLineA", k_GetCommandLineA), K("GetCommandLineW", k_GetCommandLineW),
    K("GetEnvironmentStrings", k_GetEnvironmentStrings), K("GetEnvironmentStringsW", k_GetEnvironmentStrings),
    K("FreeEnvironmentStringsA", k_FreeEnvironmentStrings), K("FreeEnvironmentStringsW", k_FreeEnvironmentStrings),
    K("GetEnvironmentVariableA", k_GetEnvironmentVariableA), K("SetEnvironmentVariableA", k_SetEnvironmentVariable),
    K("SetEnvironmentVariableW", k_SetEnvironmentVariable), K("GetStartupInfoA", k_GetStartupInfoA),
    K("GetStdHandle", k_GetStdHandle), K("SetStdHandle", k_SetStdHandle), K("SetHandleCount", k_SetHandleCount),
    K("GetVersion", k_GetVersion), K("GetVersionExA", k_GetVersionExA), K("GetVersionExW", k_GetVersionExW),
    K("GetComputerNameA", k_GetComputerNameA), K("GetComputerNameW", k_GetComputerNameW),
    K("GetSystemDirectoryA", k_GetSystemDirectoryA), K("GetSystemDirectoryW", k_GetSystemDirectoryW),
    K("GetWindowsDirectoryA", k_GetWindowsDirectoryA), K("GetWindowsDirectoryW", k_GetWindowsDirectoryW),
    K("GetLastError", k_GetLastError), K("SetLastError", k_SetLastError), K("SetErrorMode", k_SetErrorMode),
    K("ExitProcess", k_ExitProcess), K("TerminateProcess", k_TerminateProcess), K("GetCurrentProcess", k_GetCurrentProcess),
    K("GetCurrentProcessId", k_GetCurrentProcessId), K("GetCurrentThread", k_GetCurrentThread),
    K("GetCurrentThreadId", k_GetCurrentThreadId), K("GetProcessWorkingSetSize", k_GetProcessWorkingSetSize),
    K("CreateProcessA", k_CreateProcess), K("CreateProcessW", k_CreateProcess), K("GetExitCodeProcess", k_GetExitCodeProcess),
    K("CreatePipe", k_CreatePipe), K("PeekNamedPipe", k_PeekNamedPipe), K("DuplicateHandle", k_DuplicateHandle),
    K("CreateMutexA", k_CreateMutex), K("CreateMutexW", k_CreateMutex),
    K("OutputDebugStringA", k_OutputDebugStringA), K("OutputDebugStringW", k_OutputDebugStringW),
    K("DebugBreak", k_DebugBreak), K("Beep", k_Beep), K("FormatMessageA", k_FormatMessage), K("FormatMessageW", k_FormatMessage),
    K("HeapCreate", k_HeapCreate), K("HeapDestroy", k_HeapDestroy), K("GetProcessHeap", k_GetProcessHeap),
    K("HeapAlloc", k_HeapAlloc), K("HeapFree", k_HeapFree), K("HeapReAlloc", k_HeapReAlloc), K("HeapSize", k_HeapSize),
    K("HeapValidate", k_HeapValidate), K("HeapCompact", k_HeapCompact), K("HeapWalk", k_HeapWalk),
    K("GlobalAlloc", k_GlobalAlloc), K("GlobalLock", k_GlobalLock), K("GlobalUnlock", k_GlobalUnlock),
    K("GlobalMemoryStatus", k_GlobalMemoryStatus), K("VirtualAlloc", k_VirtualAlloc), K("VirtualFree", k_VirtualFree),
    K("VirtualProtect", k_VirtualProtect), K("VirtualQuery", k_VirtualQuery),
    K("IsBadReadPtr", k_IsBad2), K("IsBadWritePtr", k_IsBad2), K("IsBadCodePtr", k_IsBad1),
    K("GetSystemInfo", k_GetSystemInfo), K("IsProcessorFeaturePresent", k_IsProcessorFeaturePresent),
    K("InitializeCriticalSection", k_CriticalSection), K("DeleteCriticalSection", k_CriticalSection),
    K("EnterCriticalSection", k_CriticalSection), K("LeaveCriticalSection", k_CriticalSection),
    K("InterlockedIncrement", k_InterlockedIncrement), K("InterlockedDecrement", k_InterlockedDecrement),
    K("InterlockedExchange", k_InterlockedExchange),
    K("TlsAlloc", k_TlsAlloc), K("TlsFree", k_TlsFree), K("TlsGetValue", k_TlsGetValue), K("TlsSetValue", k_TlsSetValue),
    K("CreateThread", k_CreateThread), K("ExitThread", k_ExitThread), K("ResumeThread", k_ResumeThread),
    K("SetThreadPriority", k_SetThreadPriority), K("WaitForSingleObject", k_WaitForSingleObject),
    K("RaiseException", k_RaiseException), K("RtlUnwind", k_RtlUnwind),
    K("SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter), K("UnhandledExceptionFilter", k_UnhandledExceptionFilter),
    K("SetConsoleCtrlHandler", k_SetConsoleCtrlHandler),
    K("GetACP", k_GetACP), K("GetOEMCP", k_GetOEMCP), K("GetCPInfo", k_GetCPInfo), K("IsDBCSLeadByteEx", k_IsDBCSLeadByteEx),
    K("MultiByteToWideChar", k_MultiByteToWideChar), K("WideCharToMultiByte", k_WideCharToMultiByte),
    K("LCMapStringA", k_LCMapStringA), K("LCMapStringW", k_LCMapStringW), K("GetStringTypeA", k_GetStringTypeA),
    K("GetStringTypeW", k_GetStringTypeW), K("CompareStringA", k_CompareStringA), K("CompareStringW", k_CompareStringW),
    K("GetDateFormatA", k_GetDateFormatA), K("GetTimeFormatA", k_GetTimeFormatA), K("EnumSystemLocalesA", k_EnumSystemLocalesA),
    K("IsValidCodePage", k_IsValidCodePage), K("IsValidLocale", k_IsValidLocale), K("GetUserDefaultLCID", k_GetUserDefaultLCID),
    K("GetThreadLocale", k_GetThreadLocale), K("GetSystemDefaultLangID", k_GetSystemDefaultLangID),
    K("GetLocaleInfoA", k_GetLocaleInfoA), K("GetLocaleInfoW", k_GetLocaleInfoW),
    K("GlobalAddAtomW", k_GlobalAddAtomW), K("GlobalDeleteAtom", k_GlobalDeleteAtom),
    K("GetConsoleCP", k_ret0_0), K("GetConsoleOutputCP", k_ret0_0), K("GetConsoleMode", k_ret0_2), K("SetConsoleMode", k_ret0_2),
    K("GetNumberOfConsoleInputEvents", k_ret0_2), K("PeekConsoleInputA", k_ret0_4), K("ReadConsoleA", k_ret0_5),
    K("ReadConsoleW", k_ret0_5), K("ReadConsoleInputA", k_ret0_4), K("ReadConsoleInputW", k_ret0_4),
    K("WriteConsoleA", k_WriteConsoleA), K("WriteConsoleW", k_WriteConsoleW),
    {0, 0, 0}
};
