// Game card: ROM reads come from the ROM image in memory, the backup chip (256 KiB flash for this game) is a file.
#include "port.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "types.h"
#include "card_enable_and_lock.h"
#include "card_removal_handling.h"
#include "card_rom_backup_access.h"
#include "reset_interrupts_halt.h"

void port_os_poll(void);

const uint8_t *g_rom;
uint32_t g_rom_size;

bool port_rom_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        port_log("rom: cannot open %s", path);
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    void *p = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED || st.st_size < 0x4000) {
        port_log("rom: cannot map %s", path);
        return false;
    }
    g_rom = p;
    g_rom_size = (uint32_t) st.st_size;
    if (memcmp(g_rom + 0x0c, "AMCP", 4) != 0) {
        port_log("rom: game code %.4s, expected AMCP (Mario Kart DS, Europe)", (const char *) g_rom + 0x0c);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ ROM
void CARD_Init(void) {
    // The firmware leaves the ROM header in main memory for the game.
    memcpy((void *) 0x027ffe00, g_rom, 0x160);
    memcpy(&SYSTEM_WORK->mRomHeader, g_rom, sizeof(SYSTEM_WORK->mRomHeader));
    memcpy(HW_CARD_ROM_HEADER, g_rom, 0x160);
}

u32 CARD_IsEnabled(void) { return 1; }
void CARD_CheckEnabled(void) {}
void CARD_Enable(u32 enable) { (void) enable; }
void CARD_LockRom(u16 lockId) { (void) lockId; }
void CARD_UnlockRom(u16 lockId) { (void) lockId; }
int CARD_WaitRomAsync(void) { return 1; }
u32 CARD_IsPulledOut(void) { return 0; }
void CARD_SetPulledOutCallback(CARDPulledOutCallback callback) { (void) callback; }
void CARD_InitPulledOutCallback(void) {}
void CARD_TerminateForPulledOut(void) {}

typedef struct DeferredCall {
    CARDCallback fn;
    void *arg;
} DeferredCall;

static void run_callback(void *p) {
    DeferredCall *call = p;
    CARDCallback fn = call->fn;
    void *arg = call->arg;
    free(call);
    fn(arg);
}

static void complete(CARDCallback callback, void *arg, u32 isAsync) {
    if (!callback) {
        return;
    }
    if (isAsync) {
        DeferredCall *call = malloc(sizeof(*call));
        call->fn = callback;
        call->arg = arg;
        port_os_defer(run_callback, call);
    } else {
        callback(arg);
    }
}

void CARDi_ReadRom(u32 dma, u32 src, u32 dst, u32 len, CARDCallback callback, void *callbackArg, u32 isAsync) {
    (void) dma;
    if ((uint64_t) src + len > g_rom_size) {
        port_log("card: ROM read %08x+%x past the end", src, len);
        len = src < g_rom_size ? g_rom_size - src : 0;
    }
    memcpy((void *) (uintptr_t) dst, g_rom + src, len);
    complete(callback, callbackArg, isAsync);
    port_os_poll(); // file loading is where the DS takes its VBlank interrupts
}

// ------------------------------------------------------------------------------------------------ backup
#define SAVE_SIZE 0x40000u
static uint8_t s_save[SAVE_SIZE];
static bool s_save_loaded, s_save_dirty;
static char s_save_path[512];
static u32 s_result;

void port_save_set_path(const char *path) {
    snprintf(s_save_path, sizeof(s_save_path), "%s", path);
    memset(s_save, 0xff, SAVE_SIZE); // erased flash
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s_save, 1, SAVE_SIZE, f);
        fclose(f);
        port_log("save: loaded %zu bytes from %s", n, path);
    }
    s_save_loaded = true;
}

void port_save_flush(void) {
    if (!s_save_dirty || !s_save_path[0]) {
        return;
    }
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        port_log("save: cannot write %s", tmp);
        return;
    }
    fwrite(s_save, 1, SAVE_SIZE, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, s_save_path);
    s_save_dirty = false;
}

void CARD_LockBackup(u16 lockId) { (void) lockId; }
void CARD_UnlockBackup(u16 lockId) { (void) lockId; }
u32 CARD_GetResultCode(void) { return s_result; }
int CARD_IdentifyBackup(s32 type) {
    if (!s_save_loaded) {
        memset(s_save, 0xff, SAVE_SIZE);
        s_save_loaded = true;
    }
    port_log("card: backup type %04x", (unsigned) type);
    s_result = BACKUP_RESULT_OK;
    return 1;
}
int CARD_WaitBackupAsync(void) { return s_result == BACKUP_RESULT_OK; }
int CARD_TryWaitBackupAsync(void) { return 1; }

// Request types of the backup stream commands.
enum {
    CARD_REQ_READ_BACKUP = 6,
    CARD_REQ_WRITE_BACKUP = 7,
    CARD_REQ_PROGRAM_BACKUP = 8,
    CARD_REQ_VERIFY_BACKUP = 9,
    CARD_REQ_ERASE_PAGE_BACKUP = 10,
    CARD_REQ_ERASE_SECTOR_BACKUP = 11,
};

int CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, CARDCallback callback, void *callbackArg, u32 isAsync,
                               u32 reqType, u32 reqRetry, u32 reqMode) {
    (void) reqRetry, (void) reqMode;
    s_result = BACKUP_RESULT_OK;
    switch (reqType) {
    case CARD_REQ_READ_BACKUP:
        if ((uint64_t) src + len > SAVE_SIZE) {
            s_result = BACKUP_RESULT_BAD_ARGUMENT;
            break;
        }
        memcpy((void *) (uintptr_t) dst, s_save + src, len);
        break;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:
        if ((uint64_t) dst + len > SAVE_SIZE) {
            s_result = BACKUP_RESULT_BAD_ARGUMENT;
            break;
        }
        memcpy(s_save + dst, (const void *) (uintptr_t) src, len);
        s_save_dirty = true;
        break;
    case CARD_REQ_VERIFY_BACKUP:
        if ((uint64_t) dst + len > SAVE_SIZE || memcmp(s_save + dst, (const void *) (uintptr_t) src, len) != 0) {
            s_result = BACKUP_RESULT_FAILED;
        }
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
    case CARD_REQ_ERASE_SECTOR_BACKUP:
        if ((uint64_t) dst + len <= SAVE_SIZE) {
            memset(s_save + dst, 0xff, len);
            s_save_dirty = true;
        }
        break;
    default:
        port_log("card: backup request %u not supported", reqType);
        s_result = BACKUP_RESULT_NOT_SUPPORTED;
        break;
    }
    complete(callback, callbackArg, isAsync);
    return s_result == BACKUP_RESULT_OK;
}
