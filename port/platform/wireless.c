// Wireless hardware (local multiplayer, DS Download Play, Nintendo WFC) does not exist in this port: every
// request fails the way the SDK reports an unusable wireless module, and the game shows its error screens.
// Generated from the decomp's prototypes (see git history); keep the signatures in sync with the headers.
#include "port.h"
#include "types.h"
#include "multiboot_checksum.h"
#include "multiboot_communication.h"
#include "multiboot_data_pieces.h"
#include "multiboot_file_register.h"
#include "multiboot_game_entry.h"
#include "multiboot_game_info_beacon.h"
#include "multiboot_parent_desc.h"
#include "multiboot_parent_state_machine.h"
#include "multiboot_scan_lock.h"
#include "multiboot_segment_read.h"
#include "multiboot_task_thread.h"
#include "wireless_beacon_wep_settings.h"
#include "wireless_driver_startup.h"
#include "wireless_key_data_sharing.h"
#include "wireless_link_control.h"
#include "wireless_system_commands.h"

#define WM_ERRCODE_WM_DISABLE 4
void *MB_CommGetChildUser(u32 child) { (void) child; return NULL;}
u32 MB_CommIsBootable(u32 child) { (void) child; return 0;}
u32 MB_CommResponseRequest(u32 child, u32 pairIndex) { (void) child; (void) pairIndex; return 0;}
void MB_CommSetParentStateCallback(MBParentStateCallback parentStateCallback) { (void) parentStateCallback; }
void MB_CountGameInfoLifetime(EntryCallbackFunc callback, u32 value) { (void) callback; (void) value; }
void MB_DisconnectChild(u32 childIndex) { (void) childIndex; }
void MB_EndToIdle(void) { }
u32 MB_FakeGetParentBssDesc(u32 index, WMBssDesc *destination) { (void) index; (void) destination; return 0;}
u32 MB_FakeReadParentBssDesc(u32 entryIndex, WMBssDesc *scanBlock, u16 parentMaxSize, u16 childMaxSize, u32 ksFlag, u32 flagTwo) { (void) entryIndex; (void) scanBlock; (void) parentMaxSize; (void) childMaxSize; (void) ksFlag; (void) flagTwo; return 0;}
MBGameInfoRecvList *MB_GetGameInfoRecvList() { return NULL;}
u32 MB_GetSegmentLength(FSFile *file) { (void) file; return 0;}
s32 MB_Init(void *buffer, MBUserInfo *user, u32 ggid, u32 tgid, u32 dmaNo) { (void) buffer; (void) user; (void) ggid; (void) tgid; (void) dmaNo; return WM_ERRCODE_WM_DISABLE;}
void MB_InitRecvGameInfoStatus(void) { }
void MB_ReadMultiBootParentBssDesc(WMBssDesc *bssDesc, u16 parentMaxSize, u16 childMaxSize, u32 ksFlag, u32 flagTwo) { (void) bssDesc; (void) parentMaxSize; (void) childMaxSize; (void) ksFlag; (void) flagTwo; }
int MB_ReadSegment(FSFile *file, void *buffer, u32 bufferSize) { (void) file; (void) buffer; (void) bufferSize; return WM_ERRCODE_WM_DISABLE;}
u32 MB_RecvGameInfoBeacon(EntryCallbackFunc callback, u32 value, WMBssDesc *scanBlock) { (void) callback; (void) value; (void) scanBlock; return 0;}
u32 MB_RegisterFile(void *rangeTable, SegmentBuffer *source) { (void) rangeTable; (void) source; return 0;}
u32 MB_SetParentCommParam(u32 sendDataSize, u32 maxChildren) { (void) sendDataSize; (void) maxChildren; return 0;}
s32 MB_StartParentFromIdle(u16 channel) { (void) channel; return WM_ERRCODE_WM_DISABLE;}
void MBi_CommChangeParentStateCallbackOnly(u32 child, u32 value, void *data) { (void) child; (void) value; (void) data; }
u8 *MBi_MakeChildSendBuffer(ChildMessage *message, u8 *buffer) { (void) message; (void) buffer; return NULL;}
u8 MBi_SendRequestDataPiece(void *output, void *pieces) { (void) output; (void) pieces; return 0;}
void MBi_SetBeaconRecvStatusBuffer(void *entryTable) { (void) entryTable; }
void MBi_SetChildMPMaxSize(s32 packetSize) { (void) packetSize; }
u8 *MBi_SetRecvBufferFromParent(ParentMessage *message, u8 *buffer) { (void) message; (void) buffer; return NULL;}
void MBi_SetScanLockFunc(void (*setCallback)(const u8 *), void (*clearCallback)(void)) { }
s32 WM_Disable(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_Disconnect(WirelessMessageCallback callback, u32 aid) { (void) callback; (void) aid; return WM_ERRCODE_WM_DISABLE;}
s32 WM_DisconnectChildren(WirelessMessageCallback callback, u32 aidBitmap) { (void) callback; (void) aidBitmap; return WM_ERRCODE_WM_DISABLE;}
s32 WM_Enable(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_End(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_EndDCF(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
int WM_EndDataSharing(WMDataSharingInfo *dsInfo) { (void) dsInfo; return WM_ERRCODE_WM_DISABLE;}
int WM_EndKeySharing(WMDataSharingInfo *buf) { (void) buf; return WM_ERRCODE_WM_DISABLE;}
s32 WM_EndMP(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_EndParent(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_EndScan(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_Finish(void) { return WM_ERRCODE_WM_DISABLE;}
u16 WM_GetAllowedChannel(void) { return 0;}
u16 WM_GetDispersionBeaconPeriod(void) { return 0;}
u16 WM_GetDispersionScanPeriod(void) { return 0;}
u16 WM_GetLinkLevel(void) { return 0;}
u16 WM_GetNextTgid(void) { return 0;}
WMOtherElements WM_GetOtherElements(const u16 *bssDesc) { (void) bssDesc; return 0;}
u8 *WM_GetSharedDataAddress(WMDataSharingInfo *dsInfo, WMDataSet *receiveData, u32 aid) { (void) dsInfo; (void) receiveData; (void) aid; return NULL;}
s32 WM_Init(void *wmSysBuf, u16 dmaNo) { (void) wmSysBuf; (void) dmaNo; return WM_ERRCODE_WM_DISABLE;}
s32 WM_Initialize(void *wmSysBuf, WirelessMessageCallback callback, u16 dmaNo) { (void) wmSysBuf; (void) callback; (void) dmaNo; return WM_ERRCODE_WM_DISABLE;}
int WM_MeasureChannel(WirelessMessageCallback callback, u16 ccaMode, u16 edThreshold, u16 channel, u16 measureTime) { (void) callback; (void) ccaMode; (void) edThreshold; (void) channel; (void) measureTime; return WM_ERRCODE_WM_DISABLE;}
s32 WM_PowerOff(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_PowerOn(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_ReadStatus(void *statusBuf) { (void) statusBuf; return WM_ERRCODE_WM_DISABLE;}
s32 WM_Reset(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
int WM_SetBeaconIndication(WirelessMessageCallback callback, u16 flag) { (void) callback; (void) flag; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetDCFData(WirelessMessageCallback callback, const void *destAdr, void *sendData, u16 sendDataSize) { (void) callback; (void) destAdr; (void) sendData; (void) sendDataSize; return WM_ERRCODE_WM_DISABLE;}
int WM_SetEntry(WirelessMessageCallback callback, BOOL enabled) { (void) callback; (void) enabled; return WM_ERRCODE_WM_DISABLE;}
int WM_SetGameInfo(WirelessMessageCallback callback, const void *userGameInfo, u16 userGameInfoSize, u32 ggid, u16 tgid, u8 attr) { (void) callback; (void) userGameInfo; (void) userGameInfoSize; (void) ggid; (void) tgid; (void) attr; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetIndCallback(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
int WM_SetLifeTime(WirelessMessageCallback callback, u32 tableNumber, u32 camLifeTime, u32 frameLifeTime, u16 mpLifeTime) { (void) callback; (void) tableNumber; (void) camLifeTime; (void) frameLifeTime; (void) mpLifeTime; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetMPDataToPortEx(WirelessSendCallback callback, void *arg, void *sendData, s32 sendDataSize, u16 destBitmap, u16 port, u16 prio) { (void) callback; (void) arg; (void) sendData; (void) sendDataSize; (void) destBitmap; (void) port; (void) prio; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetMPFrequency(WirelessMessageCallback callback, u32 mpFreq) { (void) callback; (void) mpFreq; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetParentParameter(WirelessMessageCallback callback, ParentParameter *pparaBuf) { (void) callback; (void) pparaBuf; return WM_ERRCODE_WM_DISABLE;}
s32 WM_SetPortCallback(u16 port, WirelessPortCallback callback, void *arg) { (void) port; (void) callback; (void) arg; return WM_ERRCODE_WM_DISABLE;}
int WM_SetWEPKey(WirelessMessageCallback callback, u32 wepmode, void *wepkey) { (void) callback; (void) wepmode; (void) wepkey; return WM_ERRCODE_WM_DISABLE;}
int WM_SetWEPKeyEx(WirelessMessageCallback callback, u16 wepmode, u16 wepkeyid, void *wepkey) { (void) callback; (void) wepmode; (void) wepkeyid; (void) wepkey; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartConnectEx(WirelessMessageCallback callback, WMBssDesc *pInfo, void *ssid, BOOL powerSave, u16 authMode) { (void) callback; (void) pInfo; (void) ssid; (void) powerSave; (void) authMode; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartDCF(WirelessMessageCallback callback, void *recvBuf, u16 recvBufSize) { (void) callback; (void) recvBuf; (void) recvBufSize; return WM_ERRCODE_WM_DISABLE;}
int WM_StartDataSharing(WMDataSharingInfo *dsInfo, u16 port, u16 aidBitmap, u16 dataLength, BOOL doubleMode) { (void) dsInfo; (void) port; (void) aidBitmap; (void) dataLength; (void) doubleMode; return WM_ERRCODE_WM_DISABLE;}
int WM_StartKeySharing(WMDataSharingInfo *buf, u16 port) { (void) buf; (void) port; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartMPEx(WirelessMessageCallback callback, void *recvBuf, u16 recvBufSize, void *sendBuf, u16 sendBufSize, u16 mpFreq, u16 defaultRetryCount, BOOL minPollBmpMode, BOOL singlePacketMode, BOOL fixFreqMode, BOOL ignoreFatalError) { (void) callback; (void) recvBuf; (void) recvBufSize; (void) sendBuf; (void) sendBufSize; (void) mpFreq; (void) defaultRetryCount; (void) minPollBmpMode; (void) singlePacketMode; (void) fixFreqMode; (void) ignoreFatalError; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartParent(WirelessMessageCallback callback) { (void) callback; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartScan(WirelessMessageCallback callback, ScanParam *param) { (void) callback; (void) param; return WM_ERRCODE_WM_DISABLE;}
s32 WM_StartScanEx(WirelessMessageCallback callback, ScanExParam *param) { (void) callback; (void) param; return WM_ERRCODE_WM_DISABLE;}
int WM_StepDataSharing(WMDataSharingInfo *dsInfo, void *sendData, WMDataSet *receiveData) { (void) dsInfo; (void) sendData; (void) receiveData; return WM_ERRCODE_WM_DISABLE;}
WMStatus *WMi_GetStatusAddress(void) { return NULL;}
s32 WVR_StartUpAsync(u32 vram, void (*callback)(u32, s32), u32 arg) { (void) vram; (void) arg; if (callback) callback(arg, 0); return 1;}
s32 WVR_TerminateAsync(void (*callback)(u32, s32), u32 arg) { (void) arg; if (callback) callback(arg, 0); return 1;}
