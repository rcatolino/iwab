#ifndef _NET_H_
#define _NET_H_

#include <linux/if_packet.h>
#include <stdint.h>
#include <sys/uio.h>

struct iwab_head {
  uint64_t timestamp;
  uint32_t seq;
  uint16_t length;
  uint8_t version;
  uint8_t retry;
  uint8_t pad[6];
}__attribute__((packed)); // size 16

struct iwab {
  int fd;
  // the following pointers are used when receiving,
  // the buffer will be allocated by the user
  struct iwab_head iw_h;
  // the following struct are used when sending, allocation is static
  struct iovec iov[2]; //Fixed headers, body
  struct msghdr message;
  struct sockaddr_ll dst_addr;
};

int iwab_open(struct iwab* iw, const char *iface);
int iwab_close(struct iwab* iw);
int iwab_send(struct iwab* iw, char* buffer, ssize_t length, uint64_t timestamp, uint8_t retried);
ssize_t iwab_read(struct iwab* iw, char* buffer, ssize_t max_length, size_t* data_offset);

#endif
