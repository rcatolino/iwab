#include <assert.h>
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

ssize_t iwab2_recv(struct iwab2* iw, char* buffer, ssize_t max_length, size_t* data_offset) {
  ssize_t read_size;
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  if (iw->send != 0) {
    errno = EOPNOTSUPP;
    return -2;
  }

  iw->iov[1].iov_base = buffer;
  iw->iov[1].iov_len = max_length;
  int iovcnt = sizeof(iw->iov) / sizeof(struct iovec);
  read_size = readv(iw->fd, iw->iov, iovcnt);
  if (read_size <= 0) {
    return read_size;
  }

  if (read_size <= iw->iov[0].iov_len) {
    errno = EBADMSG;
    return 0;
  }

  *data_offset = read_size - iw->iov[0].iov_len;
  return read_size - iw->iov[0].iov_len;
}

ssize_t iwab2_send(struct iwab2* iw, char* buffer, ssize_t length,
    uint64_t timestamp, uint8_t retried) {
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  if (iw->send == 0) {
    errno = EOPNOTSUPP;
    return -2;
  }

  iw->head.length = length;
  if (retried == 0) {
      iw->head.seq += 1;
  }

  iw->head.timestamp = timestamp;
  iw->head.retry = retried;
  iw->iov[1].iov_base = buffer;
  iw->iov[1].iov_len = length;
  int iovcnt = sizeof(iw->iov) / sizeof(struct iovec);
  return writev(iw->fd, iw->iov, iovcnt);
}

static void iwab_setup(struct iwab2* iw) {
  // Swag header
  iw->head.version = 0;
  iw->head.length = 0; // set channel size here for each packet
  iw->head.seq = 0; // increment by one for each new packet
  iw->head.timestamp = 0;
  iw->head.retry = 0; // set if it's a retry

  // Setup iovec
  iw->iov[0].iov_base = iw->h_buff;
  iw->iov[0].iov_len = sizeof(iw->h_buff);
  // iov[1] must be filled for each send
  iw->iov[1].iov_base = NULL;
  iw->iov[1].iov_len = 0;
}

int iwab2_close(struct iwab2* iw) {
  int ret = close(iw->fd);
  memset(iw, 0, sizeof(struct iwab2));
  return ret;
}

int iwab2_open(struct iwab2* iw, const char* iface, uint16_t port, uint8_t send) {
  struct ipv6_mreq mc_req;
  memset(&mc_req, 0, sizeof(struct ipv6_mreq));
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(struct ifreq));

  if (!iw || !iface) {
    errno = EINVAL;
    return -1;
  }

  memset(iw, 0, sizeof(struct iwab2));

  int fd = socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("socket creation error : %s\n", strerror(errno));
    return -1;
  }

  strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name) - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    printf("ifindex lookup ioctl error for interface %s: %s\n", iface, strerror(errno));
    return -2;
  }

  mc_req.ipv6mr_interface = ifr.ifr_ifindex;
  if (inet_pton(AF_INET6, mcast_group_ip6, &mc_req.ipv6mr_multiaddr) == -1) {
    printf("inet_pton mc_req : %s\n", strerror(errno));
    return -3;
  }

  if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mc_req, sizeof(mc_req)) == -1) {
  	printf("IP_ADD_MEMBERSHIP error : %s\n", strerror(errno));
    return -4;
  }

  if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifr.ifr_ifindex, sizeof(ifr.ifr_ifindex)) == -1) {
  	printf("IP_MULTICAST_IF error : %s\n", strerror(errno));
    return -1;
  }

  iw->group_addr.sin6_family = AF_INET6;
  iw->group_addr.sin6_port = htobe16(port);
  iw->group_addr.sin6_addr = mc_req.ipv6mr_multiaddr;
  if (send == 0) {
    if (bind(fd, (struct sockaddr *) &iw->group_addr, sizeof(iw->group_addr)) < 0) {
      printf("Error binding to multicast group %s:%d : %s\n", mcast_group_ip6, port, strerror(errno));
      return -5;
    }
  } else {
    if (connect(fd, (struct sockaddr *) &iw->group_addr, sizeof(iw->group_addr)) < 0) {
      printf("Error connecting to multicast group address %s\n", strerror(errno));
      return -5;
    }
  }

  iwab_setup(iw);
  iw->fd = fd;
  iw->send = send;
  return 0;
}
