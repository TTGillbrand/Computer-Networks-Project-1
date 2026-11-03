/*
 * echoserver.c - A simple connection-based echo server
 * usage: echoserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "quirc-master/lib/quirc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define BUFSIZE 1024
#define QSIZE 5

// default values for optional arguments.
#define DEFAULT_PORT 2012
#define DEFAULT_RATE_REQUESTS 3
#define DEFAULT_RATE_SECONDS 60
#define DEFAULT_MAX_USERS 3
#define DEFAULT_TIMEOUT_SECONDS 80

// max values to protect server
#define MAX_FILE_SZ 20*1024*1024 // max for qr file
#define MAX_URL_SZ  200 // in characters

// initialize all config parameters
int portno = DEFAULT_PORT;
// do same for all other args.

/*
 * error - wrapper for perror
 */
void error(char *msg) {
  perror(msg);
  exit(1);
}

// todo:: Phase 4
void process_args(int argc, char **argv) {
  // initialize all argumesnts.
  // create DEFINES, set arg varaibles to those.
  // also do validation.
  // while loop to process all optional args.
}

void process_client_request(int connfd, struct sockaddr_in clientaddr) {
  uint32_t fsize;

  char *hostaddrp; /* dotted decimal host addr string */
  struct hostent *hostp; /* client host info */
  int hostport; /* client port */


  char hostname[NI_MAXHOST];
  char portname[NI_MAXSERV];

  // Look up both hostname and port safely
  int s = getnameinfo((struct sockaddr *)&clientaddr, sizeof(struct sockaddr_in),
                      hostname, sizeof(hostname),
                      portname, sizeof(portname),
                      0); // Use NI_NAMEREQD flag if you want it to strictly fail without DNS

  printf("server established connection with %s:%s\n",
  hostname, portname);

  // read: file size from the client
  if (recv (connfd, &fsize, sizeof (uint32_t), 0) < 0) {
    printf("ERROR reading fsize from socket");
    return;
  }
  fsize = ntohl(fsize);

  if (fsize > MAX_FILE_SZ) {
    fprintf(stderr, "ERROR: oversized file: %u bytes\n", fsize);
    return;
  }
   
  // get file content
  //char buffer[fsize];
  //buffer[fsize] = '\0';
  
  uint8_t *buffer = malloc(fsize);

  if(buffer == NULL){
    perror("Failed to allocate image buffer");
    return;
  }
  
  int  bytesActuallyReceived;
  uint32_t bytesReceivedsoFar = 0;
  while (bytesReceivedsoFar < fsize) {
    bytesActuallyReceived = recv(connfd, buffer + bytesReceivedsoFar, fsize - bytesReceivedsoFar, 0);
      // 1. Check for system/network errors
      if (bytesActuallyReceived < 0) {
          printf("Network error during recv");
          free(buffer);
          break;
      }
      
      // 2. Check if the client disconnected too early
      if (bytesActuallyReceived == 0) {
          fprintf(stderr, "Error: Client closed connection prematurely at %u / %u bytes.\n", 
                  bytesReceivedsoFar, fsize);
          free(buffer);
          break;
      } 
    bytesReceivedsoFar += bytesActuallyReceived;
  }

  if (bytesReceivedsoFar == fsize) {
      printf("SUCCESS: File completely received! Total bytes: %u / %u\n", bytesReceivedsoFar, fsize);
      printf("%s\n", buffer);
  } else {
    printf("FAILURE: File transfer incomplete. Skipping file dump.\n");
  } 

  // TODO: call qr decoder here when ready
  
  //decoding an image into greyscale pixel array
  int w, h, channels;
  uint8_t *pixels = stbi_load_from_memory(buffer, fsize, &w, &h, &channels, 1);
  free(buffer);

  if(!pixels){
    fprintf(stderr, "Failed to decode image", stbi_failure_reason());
  }

  printf("Image: %d x %d\n", w, h);
  
  struct quirc *qr = quirc_new();\

  if(!qr){
    perror("Failed to allocate memory");
    stbi_image_free(pixels);
    return;
  }

  if(quirc_resize(qr, w, h)){
    perror("Failed to allocate video memory");
    quirc_destroy(qr);
    stbi_image_free(pixels);
    return;
  }

  int qw, qh;//get these from the passed image
  uint8_t *qimage = quirc_begin(qr, &qw, &qh);
  
  memcpy(qimage, pixels, (size_t)w*h);
  stbi_image_free(pixels);
  quirc_end(qr);

  int num_codes = quirc_count(qr);
  printf("Found %d QR codes\n", num_codes);

  for(int i = 0; i < num_codes; i++){
    struct quirc_code code;
    struct quirc_data data;
    quirc_decode_error_t err;

    quirc_extract(qr, i, &code);

    /* Decoding stage */
    err = quirc_decode(&code, &data);
    if (err)
        printf("DECODE FAILED: %s\n", quirc_strerror(err));
    else
        printf("Data: %s\n", data.payload);
  }

  quirc_destroy(qr);

}

int main(int argc, char **argv) {
  int listenfd; /* listening socket */
  int connfd; /* connection socket */
  socklen_t clientlen ; /* byte size of client's address */
  struct sockaddr_in serveraddr; /* server's addr */
  struct sockaddr_in clientaddr; /* client addr */
  int optval; /* flag value for setsockopt */
  /* check command line args */
  process_args(argc, argv);

  /* socket: create a socket */
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    error("ERROR opening socket");

  /* setsockopt: Handy debugging trick that lets
   * us rerun the server immediately after we kill it;
   * otherwise we have to wait about 20 secs.
   * Eliminates "ERROR on binding: Address already in use" error.
   */
  optval = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
	     (const void *)&optval , sizeof(int));

  /* build the server's internet address */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET; /* we are using the Internet */
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); /* accept reqs to any IP addr */
  serveraddr.sin_port = htons((unsigned short)portno); /* port to listen on */

  /* bind: associate the listening socket with a port */
  if (bind(listenfd, (struct sockaddr *) &serveraddr,
	   sizeof(serveraddr)) < 0)
    error("ERROR on binding");

  /* listen: make it a listening socket ready to accept connection requests */
  if (listen(listenfd, QSIZE) < 0) /* allow n requests to queue up */
    error("ERROR on listen");

  /* main loop: wait for a connection request, echo input line,
     then close connection. */
  clientlen = sizeof(clientaddr);
  while (1) {
    /* accept: wait for a connection request */
    connfd = accept(listenfd, (struct sockaddr *) &clientaddr, &clientlen);
    if (connfd < 0)
      error("ERROR on accept");

    process_client_request(connfd, clientaddr);

    close(connfd);
  }
}