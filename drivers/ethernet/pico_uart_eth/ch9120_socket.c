#include <zephyr/net/net_ip.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/logging/log.h>

#include "ch9120.h"

static const struct socket_op_vtable ch9120_socket_fd_op_vtable;

static ssize_t ch9120_read(void *obj, void *buf, size_t sz)
{
	return 0;
}

static ssize_t ch9120_write(void *obj, const void *buf, size_t sz)
{
	return 0;
}

static int ch9120_close(void *obj)
{
    return 0;
}

static int ch9120_ioctl(void *obj, unsigned int request, va_list args)
{
    return 0;
}

static int ch9120_bind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
    return 0;
}

static int ch9120_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	return 0;
}

static int ch9120_listen(void *obj, int backlog)
{
	return 0;
}

static int ch9120_accept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static ssize_t ch9120_sendto(void *obj, const void *buf, size_t len, int flags,
			   const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	return 0;
}

static ssize_t ch9120_sendmsg(void *obj, const struct net_msghdr *msg, int flags)
{
	return 0;
}

static ssize_t ch9120_recvfrom(void *obj, void *buf, size_t len, int flags,
			     struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static ssize_t ch9120_recvmsg(void *obj, struct net_msghdr *msg, int flags)
{
	return 0;
}

static int ch9120_getsockopt(void *obj, int level, int optname,
			   void *optval, net_socklen_t *optlen)
{
	return 0;
}

static int ch9120_setsockopt(void *obj, int level, int optname,
			   const void *optval, net_socklen_t optlen)
{
	return 0;
}

static int ch9120_getpeername(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

static int ch9120_getsockname(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return 0;
}

// Utils
static int socket_family_is_supported(int family)
{
	switch (family) {
	case NET_AF_INET:
		break;
	default:
		return -1;
	}

	return 0;
}

static int socket_type_is_supported(int type)
{
	switch (type) {
	case NET_SOCK_STREAM:
		break;
	case NET_SOCK_DGRAM:
		break;
	default:
		return -1;
	}

	return 0;
}

static int socket_proto_is_supported(int proto)
{
	switch (proto) {
	case NET_IPPROTO_TCP:
		break;
	case NET_IPPROTO_UDP:
		break;
	default:
		return -1;
	}

	return 0;
}
// Utils End

static bool ch9120_socket_is_supported(int family, int type, int proto)
{
    int ret;
    
    ret = socket_family_is_supported(family);
    if (ret < 0) {
        return false;
    }

    ret = socket_type_is_supported(type);
    if (ret < 0) {
        return false;
    }

    ret = socket_proto_is_supported(proto);
    if (ret < 0) {
        return false;
    }

	return true;
}

int ch9120_socket_create(int family, int type, int proto)
{
    int fd;

    fd = zvfs_reserve_fd();
	if (fd < 0) {
		return -1;
	}

    // zvfs_finalize_typed_fd(fd, sock, &ch9120_socket_fd_op_vtable.fd_vtable, ZVFS_MODE_IFSOCK);
	(void)ch9120_socket_fd_op_vtable;

	return 0;
}

static const struct socket_op_vtable ch9120_socket_fd_op_vtable = {
	.fd_vtable = {
		.read = ch9120_read,
		.write = ch9120_write,
		.close = ch9120_close,
		.ioctl = ch9120_ioctl,
	},
	.bind = ch9120_bind,
	.connect = ch9120_connect,
	.listen = ch9120_listen,
	.accept = ch9120_accept,
	.sendto = ch9120_sendto,
	.sendmsg = ch9120_sendmsg,
	.recvfrom = ch9120_recvfrom,
	.recvmsg = ch9120_recvmsg,
	.getsockopt = ch9120_getsockopt,
	.setsockopt = ch9120_setsockopt,
	.getpeername = ch9120_getpeername,
	.getsockname = ch9120_getsockname,
};

NET_SOCKET_OFFLOAD_REGISTER(
    ch9120,
    CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY,
    AF_UNSPEC,
	ch9120_socket_is_supported,
    ch9120_socket_create);