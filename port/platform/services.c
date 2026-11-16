// The services the ARM7 provides to the game on the DS (touch screen, clock, power management, microphone), and the
// wireless hardware, which this port does not have.
#include "port.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "types.h"
#include "registers.h"
#include "touch_panel_sampling.h"
#include "clock_chip_access.h"
#include "power_management_commands.h"
#include "microphone_sampling.h"
#include "arm_fifo_messaging.h"
#include "startup_entry.h"
#include "build_info_layout.h"

// ------------------------------------------------------------------------------------------------ touch panel
// Raw samples carry screen coordinates directly; the calibration is the identity.
u16 gTouchPanelInitialized;
TouchPanelState gTouchPanelState;

static void sample(TPData *out) {
    PortInput in;
    port_input_get(&in);
    out->mTouch = in.touching ? 1 : 0;
    out->mValidity = 0;
    out->mX = in.touching ? in.touch_x : 0;
    out->mY = in.touching ? in.touch_y : 0;
}

void TP_Init(void) { gTouchPanelInitialized = 1; }
BOOL TP_GetUserInfo(TPCalibrateParam *calibrate) {
    memset(calibrate, 0, sizeof(*calibrate));
    return 1;
}
void TP_SetCalibrateParam(TPCalibrateParam *param) { gTouchPanelState.mCalibrated = param != NULL; }
void TP_RequestSamplingAsync(void) { sample(&gTouchPanelState.mRawSample); }
u32 TP_WaitRawResult(TPData *result) {
    *result = gTouchPanelState.mRawSample;
    return 0;
}
void TP_GetCalibratedPoint(TPData *disp, const TPData *raw) {
    *disp = *raw;
    if (!raw->mTouch) {
        disp->mX = disp->mY = 0;
    }
    if (disp->mX > TOUCH_SCREEN_MAX_X) {
        disp->mX = TOUCH_SCREEN_MAX_X;
    }
    if (disp->mY > TOUCH_SCREEN_MAX_Y) {
        disp->mY = TOUCH_SCREEN_MAX_Y;
    }
}
void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequence, TPData *samplingBufs, u16 bufSize) {
    (void) vcount;
    gTouchPanelState.mFrequence = frequence;
    gTouchPanelState.mAutoSampleBuffer = samplingBufs;
    gTouchPanelState.mAutoSampleBufferCount = bufSize;
    gTouchPanelState.mAutoSampleIndex = 0;
    gTouchPanelState.mState = TP_STATE_AUTO_SAMPLING;
}
void TP_RequestAutoSamplingStopAsync(void) { gTouchPanelState.mState = TP_STATE_READY; }
u16 TP_GetLatestIndexInAuto(void) { return gTouchPanelState.mAutoSampleIndex; }
u32 TP_CheckError(u32 commandFlgs) { (void) commandFlgs; return 0; }
void TP_WaitBusy(u32 commandFlgs) { (void) commandFlgs; }

// Called every frame: automatic sampling fills the game's ring once per frame.
void port_tp_vblank(void) {
    TouchPanelState *tp = &gTouchPanelState;
    if (tp->mState == TP_STATE_AUTO_SAMPLING && tp->mAutoSampleBuffer && tp->mAutoSampleBufferCount) {
        tp->mAutoSampleIndex = (u16) ((tp->mAutoSampleIndex + 1) % tp->mAutoSampleBufferCount);
        sample(&tp->mAutoSampleBuffer[tp->mAutoSampleIndex]);
    }
}

// ------------------------------------------------------------------------------------------------ real-time clock
void RTC_Init(void) {}
RTCResult RTC_GetDate(RTCDate *date) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    date->mYear = (u32) (tm.tm_year % 100);
    date->mMonth = (u32) tm.tm_mon + 1;
    date->mDay = (u32) tm.tm_mday;
    date->mWeek = (RTCWeek) tm.tm_wday;
    return 0;
}
RTCResult RTC_GetTime(RTCTime *t) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    t->mHour = (u32) tm.tm_hour;
    t->mMinute = (u32) tm.tm_min;
    t->mSecond = (u32) tm.tm_sec;
    return 0;
}

// ------------------------------------------------------------------------------------------------ power management
static u32 s_lcd_power = 1;
void PM_Init(void) {}
BOOL PM_SetLCDPower(u32 sw) {
    s_lcd_power = sw;
    return 1;
}
int PM_GetLCDPower(void) { return (int) s_lcd_power; }
u32 PM_SetBackLight(u32 target, u32 sw) { (void) target, (void) sw; return 0; }
u32 PM_GetBackLight(u32 *top, u32 *bottom) {
    if (top) *top = 1;
    if (bottom) *bottom = 1;
    return 0;
}
u32 PM_SetAmp(u32 status) { (void) status; return 0; }
u32 PM_SetAmpGain(u32 gain) { (void) gain; return 0; }
u32 PM_ForceToPowerOff(void) {
    port_log("PM_ForceToPowerOff");
    g_quit = true;
    return 0;
}
u32 PM_GetLEDPattern(u32 *patternBuf) {
    if (patternBuf) *patternBuf = 0;
    return 0;
}
u32 PMi_SendLEDPatternCommand(u32 pattern) { (void) pattern; return 0; }
// Closing the lid: the game sleeps; nothing to do on a phone (the activity pauses instead).
void PM_GoSleepMode(u32 trigger, u32 logic, u32 keyPattern) { (void) trigger, (void) logic, (void) keyPattern; }
void PM_AppendPreSleepCallback(PMSleepCallbackInfo *info) { (void) info; }
void PM_PrependPreSleepCallback(PMSleepCallbackInfo *info) { (void) info; }
void PM_AppendPostSleepCallback(PMSleepCallbackInfo *info) { (void) info; }
void PM_DeletePreSleepCallback(PMSleepCallbackInfo *info) { (void) info; }
void PM_DeletePostSleepCallback(PMSleepCallbackInfo *info) { (void) info; }

