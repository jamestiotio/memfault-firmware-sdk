//! @file
//!
//! Copyright (c) Memfault, Inc.
//! See License.txt for details
//!
//! @brief
//! A RAM-backed implementation used for tracking state across system reboots. More details about
//! how to use the API can be found in reboot_tracking.h
//! Assumptions:
//!  - RAM state survives across resets (this is generally true as long as power is stable)
//!    If power is lost, nothing will fail but the reboot will not be recorded
//!  - The memory which needs to persist in RAM must _not_ be initialized by any of the firmwares
//!    upon reboot & the memory must be placed in the same region for the firmwares running on the
//!    system (i.e bootloader & main image).

#include "memfault/core/reboot_tracking.h"
#include "memfault_reboot_tracking_private.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "memfault/core/compiler.h"
#include "memfault/core/errors.h"

#define MEMFAULT_REBOOT_INFO_MAGIC 0x21544252

#define MEMFAULT_REBOOT_INFO_VERSION 2

#define MEMFAULT_REBOOT_REASON_NOT_SET 0xffffffff

typedef MEMFAULT_PACKED_STRUCT MfltRebootInfo {
  //! A cheap way to check if the data within the struct is valid
  uint32_t magic;
  //! Version of the struct. If a new field is added it should be appended right before rsvd. This
  //! way we can remain backwards compatible but know what fields are valid.
  uint8_t version;
  //! The number of times the system has reset due to an error
  //! without any crash data being read out via the Memfault packetizer
  uint8_t crash_count;
  uint8_t rsvd1[1];
  uint8_t coredump_saved;
  uint32_t last_reboot_reason; // eMemfaultRebootReason or MEMFAULT_REBOOT_REASON_NOT_SET
  uint32_t pc;
  uint32_t lr;
  //! Most MCUs have a register which reveals why a device rebooted.
  //!
  //! This can be particularly useful for debugging reasons for unexpected reboots
  //! (where no coredump was saved or no user initiated reset took place). Examples
  //! of this include brown out resets (BORs) & hardware watchdog resets.
  uint32_t reset_reason_reg0;
  // Reserved for future additions
  uint32_t rsvd2[10];
} sMfltRebootInfo;

MEMFAULT_STATIC_ASSERT(sizeof(sMfltRebootInfo) == MEMFAULT_REBOOT_TRACKING_REGION_SIZE,
                       "struct doesn't match expected size");

static sMfltRebootInfo *s_mflt_reboot_info;

//! Struct to retrieve reboot reason data from. Matches the fields of sMfltRebootReason
//! as documented in reboot_tracking.h
typedef struct {
  eMemfaultRebootReason reboot_reg_reason;
  eMemfaultRebootReason prior_stored_reason;
  bool is_valid;
} sMfltRebootReasonData;

// Private struct to store reboot reason after reboot tracking is initialized
static sMfltRebootReasonData s_reboot_reason_data = {
  .is_valid = false,
};

static bool prv_check_or_init_struct(void) {
  if (s_mflt_reboot_info == NULL) {
    return false;
  }

  if (s_mflt_reboot_info->magic == MEMFAULT_REBOOT_INFO_MAGIC) {
    return true;
  }

  // structure doesn't match what we expect, reset it
  *s_mflt_reboot_info = (sMfltRebootInfo) {
    .magic = MEMFAULT_REBOOT_INFO_MAGIC,
    .version = MEMFAULT_REBOOT_INFO_VERSION,
    .last_reboot_reason = MEMFAULT_REBOOT_REASON_NOT_SET,
  };
  return true;
}

static bool prv_read_reset_info(sMfltResetReasonInfo *info) {
  if ((s_mflt_reboot_info->last_reboot_reason == MEMFAULT_REBOOT_REASON_NOT_SET) &&
      (s_mflt_reboot_info->reset_reason_reg0 == 0)) {
    return false; // no reset crashes!
  }

  *info = (sMfltResetReasonInfo) {
    .reason = (eMemfaultRebootReason)s_mflt_reboot_info->last_reboot_reason,
    .pc = s_mflt_reboot_info->pc,
    .lr = s_mflt_reboot_info->lr,
    .reset_reason_reg0 = s_mflt_reboot_info->reset_reason_reg0,
    .coredump_saved = s_mflt_reboot_info->coredump_saved == 1,
  };

  return true;
}

//! Records reboot reasons from reboot register and prior saved reboot
//!
//! Stores both the new reboot reason derived from a platform's reboot register and
//! any previously saved reboot reason. If there is no previously stored reboot reason,
//! the reboot register reason is used.
//!
//! @param reboot_reg_reason New reboot reason from this boot
//! @param prior_stored_reason Prior reboot reason stored in s_mflt_reboot_info
static void prv_record_reboot_reason(eMemfaultRebootReason reboot_reg_reason,
                                     uint32_t prior_stored_reason) {
  s_reboot_reason_data.reboot_reg_reason = reboot_reg_reason;

  if (prior_stored_reason != (eMemfaultRebootReason)MEMFAULT_REBOOT_REASON_NOT_SET) {
    s_reboot_reason_data.prior_stored_reason = (eMemfaultRebootReason)prior_stored_reason;
  } else {
    s_reboot_reason_data.prior_stored_reason = reboot_reg_reason;
  }

  s_reboot_reason_data.is_valid = true;
}

