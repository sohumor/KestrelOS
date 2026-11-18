#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* Talon typed clipboard parcels: per-UID 8-slot MRU ring.
 * Slot 0 is always the most recently pushed parcel for that UID.
 * Buffers passed to these entry points live in kernel space; syscall.c
 * performs all copy_from_user / copy_to_user validation. */

long parcel_put(const struct k_parcel_put *req, const void *kdata);
long parcel_get(uint32_t slot_or_id, struct k_parcel_info *info_out,
                void *buf_out, uint32_t bufsz);
long parcel_list(uint32_t index, struct k_parcel_info *info_out);

/* Render all active UID parcel rings as a @krec stream (buf may be NULL). */
uint32_t parcel_snapshot_krec(char *buf, uint32_t cap);