// ------------------------------------------------------------------------------------------------ microphone
// Silence (blowing into the microphone is not supported yet).
static const MICAutoParam *s_mic;
void MIC_Init(void) { *(u32 *) 0x027fff90 = 0; }
MICResult MIC_StartAutoSampling(const MICAutoParam *param) {
    s_mic = param;
    if (param && param->mBuffer) {
        u8 silence = (param->mType == MIC_SAMPLING_TYPE_8BIT || param->mType == MIC_SAMPLING_TYPE_12BIT ||
                      param->mType == MIC_SAMPLING_TYPE_12BIT_FILTER_OFF) ? 0x80 : 0x00;
        memset(param->mBuffer, silence, param->mSize);
        *(u32 *) 0x027fff90 = (u32) (uintptr_t) param->mBuffer;
    }
    return MIC_RESULT_SUCCESS;
}
MICResult MIC_StopAutoSampling(void) {
    s_mic = NULL;
    return MIC_RESULT_SUCCESS;
}
void *MIC_GetLastSamplingAddress(void) { return (void *) (uintptr_t) *(u32 *) 0x027fff90; }

// ------------------------------------------------------------------------------------------------ PXI
// The services above are called directly; the FIFO only carries what the sound library sends (port/platform/sound.c).
PXIFifoCallback FifoRecvCallbackTable[32];
void PXI_SetFifoRecvCallback(s32 fifotag, PXIFifoCallback callback) { FifoRecvCallbackTable[fifotag & 31] = callback; }
s32 PXI_IsCallbackReady(s32 fifotag, s32 proc) { (void) fifotag, (void) proc; return 1; }
void port_snd_pxi_word(u32 data); // sound.c
s32 PXI_SendWordByFifo(s32 fifotag, u32 data, s32 err) {
    (void) err;
    if (fifotag == 7) { // sound
        port_snd_pxi_word(data);
        return 0;
    }
    port_log("pxi: tag %d word %08x ignored", (int) fifotag, data);
    return 0;
}

// ------------------------------------------------------------------------------------------------ runtime helpers
// What the game calls in CodeWarrior's runtime and C library by name.
u32 Abs(u32 value) { return (s32) value < 0 ? -value : value; }
void AssertionFailed(const char *expression, const char *fileName, const char *functionName, int lineNumber) {
    port_log("assertion failed: %s (%s, %s:%d)", expression, functionName, fileName, lineNumber);
}
int CompareStringsMapped(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char) *a), cb = tolower((unsigned char) *b);
        if (ca != cb || !ca) {
            return ca - cb;
        }
    }
}
long StrtolBase10(const char *string) { return strtol(string, NULL, 10); }
u64 ShiftRightLogical64(u64 operand, u32 bitCount) { return bitCount >= 64 ? 0 : operand >> bitCount; }
u64 _ll_mul(u64 left, u64 right) { return left * right; }
u64 _ll_udiv(u64 dividend, u64 divisor) { return divisor ? dividend / divisor : 0; }
u64 _s32_div_f(u32 numerator, u32 denominator) {
    s32 n = (s32) numerator, d = (s32) denominator;
    if (d == 0) {
        return 0;
    }
    return ((u64) (u32) (n % d) << 32) | (u32) (n / d);
}

#define DIGITS_8(c) [c] = 8
const u16 gCharClassFlags[128] = {DIGITS_8('0'), DIGITS_8('1'), DIGITS_8('2'), DIGITS_8('3'), DIGITS_8('4'),
                                   DIGITS_8('5'), DIGITS_8('6'), DIGITS_8('7'), DIGITS_8('8'), DIGITS_8('9')};
const u8 gLowerCaseMap[128] = {
#define L(i) [i] = (u8) ((i) >= 'A' && (i) <= 'Z' ? (i) + 32 : (i))
#define L8(i) L(i), L(i + 1), L(i + 2), L(i + 3), L(i + 4), L(i + 5), L(i + 6), L(i + 7)
    L8(0), L8(8), L8(16), L8(24), L8(32), L8(40), L8(48), L8(56),
    L8(64), L8(72), L8(80), L8(88), L8(96), L8(104), L8(112), L8(120)};
const u8 gUpperCaseMap[128] = {
#define U(i) [i] = (u8) ((i) >= 'a' && (i) <= 'z' ? (i) - 32 : (i))
#define U8(i) U(i), U(i + 1), U(i + 2), U(i + 3), U(i + 4), U(i + 5), U(i + 6), U(i + 7)
    U8(0), U8(8), U8(16), U8(24), U8(32), U8(40), U8(48), U8(56),
    U8(64), U8(72), U8(80), U8(88), U8(96), U8(104), U8(112), U8(120)};
int gLibErrorCode;
struct MWiDestructorChain *gGlobalDestructorChain;

BuildInfoLayout BuildInfo;
const void *OSi_ReferSymbol(const void *symbol) { return symbol; }
