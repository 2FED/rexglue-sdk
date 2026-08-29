/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

uint32_t LoadForceFeedbackWord(uint8_t* base, uint32_t guest_address, size_t word_index) {
  return rex::memory::load_and_swap<uint32_t>(base + guest_address + word_index * sizeof(uint32_t));
}

rex::input::ForceFeedbackEffect ReadForceFeedbackEffect(uint8_t* base, uint32_t guest_address) {
  std::array<uint32_t, rex::input::kForceFeedbackEffectWordCount> words = {};
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = LoadForceFeedbackWord(base, guest_address, i);
  }
  return rex::input::DecodeForceFeedbackEffectWords(words);
}

u32 HidReadKeys_entry(u32 unk1, u32 unk2, u32 unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}

u32 HidGetCapabilities_entry(u32 unk1, u32 caps_ptr) {
  // HidGetCapabilities - ordinal 0x01EA
  // Returns capabilities for HID device (keyboard/mouse)
  // Not supported in rexglue - return unsuccessful
  return X_STATUS_UNSUCCESSFUL;
}

u32 HidGetLastInputTime_entry(mapped_u32 time_ptr) {
  // HidGetLastInputTime - ordinal 0x01F1
  // Returns the last time any HID input was received
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

u32 HidReadMouseChanges_entry(u32 unk1, u32 unk2) {
  // HidReadMouseChanges - ordinal 0x0273
  // Reads mouse input changes - not supported in rexglue
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
REX_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
REX_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
REX_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)

// XInputd stubs
REX_EXPORT_STUB(__imp__XInputdGetCapabilities);
REX_EXPORT_STUB(__imp__XInputdReadState);
REX_EXPORT_STUB(__imp__XInputdWriteState);
REX_EXPORT_STUB(__imp__XInputdNotify);
REX_EXPORT_STUB(__imp__XInputdRawState);
REX_EXPORT_STUB(__imp__XInputdGetDeviceStats);
REX_EXPORT_STUB(__imp__XInputdResetDevice);
REX_EXPORT_STUB(__imp__XInputdSetRingOfLight);
REX_EXPORT_STUB(__imp__XInputdSetRFPowerMode);
REX_EXPORT_STUB(__imp__XInputdSetRadioFrequency);
REX_EXPORT_STUB(__imp__XInputdPassThroughRFCommand);
REX_EXPORT_STUB(__imp__XInputdPowerDownDevice);
REX_EXPORT_STUB(__imp__XInputdReadTextKeystroke);
REX_EXPORT_STUB(__imp__XInputdSendStayAliveRequest);
REX_HOOK_RAW(__imp__XInputdFFGetDeviceInfo) {
  if (ctx.r4.u32) {
    const uint32_t slots =
        rex::kernel::xboxkrnl::input_system()->GetForceFeedbackMaxEffectSlots(ctx.r3.u32);
    rex::memory::store_and_swap<uint32_t>(base + ctx.r4.u32, slots);
  }
  ctx.r3.u64 = 0;
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFGetDeviceInfo(
    "__imp__XInputdFFGetDeviceInfo", &__imp__XInputdFFGetDeviceInfo);

REX_HOOK_RAW(__imp__XInputdFFSetEffect) {
  if (!ctx.r4.u32) {
    ctx.r3.u64 = 0xA0;
  } else {
    const auto effect = rex::kernel::xboxkrnl::ReadForceFeedbackEffect(base, ctx.r4.u32);
    ctx.r3.u64 =
        rex::kernel::xboxkrnl::input_system()->SetForceFeedbackEffect(ctx.r3.u32, effect, false);
  }
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFSetEffect(
    "__imp__XInputdFFSetEffect", &__imp__XInputdFFSetEffect);

REX_HOOK_RAW(__imp__XInputdFFUpdateEffect) {
  if (!ctx.r4.u32) {
    ctx.r3.u64 = 0xA0;
  } else {
    const auto effect = rex::kernel::xboxkrnl::ReadForceFeedbackEffect(base, ctx.r4.u32);
    ctx.r3.u64 =
        rex::kernel::xboxkrnl::input_system()->SetForceFeedbackEffect(ctx.r3.u32, effect, true);
  }
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFUpdateEffect(
    "__imp__XInputdFFUpdateEffect", &__imp__XInputdFFUpdateEffect);

REX_HOOK_RAW(__imp__XInputdFFEffectOperation) {
  if (!ctx.r4.u32 || (ctx.r6.u32 != 1 && ctx.r6.u32 != 2)) {
    ctx.r3.u64 = 0xA0;
  } else {
    // MCLA passes an array of {effect_index, iterations} entries. r5 is the
    // entry count, r6 is the start/stop operation, and r7 carries flags.
    const auto operation = rex::input::DecodeForceFeedbackOperationWords(
        rex::kernel::xboxkrnl::LoadForceFeedbackWord(base, ctx.r4.u32, 0),
        ctx.r6.u32 == 1 ? rex::kernel::xboxkrnl::LoadForceFeedbackWord(base, ctx.r4.u32, 1) : 1);
    ctx.r3.u64 = rex::kernel::xboxkrnl::input_system()->SetForceFeedbackEffectRunning(
        ctx.r3.u32, operation.effect_index, ctx.r6.u32 == 1, operation.iterations);
  }
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFEffectOperation(
    "__imp__XInputdFFEffectOperation", &__imp__XInputdFFEffectOperation);
REX_HOOK_RAW(__imp__XInputdFFDeviceControl) {
  ctx.r3.u64 =
      rex::kernel::xboxkrnl::input_system()->ForceFeedbackDeviceControl(ctx.r3.u32, ctx.r4.u32);
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFDeviceControl(
    "__imp__XInputdFFDeviceControl", &__imp__XInputdFFDeviceControl);
REX_HOOK_RAW(__imp__XInputdFFSetDeviceGain) {
  static std::atomic_bool logged = false;
  if (!logged.exchange(true)) {
    REXLOG_INFO(
        "XINPUTD_FF_AUDIT v=1 export=SetDeviceGain action=compat-success-noop "
        "r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  ctx.r3.u64 = 0;
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFSetDeviceGain(
    "__imp__XInputdFFSetDeviceGain", &__imp__XInputdFFSetDeviceGain);

REX_HOOK_RAW(__imp__XInputdFFCancelIo) {
  static std::atomic_bool logged = false;
  if (!logged.exchange(true)) {
    REXLOG_INFO(
        "XINPUTD_FF_AUDIT v=1 export=CancelIo action=compat-success-noop "
        "r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  ctx.r3.u64 = 0;
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFCancelIo(
    "__imp__XInputdFFCancelIo", &__imp__XInputdFFCancelIo);

REX_HOOK_RAW(__imp__XInputdFFSetRumble) {
  static std::atomic_bool logged = false;
  if (!logged.exchange(true)) {
    REXLOG_INFO(
        "XINPUTD_FF_AUDIT v=1 export=SetRumble action=compat-success-noop "
        "r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  ctx.r3.u64 = 0;
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XInputdFFSetRumble(
    "__imp__XInputdFFSetRumble", &__imp__XInputdFFSetRumble);
REX_EXPORT_STUB(__imp__XInputdGetLastTextInputTime);
REX_EXPORT_STUB(__imp__XInputdSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XInputdSetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdGetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdControl);
REX_EXPORT_STUB(__imp__XInputdSetWifiChannel);
REX_EXPORT_STUB(__imp__XInputdGetDevicePid);
REX_EXPORT_STUB(__imp__XInputdGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XInputdSetFailedConnectionOrBindCallback);
REX_EXPORT_STUB(__imp__XInputdSetMinMaxAuthDelay);

// Drv stubs
REX_EXPORT_STUB(__imp__DrvSetSysReqCallback);
REX_EXPORT_STUB(__imp__DrvSetUserBindingCallback);
REX_EXPORT_STUB(__imp__DrvSetContentStorageCallback);
REX_EXPORT_STUB(__imp__DrvSetAutobind);
REX_EXPORT_STUB(__imp__DrvGetContentStorageNotification);
REX_EXPORT_STUB(__imp__DrvXenonButtonPressed);
REX_EXPORT_STUB(__imp__DrvBindToUser);
REX_EXPORT_STUB(__imp__DrvSetDeviceConfigChangeCallback);
REX_EXPORT_STUB(__imp__DrvDeviceConfigChange);
REX_EXPORT_STUB(__imp__DrvSetMicArrayStartCallback);
REX_EXPORT_STUB(__imp__DrvSetAudioLatencyCallback);
