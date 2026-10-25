/* boomlaunch: start the ORIGINAL Themida-protected ca.exe and patch it in memory once Themida
 * has decrypted the image (same patches as tools/patch_client.py, but the file on disk is untouched).
 *   boomlaunch.exe [server_ip] [ca.exe args...]      default server_ip = 127.0.0.1
 * Build: i686-w64-mingw32-gcc -O2 -o boomlaunch.exe boomlaunch.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { DWORD va; const char *old; const char *new_; int len; int done; } Patch;

static const char *IPS[][2] = {   /* va, original string (see tools/patch_client.py IP_STRINGS) */
    {(const char *)0xC88930, "222.255.122.204"}, {(const char *)0xC88920, "120.138.77.22"},
    {(const char *)0xC88910, "120.138.77.23"},   {(const char *)0xC88900, "120.138.77.24"},
    {(const char *)0xC888F0, "120.138.77.25"},   {(const char *)0xC888E0, "120.138.77.83"},
    {(const char *)0xC888D0, "120.138.77.84"},   {(const char *)0xC888C0, "120.138.77.85"},
    {(const char *)0xC7C638, "118.102.5.126"},   {(const char *)0xC83534, "175.207.3.35"},
};

#define SITE 0x008F4D46u
#define TARGET_LO 0x011E4000u
#define TARGET_HI 0x016AB000u
#define CAVE_SIZE 0x1000u
#define FLAG_OFF 0xFF0u
#define REGS_OFF 0x800u
#define MAX_THREADS 128
#define CAPTURE_BYTES (512ULL * 1024ULL * 1024ULL)
#define CAPTURE_MS 10000

typedef struct { HANDLE h[MAX_THREADS]; int n; } Suspended;
typedef struct {
    PROCESS_INFORMATION *pi;
    LPVOID cave;
    DWORD flag, regs, original_target;
    DWORD site_old_protect;
    BYTE original[5], hook[5];
    int site_protection_known, attempted, installed, capture_done, fatal;
} Capture;

static int write_full(HANDLE p, LPVOID at, const void *data, SIZE_T len) {
    SIZE_T n = 0;
    return WriteProcessMemory(p, at, data, len, &n) && n == len;
}

static int read_full(HANDLE p, LPCVOID at, void *data, SIZE_T len) {
    SIZE_T n = 0;
    return ReadProcessMemory(p, at, data, len, &n) && n == len;
}

static int resume_threads(Suspended *s) {
    int i, ok = 1;
    for (i = 0; i < s->n; i++) {
        if (ResumeThread(s->h[i]) == (DWORD)-1) {
            printf("thread resume FAILED error=%lu\n", GetLastError());
            ok = 0;
        }
        CloseHandle(s->h[i]);
    }
    s->n = 0;
    return ok;
}

static int suspend_threads(DWORD pid, Suspended *s) {
    HANDLE snapshot;
    THREADENTRY32 te;
    BOOL more;

    memset(s, 0, sizeof *s);
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        printf("thread snapshot FAILED error=%lu\n", GetLastError());
        return 0;
    }
    te.dwSize = sizeof te;
    more = Thread32First(snapshot, &te);
    if (!more) {
        printf("Thread32First FAILED error=%lu\n", GetLastError());
        CloseHandle(snapshot);
        return 0;
    }
    do {
        HANDLE thread;
        DWORD result;
        if (te.th32OwnerProcessID != pid) continue;
        if (s->n == MAX_THREADS) {
            printf("thread suspension FAILED: more than %d target threads\n", MAX_THREADS);
            CloseHandle(snapshot);
            resume_threads(s);
            return 0;
        }
        thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!thread) {
            printf("OpenThread tid=%lu FAILED error=%lu\n", te.th32ThreadID, GetLastError());
            CloseHandle(snapshot);
            resume_threads(s);
            return 0;
        }
        result = SuspendThread(thread);
        if (result == (DWORD)-1) {
            printf("SuspendThread tid=%lu FAILED error=%lu\n", te.th32ThreadID, GetLastError());
            CloseHandle(thread);
            CloseHandle(snapshot);
            resume_threads(s);
            return 0;
        }
        s->h[s->n++] = thread;
    } while ((more = Thread32Next(snapshot, &te)) != FALSE);
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        printf("Thread32Next FAILED error=%lu\n", GetLastError());
        CloseHandle(snapshot);
        resume_threads(s);
        return 0;
    }
    CloseHandle(snapshot);
    if (!s->n) {
        printf("thread suspension FAILED: no target threads enumerated\n");
        return 0;
    }
    printf("thread snapshot race limitation: threads created after enumeration are not suspended\n");
    return 1;
}

/*
 * Stub offset table (relative to cave+0x10; emitted length 97 bytes):
 *   000 pushfd, 001 pushad, 002 mov eax,0, 007 mov ebx,1,
 *   00c lock cmpxchg (ends 014), 014 jne restore (next 016),
 *   016 TEB load, 01c TEB store, 021 esi=esp, 023 edi=regs, 028 ecx=9,
 *   02d cld, 02e rep movsd (ends 030), 030 flag READY (ends 03a),
 *   03a ecx=count (ends 03f), 03f spin, 041 cmp flag (ends 04b),
 *   04b je restore (next 04d), 04d dec ecx, 04e jnz spin (next 050),
 *   050 flag EXPIRED (ends 05a), 05a restore popad/popfd (ends 05c),
 *   05c jmp original target (ends 061).  Branch displacements are label-derived.
 */
