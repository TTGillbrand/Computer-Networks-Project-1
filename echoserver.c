/*
 * echoserver.c - QR code server (threaded, length-prefixed binary protocol)
 * usage: ./echoserver [PORT p] [RATE requests seconds] [MAX_USERS n] [TIME_OUT s]
 *
 * Request  (client -> server): [fsize: u32 BE][ftype: u32 BE][fsize bytes]
 * Response (server -> client): [status: u32 BE][len: u32 BE][len bytes]
 *   status 0 success, 1 failure (no payload, except "server busy"),
 *   2 timeout (message), 3 rate limit exceeded (message)
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include "quirc-master/lib/quirc.h"

// Only build the decoders for formats the client can send (smaller attack surface).
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define QSIZE 5

#define FILE_TYPE_NEITHER 0
#define FILE_TYPE_IMAGE 1
#define FILE_TYPE_TEXT 2

#define RESPONSE_STATUS_SUCCESS 0
#define RESPONSE_STATUS_ERROR 1
#define RESPONSE_STATUS_TIMEOUT 2
#define RESPONSE_STATUS_RATELIMIT 3

// default values for optional arguments.
#define DEFAULT_PORT 2012
#define DEFAULT_RATE_REQUESTS 3
#define DEFAULT_RATE_SECONDS 60
#define DEFAULT_MAX_USERS 3
#define DEFAULT_TIMEOUT_SECONDS 80

// max values to protect server
#define MAX_FILE_SZ UINT32_MAX   // file size is encoded as a 32-bit unsigned integer
#define MAX_PIXELS  (25ULL * 1000 * 1000)   // max decoded image size
#define MAX_URL_SZ  200                     // in characters

struct client_context {
  int connfd;
  struct sockaddr_in clientaddr;
};

struct rate_entry {
  uint32_t client_ip;
  uint16_t client_port;
  struct timespec timestamp;
  struct rate_entry *next;
};

// initialize values to default
static int portno = DEFAULT_PORT;
static int rate_requests = DEFAULT_RATE_REQUESTS;
static int rate_seconds = DEFAULT_RATE_SECONDS;
static int max_users = DEFAULT_MAX_USERS;
static int timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
static int active_users = 0;
static struct rate_entry *rate_entries = NULL;

// stb_image keeps its failure reason in shared global state.
static pthread_mutex_t stbi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t users_cv = PTHREAD_COND_INITIALIZER;

static FILE *logfp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t stop_requested = 0;

// marks that the server should stop accepting new connections.
static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

// escapes quotes, backslashes, and control bytes for log-detail JSON strings.
static void json_escape(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (; *in && o + 7 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c < 0x20 || c == 0x7F) {
      o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
}

//twin helper functions for logging
static void log_event(const char *ip, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void log_event(const char *ip, const char *event, const char *fmt, ...) {
  int saved = errno;
  char ts[32], detail[512], esc[1100];
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

  errno = saved; 
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof detail, fmt, ap);
  va_end(ap);
  json_escape(detail, esc, sizeof esc);

  pthread_mutex_lock(&log_mutex);
  FILE *out = logfp ? logfp : stderr;
  fprintf(out, "%s %s {\"event\":\"%s\",\"detail\":\"%s\"}\n", ts, ip, event, esc);
  fflush(out);
  pthread_mutex_unlock(&log_mutex);
  errno = saved;
}

//helper functions for socket
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

// Returns 1 for EOF before receiving data, 0 on success, and -1 on errors or truncated input.
static int recv_all(int fd, void *buffer, size_t length) {
  size_t received = 0;
  while (received < length) {
    ssize_t result = recv(fd, (char *)buffer + received, length - received, 0);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (result == 0) {
      if (received == 0)
        return 1;
      errno = ECONNRESET;
      return -1;
    }
    received += (size_t)result;
  }
  return 0;
}

// deletes the given bytes
static int discard_bytes(int fd, uint64_t n) {
  uint8_t tmp[1024];
  while (n > 0) {
    size_t want = n < sizeof tmp ? (size_t)n : sizeof tmp;
    ssize_t r = recv(fd, tmp, want, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return 1;
    n -= (uint64_t)r;
  }
  return 0;
}

// in case our buffer is not big enough, this function helps drain the buffer
static void drain_pending(int fd, const char *ip) {
  uint8_t tmp[1024];
  uint64_t total = 0;
  for (;;) {
    ssize_t r = recv(fd, tmp, sizeof tmp, MSG_DONTWAIT);
    if (r > 0) {
      total += (uint64_t)r;
      continue;
    }
    if (r < 0 && errno == EINTR)
      continue;
    break;
  }
  if (total > 0)
    log_event(ip, "extra_bytes_discarded", "%llu bytes beyond declared size",
              (unsigned long long)total);
}

// sends a response status, payload length, and optional payload to the client.
static int send_response(int fd, uint32_t status, const uint8_t *payload, uint32_t payload_len) {
  uint32_t net_status = htonl(status);
  uint32_t net_payload_len = htonl(payload_len);

  if (send_all(fd, &net_status, sizeof(net_status)) < 0 ||
      send_all(fd, &net_payload_len, sizeof(net_payload_len)) < 0)
    return -1;
  if (payload_len > 0 && send_all(fd, payload, payload_len) < 0)
    return -1;
  return 0;
}

// prints a file response status and output to the server console.
static void print_file_response(uint32_t status, uint32_t file_type,
                                const uint8_t *payload, uint32_t payload_len) {
  int lock_result = pthread_mutex_lock(&stdout_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR locking stdout mutex: %s\n", strerror(lock_result));
    return;
  }

  printf("Status: %u\n", status);
  if (status == RESPONSE_STATUS_SUCCESS) {
    if (file_type == FILE_TYPE_TEXT) {
      printf("Text file contents:\n");
    } else {
      printf("URL (%u bytes): ", payload_len);
    }
    if (payload_len > 0)
      fwrite(payload, 1, payload_len, stdout);
    if (payload_len == 0 || payload[payload_len - 1] != '\n')
      fputc('\n', stdout);
  } else if (status == RESPONSE_STATUS_ERROR) {
    fputs("ERROR: ", stdout);
    if (payload_len > 0)
      fwrite(payload, 1, payload_len, stdout);
    else
      fputs("server could not process the file", stdout);
    fputc('\n', stdout);
  } else if (status == RESPONSE_STATUS_TIMEOUT ||
             status == RESPONSE_STATUS_RATELIMIT) {
    if (payload_len > 0)
      fwrite(payload, 1, payload_len, stdout);
    else if (status == RESPONSE_STATUS_TIMEOUT)
      fputs("Connection timed out.", stdout);
    else
      fputs("Rate limit exceeded.", stdout);
    if (payload_len == 0 || payload[payload_len - 1] != '\n')
      fputc('\n', stdout);
  }
  fflush(stdout);

  lock_result = pthread_mutex_unlock(&stdout_mutex);
  if (lock_result != 0)
    fprintf(stderr, "ERROR unlocking stdout mutex: %s\n", strerror(lock_result));
}

// sends a file response and prints the same response on the server console (small-seg test).
static int send_file_response(int fd, uint32_t status, uint32_t file_type,
                              const uint8_t *payload, uint32_t payload_len) {
  int result = send_response(fd, status, payload, payload_len);
  if (result == 0)
    print_file_response(status, file_type, payload, payload_len);
  return result;
}

// sends a string message using the standard response framing.
static int send_message(int fd, uint32_t status, const char *msg) {
  return send_response(fd, status, (const uint8_t *)msg, (uint32_t)strlen(msg));
}

// handles a failed recv_all/discard_bytes: logs, and on timeout tells the client.
static void fail_recv(int fd, const char *ip, int result, const char *what) {
  if (result == 1) {
    log_event(ip, "client_eof", "client closed while reading %s", what);
  } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
    log_event(ip, "timeout", "no data for %d s while reading %s", timeout_seconds, what);
    if (send_message(fd, RESPONSE_STATUS_TIMEOUT, "\nConnection timed out.") < 0) {
      log_event(ip, "send_failed", "could not send timeout message: %m");
    } else {
      static const uint8_t message[] = "Connection timed out.";
      print_file_response(RESPONSE_STATUS_TIMEOUT, FILE_TYPE_NEITHER,
                          message, (uint32_t)(sizeof(message) - 1));
    }
  } else if (errno == ECONNRESET) {
    log_event(ip, "client_reset", "connection reset or truncated while reading %s", what);
  } else {
    log_event(ip, "socket_error", "recv failed reading %s: %m", what);
  }
}

// after a rejection reply, the client's file bytes are still in the socket. Consume them
// so the next 8 bytes we read really are the next request's header.
// Returns 1 to keep the session, 0 to close it.
static int skip_body(int fd, const char *ip, uint32_t fsize) {
  if (fsize == 0)
    return 1;
  int r = discard_bytes(fd, fsize);
  if (r == 0)
    return 1;
  fail_recv(fd, ip, r, "rejected file body");
  return 0;
}

// Sends a rejection (msg == NULL means code 1 with no payload), then skips the body.
static int reject_and_skip(int fd, const char *ip, uint32_t status, const char *msg,
                           uint32_t fsize, uint32_t file_type) {
  const uint8_t *payload = (const uint8_t *)msg;
  uint32_t payload_len = msg ? (uint32_t)strlen(msg) : 0;
  int rc = send_file_response(fd, status, file_type, payload, payload_len);
  if (rc < 0) {
    log_event(ip, "send_failed", "could not send rejection: %m");
    return 0;
  }
  return skip_body(fd, ip, fsize);
}


/*
    checks that it is acceptiable ASCII, including whitespace, newline, etc
    returns 1 if text is vlaid, 0 if not
*/
static int is_valid_text(const uint8_t *data, size_t length) {
  int remaining_utf8 = 0;
  uint8_t next_min = 0x80;
  uint8_t next_max = 0xBF;

  for (size_t i = 0; i < length; i++) {
    uint8_t byte = data[i];
    if (remaining_utf8 > 0) {
      if (byte < next_min || byte > next_max)
        return 0;
      remaining_utf8--;
      next_min = 0x80;
      next_max = 0xBF;
    } else if (byte <= 0x7F) {
      if (byte != '\t' && byte != '\n' && byte != '\v' &&
          byte != '\f' && byte != '\r' &&
          (byte < 0x20 || byte == 0x7F))
        return 0;
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
      return 0;
    }
  }
  return remaining_utf8 == 0;
}

