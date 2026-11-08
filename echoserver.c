  /*
  * echoserver.c - A simple connection-based echo server
  * usage: echoserver <port>
  */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
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

  #define STB_IMAGE_IMPLEMENTATION
  #include "stb_image.h"

#define BUFSIZE 1024
#define QSIZE 5

  #define FILE_TYPE_NEITHER 0
  #define FILE_TYPE_IMAGE 1
  #define FILE_TYPE_TEXT 2

#define RESPONSE_STATUS_ACCEPTED 0
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

  struct client_context {
  int connfd;
  struct sockaddr_in clientaddr;
};

struct rate_entry {
  uint32_t client_ip;
  struct timespec timestamp;
  struct rate_entry *next;
};

// initialize values to default
static   int portno = DEFAULT_PORT;
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

// wrapper for perror
void error(char *msg) {
  perror(msg);
  exit(1);
}
  //wrapper for perror
  void error(char *msg) {
    perror(msg);
    exit(1);
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
      return 1;
    }
    if (result == 0)
      return 0;
    received += (size_t)result;
  }
  return 1;
}

static int send_response(int fd, uint32_t status, const uint8_t *payload, uint32_t payload_len) {
  uint32_t net_status = htonl(status);
  uint32_t net_payload_len = htonl(payload_len);

  if (send_all(fd, &net_status, sizeof(net_status)) < 0 || send_all(fd, &net_payload_len, sizeof(net_payload_len)) < 0)
    return -1;
  if (payload_len > 0 && send_all(fd, payload, payload_len) < 0)
    return -1;
  return 0;
}

static int send_error_response(int fd, const char *message) {
  return send_response(fd, RESPONSE_STATUS_ERROR, (const uint8_t *)message,
                       (uint32_t)strlen(message));
}

