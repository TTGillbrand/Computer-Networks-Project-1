/* 
 * echoclient.c - A simple connection-based client
 * usage: echoclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <sys/stat.h>
#include <unistd.h>

#define BUFSIZE 1024

/* 
 * error - wrapper for perror
 */
char *hostname;;
int portno;

void error(char *msg) {
    perror(msg);
    exit(0);
}

void process_args(int argc, char **argv) {
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
}

int32_t get_file_size(const char *filename) {
    FILE *fp = fopen(filename, "rb"); 
    if (fp == NULL) {
        return -1; 
    }

    // Move pointer to the end of the file
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    // Get the current position (which matches total bytes)
    long size = ftell(fp); 

    fclose(fp);
    return (int32_t) size;
}

int send_file(int sockfd, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error opening file %s.\n", filename);
        return -1;
    }

    char buffer[BUFSIZE];
    bzero(buffer, BUFSIZE);
    size_t bytes_read = 0;
    // Read the file chunk by chunk until the end (EOF)
    // outter loop: ensure buffer size is bounded (does not depend on file size)
    while ((bytes_read = fread(buffer, 1, BUFSIZE, file)) > 0) {
        size_t total_sent = 0;
        while (total_sent < bytes_read) {
            ssize_t sent = send(sockfd, buffer + total_sent, bytes_read - total_sent, 0);
            if (sent < 0) {
                printf("Error sending data");
                fclose(file);
                return -1;
            }
            for (int i = total_sent; i < total_sent + sent; i++) {    
                putchar(buffer[i]); // Or: printf("%c", buffer[i]);        
            }
            
            //printf("DEBUG SEND: %ld bytes\n", sent); //TODO: print part of buffer that was sent in this iteration.
            total_sent += sent; // Advance the pointer by the amount actually sent
        }

        bzero(buffer, BUFSIZE); 
    }

    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    int sockfd,  n;
    struct sockaddr_in serveraddr;
    struct hostent *server;

    char buf[BUFSIZE];

    uint32_t fsize;

    /* check command line arguments */
    process_args(argc, argv);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    /* connect: create a connection with the server */
    if (connect(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
      error("ERROR connecting");

    /* get message line from the user */
    bzero(buf, BUFSIZE);
    printf("Please enter filename: ");

    if (fgets(buf, BUFSIZE, stdin) == NULL) {
      printf("\nfgets failure...\n");
      }
 
    // Strip newline character for fopen() to work
    buf[strcspn(buf, "\n")] = '\0';

    // compute and send file size.
    fsize = get_file_size(buf); 

    //printf("DEBUG file size is %d\n", fsize);

    /* send: send the file size to the server*/
    uint32_t net_fsize = htonl(fsize);

    n = send(sockfd, &net_fsize, sizeof(net_fsize), 0);

    if (n < 0) 
      error("ERROR sending to socket");

    // now read file content into buf and send it to server.
    n = send_file(sockfd, buf); // TODO: same with fopen call here too. as line 153 buf); 
    if (n < 0) 
      error("ERROR writing to socket");

    /* Part2: recv: print the server's reply - url from QR code or error messages. */
    /* TODO: enable in part 2, for QRCode url, switch to recv instead of read.*/
    /* TODO: To be implemented in phase2 */
    /*
    bzero(buf, BUFSIZE);
    n = read(sockfd, buf, BUFSIZE);
    if (n < 0) 
      error("ERROR reading from socket");
    printf("Echo from server: %s", buf);
    */
    close(sockfd);
    return 0;
}