/*
   converts given string to a number, returning 0 on success
*/
static int parse_positive_int(const char *text, int *value) {
  char *end = NULL;
  errno = 0;
  long parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      parsed < 1 || parsed > INT_MAX)
    return -1;
  *value = (int)parsed;
  return 0;
}

// returns whether arg matches either of the two supplied names.
static int opt_is(const char *arg, const char *a, const char *b) {
  return strcmp(arg, a) == 0 || strcmp(arg, b) == 0;
}

// parses and validates the server's command-line options.
static int process_args(int argc, char **argv) {
  int seen_port = 0, seen_rate = 0, seen_users = 0, seen_timeout = 0;
  int i = 1;
  while (i < argc) {
    int value = 0;
    if (strcmp(argv[i], "PORT") == 0) {
      // port already seen, no value given, value not a number, or outside 2000-3000
      if (seen_port++ || i + 1 >= argc ||
          parse_positive_int(argv[i + 1], &value) < 0 ||
          value < 2000 || value > 3000) {
        fprintf(stderr, "Usage: %s [PORT 2000-3000] [RATE requests seconds] "
                        "[MAX_USERS users] [TIME_OUT seconds]\n", argv[0]);
        return -1;
      }
      portno = value;
      i += 2;
    } else if (strcmp(argv[i], "RATE") == 0) {
      // rate already seen, not enough values, or either value invalid
      if (seen_rate++ || i + 2 >= argc ||
          parse_positive_int(argv[i + 1], &rate_requests) < 0 ||
          parse_positive_int(argv[i + 2], &rate_seconds) < 0) {
        fprintf(stderr, "Invalid or duplicate RATE option.\n");
        return -1;
      }
      i += 3;
    } else if (opt_is(argv[i], "MAXUSERS", "MAX_USERS")) {
      if (seen_users++ || i + 1 >= argc ||
          parse_positive_int(argv[i + 1], &max_users) < 0) {
        fprintf(stderr, "Invalid or duplicate MAX_USERS option.\n");
        return -1;
      }
      i += 2;
    } else if (opt_is(argv[i], "TIMEOUT", "TIME_OUT")) {
      if (seen_timeout++ || i + 1 >= argc ||
          parse_positive_int(argv[i + 1], &timeout_seconds) < 0) {
        fprintf(stderr, "Invalid or duplicate TIME_OUT option.\n");
        return -1;
      }
      i += 2;
    } else {
      fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
      return -1;
    }
  }
  return 0;
}

