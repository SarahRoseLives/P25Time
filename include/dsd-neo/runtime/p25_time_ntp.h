// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int valid;
    int system_time_not_locked;
    int microslot_rollover_unlocked;
    uint8_t leap_second_correction;
    int local_time_offset_valid;
    int16_t local_time_offset_minutes;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint16_t microslots;
    uint64_t unix_ns;
    uint64_t monotonic_ns;
} dsd_p25_sync_broadcast;

typedef struct {
    uint8_t leap_indicator;
    uint8_t version;
    uint8_t mode;
    uint8_t stratum;
    int8_t precision;
    char refid[5];
    uint64_t reference_unix_ns;
    uint64_t receive_unix_ns;
    uint64_t transmit_unix_ns;
} dsd_ntp_query_result;

int dsd_p25_sync_broadcast_decode(const uint8_t tsbk_bytes[12], dsd_p25_sync_broadcast* out);
uint64_t dsd_p25_sync_broadcast_current_unix_ns(const dsd_p25_sync_broadcast* broadcast, uint64_t now_monotonic_ns);

int dsd_p25_time_ntp_start(const dsd_opts* opts);
void dsd_p25_time_ntp_stop(void);
void dsd_p25_time_ntp_publish(const dsd_p25_sync_broadcast* broadcast);
int dsd_p25_time_ntp_query(const char* hostname, int port, dsd_ntp_query_result* out);

#ifdef __cplusplus
}
#endif
