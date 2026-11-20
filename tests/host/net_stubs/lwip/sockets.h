#pragma once
#include <stddef.h>
#include <stdint.h>
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_RAW 3
#define IPPROTO_TCP 6
#define IPPROTO_ICMP 1
#define SOL_SOCKET 1
#define SO_ERROR 4
#define FD_SETSIZE 32
typedef unsigned socklen_t;
typedef struct { uint32_t bits; } fd_set;
#define FD_ZERO(set) ((set)->bits = 0U)
#define FD_SET(fd,set) ((set)->bits |= (1U << (fd)))
#define FD_ISSET(fd,set) (((set)->bits & (1U << (fd))) != 0U)
struct timeval { long tv_sec, tv_usec; };
struct in_addr { uint32_t s_addr; };
struct sockaddr { uint16_t sa_family; char data[14]; };
struct sockaddr_in { uint16_t sin_family, sin_port; struct in_addr sin_addr; char padding[8]; };
int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *wait);
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);
int sendto(int fd, const void *data, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
int recvfrom(int fd, void *data, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
