// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/p25_time_ntp.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    DSD_P25_SYNC_OPCODE = 0x30,
    DSD_NTP_PACKET_LEN = 48,
};

static const uint64_t DSD_NTP_UNIX_EPOCH_DELTA = 2208988800ULL;

typedef struct {
    dsd_socket_t sockfd;
    dsd_thread_t thread;
    dsd_mutex_t mutex;
    int mutex_init;
    int running;
    volatile int stop_flag;
    int port;
    char bindaddr[1024];
    dsd_p25_sync_broadcast snapshot;
} dsd_p25_time_ntp_service;

static dsd_p25_time_ntp_service g_ntp_service = {
    .sockfd = DSD_INVALID_SOCKET,
};

static uint64_t
dsd_extract_payload_bits(uint64_t payload, int bit_start, int bit_len) {
    if (bit_len <= 0 || bit_len > 64 || bit_start < 0 || bit_start + bit_len > 64) {
        return 0;
    }
    const int shift = 64 - (bit_start + bit_len);
    const uint64_t mask = (bit_len == 64) ? UINT64_MAX : ((1ULL << bit_len) - 1ULL);
    return (payload >> shift) & mask;
}

static int64_t
dsd_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? (unsigned)-3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int
dsd_p25_sync_validate(const dsd_p25_sync_broadcast* out) {
    if (!out) {
        return 0;
    }
    if (out->year < 2000 || out->year > 2127) {
        return 0;
    }
    if (out->month < 1 || out->month > 12) {
        return 0;
    }
    if (out->day < 1 || out->day > 31) {
        return 0;
    }
    if (out->hour > 23 || out->minute > 59) {
        return 0;
    }
    if (out->microslots > 7999) {
        return 0;
    }
    return 1;
}

static uint64_t
dsd_p25_sync_to_unix_ns(const dsd_p25_sync_broadcast* out) {
    int64_t days = dsd_days_from_civil((int)out->year, out->month, out->day);
    int64_t seconds = days * 86400 + ((int64_t)out->hour * 3600) + ((int64_t)out->minute * 60);
    uint64_t unix_ns = (uint64_t)seconds * 1000000000ULL;
    unix_ns += (uint64_t)out->microslots * 7500000ULL;
    unix_ns += (uint64_t)out->leap_second_correction * 2500000ULL;
    return unix_ns;
}

