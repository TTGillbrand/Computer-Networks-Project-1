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

#include "quirc-master/lib/quirc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define BUFSIZE 1024
#define QSIZE 5

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

struct client_context {
  int connfd;
  struct sockaddr_in clientaddr;
};

// initialize all config parameters
int portno = DEFAULT_PORT;
// do same for all other args.

/* stb_image keeps its failure reason in shared global state. */
static pthread_mutex_t stbi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

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
      return -1;
    }
    if (result == 0)
      return 0;
    received += (size_t)result;
  }
  return 1;
}

static int send_response(int fd, uint32_t status, const uint8_t *payload,
                         uint32_t payload_len) {
  uint32_t net_status = htonl(status);
  uint32_t net_payload_len = htonl(payload_len);

  if (send_all(fd, &net_status, sizeof(net_status)) < 0 ||
      send_all(fd, &net_payload_len, sizeof(net_payload_len)) < 0)
    return -1;
  if (payload_len > 0 && send_all(fd, payload, payload_len) < 0)
    return -1;
  return 0;
}

static int send_error_response(int fd, const char *message) {
  return send_response(fd, RESPONSE_STATUS_ERROR, (const uint8_t *)message,
                       (uint32_t)strlen(message));
}

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

// todo:: Phase 4
void process_args(int argc, char **argv) {
  // initialize all argumesnts.
  // create DEFINES, set arg varaibles to those.
  // also do validation.
  // while loop to process all optional args.
}