// tracks and enforces the request limit independently for each client.
static int rate_allows_request(const struct sockaddr_in *clientaddr) {
  struct timespec now;

  // gets the current monotonic time
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
    perror("ERROR reading monotonic clock");
    return -1;
  }

  // locks the shared rate-limit data, removes expired request records, and counts the remaining records
  // for that IP. If the client has already reached rate_requests within rate_seconds,
  // it returns 0 (denied). Otherwise, it records this request and returns 1 (allowed).
  // It returns -1 if it can't read the clock, lock the data, or allocate a record.
  int lock_result = pthread_mutex_lock(&rate_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR locking rate-limit state: %s\n", strerror(lock_result));
    return -1;
  }

  struct rate_entry **entry = &rate_entries;
  int client_requests = 0;
  while (*entry != NULL) {
    struct rate_entry *current = *entry;
    time_t elapsed = now.tv_sec - current->timestamp.tv_sec;
    int expired = elapsed > rate_seconds ||
                  (elapsed == rate_seconds && now.tv_nsec >= current->timestamp.tv_nsec);
    if (expired) {
      *entry = current->next;
      free(current);
      continue;
    }

    if (current->client_ip == clientaddr->sin_addr.s_addr &&
        current->client_port == clientaddr->sin_port)
      client_requests++;
    entry = &current->next;
  }

  if (client_requests >= rate_requests) {
    lock_result = pthread_mutex_unlock(&rate_mutex);
    if (lock_result != 0)
      fprintf(stderr, "ERROR unlocking rate-limit state: %s\n", strerror(lock_result));
    return 0;
  }

  struct rate_entry *request = malloc(sizeof(*request));
  if (request == NULL) {
    perror("ERROR allocating rate-limit state");
    lock_result = pthread_mutex_unlock(&rate_mutex);
    if (lock_result != 0)
      fprintf(stderr, "ERROR unlocking rate-limit state: %s\n", strerror(lock_result));
    return -1;
  }
  request->client_ip = clientaddr->sin_addr.s_addr;
  request->client_port = clientaddr->sin_port;
  request->timestamp = now;
  request->next = rate_entries;
  rate_entries = request;

  lock_result = pthread_mutex_unlock(&rate_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR unlocking rate-limit state: %s\n", strerror(lock_result));
    return -1;
  }
  return 1;
}