int
dsd_p25_sync_broadcast_decode(const uint8_t tsbk_bytes[12], dsd_p25_sync_broadcast* out) {
    if (!tsbk_bytes || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if ((tsbk_bytes[0] & 0x3F) != DSD_P25_SYNC_OPCODE) {
        return 0;
    }

    uint64_t payload = 0;
    for (int i = 2; i < 10; i++) {
        payload = (payload << 8) | tsbk_bytes[i];
    }

    out->system_time_not_locked = (int)dsd_extract_payload_bits(payload, 13, 1);
    out->microslot_rollover_unlocked = (int)dsd_extract_payload_bits(payload, 14, 1);
    out->leap_second_correction = (uint8_t)dsd_extract_payload_bits(payload, 15, 2);
    out->local_time_offset_valid = (int)dsd_extract_payload_bits(payload, 17, 1);

    int16_t offset_minutes = (int16_t)(dsd_extract_payload_bits(payload, 19, 4) * 60U);
    if (dsd_extract_payload_bits(payload, 23, 1) != 0) {
        offset_minutes += 30;
    }
    if (dsd_extract_payload_bits(payload, 18, 1) != 0) {
        offset_minutes = (int16_t)-offset_minutes;
    }
    out->local_time_offset_minutes = offset_minutes;

    out->year = (uint16_t)(2000U + dsd_extract_payload_bits(payload, 24, 7));
    out->month = (uint8_t)dsd_extract_payload_bits(payload, 31, 4);
    out->day = (uint8_t)dsd_extract_payload_bits(payload, 35, 5);
    out->hour = (uint8_t)dsd_extract_payload_bits(payload, 40, 5);
    out->minute = (uint8_t)dsd_extract_payload_bits(payload, 45, 6);
    out->microslots = (uint16_t)dsd_extract_payload_bits(payload, 51, 13);

    if (!dsd_p25_sync_validate(out)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    out->monotonic_ns = dsd_time_monotonic_ns();
    out->unix_ns = dsd_p25_sync_to_unix_ns(out);
    out->valid = 1;
    return 1;
}

uint64_t
dsd_p25_sync_broadcast_current_unix_ns(const dsd_p25_sync_broadcast* broadcast, uint64_t now_monotonic_ns) {
    if (!broadcast || !broadcast->valid) {
        return 0;
    }
    if (now_monotonic_ns <= broadcast->monotonic_ns) {
        return broadcast->unix_ns;
    }
    return broadcast->unix_ns + (now_monotonic_ns - broadcast->monotonic_ns);
}

static void
dsd_ntp_write_u32be(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void
dsd_ntp_write_timestamp(uint8_t* out, uint64_t unix_ns) {
    if (unix_ns == 0) {
        memset(out, 0, 8);
        return;
    }

    uint64_t seconds = (unix_ns / 1000000000ULL) + DSD_NTP_UNIX_EPOCH_DELTA;
    uint64_t rem_ns = unix_ns % 1000000000ULL;
    uint32_t fraction = (uint32_t)((double)rem_ns * 4294967296.0 / 1000000000.0);

    dsd_ntp_write_u32be(out, (uint32_t)seconds);
    dsd_ntp_write_u32be(out + 4, fraction);
}

static uint32_t
dsd_ntp_read_u32be(const uint8_t* in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static uint64_t
dsd_ntp_read_timestamp(const uint8_t* in) {
    uint32_t seconds = dsd_ntp_read_u32be(in);
    uint32_t fraction = dsd_ntp_read_u32be(in + 4);
    if (seconds == 0 && fraction == 0) {
        return 0;
    }
    if ((uint64_t)seconds < DSD_NTP_UNIX_EPOCH_DELTA) {
        return 0;
    }
    uint64_t unix_seconds = (uint64_t)seconds - DSD_NTP_UNIX_EPOCH_DELTA;
    uint64_t unix_ns = unix_seconds * 1000000000ULL;
    unix_ns += ((uint64_t)fraction * 1000000000ULL) >> 32;
    return unix_ns;
}

static int
dsd_p25_time_bind_socket(dsd_p25_time_ntp_service* service) {
    service->sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (service->sockfd == DSD_INVALID_SOCKET) {
        LOG_ERROR("P25Time NTP: failed to create UDP socket\n");
        return -1;
    }

    int one = 1;
    (void)dsd_socket_setsockopt(service->sockfd, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
    (void)dsd_socket_set_recv_timeout(service->sockfd, 250);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)service->port);

    if (service->bindaddr[0] == '\0' || strcmp(service->bindaddr, "*") == 0
        || strcmp(service->bindaddr, "0.0.0.0") == 0) {
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    } else if (dsd_socket_resolve(service->bindaddr, service->port, &bind_addr) != 0) {
        LOG_ERROR("P25Time NTP: failed to resolve bind address %s\n", service->bindaddr);
        dsd_socket_close(service->sockfd);
        service->sockfd = DSD_INVALID_SOCKET;
        return -1;
    }

    if (dsd_socket_bind(service->sockfd, (const struct sockaddr*)&bind_addr, (int)sizeof(bind_addr)) != 0) {
        LOG_ERROR("P25Time NTP: failed to bind UDP/%d on %s\n", service->port,
                  service->bindaddr[0] ? service->bindaddr : "0.0.0.0");
        dsd_socket_close(service->sockfd);
        service->sockfd = DSD_INVALID_SOCKET;
        return -1;
    }

    return 0;
}

static void
dsd_p25_time_handle_request(dsd_p25_time_ntp_service* service, const uint8_t* req,
                            const struct sockaddr* src_addr, int src_addrlen) {
    if (!service || !req || !src_addr || src_addrlen <= 0) {
        return;
    }

    dsd_p25_sync_broadcast snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    if (service->mutex_init) {
        dsd_mutex_lock(&service->mutex);
        snapshot = service->snapshot;
        dsd_mutex_unlock(&service->mutex);
    }

    uint8_t resp[DSD_NTP_PACKET_LEN];
    memset(resp, 0, sizeof(resp));

    const uint64_t now_monotonic_ns = dsd_time_monotonic_ns();
    const uint64_t now_unix_ns = dsd_p25_sync_broadcast_current_unix_ns(&snapshot, now_monotonic_ns);
    const int synchronized = (snapshot.valid && now_unix_ns != 0);

    resp[0] = (uint8_t)(((synchronized ? 0 : 3) << 6) | (4 << 3) | 4);
    resp[1] = synchronized ? 1 : 16;
    resp[2] = req[2] ? req[2] : 6;
    resp[3] = (uint8_t)(-20 & 0xFF);
    dsd_ntp_write_u32be(resp + 8, synchronized ? (1U << 16) : (16U << 16));
    memcpy(resp + 12, synchronized ? "P25T" : "INIT", 4);
    dsd_ntp_write_timestamp(resp + 16, synchronized ? snapshot.unix_ns : 0);
    memcpy(resp + 24, req + 40, 8);
    dsd_ntp_write_timestamp(resp + 32, synchronized ? now_unix_ns : 0);
    dsd_ntp_write_timestamp(resp + 40, synchronized ? now_unix_ns : 0);

    (void)dsd_socket_sendto(service->sockfd, resp, sizeof(resp), 0, src_addr, src_addrlen);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    dsd_p25_time_ntp_thread(void* arg) {
    dsd_p25_time_ntp_service* service = (dsd_p25_time_ntp_service*)arg;
    uint8_t req[DSD_NTP_PACKET_LEN];

    while (!service->stop_flag) {
        struct sockaddr_in src_addr;
        int src_len = (int)sizeof(src_addr);
        int n = dsd_socket_recvfrom(service->sockfd, req, sizeof(req), 0, (struct sockaddr*)&src_addr, &src_len);
        if (n >= DSD_NTP_PACKET_LEN) {
            dsd_p25_time_handle_request(service, req, (const struct sockaddr*)&src_addr, src_len);
        }
    }

    DSD_THREAD_RETURN;
}

int
dsd_p25_time_ntp_start(const dsd_opts* opts) {
    if (!opts || !opts->ntp_enable || opts->ntp_portno <= 0) {
        return 0;
    }

    dsd_p25_time_ntp_stop();

    memset(&g_ntp_service.snapshot, 0, sizeof(g_ntp_service.snapshot));
    g_ntp_service.port = opts->ntp_portno;
    snprintf(g_ntp_service.bindaddr, sizeof(g_ntp_service.bindaddr), "%s", opts->ntp_bindaddr);
    g_ntp_service.bindaddr[sizeof(g_ntp_service.bindaddr) - 1] = '\0';
    g_ntp_service.stop_flag = 0;

    if (dsd_mutex_init(&g_ntp_service.mutex) != 0) {
        LOG_ERROR("P25Time NTP: failed to initialize mutex\n");
        return -1;
    }
    g_ntp_service.mutex_init = 1;

    if (dsd_p25_time_bind_socket(&g_ntp_service) != 0) {
        dsd_mutex_destroy(&g_ntp_service.mutex);
        g_ntp_service.mutex_init = 0;
        return -1;
    }

    if (dsd_thread_create(&g_ntp_service.thread, dsd_p25_time_ntp_thread, &g_ntp_service) != 0) {
        LOG_ERROR("P25Time NTP: failed to start responder thread\n");
        dsd_socket_close(g_ntp_service.sockfd);
        g_ntp_service.sockfd = DSD_INVALID_SOCKET;
        dsd_mutex_destroy(&g_ntp_service.mutex);
        g_ntp_service.mutex_init = 0;
        return -1;
    }

    g_ntp_service.running = 1;
    LOG_NOTICE("P25Time NTP source listening on %s:%d\n",
               g_ntp_service.bindaddr[0] ? g_ntp_service.bindaddr : "0.0.0.0", g_ntp_service.port);
    return 0;
}

void
dsd_p25_time_ntp_stop(void) {
    if (!g_ntp_service.running && !g_ntp_service.mutex_init) {
        return;
    }

    g_ntp_service.stop_flag = 1;
    if (g_ntp_service.sockfd != DSD_INVALID_SOCKET) {
        (void)dsd_socket_shutdown(g_ntp_service.sockfd, SHUT_RDWR);
    }
    if (g_ntp_service.running) {
        dsd_thread_join(g_ntp_service.thread);
    }
    g_ntp_service.running = 0;

    if (g_ntp_service.sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(g_ntp_service.sockfd);
        g_ntp_service.sockfd = DSD_INVALID_SOCKET;
    }
    if (g_ntp_service.mutex_init) {
        dsd_mutex_destroy(&g_ntp_service.mutex);
        g_ntp_service.mutex_init = 0;
    }

    memset(&g_ntp_service.snapshot, 0, sizeof(g_ntp_service.snapshot));
    g_ntp_service.bindaddr[0] = '\0';
    g_ntp_service.port = 0;
}

void
dsd_p25_time_ntp_publish(const dsd_p25_sync_broadcast* broadcast) {
    if (!broadcast || !broadcast->valid || !g_ntp_service.mutex_init) {
        return;
    }

    dsd_mutex_lock(&g_ntp_service.mutex);
    g_ntp_service.snapshot = *broadcast;
    dsd_mutex_unlock(&g_ntp_service.mutex);
}

int
dsd_p25_time_ntp_query(const char* hostname, int port, dsd_ntp_query_result* out) {
    if (!hostname || hostname[0] == '\0' || port <= 0 || port > 65535 || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    if (dsd_socket_resolve(hostname, port, &addr) != 0) {
        dsd_socket_close(sockfd);
        return -1;
    }
    if (dsd_socket_set_recv_timeout(sockfd, 1000) != 0) {
        dsd_socket_close(sockfd);
        return -1;
    }

    uint8_t req[DSD_NTP_PACKET_LEN];
    memset(req, 0, sizeof(req));
    req[0] = 0x23;
    req[2] = 4;

    if (dsd_socket_sendto(sockfd, req, sizeof(req), 0, (const struct sockaddr*)&addr, (int)sizeof(addr)) < 0) {
        dsd_socket_close(sockfd);
        return -1;
    }

    uint8_t resp[DSD_NTP_PACKET_LEN];
    struct sockaddr_in src_addr;
    int src_len = (int)sizeof(src_addr);
    int n = dsd_socket_recvfrom(sockfd, resp, sizeof(resp), 0, (struct sockaddr*)&src_addr, &src_len);
    dsd_socket_close(sockfd);
    if (n < DSD_NTP_PACKET_LEN) {
        return -1;
    }

    out->leap_indicator = (uint8_t)((resp[0] >> 6) & 0x3);
    out->version = (uint8_t)((resp[0] >> 3) & 0x7);
    out->mode = (uint8_t)(resp[0] & 0x7);
    out->stratum = resp[1];
    out->precision = (int8_t)resp[3];
    memcpy(out->refid, resp + 12, 4);
    out->refid[4] = '\0';
    out->reference_unix_ns = dsd_ntp_read_timestamp(resp + 16);
    out->receive_unix_ns = dsd_ntp_read_timestamp(resp + 32);
    out->transmit_unix_ns = dsd_ntp_read_timestamp(resp + 40);
    return 0;
}
