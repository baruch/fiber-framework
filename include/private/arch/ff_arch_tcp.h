#ifndef FF_ARCH_TCP_PRIVATE
#define FF_ARCH_TCP_PRIVATE

#include "private/arch/ff_arch_net_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ff_arch_tcp;

struct ff_arch_tcp *ff_arch_tcp_create();

void ff_arch_tcp_delete(struct ff_arch_tcp *tcp);

int ff_arch_tcp_bind(struct ff_arch_tcp *tcp, const struct ff_arch_net_addr *addr, int is_listening);

int ff_arch_tcp_connect(struct ff_arch_tcp *tcp, const struct ff_arch_net_addr *addr);

struct ff_arch_tcp *ff_arch_tcp_accept(struct ff_arch_tcp *tcp, struct ff_arch_net_addr *remote_addr);

int ff_arch_tcp_read(struct ff_arch_tcp *tcp, void *buf, int len);

int ff_arch_tcp_read_with_timeout(struct ff_arch_tcp *tcp, void *buf, int len, int timeout);

int ff_arch_tcp_write(struct ff_arch_tcp *tcp, const void *buf, int len);

int ff_arch_tcp_write_with_timeout(struct ff_arch_tcp *tcp, const void *buf, int len, int timeout);

void ff_arch_tcp_disconnect(struct ff_arch_tcp *tcp);

#ifdef __cplusplus
}
#endif

#endif