/*
  handles a single request, and then returns a value to declare if the client should be kept open:
  1 - kept open
  0 - close
*/
static int process_client_request(int connfd, const char *ip, struct sockaddr_in clientaddr) {
  uint32_t fsize, ftype;

  // read file size from the client
  int result = recv_all(connfd, &fsize, sizeof(fsize));
  if (result == 1)
    return 0;                          // clean close between requests; client_thread logs it
  if (result < 0) {
    fail_recv(connfd, ip, result, "file size");
    return 0;
  }
  fsize = ntohl(fsize);

  // read file type from client
  result = recv_all(connfd, &ftype, sizeof(ftype));
  if (result != 0) {
    fail_recv(connfd, ip, result, "file type");
    return 0;
  }
  ftype = ntohl(ftype);

  // Checks the client's request rate and rejects requests that exceed the limit.
  int rate_result = rate_allows_request(&clientaddr);
  if (rate_result == 0) {
    char message[160];
    snprintf(message, sizeof(message),
             "Rate limit exceeded: maximum %d requests per %d seconds.",
             rate_requests, rate_seconds);
    log_event(ip, "rate_limited", "%d requests per %d s exceeded (fsize=%u)",
              rate_requests, rate_seconds, fsize);
    return reject_and_skip(connfd, ip, RESPONSE_STATUS_RATELIMIT, message, fsize, ftype);
  }
  if (rate_result < 0) {
    log_event(ip, "rate_error", "could not enforce the rate limit");
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  // Rejects oversized files, empty files, and unsupported file types (session stays open).
  if (fsize > UINT32_MAX) {
    log_event(ip, "file_too_large", "%u bytes (max %u)", fsize, UINT32_MAX);
    return reject_and_skip(connfd, ip, RESPONSE_STATUS_ERROR, NULL, fsize, ftype);
  }
  if (fsize == 0 || (ftype != FILE_TYPE_IMAGE && ftype != FILE_TYPE_TEXT)) {
    log_event(ip, "bad_type", "fsize=%u ftype=%u", fsize, ftype);
    return reject_and_skip(connfd, ip, RESPONSE_STATUS_ERROR, NULL, fsize, ftype);
  }

  uint8_t *buffer = malloc((size_t)fsize);
  if (buffer == NULL) {
    log_event(ip, "alloc_failed", "could not allocate %u bytes for upload", fsize);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;                          // body not consumed, so close
  }

  // read exactly fsize bytes; the socket receive timeout covers inactivity
  result = recv_all(connfd, buffer, (size_t)fsize);
  if (result != 0) {
    free(buffer);
    fail_recv(connfd, ip, result, "file body");
    return 0;
  }
  // discard anything beyond the declared size that has already arrived
  drain_pending(connfd, ip);
  log_event(ip, "file_received", "%u bytes type=%s", fsize,
            ftype == FILE_TYPE_TEXT ? "text" : "image");

  /*
    If the file is text, validates it as UTF-8, prints it, and sends the text back (code 0).
    If it's an image, decodes it and searches for a QR code. It sends back the QR payload if found,
    or code 1 otherwise.
  */
  if (ftype == FILE_TYPE_TEXT) {
    if (!is_valid_text(buffer, fsize)) {
      free(buffer);
      log_event(ip, "invalid_text", "received text file is not valid UTF-8 text");
      return send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0) == 0;
    }

    int response_result = send_file_response(connfd, RESPONSE_STATUS_SUCCESS, ftype, buffer, fsize);
    free(buffer);
    if (response_result < 0) {
      log_event(ip, "send_failed", "could not send text response: %m");
      return 0;
    }
    log_event(ip, "text_ok", "echoed %u bytes", fsize);
    return 1;
  }

  // ---- image: decode to an 8-bit greyscale pixel array ----
  int w = 0, h = 0, channels = 0;
  int mutex_result = pthread_mutex_lock(&stbi_mutex);
  if (mutex_result != 0) {
    log_event(ip, "internal_error", "locking image decoder mutex: %s", strerror(mutex_result));
    free(buffer);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  // Check dimensions from the header first so a tiny file can't expand to gigabytes.
  const char *reason = NULL;
  uint8_t *pixels = NULL;
  if (!stbi_info_from_memory(buffer, (int)fsize, &w, &h, &channels)) {
    reason = stbi_failure_reason();
  } else if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > MAX_PIXELS) {
    reason = "image dimensions exceed limit";
  } else {
    pixels = stbi_load_from_memory(buffer, (int)fsize, &w, &h, &channels, 1);
    if (!pixels)
      reason = stbi_failure_reason();
  }
  char reason_copy[128];               // copy while still holding the lock
  snprintf(reason_copy, sizeof reason_copy, "%s", reason ? reason : "unknown");

  mutex_result = pthread_mutex_unlock(&stbi_mutex);
  free(buffer);
  if (mutex_result != 0) {
    log_event(ip, "internal_error", "unlocking image decoder mutex: %s", strerror(mutex_result));
    if (pixels)
      stbi_image_free(pixels);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  if (!pixels) {
    log_event(ip, "image_rejected", "%s", reason_copy);
    return send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0) == 0;
  }

  struct quirc *qr = quirc_new();
  if (!qr) {
    log_event(ip, "alloc_failed", "quirc_new failed");
    stbi_image_free(pixels);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  // QR decoder couldn't resize its working area to fit the image.
  if (quirc_resize(qr, w, h)) {
    log_event(ip, "alloc_failed", "quirc_resize failed for %dx%d", w, h);
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  int qw, qh;
  uint8_t *qimage = quirc_begin(qr, &qw, &qh);
  if (!qimage) {
    log_event(ip, "internal_error", "quirc_begin returned no image buffer");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
    return 0;
  }

  memcpy(qimage, pixels, (size_t)w * (size_t)h);
  stbi_image_free(pixels);
  quirc_end(qr);

  int num_codes = quirc_count(qr);
  if (num_codes == 0)
    log_event(ip, "qr_not_found", "no QR code found in %dx%d image", w, h);

  uint8_t url[MAX_URL_SZ];
  uint32_t url_len = 0;
  int have_url = 0;
  for (int i = 0; i < num_codes; i++) {
    struct quirc_code code;
    struct quirc_data data;

    quirc_extract(qr, i, &code);
    quirc_decode_error_t err = quirc_decode(&code, &data);
    if (err) {
      // A QR pattern was found, but its contents couldn't be decoded.
      log_event(ip, "qr_decode_failed", "code %d of %d: %s", i + 1, num_codes, quirc_strerror(err));
      continue;
    }
    if (data.payload_len > 0 && data.payload_len <= MAX_URL_SZ) {
      memcpy(url, data.payload, (size_t)data.payload_len);
      url_len = (uint32_t)data.payload_len;
      have_url = 1;
      log_event(ip, "qr_ok", "decoded %u bytes: %.*s", url_len, (int)url_len, (const char *)url);
      break;
    }
    // decoded, but contents were empty or too large to handle
    log_event(ip, "qr_bad_payload", "payload length %d (max %d)", data.payload_len, MAX_URL_SZ);
  }

  int response_result;
  if (have_url)
    response_result = send_file_response(connfd, RESPONSE_STATUS_SUCCESS, ftype, url, url_len);
  else
    response_result = send_file_response(connfd, RESPONSE_STATUS_ERROR, ftype, NULL, 0);
  quirc_destroy(qr);

  if (response_result < 0) {
    log_event(ip, "send_failed", "could not send decode response: %m");
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Connection management                                               */
/* ------------------------------------------------------------------ */

/*
  decrements the server's shared active_users count when a client connection is finished or setup fails.
  Locks users_mutex while changing the count, wakes the shutdown waiter, then unlocks.
*/
static void release_user_slot(void) {
  int lock_result = pthread_mutex_lock(&users_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR locking user count: %s\n", strerror(lock_result));
    return;
  }
  active_users--;
  pthread_cond_broadcast(&users_cv);
  lock_result = pthread_mutex_unlock(&users_mutex);
  if (lock_result != 0)
    fprintf(stderr, "ERROR unlocking user count: %s\n", strerror(lock_result));
}

/*
  Handles a new connection when the server has already reached its max_users limit. It sends the client
  a "Server is busy" error, shuts down the write side so the response can finish, then closes the socket.
  It doesn't start a client-handling thread or take up an active-user slot.
*/
static void reject_busy_connection(int connfd, const char *ip) {
  log_event(ip, "busy_rejected", "server full (max_users=%d)", max_users);
  if (send_message(connfd, RESPONSE_STATUS_ERROR, "Server is busy. Please try again later.") < 0)
    log_event(ip, "send_failed", "could not send busy response: %m");

  if (shutdown(connfd, SHUT_WR) < 0 && errno != ENOTCONN)
    log_event(ip, "socket_error", "shutdown of busy connection failed: %m");
  close(connfd);
}

/*
  Handles a client connection in its own thread. It retrieves the socket and client address from the
  supplied context, then frees that context. It sends an acceptance response, then repeatedly calls
  process_client_request while the session stays aligned. When the connection ends it logs the
  disconnect, closes the socket, and releases the user slot.
*/
static void *client_thread(void *arg) {
  struct client_context *context = arg;
  int connfd = context->connfd;
  struct sockaddr_in clientaddr = context->clientaddr;
  free(context);

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof ip);
  log_event(ip, "connect", "client port %u", (unsigned)ntohs(clientaddr.sin_port));

  if (send_response(connfd, RESPONSE_STATUS_SUCCESS, NULL, 0) < 0) {
    log_event(ip, "send_failed", "could not send connection acceptance: %m");
  } else {
    while (process_client_request(connfd, ip, clientaddr)) {}
  }

  log_event(ip, "disconnect", "client port %u; closing connection",
            (unsigned)ntohs(clientaddr.sin_port));
  close(connfd);
  release_user_slot();
  return NULL;
}

// logs a fatal startup error, releases initialized resources, and exits. 
static void fatal(const char *msg, int listenfd, pthread_attr_t *attr) {
  log_event("0.0.0.0", "fatal", "%s: %m", msg);
  perror(msg);
  if (listenfd >= 0)
    close(listenfd);
  if (attr)
    pthread_attr_destroy(attr);
  if (logfp)
    fclose(logfp);
  exit(1);
}

// configures the server, accepts client connections, and shuts down cleanly.
int main(int argc, char **argv) {
  int listenfd = -1;                   // listening socket
  int connfd;                          // connection socket
  socklen_t clientlen;                 // byte size of client's address
  struct sockaddr_in serveraddr;       // server's addr
  struct sockaddr_in clientaddr;       // client addr
  int optval;                          // flag value for setsockopt
  struct sigaction action;
  pthread_attr_t thread_attr;

  //Port parsing, rate requesting, critical clients, and somthing alliterative with timeout
  if (process_args(argc, argv) < 0)
    return 1;

  //opening with w so that each time we run the server we get a new log file
  logfp = fopen("qrserver.log", "w"); 
  if (!logfp)
    perror("WARNING: could not open qrserver.log, logging to stderr");
  log_event("0.0.0.0", "server_start", "port=%d rate=%d/%ds max_users=%d timeout=%ds",
            portno, rate_requests, rate_seconds, max_users, timeout_seconds);

  // SIGPIPE ignored so a dead client gives EPIPE instead of killing the server.
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPIPE, &action, NULL) < 0)
    fatal("ERROR configuring SIGPIPE", -1, NULL);

  // SIGINT/SIGTERM request a clean shutdown. No SA_RESTART, so accept() returns EINTR.
  struct sigaction stop_action;
  memset(&stop_action, 0, sizeof(stop_action));
  stop_action.sa_handler = on_signal;
  sigemptyset(&stop_action.sa_mask);
  if (sigaction(SIGINT, &stop_action, NULL) < 0 ||
      sigaction(SIGTERM, &stop_action, NULL) < 0)
    fatal("ERROR configuring stop signals", -1, NULL);

  // Detached worker-thread settings.
  int thread_result = pthread_attr_init(&thread_attr);
  if (thread_result != 0) {
    fprintf(stderr, "ERROR initializing thread attributes: %s\n", strerror(thread_result));
    if (logfp)
      fclose(logfp);
    return 1;
  }
  thread_result = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
  if (thread_result != 0) {
    fprintf(stderr, "ERROR configuring detached threads: %s\n", strerror(thread_result));
    pthread_attr_destroy(&thread_attr);
    if (logfp)
      fclose(logfp);
    return 1;
  }

  // Create a TCP socket
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    fatal("ERROR opening socket", -1, &thread_attr);

  optval = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int)) < 0)
    fatal("ERROR setting socket options", listenfd, &thread_attr);

  // build the server's internet address
  memset(&serveraddr, 0, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);

  if (bind(listenfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
    fatal("ERROR on binding", listenfd, &thread_attr);

  if (listen(listenfd, QSIZE) < 0)
    fatal("ERROR on listen", listenfd, &thread_attr);

  // accept loop: runs until SIGINT/SIGTERM
  while (!stop_requested) {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
    if (connfd < 0) {
      if (errno == EINTR)
        continue;
      log_event("0.0.0.0", "accept_error", "%m");
      continue;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof ip);

    // inactivity timeout: recv() fails with EAGAIN after this long without data
    struct timeval timeout = {timeout_seconds, 0};
    if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      log_event(ip, "socket_error", "could not set receive timeout: %m");
      close(connfd);
      continue;
    }

    int lock_result = pthread_mutex_lock(&users_mutex);
    if (lock_result != 0) {
      fprintf(stderr, "ERROR locking user count: %s\n", strerror(lock_result));
      close(connfd);
      continue;
    }

    // Check client limit; reserve a slot if there is room.
    int server_busy = active_users >= max_users;
    if (!server_busy)
      active_users++;
    lock_result = pthread_mutex_unlock(&users_mutex);
    if (lock_result != 0) {
      fprintf(stderr, "ERROR unlocking user count: %s\n", strerror(lock_result));
      if (!server_busy)
        release_user_slot();
      close(connfd);
      continue;
    }
    if (server_busy) {
      reject_busy_connection(connfd, ip);
      continue;
    }

    // If setup fails, close the socket and release the reserved client slot.
    struct client_context *context = malloc(sizeof(*context));
    if (context == NULL) {
      log_event(ip, "alloc_failed", "could not allocate client context");
      close(connfd);
      release_user_slot();
      continue;
    }
    context->connfd = connfd;
    context->clientaddr = clientaddr;

    pthread_t worker;
    thread_result = pthread_create(&worker, &thread_attr, client_thread, context);
    if (thread_result != 0) {
      log_event(ip, "thread_error", "pthread_create: %s", strerror(thread_result));
      free(context);
      close(connfd);
      release_user_slot();
    }
  }

  // ---- clean shutdown ----
  log_event("0.0.0.0", "server_stop", "shutdown requested");
  close(listenfd);

  // wait for every client thread to finish and release its slot
  pthread_mutex_lock(&users_mutex);
  while (active_users > 0)
    pthread_cond_wait(&users_cv, &users_mutex);
  pthread_mutex_unlock(&users_mutex);

  // free the rate-limit list
  pthread_mutex_lock(&rate_mutex);
  while (rate_entries) {
    struct rate_entry *n = rate_entries->next;
    free(rate_entries);
    rate_entries = n;
  }
  pthread_mutex_unlock(&rate_mutex);

  pthread_attr_destroy(&thread_attr);
  if (logfp) {
    fclose(logfp);
    logfp = NULL;
  }
  return 0;
}