static int build_stub(BYTE *b, DWORD cave, DWORD regs, DWORD flag, DWORD target) {
    int n = 0;
    int jne_restore_disp, je_restore_disp, jnz_spin_disp;
    int jne_restore_next, je_restore_next, jnz_spin_next;
    int restore, spin;
    DWORD rel;

#define E1(x) b[n++] = (BYTE)(x)
#define E4(x) do { DWORD value = (DWORD)(x); memcpy(b + n, &value, 4); n += 4; } while (0)

    E1(0x9c); E1(0x60);                         /* pushfd; pushad */
    E1(0xb8); E4(0); E1(0xbb); E4(1);           /* eax=0, ebx=INITIALIZING */
    E1(0xf0); E1(0x0f); E1(0xb1); E1(0x1d); E4(flag); /* lock cmpxchg [flag],ebx */
    E1(0x75); jne_restore_disp = n; E1(0);      /* loser -> restore */

    E1(0x64); E1(0xa1); E4(0x18);               /* eax=TEB */
    E1(0xa3); E4(regs + 36);                    /* save TEB */
    E1(0x89); E1(0xe6); E1(0xbf); E4(regs);     /* esi=esp; edi=regs */
    E1(0xb9); E4(9); E1(0xfc); E1(0xf3); E1(0xa5); /* cld; copy pushad + flags */
    E1(0xc7); E1(0x05); E4(flag); E4(2);       /* READY after registers copied */
    E1(0xb9); E4(0x10000000);                  /* finite count, not milliseconds */

    spin = n;
    E1(0xf3); E1(0x90);                         /* pause */
    E1(0x81); E1(0x3d); E4(flag); E4(3);        /* cmp flag, RELEASE */
    E1(0x74); je_restore_disp = n; E1(0);       /* release -> restore */
    E1(0x49);                                   /* dec ecx */
    E1(0x75); jnz_spin_disp = n; E1(0);         /* jnz spin */
    E1(0xc7); E1(0x05); E4(flag); E4(4);        /* EXPIRED */

    restore = n;
    E1(0x61); E1(0x9d);                         /* popad; popfd */
    E1(0xe9); rel = target - (cave + n + 4); E4(rel); /* original E9 target */

    jne_restore_next = jne_restore_disp + 1;
    je_restore_next = je_restore_disp + 1;
    jnz_spin_next = jnz_spin_disp + 1;
    if (restore - jne_restore_next < -128 || restore - jne_restore_next > 127 ||
        restore - je_restore_next < -128 || restore - je_restore_next > 127 ||
        spin - jnz_spin_next < -128 || spin - jnz_spin_next > 127) {
        printf("capture stub branch out of range\n");
        return 0;
    }
    b[jne_restore_disp] = (BYTE)(restore - jne_restore_next);
    b[je_restore_disp] = (BYTE)(restore - je_restore_next);
    b[jnz_spin_disp] = (BYTE)(spin - jnz_spin_next);

#undef E1
#undef E4
    printf("capture stub emitted=%d bytes restore=%d spin=%d jne_disp=%d je_disp=%d jnz_disp=%d\n",
           n, restore, spin, jne_restore_disp, je_restore_disp, jnz_spin_disp);
    return n;
}

