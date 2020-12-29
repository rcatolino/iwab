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

#include "net.h"

ssize_t wicast_read(struct wicast* wc, char* buffer, ssize_t max_length) {
  ssize_t read_size;
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  wc->iov[3].iov_base = buffer;
  wc->iov[3].iov_len = max_length - wc->head_length;
  int iovcnt = sizeof(wc->iov) / sizeof(struct iovec);
  read_size = readv(wc->fd, wc->iov, iovcnt);
  if (read_size < 0) {
    return read_size;
  }

  if (read_size <= wc->head_length) {
    errno = EINTR; // TODO: mb there is a more fitting error code.
    return -2;
  }

  return read_size - wc->head_length;
}

int wicast_send(struct wicast* wc, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried) {
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  wc->sw_h.length = length;
  wc->sw_h.seq += 1;
  wc->sw_h.timestamp = timestamp;
  wc->sw_h.retry = retried;
  wc->iov[3].iov_base = buffer;
  wc->iov[3].iov_len = length;
  int iovcnt = sizeof(wc->iov) / sizeof(struct iovec);
  return writev(wc->fd, wc->iov, iovcnt);
}

static void wicast_setup(struct wicast* wc) {
  memset(wc, 0, sizeof(struct wicast));
  wc->rt_h.head.version = 0;
  wc->rt_h.head.length = sizeof(wc->rt_h) + 5; // If the number of argument changes in the bitmap, type args count must be raised in net.h
  wc->rt_h.head.bitmap = RADIOTAP_TX_FLAGS | RADIOTAP_MCS;

  wc->rt_h.args[0] = 0x20 | 0x08; // TX_FLAGS = RADIOTAP_F_TX_NOACK | RADIOTAP_F_TX_DONT_REORDER
  wc->rt_h.args[1] = 0x0; // TX_FLAGS
  wc->rt_h.args[2] = 0x2 | 0x10; // MCS_INDEX_KNOWN | FEC KNOWN
  wc->rt_h.args[3] = 0x10; // Use LDPC FEC encoding
  wc->rt_h.args[4] = 0x1; // MCS INDEX (QPSK 1/2)
  //wc->rt_args[4] = 0x3; // MCS INDEX (16-QAM 1/2)

  wc->wi_h.version = 0;
  wc->wi_h.type = 2; // data frame
  wc->wi_h.subtype = 8; // qos frame
  wc->wi_h.flags = 0;
  wc->wi_h.duration = 0;
  memset(wc->wi_h.addr1, 0, sizeof(wc->wi_h.addr1));
  memset(wc->wi_h.addr2, 0, sizeof(wc->wi_h.addr2));
  memset(wc->wi_h.addr3, 0, sizeof(wc->wi_h.addr3));
  wc->wi_h.frag_nb = 0;
  wc->wi_h.seq_nb = 0;
  wc->wi_h.qos_control = 0;
  memset(wc->wi_h.src_mac, 0, sizeof(wc->wi_h.src_mac));
  memset(wc->wi_h.dst_mac, 0, sizeof(wc->wi_h.dst_mac));
  wc->wi_h.ethertype = 0x8454;

  // Swag header
  wc->sw_h.version = 0;
  wc->sw_h.channel = 0;
  wc->sw_h.length = 0; // set channel size here for each packet
  wc->sw_h.seq = 0; // increment by one for each new packet
  wc->sw_h.timestamp = 0;
  wc->sw_h.retry = 0; // set if it's a retry

  // Setup iovec
  wc->iov[0].iov_base = wc->rt_h_buff;
  wc->iov[0].iov_len = wc->rt_h.head.length;
  wc->iov[1].iov_base = wc->wi_h_buff;
  wc->iov[1].iov_len = sizeof(wc->wi_h_buff);
  wc->iov[2].iov_base = wc->sw_h_buff;
  wc->iov[2].iov_len = sizeof(wc->sw_h_buff);
  // iov[3] must be filled for each send
  wc->head_length = wc->iov[0].iov_len;
  wc->head_length += wc->iov[1].iov_len;
  wc->head_length += wc->iov[2].iov_len;
  wc->iov[3].iov_base = NULL;
  wc->iov[3].iov_len = 0;
}

int wicast_close(struct wicast* wc) {
  return close(wc->fd);
}

int wicast_open(struct wicast* wc, const char* iface) {
  struct sockaddr_ll sll;
  struct ifreq ifr;
  memset(&sll, 0, sizeof(sll));
  memset(&ifr, 0, sizeof(ifr));

  if (!wc || !iface) {
    errno = EINVAL;
    return -1;
  }

  int fd = socket(AF_PACKET, SOCK_RAW, htobe16(ETH_P_ALL));
  if (fd < 0) {
    printf("Error oppening raw socket : %s\n", strerror(errno));
    return -2;
  }

  strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name) - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		printf("ifindex lookup ioctl error for interface %s: %s\n", iface, strerror(errno));
		return -3;
	}

  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htobe16(ETH_P_ALL);
  sll.sll_ifindex = ifr.ifr_ifindex;

  if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
    printf("Error binding to interface %s : %s\n", iface, strerror(errno));
    return -4;
  }

  wicast_setup(wc);
  wc->fd = fd;
  return 0;
}
