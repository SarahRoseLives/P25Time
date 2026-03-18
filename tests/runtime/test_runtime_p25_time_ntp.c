// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/p25_time_ntp.h>

#include <dsd-neo/platform/timing.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
set_payload_bits(uint64_t* payload, int bit_start, int bit_len, uint64_t value) {
    const int shift = 64 - (bit_start + bit_len);
    const uint64_t field_mask = (bit_len == 64) ? UINT64_MAX : ((1ULL << bit_len) - 1ULL);
    const uint64_t mask = field_mask << shift;
    *payload = (*payload & ~mask) | ((value & field_mask) << shift);
}

static void
build_sync_tsbk(uint8_t tsbk[12]) {
    memset(tsbk, 0, 12);
    tsbk[0] = 0x30;
    tsbk[1] = 0x00;

    uint64_t payload = 0;
    set_payload_bits(&payload, 13, 1, 1);
    set_payload_bits(&payload, 14, 1, 0);
    set_payload_bits(&payload, 15, 2, 2);
    set_payload_bits(&payload, 17, 1, 1);
    set_payload_bits(&payload, 18, 1, 1);
    set_payload_bits(&payload, 19, 4, 5);
    set_payload_bits(&payload, 23, 1, 1);
    set_payload_bits(&payload, 24, 7, 26);
    set_payload_bits(&payload, 31, 4, 3);
    set_payload_bits(&payload, 35, 5, 18);
    set_payload_bits(&payload, 40, 5, 0);
    set_payload_bits(&payload, 45, 6, 38);
    set_payload_bits(&payload, 51, 13, 706);

    for (int i = 9; i >= 2; --i) {
        tsbk[i] = (uint8_t)(payload & 0xFF);
        payload >>= 8;
    }
}

static int
test_sync_decode_extracts_expected_fields(void) {
    uint8_t tsbk[12];
    build_sync_tsbk(tsbk);

    dsd_p25_sync_broadcast broadcast;
    if (!dsd_p25_sync_broadcast_decode(tsbk, &broadcast)) {
        fprintf(stderr, "expected sync broadcast decode to succeed\n");
        return 1;
    }

    int rc = 0;
    if (!broadcast.valid || broadcast.year != 2026 || broadcast.month != 3 || broadcast.day != 18 || broadcast.hour != 0
        || broadcast.minute != 38 || broadcast.microslots != 706) {
        fprintf(stderr, "unexpected decoded UTC fields y=%u m=%u d=%u h=%u min=%u microslots=%u valid=%d\n",
                broadcast.year, broadcast.month, broadcast.day, broadcast.hour, broadcast.minute, broadcast.microslots,
                broadcast.valid);
        rc = 1;
    }
    if (broadcast.system_time_not_locked != 1 || broadcast.microslot_rollover_unlocked != 0
        || broadcast.leap_second_correction != 2 || broadcast.local_time_offset_valid != 1
        || broadcast.local_time_offset_minutes != -330) {
        fprintf(stderr, "unexpected status fields lock=%d rollover=%d leap=%u lto_valid=%d offset=%d\n",
                broadcast.system_time_not_locked, broadcast.microslot_rollover_unlocked,
                broadcast.leap_second_correction, broadcast.local_time_offset_valid, broadcast.local_time_offset_minutes);
        rc = 1;
    }
    return rc;
}

static int
test_sync_current_time_advances_from_monotonic_delta(void) {
    uint8_t tsbk[12];
    build_sync_tsbk(tsbk);

    dsd_p25_sync_broadcast broadcast;
    if (!dsd_p25_sync_broadcast_decode(tsbk, &broadcast)) {
        fprintf(stderr, "expected sync broadcast decode to succeed\n");
        return 1;
    }

    uint64_t later = dsd_p25_sync_broadcast_current_unix_ns(&broadcast, broadcast.monotonic_ns + 1000000000ULL);
    if (later != broadcast.unix_ns + 1000000000ULL) {
        fprintf(stderr, "expected extrapolated ns=%llu got %llu\n",
                (unsigned long long)(broadcast.unix_ns + 1000000000ULL), (unsigned long long)later);
        return 1;
    }
    return 0;
}

static int
test_sync_decode_rejects_wrong_opcode(void) {
    uint8_t tsbk[12];
    build_sync_tsbk(tsbk);
    tsbk[0] = 0x2F;

    dsd_p25_sync_broadcast broadcast;
    if (dsd_p25_sync_broadcast_decode(tsbk, &broadcast)) {
        fprintf(stderr, "expected non-sync opcode decode to fail\n");
        return 1;
    }
    return 0;
}

static int
test_ntp_query_returns_synchronized_reply(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.ntp_enable = 1;
    opts.ntp_portno = 61230;
    snprintf(opts.ntp_bindaddr, sizeof(opts.ntp_bindaddr), "%s", "127.0.0.1");

    if (dsd_p25_time_ntp_start(&opts) != 0) {
        fprintf(stderr, "failed to start ntp service\n");
        return 1;
    }

    dsd_p25_sync_broadcast broadcast;
    memset(&broadcast, 0, sizeof(broadcast));
    broadcast.valid = 1;
    broadcast.unix_ns = 1773794332000000000ULL;
    broadcast.monotonic_ns = dsd_time_monotonic_ns();
    dsd_p25_time_ntp_publish(&broadcast);
    dsd_sleep_ms(50);

    dsd_ntp_query_result result;
    int rc = dsd_p25_time_ntp_query("127.0.0.1", opts.ntp_portno, &result);
    dsd_p25_time_ntp_stop();
    if (rc != 0) {
        fprintf(stderr, "failed to query running ntp service\n");
        return 1;
    }

    if (result.stratum != 1 || result.leap_indicator != 0 || strcmp(result.refid, "P25T") != 0
        || result.transmit_unix_ns == 0) {
        fprintf(stderr, "unexpected ntp reply stratum=%u leap=%u refid=%s tx=%llu\n", result.stratum,
                result.leap_indicator, result.refid, (unsigned long long)result.transmit_unix_ns);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_sync_decode_extracts_expected_fields();
    rc |= test_sync_current_time_advances_from_monotonic_delta();
    rc |= test_sync_decode_rejects_wrong_opcode();
    rc |= test_ntp_query_returns_synchronized_reply();
    return rc;
}