static int restore_site_held(Capture *c, Suspended *s, const char *why, int force_rollback) {
    BYTE current[5];
    DWORD old_protect = 0, desired_protect, temp;
    int protection_changed = 0, write_attempted = 0, write_ok = 0;
    int restore_ok = 0, flush_ok = 0, readback_ok = 0, ok = 0;

    if (!read_full(c->pi->hProcess, (LPCVOID)(size_t)SITE, current, sizeof current)) {
        printf("%s: site read FAILED error=%lu\n", why, GetLastError());
        c->fatal = 1;
        return 0;
    }
    if (!force_rollback && !memcmp(current, c->original, sizeof current)) return 1;
    if (!force_rollback && memcmp(current, c->hook, sizeof current)) {
        printf("%s: site changed by another writer; refusing overwrite\n", why);
        c->fatal = 1;
        return 0;
    }
    if (!VirtualProtectEx(c->pi->hProcess, (LPVOID)(size_t)SITE, sizeof current,
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("%s: VirtualProtectEx failed error=%lu\n", why, GetLastError());
        c->fatal = 1;
        return 0;
    }
    desired_protect = c->site_protection_known ? c->site_old_protect : old_protect;
    protection_changed = 1;
    write_attempted = 1;
    write_ok = write_full(c->pi->hProcess, (LPVOID)(size_t)SITE, c->original, sizeof current);
    if (!write_ok) printf("%s: original site write FAILED error=%lu\n", why, GetLastError());
    if (!VirtualProtectEx(c->pi->hProcess, (LPVOID)(size_t)SITE, sizeof current,
                          desired_protect, &temp)) {
        printf("%s: protection restore FAILED error=%lu\n", why, GetLastError());
        c->fatal = 1;
    } else restore_ok = 1;
    if (write_ok) {
        flush_ok = FlushInstructionCache(c->pi->hProcess, (LPCVOID)(size_t)SITE, sizeof current);
        if (!flush_ok) printf("%s: FlushInstructionCache FAILED error=%lu\n", why, GetLastError());
        readback_ok = read_full(c->pi->hProcess, (LPCVOID)(size_t)SITE, current, sizeof current) &&
                      !memcmp(current, c->original, sizeof current);
        if (!readback_ok) printf("%s: original site readback FAILED error=%lu\n", why, GetLastError());
    }
    ok = protection_changed && write_attempted && write_ok && restore_ok && flush_ok && readback_ok;
    if (!ok) c->fatal = 1;
    printf("%s: write=%s protection=%s flush=%s readback=%s\n", why,
           write_ok ? "ok" : "FAILED", restore_ok ? "ok" : "FAILED",
           flush_ok ? "ok" : "FAILED", readback_ok ? "ok" : "FAILED");
    (void)s;
    return ok;
}

static int remove_hook(Capture *c, const char *why) {
    Suspended suspended;
    int resumed;
    if (!c->installed) return 1;
    if (!suspend_threads(c->pi->dwProcessId, &suspended)) {
        printf("%s: cannot suspend threads for hook removal\n", why);
        c->fatal = 1;
        return 0;
    }
    if (!restore_site_held(c, &suspended, why, 0)) c->fatal = 1;
    resumed = resume_threads(&suspended);
    if (!resumed) c->fatal = 1;
    if (!c->fatal) c->installed = 0;
    return !c->fatal;
}

static int install_hook(Capture *c) {
    BYTE site[5], check[5], stub[CAVE_SIZE], stub_check[CAVE_SIZE];
    BYTE hook[5];
    DWORD old_protect = 0, temp;
    SIZE_T written;
    Suspended suspended;
    int stub_len, protection_changed = 0, write_attempted = 0;
    int write_ok = 0, restore_ok = 0, flush_ok = 0, readback_ok = 0;

    if (!read_full(c->pi->hProcess, (LPCVOID)(size_t)SITE, site, sizeof site) || site[0] != 0xE9)
        return 0;
    memcpy(c->original, site, sizeof site);
    c->original_target = SITE + 5 + *(int *)(site + 1);
    if (c->original_target < TARGET_LO || c->original_target >= TARGET_HI) return 0;
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQueryEx(c->pi->hProcess, (LPCVOID)(size_t)c->original_target,
                            &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY)) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            printf("capture target is not executable committed memory\n");
            c->attempted = 1;
            return 0;
        }
    }
    c->cave = VirtualAllocEx(c->pi->hProcess, NULL, CAVE_SIZE,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!c->cave) {
        printf("capture cave allocation FAILED error=%lu\n", GetLastError());
        c->attempted = 1;
        return 0;
    }
    c->flag = (DWORD)(size_t)c->cave + FLAG_OFF;
    c->regs = (DWORD)(size_t)c->cave + REGS_OFF;
    memset(stub, 0x90, sizeof stub);
    stub_len = build_stub(stub + 0x10, (DWORD)(size_t)c->cave + 0x10,
                          c->regs, c->flag, c->original_target);
    if (!stub_len || !write_full(c->pi->hProcess, (LPVOID)((BYTE *)c->cave + 0x10),
                                 stub + 0x10, stub_len) ||
        !read_full(c->pi->hProcess, (LPCVOID)((BYTE *)c->cave + 0x10),
                    stub_check, stub_len) || memcmp(stub + 0x10, stub_check, stub_len) ||
        !FlushInstructionCache(c->pi->hProcess, c->cave, CAVE_SIZE)) {
        printf("capture cave write/readback FAILED error=%lu\n", GetLastError());
        c->attempted = 1;
        return 0;
    }
    hook[0] = 0xE9;
    *(DWORD *)(hook + 1) = (DWORD)(size_t)c->cave + 0x10 - (SITE + 5);
    memcpy(c->hook, hook, sizeof hook);

    if (!suspend_threads(c->pi->dwProcessId, &suspended)) {
        printf("capture hook skipped: thread suspension failed\n");
        c->attempted = 1;
        return 0;
    }
    if (!read_full(c->pi->hProcess, (LPCVOID)(size_t)SITE, site, sizeof site) ||
        memcmp(site, c->original, sizeof site)) {
        printf("capture hook skipped: site changed before write\n");
        if (!resume_threads(&suspended)) c->fatal = 1;
        c->attempted = 1;
        return 0;
    }
    if (!VirtualProtectEx(c->pi->hProcess, (LPVOID)(size_t)SITE, sizeof hook,
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("capture hook protection FAILED error=%lu\n", GetLastError());
        if (!resume_threads(&suspended)) c->fatal = 1;
        c->attempted = 1;
        return 0;
    }
    protection_changed = 1;
    c->site_old_protect = old_protect;
    c->site_protection_known = 1;
    write_attempted = 1;
    write_ok = write_full(c->pi->hProcess, (LPVOID)(size_t)SITE, hook, sizeof hook);
    if (!write_ok) printf("capture hook write FAILED error=%lu\n", GetLastError());
    if (!VirtualProtectEx(c->pi->hProcess, (LPVOID)(size_t)SITE, sizeof hook,
                          old_protect, &temp)) {
        printf("capture hook protection restore FAILED error=%lu\n", GetLastError());
    } else restore_ok = 1;
    if (write_ok) {
        flush_ok = FlushInstructionCache(c->pi->hProcess, (LPCVOID)(size_t)SITE, sizeof hook);
        readback_ok = read_full(c->pi->hProcess, (LPCVOID)(size_t)SITE, check, sizeof check) &&
                      !memcmp(check, hook, sizeof hook);
    }
    if (!(protection_changed && write_attempted && write_ok && restore_ok && flush_ok && readback_ok)) {
        printf("HIGH SEVERITY: capture hook transaction failed; rolling back while suspended\n");
        if (!restore_site_held(c, &suspended, "capture hook rollback", 1)) c->fatal = 1;
        if (!resume_threads(&suspended)) c->fatal = 1;
        c->attempted = 1;
        return 0;
    }
    c->attempted = 1;
    c->installed = 1;
    if (!resume_threads(&suspended)) {
        c->fatal = 1;
        return 0;
    }
    printf("capture hook installed site=%08lX target=%08lX cave=%08lX\n",
           SITE, c->original_target, (DWORD)(size_t)c->cave);
    return 1;
}

