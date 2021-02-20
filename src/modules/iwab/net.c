#include <endian.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>

#include "net.h"

ssize_t iwab_read(struct iwab* iw, char* buffer, ssize_t max_length, size_t* data_offset) {
  *data_offset = 0;
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  return 0;
}

int iwab_send(struct iwab* iw, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried) {
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  iw->iw_h.length = length;
  if (retried == 0) {
      iw->iw_h.seq += 1;
  }
  iw->iw_h.timestamp = timestamp;
  iw->iw_h.retry = retried;
  iw->iov[1].iov_base = buffer;
  iw->iov[1].iov_len = length;
  return sendmsg(iw->fd, &iw->message, 0);
}

static void iwab_setup(struct iwab* iw) {
  // Swag header
  iw->iw_h.version = 0;
  iw->iw_h.length = 0; // set channel size here for each packet
  iw->iw_h.seq = 0; // increment by one for each new packet
  iw->iw_h.timestamp = 0;
  iw->iw_h.retry = 0; // set if it's a retry

  // Setup iovec
  iw->iov[0].iov_base = &iw->iw_h;
  iw->iov[0].iov_len = sizeof(iw->iw_h);
  // iov[1] must be filled for each send
  iw->iov[1].iov_base = NULL;
  iw->iov[1].iov_len = 0;
  iw->message.msg_name = &iw->dst_addr;
  iw->message.msg_namelen = sizeof(iw->dst_addr);
  iw->message.msg_iov = iw->iov;
  iw->message.msg_iovlen = sizeof(iw->iov) / sizeof(struct iovec);
}

int iwab_close(struct iwab* iw) {
  return close(iw->fd);
}

int iwab_open(struct iwab* iw, const char* iface) {
  struct sockaddr_ll sll;
  struct ifreq ifr;
  const unsigned char dst_mac[] = {0x01, 0x8e, 0x6c, 0x7a, 0x2d, 0x37};
  memset(&sll, 0, sizeof(sll));
  memset(&ifr, 0, sizeof(ifr));
  memset(iw, 0, sizeof(struct iwab));

  if (!iw || !iface) {
    errno = EINVAL;
    return -1;
  }

  // We use SOCK_DGRAM instead of SOCK_RAW to have the kernel
  // build the L2 header for us
  int fd = socket(AF_PACKET, SOCK_DGRAM, htobe16(0x88b5));
  if (fd < 0) {
    printf("Error oppening raw socket : %s\n", strerror(errno));
    return -2;
  }

  strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name) - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    printf("ifindex lookup ioctl error for interface %s: %s\n", iface, strerror(errno));
    return -3;
  }

  iw->dst_addr.sll_family = AF_PACKET;
  iw->dst_addr.sll_protocol = htobe16(0x88b5);
  iw->dst_addr.sll_ifindex = ifr.ifr_ifindex;
  iw->dst_addr.sll_halen = sizeof(dst_mac);
  memcpy(iw->dst_addr.sll_addr, dst_mac, sizeof(dst_mac));

  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htobe16(0x88b5); // Only used on ingress to filter by ethertype
  sll.sll_ifindex = ifr.ifr_ifindex;

  if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
    printf("Error binding to interface %s : %s\n", iface, strerror(errno));
    return -4;
  }

  int max_rcv_size = 1600;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &max_rcv_size, sizeof(int)) < 0) {
    printf("Error setting receive buffer size : %s\n", strerror(errno));
  }

  iwab_setup(iw);
  iw->fd = fd;
  return 0;
}
