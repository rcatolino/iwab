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
  ssize_t read_size;
  struct radiotap_head* rt_in = NULL;
  struct ieee80211_head* dot11_in = NULL;
  struct iwab_head* iw_in = NULL;
  *data_offset = 0;
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  read_size = recv(iw->fd, buffer, max_length, 0);
  if (read_size < 0) {
    return read_size;
  }

  if (read_size <= sizeof(struct radiotap)) {
    errno = EAGAIN; // this is too small to be a valid packet
    return -2;
  }

  rt_in = (struct radiotap_head*) (buffer + *data_offset); // i guess this is violating strict aliasing rules, but buffer content should be constant
  *data_offset += rt_in->length;
  if ((read_size - *data_offset) <= sizeof(struct ieee80211_head)) {
    errno = EAGAIN; // this is too small to be a valid packet
    return -3;
  }

  dot11_in = (struct ieee80211_head*) (buffer + *data_offset);
  *data_offset += sizeof(struct ieee80211_head) + sizeof(struct ieee80211_qos); // skip qos field as well
  if (dot11_in->type != 2 || dot11_in->subtype != 8) {
    errno = EAGAIN; // too small or wrong type
    return -4;
  }

  if (memcmp(dot11_in->addr1, iw->addr_filter, sizeof(iw->addr_filter)) != 0 ||
      memcmp(dot11_in->addr2, iw->addr_filter, sizeof(iw->addr_filter)) != 0 ||
      memcmp(dot11_in->addr3, iw->addr_filter, sizeof(iw->addr_filter)) != 0) {
    errno = EAGAIN; // this is not a iwab packet
    return -5;
  }

  if (read_size - *data_offset <= sizeof(struct iwab_head)) {
    errno = EAGAIN;
    return -6;
  }

  iw_in = (struct iwab_head*) (buffer + *data_offset);
  *data_offset += sizeof(struct iwab_head);
  if (read_size - *data_offset <= 4) {
    errno = EAGAIN;
    return -7;
  }

  iw->rt_in = rt_in;
  iw->dot11_in = dot11_in;
  iw->iw_in = iw_in;
  return read_size - (*data_offset + 4); // 4 is for the FCS
}

int iwab_send(struct iwab* iw, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried) {
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  iw->wi_h.iw_h.length = length;
  if (retried == 0) {
      iw->wi_h.iw_h.seq += 1;
  }
  iw->wi_h.iw_h.timestamp = timestamp;
  iw->wi_h.iw_h.retry = retried;
  iw->iov[2].iov_base = buffer;
  iw->iov[2].iov_len = length;
  int iovcnt = sizeof(iw->iov) / sizeof(struct iovec);
  return writev(iw->fd, iw->iov, iovcnt);
}

static void iwab_setup(struct iwab* iw) {
  iw->rt_h.head.version = 0;
  iw->rt_h.head.length = sizeof(iw->rt_h);
  iw->rt_h.head.bitmap = 0;
  iw->rt_h.head.bitmap = RADIOTAP_TX_FLAGS | RADIOTAP_MCS;

  iw->rt_h.args[0] = 0x20 | 0x08; // TX_FLAGS = RADIOTAP_F_TX_NOACK | RADIOTAP_F_TX_DONT_REORDER
  iw->rt_h.args[1] = 0x0; // TX_FLAGS
  iw->rt_h.args[2] = 0x2 | 0x10; // MCS_INDEX_KNOWN | FEC KNOWN
  iw->rt_h.args[3] = 0x10; // Use LDPC FEC encoding
  iw->rt_h.args[4] = 0x1; // MCS INDEX (QPSK 1/2)
  iw->rt_h.args[4] = 0x3; // MCS INDEX (16-QAM 1/2)

  iw->wi_h.dot11.version = 0;
  iw->wi_h.dot11.type = 2; // data frame
  iw->wi_h.dot11.subtype = 8; // qos frame
  iw->wi_h.dot11.flags = 0;
  iw->wi_h.dot11.duration = 0;
  memset(iw->wi_h.dot11.addr1, 0, sizeof(iw->wi_h.dot11.addr1));
  memset(iw->wi_h.dot11.addr2, 0, sizeof(iw->wi_h.dot11.addr2));
  memset(iw->wi_h.dot11.addr3, 0, sizeof(iw->wi_h.dot11.addr3));
  iw->wi_h.dot11.frag_nb = 0;
  iw->wi_h.dot11.seq_nb = 0;
  iw->wi_h.dot11qos.priority = 0;
  iw->wi_h.dot11qos.ack_policy = 0;

  // Swag header
  iw->wi_h.iw_h.version = 0;
  iw->wi_h.iw_h.length = 0; // set channel size here for each packet
  iw->wi_h.iw_h.seq = 0; // increment by one for each new packet
  iw->wi_h.iw_h.timestamp = 0;
  iw->wi_h.iw_h.retry = 0; // set if it's a retry

  // Setup iovec
  iw->iov[0].iov_base = iw->rt_h_buff;
  iw->iov[0].iov_len = iw->rt_h.head.length;
  iw->iov[1].iov_base = iw->wi_h_buff;
  iw->iov[1].iov_len = sizeof(iw->wi_h_buff);
  // iov[2] must be filled for each send
  iw->iov[2].iov_base = NULL;
  iw->iov[2].iov_len = 0;
}

int iwab_close(struct iwab* iw) {
  return close(iw->fd);
}

int iwab_open(struct iwab* iw, const char* iface) {
  struct sockaddr_ll sll;
  struct ifreq ifr;
  memset(&sll, 0, sizeof(sll));
  memset(&ifr, 0, sizeof(ifr));
  memset(iw, 0, sizeof(struct iwab));

  if (!iw || !iface) {
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

  int max_rcv_size = 1600;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &max_rcv_size, sizeof(int)) < 0) {
    printf("Error setting receive buffer size : %s\n", strerror(errno));
  }

  iwab_setup(iw);
  iw->fd = fd;
  return 0;
}