static int write_modules(HANDLE process, const char *path) {
    FILE *file = fopen(path, "wb");
    MODULEENTRY32 module;
    HANDLE snapshot;
    BOOL more;
    int ok = 1;
    if (!file) { printf("modules.tsv open FAILED path=%s error=%lu\n", path, GetLastError()); return 0; }
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(process));
    if (snapshot == INVALID_HANDLE_VALUE) { printf("modules snapshot FAILED error=%lu\n", GetLastError()); fclose(file); return 0; }
    module.dwSize = sizeof module;
    more = Module32First(snapshot, &module);
    if (!more) { printf("Module32First FAILED error=%lu\n", GetLastError()); ok = 0; }
    while (more) {
        if (fprintf(file, "%s\t%08lX\t%08lX\t%s\n", module.szModule,
                    (DWORD)(size_t)module.modBaseAddr, module.modBaseSize,
                    module.szExePath) < 0) { printf("modules.tsv write FAILED error=%lu\n", GetLastError()); ok = 0; break; }
        more = Module32Next(snapshot, &module);
        if (!more && GetLastError() != ERROR_NO_MORE_FILES) { printf("Module32Next FAILED error=%lu\n", GetLastError()); ok = 0; }
    }
    CloseHandle(snapshot);
    if (fclose(file) != 0) { printf("modules.tsv close FAILED\n"); ok = 0; }
    return ok;
}

