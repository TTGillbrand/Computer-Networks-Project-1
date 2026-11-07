/*
 * echoclient.c - A simple connection-based client
 * usage: echoclient <host> <port>
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFSIZE 1024
#define MAX_FILE_SZ (20U * 1024U * 1024U)
#define MAX_URL_SZ 200

#define FILE_TYPE_NEITHER 0
#define FILE_TYPE_IMAGE 1
#define FILE_TYPE_TEXT 2

#define RESPONSE_STATUS_URL 0
#define RESPONSE_STATUS_ERROR 1
#define RESPONSE_STATUS_TEXT 2
#define RESPONSE_STATUS_READY 3

// default values for optional arguments.
#define DEFAULT_PORT 2012
#define DEFAULT_RATE_REQUESTS 3
#define DEFAULT_RATE_SECONDS 60
#define DEFAULT_MAX_USERS 3
#define DEFAULT_TIMEOUT_SECONDS 80

// max values to protect server
#define MAX_FILE_SZ 20*1024*1024 // max for qr file
#define MAX_URL_SZ  200 // in characters

struct client_config {
  char hostname[256];
  int portno;
};

static int process_args(int argc, char **argv, struct client_config *cfg) {
  char *end = NULL;
  long port;

  if (argc != 3) {
    fprintf(stderr, "usage: %s <hostname> <port>\n", argv[0]);
    return 1;
  }

  snprintf(cfg->hostname, sizeof(cfg->hostname), "%s", argv[1]);
  port = strtol(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
    fprintf(stderr, "Status: 1\nERROR: invalid port number '%s'\n", argv[2]);
    return 1;
  }

  cfg->portno = (int)port;
  return 0;
}

static int send_all(int fd, const void *buffer, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t result = send(fd, (const char *)buffer + sent, length - sent, 0);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (result == 0)
      return -1;
    sent += (size_t)result;
  }
  return 0;
}

static int recv_all(int fd, void *buffer, size_t length) {
  size_t received = 0;
  while (received < length) {
    ssize_t result = recv(fd, (char *)buffer + received, length - received, 0);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (result == 0)
      return 0;
    received += (size_t)result;
  }
  return 1;
}

static int get_file_type(FILE *file) {
  unsigned char header[12];
  if (fseek(file, 0, SEEK_SET) != 0)
    return FILE_TYPE_NEITHER;

  size_t header_size = fread(header, 1, sizeof(header), file);
  int is_image =
      (header_size >= 3 && header[0] == 0xFF &&
       header[1] == 0xD8 && header[2] == 0xFF) ||
      (header_size >= 8 &&
       memcmp(header, "\x89PNG\r\n\x1A\n", 8) == 0) ||
      (header_size >= 6 &&
       (memcmp(header, "GIF87a", 6) == 0 ||
        memcmp(header, "GIF89a", 6) == 0)) ||
      (header_size >= 2 && memcmp(header, "BM", 2) == 0) ||
      (header_size >= 4 &&
       (memcmp(header, "II*\0", 4) == 0 ||
        memcmp(header, "MM\0*", 4) == 0)) ||
      (header_size >= 12 && memcmp(header, "RIFF", 4) == 0 &&
       memcmp(header + 8, "WEBP", 4) == 0);
  if (is_image)
    return FILE_TYPE_IMAGE;

  if (ferror(file) || fseek(file, 0, SEEK_SET) != 0)
    return FILE_TYPE_NEITHER;

  int remaining_utf8 = 0;
  unsigned char next_min = 0x80;
  unsigned char next_max = 0xBF;
  unsigned char buffer[BUFSIZE];
  size_t bytes_read;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    for (size_t i = 0; i < bytes_read; i++) {
      unsigned char byte = buffer[i];
      if (remaining_utf8 > 0) {
        if (byte < next_min || byte > next_max)
          return FILE_TYPE_NEITHER;
        remaining_utf8--;
        next_min = 0x80;
        next_max = 0xBF;
      } else if (byte <= 0x7F) {
        if (byte != '\t' && byte != '\n' && byte != '\v' &&
            byte != '\f' && byte != '\r' &&
            (byte < 0x20 || byte == 0x7F))
          return FILE_TYPE_NEITHER;
      } else if (byte >= 0xC2 && byte <= 0xDF) {
        remaining_utf8 = 1;
      } else if (byte == 0xE0) {
        remaining_utf8 = 2;
        next_min = 0xA0;
      } else if ((byte >= 0xE1 && byte <= 0xEC) ||
                 (byte >= 0xEE && byte <= 0xEF)) {
        remaining_utf8 = 2;
      } else if (byte == 0xED) {
        remaining_utf8 = 2;
        next_max = 0x9F;
      } else if (byte == 0xF0) {
        remaining_utf8 = 3;
        next_min = 0x90;
      } else if (byte >= 0xF1 && byte <= 0xF3) {
        remaining_utf8 = 3;
      } else if (byte == 0xF4) {
        remaining_utf8 = 3;
        next_max = 0x8F;
      } else {
        return FILE_TYPE_NEITHER;
      }
    }
  }

  return !ferror(file) && remaining_utf8 == 0 ? FILE_TYPE_TEXT
                                               : FILE_TYPE_NEITHER;
}

static int send_file(int sockfd, FILE *file) {
  unsigned char buffer[BUFSIZE];
  size_t bytes_read;

  if (fseek(file, 0, SEEK_SET) != 0)
    return -1;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (send_all(sockfd, buffer, bytes_read) < 0)
      return -1;
  }
  return ferror(file) ? -1 : 0;
}

static int receive_response(int sockfd, uint32_t *status,
                            uint8_t **payload, uint32_t *payload_len,
                            uint32_t max_payload) {
  uint32_t net_status, net_payload_len;
  int result = recv_all(sockfd, &net_status, sizeof(net_status));
  if (result != 1)
    return -1;
  result = recv_all(sockfd, &net_payload_len, sizeof(net_payload_len));
  if (result != 1)
    return -1;

  *status = ntohl(net_status);
  *payload_len = ntohl(net_payload_len);
  if (*payload_len > max_payload)
    return -1;
  *payload = NULL;
  if (*payload_len == 0)
    return 0;

  *payload = malloc(*payload_len);
  if (*payload == NULL)
    return -1;
  result = recv_all(sockfd, *payload, *payload_len);
  if (result != 1) {
    free(*payload);
    *payload = NULL;
    return -1;
  }
  return 0;
}

static int report_server_error(const uint8_t *message, uint32_t length) {
  fprintf(stderr, "Status: 1\nERROR: ");
  if (length > 0)
    fwrite(message, 1, length, stderr);
  else
    fprintf(stderr, "server could not process the file");
  fputc('\n', stderr);
  return 1;
}

int main(int argc, char **argv) {
  struct client_config config;
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  char service[16];
  char filename[BUFSIZE];
  FILE *file = NULL;
  int sockfd = -1;
  int exit_status = 1;
  int64_t file_size;
  uint32_t file_type, status, payload_len;
  uint8_t *payload = NULL;

  if (process_args(argc, argv, &config) != 0)
    return 1;

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPIPE, &action, NULL) < 0) {
    perror("Status: 1\nERROR configuring signals");
    return 1;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(service, sizeof(service), "%d", config.portno);
  int address_result = getaddrinfo(config.hostname, service, &hints, &addresses);
  if (address_result != 0) {
    fprintf(stderr, "Status: 1\nERROR resolving host: %s\n",
            gai_strerror(address_result));
    return 1;
  }

  for (address = addresses; address != NULL; address = address->ai_next) {
    sockfd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
    if (sockfd >= 0 &&
        connect(sockfd, address->ai_addr, address->ai_addrlen) == 0)
      break;
    if (sockfd >= 0)
      close(sockfd);
    sockfd = -1;
  }
  freeaddrinfo(addresses);
  if (sockfd < 0) {
    perror("Status: 1\nERROR connecting to server");
    return 1;
  }

  printf("Please enter filename: ");
  if (fgets(filename, sizeof(filename), stdin) == NULL) {
    fprintf(stderr, "Status: 1\nERROR reading filename\n");
    goto cleanup;
  }
  filename[strcspn(filename, "\n")] = '\0';
  file = fopen(filename, "rb");
  if (file == NULL) {
    perror("Status: 1\nERROR opening file");
    goto cleanup;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0) {
    fprintf(stderr, "Status: 1\nERROR: unable to read a non-empty input file\n");
    goto cleanup;
  }
  if ((uint64_t)file_size > MAX_FILE_SZ) {
    fprintf(stderr, "Status: 1\nERROR: file is too large for the server (maximum %u bytes)\n",
            MAX_FILE_SZ);
    goto cleanup;
  }
  file_type = (uint32_t)get_file_type(file);
  uint32_t net_value = htonl((uint32_t)file_size);
  if (send_all(sockfd, &net_value, sizeof(net_value)) < 0) {
    perror("Status: 1\nERROR sending file size");
    goto cleanup;
  }
  net_value = htonl(file_type);
  if (send_all(sockfd, &net_value, sizeof(net_value)) < 0) {
    perror("Status: 1\nERROR sending file type");
    goto cleanup;
  }

  if (receive_response(sockfd, &status, &payload, &payload_len, BUFSIZE) < 0) {
    fprintf(stderr, "Status: 1\nERROR receiving upload acknowledgment\n");
    goto cleanup;
  }
  if (status == RESPONSE_STATUS_ERROR) {
    exit_status = report_server_error(payload, payload_len);
    goto cleanup;
  }
  free(payload);
  payload = NULL;
  if (status != RESPONSE_STATUS_READY || payload_len != 0) {
    fprintf(stderr, "Status: 1\nERROR: invalid upload acknowledgment\n");
    goto cleanup;
  }

  if (send_file(sockfd, file) < 0) {
    perror("Status: 1\nERROR sending file data");
    goto cleanup;
  }
  if (receive_response(sockfd, &status, &payload, &payload_len,
                       MAX_FILE_SZ) < 0) {
    fprintf(stderr, "Status: 1\nERROR receiving server response\n");
    goto cleanup;
  }
  if (status == RESPONSE_STATUS_ERROR) {
    exit_status = report_server_error(payload, payload_len);
    goto cleanup;
  }
  if (status == RESPONSE_STATUS_TEXT) {
    printf("Status: 0\nText file contents:\n");
  } else if (status == RESPONSE_STATUS_URL && payload_len <= MAX_URL_SZ) {
    printf("Status: 0\nURL (%u bytes): ", payload_len);
  } else {
    fprintf(stderr, "Status: 1\nERROR: invalid server response\n");
    goto cleanup;
  }
  if (payload_len > 0)
    fwrite(payload, 1, payload_len, stdout);
  if (payload_len == 0 || payload[payload_len - 1] != '\n')
    fputc('\n', stdout);
  exit_status = 0;

cleanup:
  free(payload);
  if (file != NULL)
    fclose(file);
  if (sockfd >= 0)
    close(sockfd);
  return exit_status;
}