/* 
    Accepts printable ASCII and the whitespace characters tab, newline, vertical tab, form feed, and return. 
    Rejects other ASCII control bytes and DEL.
    Checks that non-ASCII bytes form valid UTF-8 sequences, including rejecting invalid start or 
    continuation bytes, overlong encodings, surrogate values, and code points beyond Unicode’s limit.
    Rejects a buffer that ends partway through a UTF-8 sequence.
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
    Converts a string to a positive int and stores it in the variable pointed to by value.
    Uses strtol to parse a base-10 number, then rejects the input if it is empty, has trailing non-number 
    characters, causes a conversion error or overflow, or is less than 1 or greater than INT_MAX. 
    It returns 0 on success and -1 on failure.
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

// ai log here
static int process_args(int argc, char **argv) {
  int seen_port = 0, seen_rate = 0, seen_users = 0, seen_timeout = 0;
  int i = 1;

  while (i < argc) {
    int value;
    if (strcmp(argv[i], "PORT") == 0) {
      // checks if port already seen, port not given a value, or given value is outside valid range
      if (seen_port++ || i + 1 >= argc || value < 2000 || value > 3000) {
        fprintf(stderr, "Usage: %s [PORT 2000-3000] [RATE requests seconds] " "[MAX USERS users] [TIME OUT seconds]\n", argv[0]);
        return -1;
      }
      portno = value;
      i += 2;
    } else if (strcmp(argv[i], "RATE") == 0) {
      // checks if rate already seen, rate not given enough values, or either given value is invalid 
      if (seen_rate++ || i + 2 >= argc || parse_positive_int(argv[i + 1], &rate_requests) < 0 || parse_positive_int(argv[i + 2], &rate_seconds) < 0) {
        fprintf(stderr, "Invalid or duplicate RATE option.\n");
        return -1;
      }
      i += 3;
    } else if (strcmp(argv[i], "MAXUSERS") == 0) {
      // checks if max users already seen, max users not given a value, or given value is invalid 
      if (seen_users++ || i + 1 >= argc || parse_positive_int(argv[i + 2], &max_users) < 0) {
        fprintf(stderr, "Invalid or duplicate MAX USERS option.\n");
        return -1;
      }
      i += 3;
    } else if (strcmp(argv[i], "TIMEOUT") == 0) {
      // checks if time already seen, time not given a value, or given value is invalid 
      if (seen_timeout++ || i + 1 >= argc || parse_positive_int(argv[i + 2], &timeout_seconds) < 0) {
        fprintf(stderr, "Invalid or duplicate TIME OUT option.\n");
        return -1;
      }
      i += 3;
    } else {
      fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
      return -1;
    }
  }
  return 0;
}

static int is_timeout_error(int result) {
  return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static int rate_allows_request(uint32_t client_ip) {
  struct timespec now;

  // gets the current monotonic time
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
    perror("ERROR reading monotonic clock");
    return -1;
  }

  // locks the shared rate-limit data, removes expired request records, and counts the remaining records 
  // for that IP. If the client has already reached rate_requests within rate_seconds, 
  // it returns 0 (denied). Otherwise, it records this request and returns 1 (allowed). 
  // It returns -1 if it can’t read the clock, lock the data, or allocate a record.
  // Mutex makes this process safe when multiple threads run at once.
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
    int expired = elapsed > rate_seconds || (elapsed == rate_seconds && now.tv_nsec >= current->timestamp.tv_nsec);
    if (expired) {
      *entry = current->next;
      free(current);
      continue;
    }
    
    if (current->client_ip == client_ip)
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
      fprintf(stderr, "ERROR unlocking rate-limit state: %s\n",
              strerror(lock_result));
    return -1;
  }
  request->client_ip = client_ip;
  request->timestamp = now;
  request->next = rate_entries;
  rate_entries = request;

  lock_result = pthread_mutex_unlock(&rate_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR unlocking rate-limit state: %s\n",
            strerror(lock_result));
    return -1;
  }
  return 1;
}

static int process_client_request(int connfd, struct sockaddr_in clientaddr) {
  uint32_t fsize, ftype;
  char hostname[NI_MAXHOST];
  char portname[NI_MAXSERV];
  int name_result = getnameinfo((struct sockaddr *)&clientaddr, sizeof(clientaddr), hostname, sizeof(hostname),
                                portname, sizeof(portname), NI_NUMERICHOST | NI_NUMERICSERV);
  // log client address                            
  if (name_result == 0)
    printf("server established connection with %s:%s\n", hostname, portname);
  
  // read file size from the client
  int result = recv_all(connfd, &fsize, sizeof(fsize));
  if (result != 1) {
    int timed_out = is_timeout_error(result);
    if (timed_out) {
      fprintf(stderr, "Status: 1\nERROR: connection timed out after %d seconds\n",
              timeout_seconds);
      if (send_error_response(connfd, "Connection timed out.") < 0)
        perror("Status: 1\nERROR sending timeout response");
    }
    fprintf(stderr, "Status: 1\nERROR reading fsize from socket%s\n",
            result == 0 ? ": client disconnected" : "");
    return 0;
  }
  fsize = ntohl(fsize);

  // read file type from client
  result = recv_all(connfd, &ftype, sizeof(ftype));
  if (result != 1) {
    int timed_out = is_timeout_error(result);
    if (timed_out) {
      fprintf(stderr, "Status: 1\nERROR: connection timed out after %d seconds\n",
              timeout_seconds);
      if (send_error_response(connfd, "Connection timed out.") < 0)
        perror("Status: 1\nERROR sending timeout response");
    }
    fprintf(stderr, "Status: 1\nERROR reading file type from socket%s\n",
            result == 0 ? ": client disconnected" : "");
    return 0;
  }
  ftype = ntohl(ftype);

  // Checks the client’s request rate and rejects requests that exceed the limit.
  int rate_result = rate_allows_request(clientaddr.sin_addr.s_addr);
  if (rate_result == 0) {
    char message[160];
    snprintf(message, sizeof(message),
             "Rate limit exceeded: maximum %d requests per %d seconds.", rate_requests, rate_seconds);
    fprintf(stderr, "Status: 3\nERROR: %s\n", message);
    return send_error_response(connfd, message) == 0;
  }
  if (rate_result < 0) {
    send_error_response(connfd, "Server could not enforce the rate limit.");
    return 0;
  }

  // Rejects oversized files, empty files, and unsupported file types.
  if (fsize > MAX_FILE_SZ) {
    fprintf(stderr, "Status: 1\nERROR: file too large: %u bytes (maximum %u)\n",
            fsize, (unsigned int)MAX_FILE_SZ);
    send_error_response(connfd, "File is too large for the server.");
    return 0;
  }

  if (fsize == 0 || (ftype != FILE_TYPE_IMAGE && ftype != FILE_TYPE_TEXT)) {
    fprintf(stderr, "Status: 1\nERROR: invalid or unsupported file\n");
    send_error_response(connfd, "File is empty or is not a supported image or text file.");
    return 0;
  }

  // Tells the client it is ready, allocates a buffer, and receives the file data.
  if (send_response(connfd, RESPONSE_STATUS_READY, NULL, 0) < 0) {
    perror("Status: 1\nERROR acknowledging upload");
    return 0;
  }

  uint8_t *buffer = malloc((size_t)fsize);
  if (buffer == NULL) {
    perror("Status: 1\nFailed to allocate image buffer");
    send_error_response(connfd, "Server could not allocate memory for the upload.");
    return 0;
  }

  // times user out after extended inactivity
  result = recv_all(connfd, buffer, (size_t)fsize);
  if (result != 1) {
    int timed_out = is_timeout_error(result);
    if (timed_out) {
      fprintf(stderr, "Status: 1\nERROR: connection timed out after %d seconds\n",
              timeout_seconds);
      if (send_error_response(connfd, "Connection timed out.") < 0)
        perror("Status: 1\nERROR sending timeout response");
    } else if (result == 0)
      fprintf(stderr, "Status: 1\nERROR: client disconnected before sending all %u bytes\n", fsize);
    else
      perror("Status: 1\nERROR receiving binary file data");
    free(buffer);
    if (!timed_out && send_error_response(connfd, "Server did not receive the complete file.") < 0)
      perror("Status: 1\nERROR sending incomplete-file response");
    return 0;
  }

    printf("Status: 0\nSUCCESS: File completely received! Total bytes: %u\n", fsize);

  /*
    If the file text, validates it as UTF-8, prints it, and sends the text back.
    If it’s an image, decodes it and searches for a QR code. It sends back the QR payload if found, 
    or an error otherwise.
    It returns 1 when sending the final response succeeds and 0 when the request fails or the 
    response cannot be sent.
  */
  if (ftype == FILE_TYPE_TEXT) {
    // checks to make sure that text sent is valid
    if (!is_valid_text(buffer, fsize)) {
      fprintf(stderr, "Status: 1\nERROR: received text file is not valid UTF-8 text\n");
      free(buffer);
      send_error_response(connfd, "File is not valid UTF-8 text.");
      return 0;
    }

    // when the file is text
    int mutex_result = pthread_mutex_lock(&stdout_mutex);
    if (mutex_result != 0) {
      fprintf(stderr, "ERROR locking stdout mutex: %s\n", strerror(mutex_result));
    } else {
      printf("Received text file contents (%u bytes):\n", fsize);
      if (fwrite(buffer, 1, fsize, stdout) != fsize)
        perror("ERROR printing received text file");
      if (fsize == 0 || buffer[fsize - 1] != '\n')
        putchar('\n');
      fflush(stdout);
      mutex_result = pthread_mutex_unlock(&stdout_mutex);
      if (mutex_result != 0)
        fprintf(stderr, "ERROR unlocking stdout mutex: %s\n",
                strerror(mutex_result));
    }

    // sends content to client
    int response_result = send_response(connfd, RESPONSE_STATUS_TEXT, buffer, fsize);
    free(buffer);
    if (response_result < 0)
      perror("Status: 1\nERROR sending response to client");
    return response_result == 0;
  }

  // decoding an image into greyscale pixel array
  int w, h, channels;
  int mutex_result = pthread_mutex_lock(&stbi_mutex);

  // The server couldn’t lock or unlock the image decoder safely. 
  if (mutex_result != 0) {
    fprintf(stderr, "Status: 1\nERROR locking image decoder mutex: %s\n", strerror(mutex_result));
    free(buffer);
    send_error_response(connfd, "Server could not process the image.");
    return 0;
  }
  uint8_t *pixels = stbi_load_from_memory(buffer, fsize, &w, &h, &channels, 1);
  if (!pixels)
    fprintf(stderr, "Status: 1\nFailed to decode image: %s\n", stbi_failure_reason());
  mutex_result = pthread_mutex_unlock(&stbi_mutex);
  if (mutex_result != 0) {
    fprintf(stderr, "Status: 1\nERROR unlocking image decoder mutex: %s\n", strerror(mutex_result));
    if (pixels)
      stbi_image_free(pixels);
    free(buffer);
    send_error_response(connfd, "Server could not process the image.");
    return 0;
  }
  free(buffer);

  // send error if decoding fails
  if(!pixels){
    send_error_response(connfd, "File is not a valid image containing a QR code.");
    return 0;
  }
  struct quirc *qr = quirc_new();
  if(!qr){
    perror("Status: 1\nFailed to allocate memory");
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not allocate memory to decode the QR code.");
    return 0;
  }

  // QR decoder couldn’t resize its working area to fit the image or provide a buffer for the image pixels.
  if(quirc_resize(qr, w, h)){
    perror("Status: 1\nFailed to allocate memory");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not decode the QR image.");
    return 0;
  }

  int qw, qh; // get these from the passed image
  
  uint8_t *qimage = quirc_begin(qr, &qw, &qh);
  if (!qimage) {
    fprintf(stderr, "Status: 1\nFailed to obtain QR image buffer\n");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not decode the QR image.");
    return 0;
  }
  
  memcpy(qimage, pixels, (size_t)w*h);
  stbi_image_free(pixels);
  quirc_end(qr);

    int num_codes = quirc_count(qr);
    printf("Found %d QR codes\n", num_codes);

  // create url
  uint8_t url[MAX_URL_SZ];
  uint32_t url_len = 0;
  int have_url = 0;
  for(int i = 0; i < num_codes; i++){
    struct quirc_code code;
    struct quirc_data data;
    quirc_decode_error_t err;

      quirc_extract(qr, i, &code);

    // A QR pattern was found, but its contents couldn’t be decoded. 
    err = quirc_decode(&code, &data);
    if (err) {
      printf("Status: 1\nDECODE FAILED: %s\n", quirc_strerror(err));

    }
    else {
      printf("Data: %s\n", data.payload);
      if (data.payload_len > 0 && data.payload_len <= MAX_URL_SZ) {
        memcpy(url, data.payload, data.payload_len);
        url_len = (uint32_t)data.payload_len;
        have_url = 1;
        break;
      }
      // QR code decoded, but its contents were empty or too large to handle.
      fprintf(stderr, "QR payload is empty or exceeds %d bytes\n", MAX_URL_SZ);
    }
  }

  int response_result;
  if (have_url) {
    response_result = send_response(connfd, RESPONSE_STATUS_ACCEPTED, url, url_len);
  } else {
    // No QR code produced a usable payload
    response_result = send_error_response(
        connfd, "Image does not contain a valid QR code with a supported payload.");
  }
  if (response_result < 0)
    perror("Status: 1\nERROR sending response to client");

  quirc_destroy(qr);
  return response_result == 0;
}