static int dump_snap(Capture *c, const char *dir, DWORD *regs) {
    char partial_path[MAX_PATH], output_path[MAX_PATH];
    FILE *file;
    BYTE *buffer;
    DWORD count = 0, address = 0x10000;
    unsigned long long total = 0;
    DWORD start = GetTickCount();
    int partial = 0, valid = 1, enumeration_done = 0;
    MEMORY_BASIC_INFORMATION mbi;

    if (snprintf(partial_path, sizeof partial_path, "%s\\pre.partial.snap", dir) >= sizeof partial_path ||
        snprintf(output_path, sizeof output_path, "%s\\pre.snap", dir) >= sizeof output_path) {
        printf("snapshot path too long\n"); return 0;
    }
    file = fopen(partial_path, "wb");
    if (!file) { printf("snapshot open FAILED path=%s error=%lu\n", partial_path, GetLastError()); return 0; }
    {
        DWORD header[3] = {0x504E5342, 1, 0};
        if (fwrite(header, 4, 3, file) != 3 || fwrite(regs, 4, 10, file) != 10) valid = 0;
    }
    buffer = (BYTE *)malloc(0x100000);
    if (!buffer) valid = 0;
    while (valid && buffer && address < 0x7FFF0000u &&
           GetTickCount() - start < CAPTURE_MS && total < CAPTURE_BYTES) {
        DWORD base, size, protect, offset;
        int readable;
        if (!VirtualQueryEx(c->pi->hProcess, (LPCVOID)(size_t)address, &mbi, sizeof mbi)) {
            printf("snapshot VirtualQueryEx FAILED address=%08lX error=%lu\n", address, GetLastError());
            partial = 1;
            break;
        }
        base = (DWORD)(size_t)mbi.BaseAddress;
        size = (DWORD)mbi.RegionSize;
        protect = mbi.Protect;
        readable = mbi.State == MEM_COMMIT && protect && !(protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                   (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (readable) for (offset = 0; offset < size && total < CAPTURE_BYTES &&
                           GetTickCount() - start < CAPTURE_MS; offset += 0x100000) {
            DWORD want = size - offset < 0x100000 ? size - offset : 0x100000;
            SIZE_T got = 0;
            DWORD region_header[4];
            if (!ReadProcessMemory(c->pi->hProcess, (LPCVOID)(size_t)(base + offset), buffer, want, &got) || got != want) {
                partial = 1;
                printf("snapshot omitted range=%08lX+%08lX size=%08lX error=%lu\n", base, offset, want, GetLastError());
                continue;
            }
            region_header[0] = base + offset; region_header[1] = want;
            region_header[2] = protect; region_header[3] = mbi.Type;
            if (fwrite(region_header, 4, 4, file) != 4 || fwrite(buffer, 1, want, file) != want) {
                printf("snapshot write FAILED\n"); valid = 0; break;
            }
            count++; total += want;
        }
        if (base + size <= address) { printf("snapshot region address overflow\n"); partial = 1; break; }
        address = base + size;
    }
    if (valid && address >= 0x7FFF0000u) enumeration_done = 1;
    if (valid && !enumeration_done) {
        partial = 1;
        printf("snapshot stopped before normal enumeration end bytes=%llu elapsed=%lums\n", total, GetTickCount() - start);
    }
    if (valid && (total >= CAPTURE_BYTES || GetTickCount() - start >= CAPTURE_MS)) {
        partial = 1;
        printf("snapshot limits reached bytes=%llu elapsed=%lums\n", total, GetTickCount() - start);
    }
    if (valid && fseek(file, 8, SEEK_SET) != 0) { printf("snapshot header seek FAILED\n"); valid = 0; }
    if (valid && fwrite(&count, 4, 1, file) != 1) { printf("snapshot header write FAILED\n"); valid = 0; }
    if (buffer) free(buffer);
    if (fclose(file) != 0) { printf("snapshot close FAILED\n"); valid = 0; }
    if (!valid) return 0;
    printf("snapshot %s regions=%lu bytes=%llu%s\n", partial ? "partial" : "complete", count, total,
           partial ? " (pre.partial.snap retained)" : "");
    if (!partial && rename(partial_path, output_path) != 0) {
        printf("snapshot rename FAILED error=%lu\n", GetLastError());
        return 0;
    }
    return !partial;
}

static int capture_once(Capture *c, const char *dir) {
    Suspended suspended;
    DWORD flag, regs[10], release = 3;
    int dump_ok = 0, resumed, restored;
    char modules_path[MAX_PATH];

    if (c->capture_done) return 0;
    if (!read_full(c->pi->hProcess, (LPCVOID)(size_t)c->flag, &flag, sizeof flag)) {
        printf("capture flag read FAILED error=%lu\n", GetLastError());
        c->fatal = 1;
        c->capture_done = 1;
        remove_hook(c, "capture flag error cleanup");
        return 0;
    }
    if (flag == 4) {
        printf("capture owner expired before capture; skipping snapshot\n");
        c->capture_done = 1;
        remove_hook(c, "capture expired cleanup");
        return 0;
    }
    if (flag != 2) return 0;
    if (!suspend_threads(c->pi->dwProcessId, &suspended)) {
        printf("capture skipped: thread suspension failed\n");
        c->fatal = 1;
        c->capture_done = 1;
        remove_hook(c, "capture suspension error cleanup");
        return 0;
    }
    if (!read_full(c->pi->hProcess, (LPCVOID)(size_t)c->flag, &flag, sizeof flag) || flag != 2 ||
        !read_full(c->pi->hProcess, (LPCVOID)(size_t)c->regs, regs, sizeof regs)) {
        printf("capture owner no longer READY\n");
        c->fatal = 1;
    } else if (snprintf(modules_path, sizeof modules_path, "%s\\modules.tsv", dir) >= sizeof modules_path ||
               !write_modules(c->pi->hProcess, modules_path)) {
        printf("capture metadata failed; no full snapshot claim\n");
        c->fatal = 1;
    } else dump_ok = dump_snap(c, dir, regs);
    restored = restore_site_held(c, &suspended, "capture site restore", 0);
    if (!restored) c->fatal = 1;
    if (read_full(c->pi->hProcess, (LPCVOID)(size_t)c->flag, &flag, sizeof flag) && flag == 2) {
        if (!write_full(c->pi->hProcess, (LPVOID)(size_t)c->flag, &release, sizeof release)) {
            printf("HIGH SEVERITY: capture release failed error=%lu\n", GetLastError()); c->fatal = 1;
        }
    } else if (flag != 4) {
        printf("HIGH SEVERITY: capture release flag read failed\n"); c->fatal = 1;
    }
    resumed = resume_threads(&suspended);
    if (!resumed) c->fatal = 1;
    c->capture_done = 1;
    if (restored) c->installed = 0;
    else remove_hook(c, "capture restore retry");
    if (!dump_ok) printf("capture produced partial/failed output; no full snapshot claim\n");
    return dump_ok;
}

/* The historical Python IAT map ends at C0F7FC; these later thunks were left as stale ASLR addresses. */
static int repair_import_tail(const char *path) {
    static const char *nmco[] = {"NMCORegisterCallbackMessage", "NMCOInitWithMultiServer",
        "NMCOGetMyScreenName", "NMCOGetMyPosition", "NMCOLogin", "NMGSGetMemberList",
        "NMGSGetInviteCandidateList", "NMCSGetChatMessage", "NMGSInviteUser",
        "NMCOGetFriendList", "NMCOGetIdCodeFromIdx"};
    static const char *ole[] = {"CoInitialize", "OleUninitialize", "OleInitialize",
        "CoUninitialize", "CoCreateInstance", "CoCreateGuid"};
    const char **names[] = {nmco, ole};
    const char *dlls[] = {"nmco3.dll", "ole32.dll"};
    DWORD counts[] = {11, 6}, slots[] = {0xC0F800, 0xC0F830};
    BYTE *data, blob[4096] = {0};
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *sections, *cave = NULL;
    IMAGE_IMPORT_DESCRIPTOR *old = NULL, *desc = (IMAGE_IMPORT_DESCRIPTOR *)blob;
    FILE *file = fopen(path, "rb");
    long size;
    DWORD i, j, ndesc = 0, pos, raw, rva, alignment;
    int ok;
    if (!file) { fprintf(stderr, "Cannot open %s\n", path); return 1; }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 4096 || size > 64*1024*1024 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    data = malloc((size_t)size);
    if (!data) { fclose(file); return 1; }
    ok = fread(data, 1, size, file) == (size_t)size;
    fclose(file);
    if (!ok) { free(data); return 1; }
    dos = (IMAGE_DOS_HEADER *)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (DWORD)dos->e_lfanew > (DWORD)size - sizeof *nt) { free(data); return 1; }
    nt = (IMAGE_NT_HEADERS32 *)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.ImageBase != 0x400000 || nt->OptionalHeader.AddressOfEntryPoint != 0x5E5EFE ||
        nt->FileHeader.NumberOfSections > 32) { free(data); return 1; }
    sections = IMAGE_FIRST_SECTION(nt);
    if ((BYTE *)(sections + nt->FileHeader.NumberOfSections) > data + size) { free(data); return 1; }
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER *s = sections + i;
        if (!memcmp(s->Name, ".SCY\0\0\0\0", 8)) cave = s;
        if (rva >= s->VirtualAddress && rva - s->VirtualAddress < s->SizeOfRawData &&
            s->PointerToRawData + (unsigned long long)s->SizeOfRawData <= (DWORD)size)
            old = (IMAGE_IMPORT_DESCRIPTOR *)(data + s->PointerToRawData + rva - s->VirtualAddress);
    }
    if (!cave || !old || cave->SizeOfRawData || cave->Misc.VirtualSize < sizeof blob) {
        fprintf(stderr, "Expected unused .SCY and rebuilt imports; refusing write\n"); free(data); return 1;
    }
    while ((BYTE *)(old + ndesc + 1) <= data + size && ndesc < 64 && old[ndesc].Name) ndesc++;
    if (!ndesc || ndesc == 64 || (BYTE *)(old + ndesc + 1) > data + size) { free(data); return 1; }
    memcpy(blob, old, ndesc * sizeof *old);
    rva = cave->VirtualAddress;
    pos = (ndesc + 3) * sizeof *old;
    for (i = 0; i < 2; i++) {
        DWORD intpos = pos;
        pos += (counts[i] + 1) * 4;
        desc[ndesc+i].OriginalFirstThunk = rva + intpos;
        desc[ndesc+i].FirstThunk = slots[i] - 0x400000;
        desc[ndesc+i].Name = rva + pos;
        memcpy(blob + pos, dlls[i], strlen(dlls[i])+1); pos += strlen(dlls[i])+1;
        for (j = 0; j < counts[i]; j++) {
            pos = (pos + 1) & ~1u;
            if (pos + strlen(names[i][j]) + 3 > sizeof blob) { free(data); return 1; }
            *(DWORD *)(blob + intpos + 4*j) = rva + pos;
            memcpy(blob + pos + 2, names[i][j], strlen(names[i][j])+1);
            pos += strlen(names[i][j])+3;
        }
    }
    alignment = nt->OptionalHeader.FileAlignment;
    if (!alignment || (alignment & (alignment-1)) || alignment > 4096) { free(data); return 1; }
    raw = ((DWORD)size + alignment-1) & ~(alignment-1);
    cave->PointerToRawData = raw;
    cave->SizeOfRawData = sizeof blob;
    cave->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = rva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = (ndesc+3)*sizeof *old;
    file = fopen(path, "r+b");
    if (!file) { free(data); return 1; }
    /* Append first: a failed append must not publish headers pointing to unwritten import data. */
    ok = !fseek(file, raw, SEEK_SET) && fwrite(blob, 1, sizeof blob, file) == sizeof blob && !fflush(file);
    if (ok) ok = !fseek(file, 0, SEEK_SET) && fwrite(data, 1, nt->OptionalHeader.SizeOfHeaders, file) == nt->OptionalHeader.SizeOfHeaders;
    if (fclose(file)) ok = 0;
    free(data);
    printf("import tail repair %s: 11 nmco3 + 6 ole32 imports\n", ok ? "written" : "FAILED");
    return !ok;
}