void process_client_request(int connfd, struct sockaddr_in clientaddr) {
  uint32_t fsize, ftype;

  char *hostaddrp; // dotted decimal host addr string
  struct hostent *hostp; // client host info
  int hostport; // client port


  char hostname[NI_MAXHOST];
  char portname[NI_MAXSERV];

  // Look up both hostname and port safely
  int s = getnameinfo((struct sockaddr *)&clientaddr, sizeof(struct sockaddr_in),
                      hostname, sizeof(hostname),
                      portname, sizeof(portname),
                      0);

  printf("server established connection with %s:%s\n",
  hostname, portname);

  // read file size from the client
  int result = recv_all(connfd, &fsize, sizeof(fsize));
  if (result != 1) {
    fprintf(stderr, "Status: 1\nERROR reading fsize from socket%s\n",
            result == 0 ? ": client disconnected" : "");
    return;
  }
  fsize = ntohl(fsize);

  result = recv_all(connfd, &ftype, sizeof(ftype));
  if (result != 1) {
    fprintf(stderr, "Status: 1\nERROR reading file type from socket%s\n",
            result == 0 ? ": client disconnected" : "");
    return;
  }
  ftype = ntohl(ftype);

  if (fsize > MAX_FILE_SZ) {
    fprintf(stderr, "Status: 1\nERROR: file too large: %u bytes (maximum %u)\n",
            fsize, (unsigned int)MAX_FILE_SZ);
    send_error_response(connfd, "File is too large for the server.");
    return;
  }
  if (fsize == 0 || (ftype != FILE_TYPE_IMAGE && ftype != FILE_TYPE_TEXT)) {
    fprintf(stderr, "Status: 1\nERROR: invalid or unsupported file\n");
    send_error_response(connfd, "File is empty or is not a supported image or text file.");
    return;
  }

  if (send_response(connfd, RESPONSE_STATUS_READY, NULL, 0) < 0) {
    perror("Status: 1\nERROR acknowledging upload");
    return;
  }

  uint8_t *buffer = malloc((size_t)fsize);
  if (buffer == NULL) {
    perror("Status: 1\nFailed to allocate image buffer");
    send_error_response(connfd, "Server could not allocate memory for the upload.");
    return;
  }

  result = recv_all(connfd, buffer, (size_t)fsize);
  if (result != 1) {
    if (result == 0)
      fprintf(stderr, "Status: 1\nERROR: client disconnected before sending all %u bytes\n", fsize);
    else
      perror("Status: 1\nERROR receiving binary file data");
    free(buffer);
    send_error_response(connfd, "Server did not receive the complete file.");
    return;
  }

  printf("Status: 0\nSUCCESS: File completely received! Total bytes: %u\n", fsize);

  if (ftype == FILE_TYPE_TEXT) {
    if (!is_valid_text(buffer, fsize)) {
      fprintf(stderr, "Status: 1\nERROR: received text file is not valid UTF-8 text\n");
      free(buffer);
      send_error_response(connfd, "File is not valid UTF-8 text.");
      return;
    }

    int mutex_result = pthread_mutex_lock(&stdout_mutex);
    if (mutex_result != 0) {
      fprintf(stderr, "ERROR locking stdout mutex: %s\n",
              strerror(mutex_result));
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

    int response_result =
        send_response(connfd, RESPONSE_STATUS_TEXT, buffer, fsize);
    free(buffer);
    if (response_result < 0)
      perror("Status: 1\nERROR sending response to client");
    return;
  }

  // decoding an image into greyscale pixel array
  int w, h, channels;
  int mutex_result = pthread_mutex_lock(&stbi_mutex);
  if (mutex_result != 0) {
    fprintf(stderr, "ERROR locking image decoder mutex: %s\n",
            strerror(mutex_result));
    free(buffer);
    send_error_response(connfd, "Server could not process the image.");
    return;
  }
  uint8_t *pixels = stbi_load_from_memory(buffer, fsize, &w, &h, &channels, 1);
  if (!pixels)
    fprintf(stderr, "Status: 1\nFailed to decode image: %s\n",
            stbi_failure_reason());
  mutex_result = pthread_mutex_unlock(&stbi_mutex);
  if (mutex_result != 0) {
    fprintf(stderr, "ERROR unlocking image decoder mutex: %s\n",
            strerror(mutex_result));
    if (pixels)
      stbi_image_free(pixels);
    free(buffer);
    send_error_response(connfd, "Server could not process the image.");
    return;
  }
  free(buffer);

  if(!pixels){
    send_error_response(connfd, "File is not a valid image containing a QR code.");
    return;
  }
  
  struct quirc *qr = quirc_new();\

  if(!qr){
    perror("Status: 1\nFailed to allocate memory");
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not allocate memory to decode the QR code.");
    return;
  }

  if(quirc_resize(qr, w, h)){
    perror("Status: 1\nFailed to allocate video memory");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not decode the QR image.");
    return;
  }

  int qw, qh; // get these from the passed image
  uint8_t *qimage = quirc_begin(qr, &qw, &qh);
  if (!qimage) {
    fprintf(stderr, "Status: 1\nFailed to obtain QR image buffer\n");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    send_error_response(connfd, "Server could not decode the QR image.");
    return;
  }
  
  memcpy(qimage, pixels, (size_t)w*h);
  stbi_image_free(pixels);
  quirc_end(qr);

  int num_codes = quirc_count(qr);
  printf("Found %d QR codes\n", num_codes);

  uint8_t url[MAX_URL_SZ];
  uint32_t url_len = 0;
  int have_url = 0;
  for(int i = 0; i < num_codes; i++){
    struct quirc_code code;
    struct quirc_data data;
    quirc_decode_error_t err;

    quirc_extract(qr, i, &code);

    // Decoding stage
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
      fprintf(stderr, "QR payload is empty or exceeds %d bytes\n", MAX_URL_SZ);
    }
  }

  int response_result;
  if (have_url) {
    response_result = send_response(connfd, RESPONSE_STATUS_URL, url, url_len);
  } else {
    response_result = send_error_response(
        connfd, "Image does not contain a valid QR code with a supported payload.");
  }
  if (response_result < 0)
    perror("Status: 1\nERROR sending response to client");

  quirc_destroy(qr);
}

static void *client_thread(void *arg) {
  struct client_context *context = arg;
  int connfd = context->connfd;
  struct sockaddr_in clientaddr = context->clientaddr;
  free(context);

  process_client_request(connfd, clientaddr);
  close(connfd);
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

  // check command line args
  process_args(argc, argv);

  memset(&action, 0, sizeof(action)); // initialize action struct to 0
  action.sa_handler = SIG_IGN; // ignore signal
  sigemptyset(&action.sa_mask); 
  if (sigaction(SIGPIPE, &action, NULL) < 0)
    error("Status: 1\nERROR configuring signals");

  int thread_result = pthread_attr_init(&thread_attr);
  if (thread_result != 0) {
    fprintf(stderr, "ERROR initializing thread attributes: %s\n",
            strerror(thread_result));
    return 1;
  }
  thread_result = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
  
  if (thread_result != 0) {
    fprintf(stderr, "ERROR configuring detached threads: %s\n",
            strerror(thread_result));
    pthread_attr_destroy(&thread_attr);
    return 1;
  }

  // create a socket
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    error("Status: 1\nERROR opening socket");

  optval = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
	     (const void *)&optval , sizeof(int));

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

    struct client_context *context = malloc(sizeof(*context));
    if (context == NULL) {
      perror("Status: 1\nERROR allocating client context");
      close(connfd);
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
    }
  }
}