static bool prv_get_unexpected_reboot_occurred(void) {
  // Check prior_stored_reason, reboot is unexpected if prior reason is set and in error range or
  // unknown
  if (s_reboot_reason_data.prior_stored_reason !=
      (eMemfaultRebootReason)MEMFAULT_REBOOT_REASON_NOT_SET) {
    if (s_reboot_reason_data.prior_stored_reason == kMfltRebootReason_Unknown ||
        s_reboot_reason_data.prior_stored_reason >= kMfltRebootReason_UnknownError) {
      return true;
    }
  }

  // Check reboot_reg_reason second, reboot is unexpected if in error range or unknown
  return (s_reboot_reason_data.reboot_reg_reason == kMfltRebootReason_Unknown ||
          s_reboot_reason_data.reboot_reg_reason >= kMfltRebootReason_UnknownError);
}

static void prv_record_reboot_event(eMemfaultRebootReason reboot_reason,
                                    const sMfltRebootTrackingRegInfo *reg) {
  // Store both the new reason reported by hardware and the current recorded reason
  // The combination of these will be used to determine if the bootup was expected
  // by the metrics subsystem
  // s_mflt_reboot_info can be cleared by any call to memfault_reboot_tracking_collect_reset_info
  prv_record_reboot_reason(reboot_reason, s_mflt_reboot_info->last_reboot_reason);

  if (s_mflt_reboot_info->last_reboot_reason != MEMFAULT_REBOOT_REASON_NOT_SET) {
    // we are already tracking a reboot. We don't overwrite this because generally the first reboot
    // in a loop reveals what started the crash loop
    return;
  }
  s_mflt_reboot_info->last_reboot_reason = reboot_reason;
  if (reg == NULL) { // we don't have any extra metadata
    return;
  }

  s_mflt_reboot_info->pc = reg->pc;
  s_mflt_reboot_info->lr = reg->lr;
}

void memfault_reboot_tracking_boot(
    void *start_addr, const sResetBootupInfo *bootup_info) {
  s_mflt_reboot_info = start_addr;

  if (start_addr == NULL) {
    return;
  }

  if (!prv_check_or_init_struct()) {
    return;
  }

  eMemfaultRebootReason reset_reason = kMfltRebootReason_Unknown;
  if (bootup_info != NULL) {
    s_mflt_reboot_info->reset_reason_reg0 = bootup_info->reset_reason_reg;
    reset_reason = bootup_info->reset_reason;
  }

  prv_record_reboot_event(reset_reason, NULL);

  if (prv_get_unexpected_reboot_occurred()) {
    s_mflt_reboot_info->crash_count++;
  }
}

void memfault_reboot_tracking_mark_reset_imminent(eMemfaultRebootReason reboot_reason,
                                                  const sMfltRebootTrackingRegInfo *reg) {
  if (!prv_check_or_init_struct()) {
    return;
  }

  prv_record_reboot_event(reboot_reason, reg);
}

bool memfault_reboot_tracking_read_reset_info(sMfltResetReasonInfo *info) {
  if (info == NULL) {
    return false;
  }

  if (!prv_check_or_init_struct()) {
    return false;
  }

  return prv_read_reset_info(info);
}

void memfault_reboot_tracking_reset_crash_count(void) {
  if (!prv_check_or_init_struct()) {
    return;
  }

  s_mflt_reboot_info->crash_count = 0;
}

size_t memfault_reboot_tracking_get_crash_count(void) {
  if (!prv_check_or_init_struct()) {
    return 0;
  }

  return s_mflt_reboot_info->crash_count;
}

void memfault_reboot_tracking_clear_reset_info(void) {
  if (!prv_check_or_init_struct()) {
    return;
  }

  s_mflt_reboot_info->last_reboot_reason = MEMFAULT_REBOOT_REASON_NOT_SET;
  s_mflt_reboot_info->coredump_saved = 0;
  s_mflt_reboot_info->pc = 0;
  s_mflt_reboot_info->lr = 0;
  s_mflt_reboot_info->reset_reason_reg0 = 0;
}

void memfault_reboot_tracking_mark_coredump_saved(void) {
  if (!prv_check_or_init_struct()) {
    return;
  }

  s_mflt_reboot_info->coredump_saved = 1;
}

int memfault_reboot_tracking_get_reboot_reason(sMfltRebootReason *reboot_reason) {
  if (reboot_reason == NULL || !s_reboot_reason_data.is_valid) {
    return -1;
  }

  *reboot_reason = (sMfltRebootReason){
    .reboot_reg_reason = s_reboot_reason_data.reboot_reg_reason,
    .prior_stored_reason = s_reboot_reason_data.prior_stored_reason,
  };

  return 0;
}

int memfault_reboot_tracking_get_unexpected_reboot_occurred(bool *unexpected_reboot_occurred) {
  if (unexpected_reboot_occurred == NULL || !s_reboot_reason_data.is_valid) {
    return -1;
  }

  *unexpected_reboot_occurred = prv_get_unexpected_reboot_occurred();
  return 0;
}

void memfault_reboot_tracking_clear_reboot_reason(void) {
  s_reboot_reason_data = (sMfltRebootReasonData){
    .is_valid = false,
  };
}