int main(int argc, char **argv) {
    char logpath[MAX_PATH], cwd[MAX_PATH], capture_root[MAX_PATH], capture_dir[MAX_PATH];
    DWORD module_len, cwd_len, t0, exit_code = STILL_ACTIVE, final_code = STILL_ACTIVE;
    const char *ip, *dump_path;
    char cmd[2048] = "ca.exe";
    int i, np = 0, left, patch_fatal = 0, exit_check_failed = 0, child_suspended = 1;
    int capture_enabled = getenv("BOOM_CAPTURE") != NULL;
    Patch p[64];
    static char ipbuf[16][32];
    int write_error_logged[64] = {0};
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    Capture capture = {0};
    SYSTEMTIME now;

    if (argc == 3 && !strcmp(argv[1], "--repair-import-tail")) return repair_import_tail(argv[2]);

    module_len = GetModuleFileNameA(NULL, logpath, sizeof logpath);
    if (!module_len || module_len >= sizeof logpath) { fprintf(stderr, "Cannot derive launcher log path\n"); return 1; }
    {
        char *last_sep = strrchr(logpath, '\\');
        if (!last_sep) last_sep = strrchr(logpath, '/');
        if (!last_sep || strlen("boomlaunch.log") >= sizeof logpath - (size_t)(last_sep - logpath + 1)) {
            fprintf(stderr, "Cannot derive launcher log path\n"); return 1;
        }
        strcpy(last_sep + 1, "boomlaunch.log");
    }
    if (!freopen(logpath, "a", stdout)) { fprintf(stderr, "Cannot open launcher log %s\n", logpath); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    cwd_len = GetCurrentDirectoryA(sizeof cwd, cwd);
    if (!cwd_len || cwd_len >= sizeof cwd) { fprintf(stderr, "Cannot determine current directory\n"); return 1; }
    GetLocalTime(&now);
    ip = argc > 1 ? argv[1] : "127.0.0.1";
    for (i = 2; i < argc; i++) { if (strlen(cmd) + strlen(argv[i]) + 2 >= sizeof cmd) { printf("command line too long\n"); return 1; } strcat(cmd, " "); strcat(cmd, argv[i]); }

    p[np++] = (Patch){0x8E701D, "\x74\x29", "\xEB\x29", 2, 0};          /* skip HackShield/BlackCipher */
    p[np++] = (Patch){0x8E56ED, "\x57\xE8\x7B\xEB\xFF\xFF\x85\xC0\x74\x21", "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90", 10, 0}; /* skip PatchServer_CheckVersion */
    /* Wine's AMMultiMediaStream initialization calls an unimplemented winedmo export; use its existing failure path. */
    p[np++] = (Patch){0x480332, "\xE8\x4A\xCC\xFF\xFF", "\x58\x83\xC8\xFF\x90", 5, 0}; /* consume the stdcall argument, return failure */
    /* patch for wine and auto update */
    p[np++] = (Patch){0x69E636, "\x0F\x85\x4C\x01\x00\x00", "\xE9\x4D\x01\x00\x00\x90", 6, 0};
    p[np++] = (Patch){0x84E4ED, "\xE8\x7A\x32\xBB\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x852E4F, "\xE8\x18\xE9\xBA\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x855828, "\xE8\x3F\xBF\xBA\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8E29A3, "\xE8\xC4\xED\xB1\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8E31D8, "\xE8\x8F\xE5\xB1\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8E3899, "\xE8\xCE\xDE\xB1\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8E4E57, "\xE8\x10\xC9\xB1\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8F3108, "\xE8\x5F\xE6\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x8F3207, "\xE8\x60\xE5\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x900D53, "\xE8\x14\x0A\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x900D62, "\xE8\x05\x0A\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x900D71, "\xE8\xF6\x09\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    p[np++] = (Patch){0x900D81, "\xE8\xE6\x09\xB0\xFF", "\xB0\x01\x90\x90\x90", 5, 0};
    /* experimental: force suspected D3D9 Windowed flag to 1; black-screen cause unconfirmed */
    if (getenv("BOOM_EXPERIMENT_WINDOWED")) p[np++] = (Patch){0xA56791, "\x33\xC0\x3B\xCB\x0F\x94\xC0", "\xB8\x01\x00\x00\x00\x90\x90", 7, 0};
    for (i = 0; i < 10; i++) { int length = (int)strlen(IPS[i][1]); memset(ipbuf[i], 0, sizeof ipbuf[i]); strncpy(ipbuf[i], ip, length); p[np++] = (Patch){(DWORD)(size_t)IPS[i][0], IPS[i][1], ipbuf[i], length + 1, 0}; }

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) { printf("CreateProcess failed: %lu\n", GetLastError()); return 1; }
    if (capture_enabled && (snprintf(capture_root, sizeof capture_root, "S:\\Projects\\boom-re\\logs\\nondebug-capture-20261025-022318") >= sizeof capture_root ||
        snprintf(capture_dir, sizeof capture_dir, "%s\\run-%04u%02u%02u-%02u%02u%02u-%lu", capture_root, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, pi.dwProcessId) >= sizeof capture_dir ||
        !CreateDirectoryA(capture_dir, NULL))) {
        printf("capture run directory creation FAILED path=%s error=%lu\n", capture_dir, GetLastError()); patch_fatal = 1;
    }
    if (!capture_enabled) { strcpy(capture_dir, "disabled"); capture.attempted = 1; }
    printf("run %04u-%02u-%02u %02u:%02u:%02u command=ca.exe ip=%s cwd=%s capture_path=%s mapping=S:\\->/mnt/Games\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, ip, cwd, capture_dir);
    printf("created ca.exe pid=%lu primaryWindowsTID=%lu launch-to-exit ordinary process\n", pi.dwProcessId, pi.dwThreadId);
    capture.pi = &pi;
    t0 = GetTickCount();
    left = np;
    while (!patch_fatal && (left || !capture.attempted || (capture.installed && !capture.capture_done)) && GetTickCount() - t0 < 30000) {
        if (capture_enabled && !capture.installed && !capture.attempted && install_hook(&capture)) printf("capture armed while ordinary patches remain=%d\n", left);
        if (capture.fatal) { patch_fatal = 1; break; }
        for (i = 0; i < np; i++) {
            char current[64];
            SIZE_T read_bytes = 0, written = 0;
            DWORD old_protect, temp, write_error = 0, restore_error = 0, flush_error = 0;
            BOOL protect_ok, write_ok, restore_ok, flush_ok;
            if (p[i].done) continue;
            if (!ReadProcessMemory(pi.hProcess, (LPCVOID)(size_t)p[i].va, current, p[i].len, &read_bytes) || read_bytes != (SIZE_T)p[i].len) continue;
            if (!memcmp(current, p[i].new_, p[i].len)) { printf("patch %08lX already patched\n", p[i].va); p[i].done = 1; left--; continue; }
            if (memcmp(current, p[i].old, p[i].len)) continue;
            protect_ok = VirtualProtectEx(pi.hProcess, (LPVOID)(size_t)p[i].va, p[i].len, PAGE_EXECUTE_READWRITE, &old_protect);
            if (!protect_ok) { if (!write_error_logged[i]) { printf("patch %08lX VirtualProtectEx failed error=%lu at %lums\n", p[i].va, GetLastError(), GetTickCount() - t0); write_error_logged[i] = 1; } continue; }
            write_ok = WriteProcessMemory(pi.hProcess, (LPVOID)(size_t)p[i].va, p[i].new_, p[i].len, &written) && written == (SIZE_T)p[i].len;
            if (!write_ok) write_error = GetLastError();
            restore_ok = VirtualProtectEx(pi.hProcess, (LPVOID)(size_t)p[i].va, p[i].len, old_protect, &temp);
            if (!restore_ok) restore_error = GetLastError();
            flush_ok = write_ok && written == (SIZE_T)p[i].len ? FlushInstructionCache(pi.hProcess, (LPCVOID)(size_t)p[i].va, p[i].len) : FALSE;
            if (!flush_ok && write_ok) flush_error = GetLastError();
            if (!write_ok && !write_error_logged[i]) { printf("patch %08lX FAILED write_error=%lu expected=%lu written=%lu restore=%s restore_error=%lu at %lums\n", p[i].va, write_error, (unsigned long)p[i].len, (unsigned long)written, restore_ok ? "ok" : "FAILED", restore_error, GetTickCount() - t0); write_error_logged[i] = 1; }
            if (!restore_ok) { printf("patch %08lX VirtualProtectEx restore failed error=%lu at %lums\n", p[i].va, restore_error, GetTickCount() - t0); patch_fatal = 1; }
            else if (!flush_ok && write_ok) { printf("patch %08lX FlushInstructionCache failed error=%lu at %lums\n", p[i].va, flush_error, GetTickCount() - t0); patch_fatal = 1; }
            else if (write_ok) { printf("patch %08lX ok at %lums\n", p[i].va, GetTickCount() - t0); p[i].done = 1; left--; }
            if (patch_fatal) break;
        }
        if (child_suspended) {
            if (ResumeThread(pi.hThread) == (DWORD)-1) { printf("Initial ResumeThread failed: %lu\n", GetLastError()); patch_fatal = 1; }
            else child_suspended = 0;
        }
        if (capture.installed && !capture.capture_done) capture_once(&capture, capture_dir);
        if (capture.fatal) { patch_fatal = 1; break; }
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) { printf("GetExitCodeProcess failed: %lu\n", GetLastError()); exit_check_failed = 1; break; }
        if (exit_code != STILL_ACTIVE) break;
        Sleep(0);
    }
    printf("patch summary: applied %d/%d", np - left, np);
    if (!left) printf(" complete\n"); else if (exit_check_failed) printf("; exit status check failed\n"); else if (patch_fatal) printf("; fatal patch/capture failure\n"); else printf("; timeout\n");
    if (left) { printf("patches not applied:"); for (i = 0; i < np; i++) if (!p[i].done) printf(" %08lX", p[i].va); printf("\n"); }
    if (capture.installed && (!capture.capture_done || capture.fatal)) {
        capture.capture_done = 1;
        remove_hook(&capture, "capture arm deadline/fatal cleanup");
    }
    dump_path = getenv("BOOM_DUMP");
    if (!left && dump_path && *dump_path) {
        unsigned char bytes[0x100]; SIZE_T read_bytes = 0; BOOL ok = ReadProcessMemory(pi.hProcess, (LPCVOID)(size_t)0xA56711, bytes, sizeof bytes, &read_bytes) && read_bytes == sizeof bytes; FILE *file = ok ? fopen(dump_path, "wb") : NULL;
        if (!file) ok = FALSE; else { if (fwrite(bytes, 1, sizeof bytes, file) != sizeof bytes) ok = FALSE; if (fclose(file) != 0) ok = FALSE; }
        printf("dump A56711 %s bytes=%lu path=%s\n", ok ? "ok" : "FAILED", (unsigned long)read_bytes, dump_path);
    }
    if (child_suspended) TerminateProcess(pi.hProcess, 1);
    while (GetExitCodeProcess(pi.hProcess, &final_code) && final_code == STILL_ACTIVE) Sleep(100);
    if (final_code == STILL_ACTIVE) printf("final raw process code unavailable; last status STILL_ACTIVE\n"); else printf("final raw process code=%lu\n", final_code);
    if (capture.cave) printf("capture cave retained for process lifetime at %08lX\n", (DWORD)(size_t)capture.cave);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return patch_fatal || capture.fatal || exit_check_failed ? 1 : 0;
}
