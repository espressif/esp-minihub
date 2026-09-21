/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if !defined(DOXYGEN) && defined(__has_include) && __has_include(<sys/socket.h>)
#include <sys/socket.h>
#else
/* ESP newlib has no sys/socket.h; Host plugins get it from lwIP. Stub types
 * for SDK-only compiles (sizeof probe, out-of-tree -I docs/module_sdk). */
typedef uint32_t socklen_t;
struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};
#endif


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file lwip.h
 * @brief Plugin SDK — lwIP socket exports (``lwip_*`` symbols).
 *
 * Address types come from IDF ``sys/socket.h`` when present; otherwise a
 * small SDK stub (ESP newlib does not ship that header). Prefer ``lwip_*``
 * POSIX ``socket`` / ``sendto`` names when linking against Host exports.
 * Each API carries an ``@note`` with its IDF header origin.
 */

/** @brief Create a socket.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
int lwip_socket(int domain, int type, int protocol);
/** @brief Set socket options.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
int lwip_setsockopt(int s, int level, int optname, const void *opval, socklen_t optlen);
/** @brief Send datagram to address.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
ssize_t lwip_sendto(int s, const void *data, size_t size, int flags,
                    const struct sockaddr *to, socklen_t tolen);
/** @brief Receive datagram with sender address.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
ssize_t lwip_recvfrom(int s, void *mem, size_t len, int flags,
                      struct sockaddr *from, socklen_t *fromlen);
/** @brief Convert binary address to text.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
const char *lwip_inet_ntop(int af, const void *src, char *dst, socklen_t size);
/** @brief Convert text address to binary.
 *  @note IDF: ``components/lwip/include/lwip/sockets.h`` */
int lwip_inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif
