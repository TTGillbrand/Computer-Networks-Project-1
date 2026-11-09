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
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFSIZE 1024

// max values to protect server
#define MAX_FILE_SZ (20U * 1024U * 1024U)
#define MAX_URL_SZ 200
#define FILE_TYPE_NEITHER 0
#define FILE_TYPE_IMAGE 1
#define FILE_TYPE_TEXT 2

#define RESPONSE_STATUS_ACCEPTED 0
#define RESPONSE_STATUS_ERROR    1
#define RESPONSE_STATUS_TIMEOUT  2
#define RESPONSE_STATUS_READY    3

#define RESULT_OK 0     //all nominal
#define RESULT_RETRY 1  //file failed, connection works
#define RESULT_FATAL -1 //connection is broken

static int send_all(int fd, const void *buffer, size_t length);
static int recv_all(int fd, void *buffer, size_t length);
static int get_file_type(FILE *file);
static int send_file(int sockfd, FILE *file);
static int receive_response(int sockfd, uint32_t *status,
                            uint8_t **payload, uint32_t *payload_len,
                            uint32_t max_payload);
static int report_server_error(const uint8_t *message, uint32_t length);


struct client_config {
  char hostname[256];
  int portno;
};

static int send_one_file(int sockfd, const char *filename) {
  FILE *file = fopen(filename, "rb");
  uint8_t *payload = NULL;
  uint32_t status, payload_len;
  int rc = RESULT_FATAL;

  if (file == NULL) {
    perror("Status: 1\nERROR opening file");
    return RESULT_RETRY;
  }

  long file_size;
  if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0) {
    fprintf(stderr, "Status: 1\nERROR: unable to read a non-empty input file\n");
    rc = RESULT_RETRY;
    goto done;
  }
  if ((uint64_t)file_size > MAX_FILE_SZ) {
    fprintf(stderr, "Status: 1\nERROR: file too large (maximum %u bytes)\n", MAX_FILE_SZ);
    rc = RESULT_RETRY;
    goto done;
  }

  uint32_t file_type = (uint32_t)get_file_type(file);
  uint32_t net_size = htonl((uint32_t)file_size);
  uint32_t net_type = htonl(file_type);
  if (send_all(sockfd, &net_size, sizeof(net_size)) < 0 ||
      send_all(sockfd, &net_type, sizeof(net_type)) < 0) {
    perror("Status: 1\nERROR sending header");
    goto done;
  }

  // header is followed directly by the file bytes
  if (send_file(sockfd, file) < 0) {
    perror("Status: 1\nERROR sending file data");
    goto done;
  }

  if (receive_response(sockfd, &status, &payload, &payload_len, MAX_FILE_SZ) < 0) {
    fprintf(stderr, "Status: 1\nERROR receiving server response\n");
    goto done;
  }

  if (status == RESPONSE_STATUS_TIMEOUT) {
    fprintf(stderr, "Status: 2\nTIMEOUT: ");
    if (payload_len > 0) fwrite(payload, 1, payload_len, stderr);
    fputc('\n', stderr);
    goto done;                              // server closes: fatal
  }
  if (status == RESPONSE_STATUS_ERROR) {
    fprintf(stderr, "Status: 3\nRATE LIMIT: ");
    if (payload_len > 0) fwrite(payload, 1, payload_len, stderr);
    fputc('\n', stderr);
    rc = RESULT_RETRY;
    goto done;
  }
  if (status == RESPONSE_STATUS_ERROR) {
    report_server_error(payload, payload_len);
    rc = RESULT_RETRY;
    goto done;
  }
  if (status != RESPONSE_STATUS_ACCEPTED) {
    fprintf(stderr, "Status: 1\nERROR: invalid server response\n");
    goto done;
  }

  if (file_type == FILE_TYPE_TEXT) {
    printf("Status: 0\nText file contents:\n");
  } else if (payload_len <= MAX_URL_SZ) {
    printf("Status: 0\nURL (%u bytes): ", payload_len);
  } else {
    fprintf(stderr, "Status: 1\nERROR: URL longer than expected\n");
    goto done;
  }

  if (payload_len > 0)
    fwrite(payload, 1, payload_len, stdout);
  if (payload_len == 0 || payload[payload_len - 1] != '\n')
    fputc('\n', stdout);
  rc = RESULT_OK;

done:
  free(payload);
  fclose(file);
  return rc;
}


// process user input
static int process_args(int argc, char **argv, struct client_config *cfg) {
  char *end = NULL;
  long port;

  if (argc != 3) {
    fprintf(stderr, "usage: %s <hostname> <port>\n", argv[0]);
    return 1;
  }

  // check for invalid port number
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

  // read the file’s initial bytes and checks for signatures identifying common image formats, 
  // including JPEG, PNG, GIF, BMP, TIFF, and WebP. If one matches, return FILE_TYPE_IMAGE.
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

  // scan the whole file to see whether it contains valid UTF-8 text, allowing 
  // common whitespace but rejecting invalid UTF-8 and control characters. If valid, 
  // return FILE_TYPE_TEXT. If can’t read or rewind the file, or the contents match neither 
  // category, return FILE_TYPE_NEITHER.
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
  return !ferror(file) && remaining_utf8 == 0 ? FILE_TYPE_TEXT : FILE_TYPE_NEITHER;
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

static int prompt_for_filename(int sockfd, char *filename, size_t capacity) {
  printf("Please enter filename: ");
  fflush(stdout);

  for (;;) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);
    int max_fd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

    // wait for input
    int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      perror("Status: 1\nERROR waiting for filename or server response");
      return -1;
    }

    // If the server responds before a filename is entered—or waiting or reading fails—it reports the issue 
    // and returns -1. This lets the client notice server-side errors instead of waiting indefinitely for input.
    if (FD_ISSET(sockfd, &readfds)) {
      uint32_t status, payload_len;
      uint8_t *payload = NULL;
      if (receive_response(sockfd, &status, &payload, &payload_len, BUFSIZE) < 0) {
        fprintf(stderr, "Status: 1\nERROR receiving server response while waiting for filename\n");
      } else if (status == RESPONSE_STATUS_ERROR) {
        report_server_error(payload, payload_len);
      } else if (status == RESPONSE_STATUS_TIMEOUT){
        fprintf(stderr, "Status: 2\nTIMEOUT: ");
        if (payload_len > 0) {
          fwrite(payload, 1, payload_len, stderr);
        }
        fputc('\n', stderr);
      } else {
        fprintf(stderr, "Status: 1\nERROR: unexpected server response while waiting for filename\n");
      }
      free(payload);
      return -1;
    }

    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      if (fgets(filename, (int)capacity, stdin) == NULL) {
        if (feof(stdin)){
          return 1;
        }
        fprintf(stderr, "Status: 1\nERROR reading filename\n");
        return -1;
      }
      filename[strcspn(filename, "\n")] = '\0';
      return 0;
    }
  }
}

int main(int argc, char **argv) {
  struct client_config config;
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  char service[16];
  char filename[BUFSIZE];
  //FILE *file = NULL;
  int sockfd = -1;
  int exit_status = 1;
  //int64_t file_size;
  //uint32_t file_type;
  uint32_t status, payload_len;
  uint8_t *payload = NULL;

  // parse command-line options
  if (process_args(argc, argv, &config) != 0)
    return 1;

  // Configure signal handling
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

  // resolve the server address and connect to it.
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

  // read the server’s acceptance response, then ask for a filename using prompt_for_filename.
  if (receive_response(sockfd, &status, &payload, &payload_len, BUFSIZE) < 0) {
    fprintf(stderr, "Status: 1\nERROR receiving connection response\n");
    goto cleanup;
  }

  if (status == RESPONSE_STATUS_ERROR) {
    exit_status = report_server_error(payload, payload_len);
    goto cleanup;
  }
  free(payload);
  payload = NULL;
  if (status != RESPONSE_STATUS_ACCEPTED || payload_len != 0) {
    fprintf(stderr, "Status: 1\nERROR: invalid connection response\n");
    goto cleanup;
  }

  // Opens and checks the file, determines whether it’s text or an image, and sends the file size and type.
  // Waits for the server’s “ready” response, sends the file, then receives and displays either the text contents or a QR-code payload.
  // On success or failure, frees the response buffer, closes the file and socket, and returns an exit status.
  setvbuf(stdin, NULL, _IONBF, 0);

  for (;;){
    int p = prompt_for_filename(sockfd, filename, sizeof(filename));
    if (p==1) {
      exit_status = 0;
      break;
    }
    if(p < 0){
      break;
    }
    if(filename[0] == '\0') {
      exit_status = 0;
      break;
    }

    int r = send_one_file(sockfd, filename);
    if (r == RESULT_FATAL) break;
  }

cleanup:
  free(payload);
  if (sockfd >= 0){
    close(sockfd);
  }
  return exit_status;
}
