/*
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int
main(int argc, char *argv[])
{
	int sockfd;
	char buf[1024];
	struct sockaddr_in servaddr;
	socklen_t addrlen;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
		return -1;
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(atoi(argv[2]));
	inet_aton(argv[1], &servaddr.sin_addr);
	addrlen = sizeof(servaddr);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	connect(sockfd, (struct sockaddr *) &servaddr, addrlen);

	printf("connected to server\n");



	/* send request */
	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf),
		"GET /dragonite_right.png HTTP/1.2\r\n"
		"Host: abc.com\r\n"
		"User-Agent: abc_studio\r\n"
		"abc-abc: you are abc\r\n"
		"\r\n");

	write(sockfd, buf, strlen(buf));
	printf("send request to server\n");

	memset(buf, 0, sizeof(buf));
	if (read(sockfd, buf, sizeof(buf)) > 0)
		printf("read from server:\n%s\n", buf);



	close(sockfd);
	return 0;
}


