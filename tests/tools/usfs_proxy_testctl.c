// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
#include "usfs_common.h"

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) return 2;
    mid_t module;
    struct usfs_proxy_status status;
    if (usfs_proxy_query_user(&module, &status) != USFS_QUERY_PRESENT) return 1;
    if (!strcmp(argv[1], "direct-term") && argc == 2)
    {
        const mid_t implementation = usfs_kext_get_module_id();
        if (!implementation || status.state != USFS_PROXY_ACTIVE) return 1;
        errno = 0;
        const int rc = usfs_proxy_command(implementation, (dev_t)status.device,
                                          USFS_CFG_TERM, NULL, 0);
        if (rc != 0 && errno == EBUSY) return 0;
        fprintf(stderr, "direct CFG_TERM unexpectedly returned %d (errno=%d)\n", rc, errno);
        return 1;
    }
    if (!strcmp(argv[1], "open"))
    {
        int fd = open("/dev/usfs0/status", O_RDONLY);
        if (fd < 0) return errno == ENODEV || errno == ENOENT || errno == EBUSY ? 0 : 1;
        return close(fd) ? 1 : 0;
    }
    struct usfs_proxy_test_control request = {0};
    if (!strcmp(argv[1], "arm") && argc == 3)
    {
        request.operation = 1;
        request.point = (uint32_t)atoi(argv[2]);
        if (request.point < 1 || request.point > 4) return 2;
    }
    else if (!strcmp(argv[1], "release") && argc == 2) request.operation = 2;
    else if (strcmp(argv[1], "query") || argc != 2) return 2;
    errno = 0;
    int rc = usfs_proxy_command(module, 0, USFS_PROXY_TEST, &request, sizeof(request));
    if (request.operation == 0 && (rc == 0 || errno <= 4))
    { printf("%d\n", rc ? errno : 0); return 0; }
    if (rc) perror("proxy test control");
    return rc ? 1 : 0;
}
