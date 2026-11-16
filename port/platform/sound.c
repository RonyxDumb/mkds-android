// The ARM7 side of the NitroSDK sound library: receives the command lists the game's SND library sends through the
// PXI FIFO (tag 7), carries them out and reports them finished in the shared work area.
//
// First stage: commands are acknowledged so the game runs; the sequence player and the mixer come next.
#include "port.h"

#include "types.h"
#include "sound_command_queue.h"
#include "sound_shared_work_access.h"

static SNDSharedWork *s_shared;

static void run_command(const SNDCommand *cmd) {
    switch (cmd->mId) {
    case SOUND_COMMAND_SET_SHARED_WORK:
        s_shared = (SNDSharedWork *) (uintptr_t) cmd->mArg[0];
        break;
    default:
        break;
    }
}

void port_snd_pxi_word(u32 data) {
    if (data == 0) {
        return; // "process the commands now": they already are
    }
    for (const SNDCommand *cmd = (const SNDCommand *) (uintptr_t) data; cmd; cmd = cmd->mNext) {
        run_command(cmd);
    }
    if (s_shared) {
        s_shared->mFinishCommandTag++;
    }
}
