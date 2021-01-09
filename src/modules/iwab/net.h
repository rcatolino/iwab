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
}__attribute__((packed));

struct ieee80211_qos {
  uint16_t priority : 3;
  uint16_t padding : 1;
  uint16_t txop_dur_req : 1;
  uint16_t ack_policy : 2;
  uint16_t payload_type : 1;
  uint16_t txop_duration : 8;
}__attribute__((packed));

struct iwab_head {
  uint8_t version;
  uint8_t channel;
  uint16_t length;
  uint32_t seq;
  uint64_t timestamp;
  uint8_t retry;
  uint8_t pad[7]; // align on 64bit boundary
}__attribute__((packed));

struct headers {
  struct ieee80211_head dot11;
  struct ieee80211_qos dot11qos;
  struct iwab_head iw_h;
}__attribute__((packed));

struct iwab {
  int fd;
  // the following pointers are used when receiving,
  // the buffer will be allocated by the user
  struct radiotap_head* rt_in;
  struct ieee80211_head* dot11_in;
  struct iwab_head* iw_in;
  // the following struct are used when sending, allocation is static
  union {
      struct radiotap rt_h;
      uint8_t rt_h_buff[sizeof(struct radiotap)];
  };
  union {
      struct headers wi_h;
      uint8_t wi_h_buff[sizeof(struct headers)];
  };
  struct iovec iov[3]; //Fixed headers, body
  uint8_t addr_filter[6];
};

int iwab_open(struct iwab* iw, const char *iface);
int iwab_close(struct iwab* iw);
int iwab_send(struct iwab* iw, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried);
ssize_t iwab_read(struct iwab* iw, char* buffer, ssize_t max_length, size_t* data_offset);

#endif
