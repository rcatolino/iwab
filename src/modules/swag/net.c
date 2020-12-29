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

ssize_t wicast_read(struct wicast* wc, char* buffer, ssize_t max_length, size_t* data_offset) {
  ssize_t read_size;
  *data_offset = 0;
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  read_size = recv(wc->fd, buffer, max_length, 0);
  if (read_size < 0) {
    return read_size;
  }

  if (read_size <= sizeof(struct radiotap)) {
    errno = EAGAIN; // this is too small to be a wiscast packet
    return -2;
  }

  wc->rt_in = (struct radiotap_head*) (buffer + *data_offset); // i guess this is violating strict aliasing rules, but buffer content should be constant
  *data_offset += wc->rt_in->length;
  if ((read_size - *data_offset) <= sizeof(struct ieee80211_head)) {
    errno = EAGAIN; // this is too small to be a wiscast packet
    return -3;
  }

  wc->dot11_in = (struct ieee80211_head*) (buffer + *data_offset);
  *data_offset += sizeof(struct ieee80211_head) + sizeof(uint16_t); // skip qos field as well
  if ((read_size - *data_offset) <= sizeof(struct l2_head) || wc->dot11_in->type != 2 || wc->dot11_in->subtype != 8) {
    errno = EAGAIN; // too small or wrong type
    return -4;
  }

  if (memcmp(wc->wi_h.dot11.addr1, wc->addr_filter, sizeof(wc->addr_filter)) != 0 ||
      memcmp(wc->wi_h.dot11.addr2, wc->addr_filter, sizeof(wc->addr_filter)) != 0 ||
      memcmp(wc->wi_h.dot11.addr3, wc->addr_filter, sizeof(wc->addr_filter)) != 0) {
    pa_log("w1: %02x:%02x:%02x:%02x:%02x:%02x",
        wc->wi_h.dot11.addr1[0], wc->wi_h.dot11.addr1[1], wc->wi_h.dot11.addr1[2],
        wc->wi_h.dot11.addr1[3], wc->wi_h.dot11.addr1[4], wc->wi_h.dot11.addr1[5]);
    errno = EAGAIN; // this is not a wicast packet
    return -5;
  }

  wc->l2_in = (struct l2_head*) (buffer + *data_offset);
  *data_offset += sizeof(struct l2_head);
  if (read_size - *data_offset <= sizeof(struct swag_head)) {
    errno = EAGAIN;
    return -6;
  }

  wc->sw_in = (struct swag_head*) (buffer + *data_offset);
  *data_offset += sizeof(struct swag_head);
  if (read_size - *data_offset <= 4) {
    errno = EAGAIN;
    return -7;
  }

  return read_size - (*data_offset + 4); // 4 is for the FCS
}

int wicast_send(struct wicast* wc, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried) {
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }

  wc->wi_h.sw_h.length = length;
  wc->wi_h.sw_h.seq += 1;
  wc->wi_h.sw_h.timestamp = timestamp;
  wc->wi_h.sw_h.retry = retried;
  wc->iov[2].iov_base = buffer;
  wc->iov[2].iov_len = length;
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

  wc->wi_h.dot11.version = 0;
  wc->wi_h.dot11.type = 2; // data frame
  wc->wi_h.dot11.subtype = 8; // qos frame
  wc->wi_h.dot11.flags = 0;
  wc->wi_h.dot11.duration = 0;
  memset(wc->wi_h.dot11.addr1, 0, sizeof(wc->wi_h.dot11.addr1));
  memset(wc->wi_h.dot11.addr2, 0, sizeof(wc->wi_h.dot11.addr2));
  memset(wc->wi_h.dot11.addr3, 0, sizeof(wc->wi_h.dot11.addr3));
  wc->wi_h.dot11.frag_nb = 0;
  wc->wi_h.dot11.seq_nb = 0;
  wc->wi_h.dot11qos= 0;
  memset(wc->wi_h.l2.src_mac, 0, sizeof(wc->wi_h.l2.src_mac));
  memset(wc->wi_h.l2.dst_mac, 0, sizeof(wc->wi_h.l2.dst_mac));
  wc->wi_h.l2.ethertype = 0x8454;

  // Swag header
  wc->wi_h.sw_h.version = 0;
  wc->wi_h.sw_h.channel = 0;
  wc->wi_h.sw_h.length = 0; // set channel size here for each packet
  wc->wi_h.sw_h.seq = 0; // increment by one for each new packet
  wc->wi_h.sw_h.timestamp = 0;
  wc->wi_h.sw_h.retry = 0; // set if it's a retry

  // Setup iovec
  wc->iov[0].iov_base = wc->rt_h_buff;
  wc->iov[0].iov_len = wc->rt_h.head.length;
  wc->iov[1].iov_base = wc->wi_h_buff;
  wc->iov[1].iov_len = sizeof(wc->wi_h_buff);
  // iov[2] must be filled for each send
  wc->iov[2].iov_base = NULL;
  wc->iov[2].iov_len = 0;
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

  //int fd = socket(AF_PACKET, SOCK_RAW, htobe16(ETH_P_ALL));
  int fd = socket(AF_PACKET, SOCK_DGRAM, htobe16(ETH_P_ALL));
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
