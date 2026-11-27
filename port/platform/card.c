// Game card: ROM reads come from the ROM image in memory, the backup chip (256 KiB flash for this game) is a file.
#include "port.h"

#include <stdlib.h>
#include <string.h>

#include "hostapi.h"

#include "types.h"
#include "card_enable_and_lock.h"
#include "card_removal_handling.h"
#include "card_rom_backup_access.h"
#include "reset_interrupts_halt.h"

void port_os_poll(void);

static const uint8_t *g_rom;
static uint32_t g_rom_size;

// ------------------------------------------------------------------------------------------------ ROM
void CARD_Init(void) {
    g_rom = hapi_rom(&g_rom_size);
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

// Asynchronous completions: the callback runs later, at an interrupt point, as the card thread does on the DS. On
// the DS, CARD_WaitBackupAsync returns only once the callback has run (and any request it starts in turn has
// finished): the game's save code reads the result its callbacks leave (error code, decrypted data) right after the
// wait, and starts the next request then. A wait that returned before the callback left that request without its
// lock (TryLockBackup took a new lock ID each time and the earlier one was never given back): after a few dozen
// saves no lock ID was left and every later write was skipped (progress lost, issues #11 and #19).
typedef struct DeferredCall {
    CARDCallback fn;
    void *arg;
    bool backup;
} DeferredCall;

#define CARD_QUEUE 16
static DeferredCall s_queue[CARD_QUEUE];
static int s_queue_head, s_queue_count, s_backup_pending;
static bool s_drain_scheduled;

static void drain(void) {
    while (s_queue_count) {
        DeferredCall call = s_queue[s_queue_head];
        s_queue_head = (s_queue_head + 1) % CARD_QUEUE;
        s_queue_count--;
        if (call.backup) s_backup_pending--;
        call.fn(call.arg); // may queue the next request of a chain
    }
}

static void run_callbacks(void *p) {
    (void) p;
    s_drain_scheduled = false;
    drain();
}

static void complete(CARDCallback callback, void *arg, u32 isAsync, bool backup) {
    if (!callback) {
        return;
    }
    if (isAsync) {
        if (s_queue_count == CARD_QUEUE) drain();
        s_queue[(s_queue_head + s_queue_count) % CARD_QUEUE] = (DeferredCall) {callback, arg, backup};
        s_queue_count++;
        if (backup) s_backup_pending++;
        if (!s_drain_scheduled) {
            s_drain_scheduled = true;
            port_os_defer(run_callbacks, NULL);
        }
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
    complete(callback, callbackArg, isAsync, false);
    port_os_poll(); // file loading is where the DS takes its VBlank interrupts
}

// ------------------------------------------------------------------------------------------------ backup
#define SAVE_SIZE 0x40000u
static uint8_t s_save[SAVE_SIZE];
static bool s_save_loaded, s_save_dirty;
static u32 s_result;

static void save_load(void) {
    if (s_save_loaded) return;
    memset(s_save, 0xff, SAVE_SIZE); // erased flash
    hapi_save_read(s_save, SAVE_SIZE);
    s_save_loaded = true;
}

void port_save_flush(void) {
    if (!s_save_dirty) return;
    // Cleared first: a write the game makes while the file is being written marks it again for the next flush.
    s_save_dirty = false;
    if (!hapi_save_write(s_save, SAVE_SIZE)) s_save_dirty = true;
}

void CARD_LockBackup(u16 lockId) { (void) lockId; }
void CARD_UnlockBackup(u16 lockId) { (void) lockId; }
u32 CARD_GetResultCode(void) { return s_result; }
int CARD_IdentifyBackup(s32 type) {
    save_load();
    port_log("card: backup type %04x", (unsigned) type);
    s_result = BACKUP_RESULT_OK;
    return 1;
}
int CARD_WaitBackupAsync(void) {
    drain();
    return s_result == BACKUP_RESULT_OK;
}
int CARD_TryWaitBackupAsync(void) { return s_backup_pending == 0; }

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
    complete(callback, callbackArg, isAsync, true);
    return s_result == BACKUP_RESULT_OK;
}
