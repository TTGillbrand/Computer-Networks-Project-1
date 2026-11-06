/* 
 * echoclient.c - A simple connection-based client
 * usage: echoclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <sys/stat.h>
#include <unistd.h>

#define BUFSIZE 1024
#define MAX_PAYLOAD_SIZE 8896
#define MAX_TEXT_RESPONSE_SIZE (20U * 1024U * 1024U)

#define FILE_TYPE_NEITHER 0
#define FILE_TYPE_IMAGE 1
#define FILE_TYPE_TEXT 2

#define RESPONSE_STATUS_URL 0
#define RESPONSE_STATUS_ERROR 1
#define RESPONSE_STATUS_TEXT 2

struct client_config {
    char hostname[256];
    int portno;
};

static int error(const char *msg) {
    perror(msg);
    return 1;
}

static int process_args(int argc, char **argv, struct client_config *cfg) {
    char *end = NULL;
    long port = 0;

    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
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

static int resolve_server_address(const char *hostname, int portno,
                                 struct sockaddr_in *serveraddr) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    char service[32];
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(service, sizeof(service), "%d", portno);
    status = getaddrinfo(hostname, service, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "Status: 1\nERROR, no such host as %s: %s\n",
                hostname, gai_strerror(status));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET && rp->ai_socktype == SOCK_STREAM) {
            memset(serveraddr, 0, sizeof(*serveraddr));
            memcpy(serveraddr, rp->ai_addr, sizeof(*serveraddr));
            freeaddrinfo(result);
            return 0;
        }
    }

    freeaddrinfo(result);
    fprintf(stderr, "Status: 1\nERROR, no valid IPv4 address found for %s\n", hostname);
    return -1;
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

int32_t get_file_type(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
        return FILE_TYPE_NEITHER;

    unsigned char header[12];
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

    if (is_image) {
        fclose(file);
        return FILE_TYPE_IMAGE;
    }

    if (ferror(file) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return FILE_TYPE_NEITHER;
    }

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
                    goto not_text;
                remaining_utf8--;
                next_min = 0x80;
                next_max = 0xBF;
            } else if (byte <= 0x7F) {
                if (byte != '\t' && byte != '\n' && byte != '\v' &&
                    byte != '\f' && byte != '\r' &&
                    (byte < 0x20 || byte == 0x7F))
                    goto not_text;
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
                goto not_text;
            }
        }
    }

    if (ferror(file) || remaining_utf8 != 0)
        goto not_text;

    fclose(file);
    return FILE_TYPE_TEXT;

not_text:
    fclose(file);
    return FILE_TYPE_NEITHER;
}


int send_file(int sockfd, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Status: 1\nError opening file %s.\n", filename);
        return -1;
    }

    char buffer[BUFSIZE];
    bzero(buffer, BUFSIZE);
    size_t bytes_read = 0;
    // Read the file chunk by chunk until the end of file
    // outter loop: ensure buffer size is bounded (does not depend on file size)
    while ((bytes_read = fread(buffer, 1, BUFSIZE, file)) > 0) {
        size_t total_sent = 0;
        while (total_sent < bytes_read) {
            ssize_t sent = send(sockfd, buffer + total_sent, bytes_read - total_sent, 0);
            if (sent < 0) {
                if (errno == EINTR)
                    continue;
                printf("Status: 1\nError sending data");
                fclose(file);
                return -1;
            }
            if (sent == 0) {
                fprintf(stderr, "Status: 1\nERROR: connection closed while sending file\n");
                fclose(file);
                return -1;
            }
            total_sent += sent; // Advance the pointer by the amount actually sent
        }

        bzero(buffer, BUFSIZE); 
    }

    if (ferror(file)) {
        perror("Status: 1\nError reading file");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    int sockfd,  n;
    struct sockaddr_in serveraddr;
    struct client_config config;

    char buf[BUFSIZE];

    uint32_t fsize, ftype;

    // check command line arguments
    if (process_args(argc, argv, &config) != 0) {
        return 1;
    }

    // create the socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return error("Status: 1\nERROR opening socket");
    }

    // resolve the server address
    if (resolve_server_address(config.hostname, config.portno, &serveraddr) != 0) {
        close(sockfd);
        return 1;
    }

    // create a connection with the server
    if (connect(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) {
        perror("Status: 1\nERROR connecting");
        close(sockfd);
        return 1;
    }

    // get message line from the user
    bzero(buf, BUFSIZE);
    printf("Please enter filename: ");

    if (fgets(buf, BUFSIZE, stdin) == NULL) {
      fprintf(stderr, "Status: 1\nERROR reading filename\n");
      close(sockfd);
      return 1;
    }
 
    // strip newline character for fopen() to work
    buf[strcspn(buf, "\n")] = '\0';

    // compute and send file size.
    fsize = get_file_size(buf); 
    if (fsize <= 0) {
      fprintf(stderr, "Status: 1\nERROR: unable to read a non-empty input file\n");
      close(sockfd);
      return 1;
    }
    /* send the file size to the server*/
    uint32_t net_fsize = htonl(fsize);
    if (send_all(sockfd, &net_fsize, sizeof(net_fsize)) < 0) {
      fprintf(stderr, "Status: 1\nERROR sending file size to socket\n");
      close(sockfd);
      return 1;
    }

    // compute and send file type.
    ftype = (uint32_t)get_file_type(buf);
    /* send the file type to the server*/
    uint32_t net_ftype = htonl(ftype);
    n = send(sockfd, &net_ftype, sizeof(net_ftype), 0);
    if (n < 0) {
      perror("Status: 1\nERROR sending file type to socket");
      close(sockfd);
      return 1;
    }
    
    // now read file content into buf and send it to server.
    n = send_file(sockfd, buf);
    if (n < 0){
      fprintf(stderr, "Status: 1\nERROR writing data to socket\n");
      close(sockfd);
      return 1;
    }
    
    uint32_t net_status, net_payload_len;
    int result = recv_all(sockfd, &net_status, sizeof(net_status));
    if (result != 1) {
      fprintf(stderr, "Status: 1\nERROR receiving response status%s\n",
              result == 0 ? ": server disconnected" : "");
      close(sockfd);
      return 1;
    }

    uint32_t status = ntohl(net_status);
    result = recv_all(sockfd, &net_payload_len, sizeof(net_payload_len));
    if (result != 1) {
      fprintf(stderr, "Status: 1\nERROR receiving response length%s\n",
              result == 0 ? ": server disconnected" : "");
      close(sockfd);
      return 1;
    }

    uint32_t payload_len = ntohl(net_payload_len);
    if (status > RESPONSE_STATUS_TEXT ||
        (status == RESPONSE_STATUS_ERROR && payload_len != 0) ||
        (status == RESPONSE_STATUS_URL && payload_len > MAX_PAYLOAD_SIZE) ||
        (status == RESPONSE_STATUS_TEXT &&
         payload_len > MAX_TEXT_RESPONSE_SIZE)) {
      fprintf(stderr, "Status: 1\nERROR: invalid server response\n");
      close(sockfd);
      return 1;
    }
    if (status == RESPONSE_STATUS_ERROR) {
      printf("Status: 1\n no URL returned.\n");
      close(sockfd);
      return 1;
    }

    uint8_t *payload = NULL;
    if (payload_len > 0) {
      payload = malloc(payload_len);
      if (payload == NULL) {
        perror("Status: 1\nERROR allocating response buffer");
        close(sockfd);
        return 1;
      }
    }

    result = recv_all(sockfd, payload, payload_len);
    if (result != 1) {
      fprintf(stderr, "Status: 1\nERROR receiving QR payload%s\n",
              result == 0 ? ": server disconnected" : "");
      free(payload);
      close(sockfd);
      return 1;
    }

    if (status == RESPONSE_STATUS_TEXT) {
      printf("Status: 0\nText file contents:\n");
      if (fwrite(payload, 1, payload_len, stdout) != payload_len) {
        perror("Status: 1\nERROR printing text response");
        free(payload);
        close(sockfd);
        return 1;
      }
      if (payload_len == 0 || payload[payload_len - 1] != '\n')
        putchar('\n');
    } else {
      printf("Status: 0\nURL (%u bytes): ", payload_len);
      if (fwrite(payload, 1, payload_len, stdout) != payload_len) {
        perror("Status: 1\nERROR printing QR payload");
        free(payload);
        close(sockfd);
        return 1;
      }
      putchar('\n');
    }

    free(payload);
    close(sockfd);
    return 0;
}