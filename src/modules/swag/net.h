#ifndef _NET_H_
#define _NET_H_

#include <stdint.h>
#include <sys/uio.h>

enum radiotap_flags {
	RADIOTAP_TSFT = 1 << 0,
	RADIOTAP_FLAGS = 1 << 1,
	RADIOTAP_RATE = 1 << 2,
	RADIOTAP_CHANNEL = 1 << 3,
	RADIOTAP_FHSS = 1 << 4,
	RADIOTAP_DBM_ANTSIGNAL = 1 << 5,
	RADIOTAP_DBM_ANTNOISE = 1 << 6,
	RADIOTAP_LOCK_QUALITY = 1 << 7,
	RADIOTAP_TX_ATTENUATION = 1 << 8,
	RADIOTAP_DB_TX_ATTENUATION = 1 << 9,
	RADIOTAP_DBM_TX_POWER = 1 << 10,
	RADIOTAP_ANTENNA = 1 << 11,
	RADIOTAP_DB_ANTSIGNAL = 1 << 12,
	RADIOTAP_DB_ANTNOISE = 1 << 13,
	RADIOTAP_RX_FLAGS = 1 << 14,
	RADIOTAP_TX_FLAGS = 1 << 15,
	RADIOTAP_RTS_RETRIES = 1 << 16,
	RADIOTAP_DATA_RETRIES = 1 << 17,
	RADIOTAP_MCS = 1 << 19,
	RADIOTAP_AMPDU_STATUS = 1 << 20,
	RADIOTAP_VHT = 1 << 21,
	RADIOTAP_TIMESTAMP = 1 << 22,
	RADIOTAP_HE = 1 << 23,
	RADIOTAP_HE_MU = 1 << 24,
	RADIOTAP_ZERO_LEN_PSDU = 1 << 26,
	RADIOTAP_LSIG = 1 << 27,
	RADIOTAP_RADIOTAP_NAMESPACE = 1 << 29,
	RADIOTAP_VENDOR_NAMESPACE = 1 << 30,
	RADIOTAP_EXT = 1 << 31
};

struct radiotap_head {
  uint16_t version;
  uint16_t length;
  uint32_t bitmap;
}__attribute__((packed));

struct radiotap {
  struct radiotap_head head;
  uint8_t args[5];
}__attribute__((packed));

struct ieee80211_head {
  uint16_t version : 2;
  uint16_t type : 2;
  uint16_t subtype : 4;
  uint16_t flags : 8;
  uint16_t duration;
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint16_t frag_nb : 4;
  uint16_t seq_nb : 12;
  uint16_t qos_control;
  uint8_t src_mac[6];
  uint8_t dst_mac[6];
  uint16_t ethertype;
}__attribute__((packed));

struct swag_head {
  uint8_t version;
  uint8_t channel;
  uint16_t length;
  uint32_t seq;
  uint64_t timestamp;
  uint8_t retry;
}__attribute__((packed));

struct wicast {
  int fd;
  union {
      struct radiotap rt_h;
      uint8_t rt_h_buff[sizeof(struct radiotap)];
  };
  union {
      struct ieee80211_head wi_h;
      uint8_t wi_h_buff[sizeof(struct ieee80211_head)];
  };
  union {
      struct swag_head sw_h;
      uint8_t sw_h_buff[sizeof(struct swag_head)];
  };
  int head_length;
  struct iovec iov[4]; //Fixed headers, body
};

int wicast_open(struct wicast* wc, const char *iface);
int wicast_close(struct wicast* wc);
int wicast_send(struct wicast* wc, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried);
ssize_t wicast_read(struct wicast* wc, char* buffer, ssize_t max_length);

#endif