/*
  decrements the server’s shared active_users count when a client connection is finished or setup fails. 
  It locks users_mutex while changing the count so other threads can’t update it at the same time, then unlocks it.
  Frees a slot so the server can accept another client.
*/
static void release_user_slot(void) {
  int lock_result = pthread_mutex_lock(&users_mutex);
  if (lock_result != 0) {
    fprintf(stderr, "ERROR locking user count: %s\n", strerror(lock_result));
    return;
  }
  active_users--;
  lock_result = pthread_mutex_unlock(&users_mutex);
  if (lock_result != 0)
    fprintf(stderr, "ERROR unlocking user count: %s\n",
            strerror(lock_result));
}

/*
  Handles a new connection when the server has already reached its max_users limit. It sends the client 
  a “Server is busy” error, shuts down the connection’s writing side so the response can finish, then closes 
  the socket.
  It doesn’t start a client-handling thread or take up an active-user slot.
*/
static void reject_busy_connection(int connfd) {
  if (send_error_response(connfd,
                          "Server is busy. Please try again later.") < 0)
    perror("Status: 1\nERROR sending busy response");

  if (shutdown(connfd, SHUT_WR) < 0 && errno != ENOTCONN)
    perror("ERROR shutting down busy connection's write side");
  close(connfd);
}

