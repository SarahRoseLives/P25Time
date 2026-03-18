// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>

#include <stdio.h>

void
dsd_cli_usage(void) {
    printf("\n");
    printf("P25Time\n");
    printf("Built with DSD-Neo components for P25 Phase 1 control-channel time recovery.\n");
    printf("\n");
    printf("Usage:\n");
    printf("  P25Time -i <source>\n");
    printf("  P25Time -h\n");
    printf("\n");
    printf("Options:\n");
    printf("  -i <source>   SDR or sample source, including frequency in the source string.\n");
    printf("                Examples:\n");
    printf("                  rtl:0:771.18125M\n");
    printf("                  rtl:0:771.18125M:22:0:24:0:2\n");
    printf("                  rtltcp:127.0.0.1:1234:771.18125M\n");
    printf("  -h            Show this help.\n");
    printf("\n");
    printf("Behavior:\n");
    printf("  - Embedded NTP binds automatically to UDP/123 by default.\n");
    printf("  - Runtime output is limited to synchronization broadcast lines.\n");
    printf("  - Run as root (or with CAP_NET_BIND_SERVICE) to bind UDP/123.\n");
}