/*
  Handles a client connection in its own thread. It retrieves the socket and client address from the 
  supplied context, then frees that context.
  It sends an acceptance response to the client and repeatedly calls process_client_request while requests 
  continue. When the connection ends or a request fails, it closes the socket and calls release_user_slot so the 
  server can accept another client. If sending the initial acceptance fails, it closes the socket and releases 
  the slot immediately.
*/
static void *client_thread(void *arg) {
  struct client_context *context = arg;
  int connfd = context->connfd;
  struct sockaddr_in clientaddr = context->clientaddr;
  free(context);

  if (send_response(connfd, RESPONSE_STATUS_ACCEPTED, NULL, 0) < 0) {
    perror("ERROR sending connection acceptance");
    close(connfd);
    release_user_slot();
    return NULL;
  }

  while (process_client_request(connfd, clientaddr)) {}
  close(connfd);
  release_user_slot();
  return NULL;
}

int main(int argc, char **argv) {
  int listenfd; // listening socket
  int connfd; // connection socket
  socklen_t clientlen ; // byte size of client's address
  struct sockaddr_in serveraddr; // server's addr
  struct sockaddr_in clientaddr; // client addr
  int optval; // flag value for setsockopt
  struct sigaction action; // for signal handling in multithreaded server
  pthread_attr_t thread_attr;

  // Parse port, request rate, maximum clients, and timeout.
  if (process_args(argc, argv) < 0)
    return 1;

  memset(&action, 0, sizeof(action)); // initialize action struct to 0
  action.sa_handler = SIG_IGN; // ignore signal
  sigemptyset(&action.sa_mask); 
  if (sigaction(SIGPIPE, &action, NULL) < 0)
    error("Status: 1\nERROR configuring signals");

  // Configure signal handling and detached worker-thread settings.
  int thread_result = pthread_attr_init(&thread_attr);
  if (thread_result != 0) {
    fprintf(stderr, "ERROR initializing thread attributes: %s\n",
            strerror(thread_result));
    return 1;
  }

  thread_result = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
  // any result from pthread_attr_setdetachstate means error
  if (thread_result != 0) {
    fprintf(stderr, "ERROR configuring detached threads: %s\n",
            strerror(thread_result));
    pthread_attr_destroy(&thread_attr);
    return 1;
  }


  // Create a TCP socket
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    error("Status: 1\nERROR opening socket");

  // configure socket for server
  optval = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                 (const void *)&optval, sizeof(int)) < 0)
    error("Status: 1\nERROR setting socket options");

    // build the server's internet address 
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; // we are using the Internet
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); // accept reqs to any IP addr
    serveraddr.sin_port = htons((unsigned short)portno); // port to listen on

    // bind the listening socket to a port
    if (bind(listenfd, (struct sockaddr *) &serveraddr,
      sizeof(serveraddr)) < 0)
      error("Status: 1\nERROR on binding");

    // make it a listening socket ready to accept connection requests
    if (listen(listenfd, QSIZE) < 0) // allow n requests to queue up
      error("Status: 1\nERROR on listen");

  // wait for a connection request, echo input line, then close connection.
  while (1) {
    // wait for a connection request 
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (struct sockaddr *) &clientaddr, &clientlen);
    if (connfd < 0) {
      if (errno == EINTR)
        continue;
      perror("Status: 1\nERROR on accept");
      continue;
    }

    // set timeout for socket
    struct timeval timeout = {timeout_seconds, 0};
    if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      perror("Status: 1\nERROR configuring connection timeout");
      close(connfd);
      continue;
    }    
    int lock_result = pthread_mutex_lock(&users_mutex);
    if (lock_result != 0) {
      fprintf(stderr, "ERROR locking user count: %s\n", strerror(lock_result));
      close(connfd);
      continue;
    }

    // Check client limit.
    // Reject connections if the server is full; otherwise, create a client context and 
    // start a client_thread to handle the connection. 
    int server_busy = active_users >= max_users;
    if (!server_busy)
      active_users++;
    lock_result = pthread_mutex_unlock(&users_mutex);
    if (lock_result != 0) {
      fprintf(stderr, "ERROR unlocking user count: %s\n",
              strerror(lock_result));
      close(connfd);
      continue;
    }
    if (server_busy) {
      reject_busy_connection(connfd);
      continue;
    }

   // If setup fails, close the socket and release the reserved client slot.
    struct client_context *context = malloc(sizeof(*context));
    if (context == NULL) {
      perror("Status: 1\nERROR allocating client context");
      close(connfd);
      release_user_slot();
      continue;
    }
    context->connfd = connfd;
    context->clientaddr = clientaddr;

    pthread_t worker;
    thread_result = pthread_create(&worker, &thread_attr, client_thread,
                                   context);
    if (thread_result != 0) {
      fprintf(stderr, "Status: 1\nERROR creating client worker: %s\n",
              strerror(thread_result));
      free(context);
      close(connfd);
      release_user_slot();
    }
  }
}