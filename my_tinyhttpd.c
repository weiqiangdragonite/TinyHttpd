/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */

/*
 * Study From TinyHttpd Under GNU Public License
 *
 * rewrite by <weiqiangdragonite@gmail.com>
 *
 * CGI未测试
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>


#define SERVER_STRING	"Server: tinyhttpd/0.1.0\r\n"
#define BAKLOG		128

/* funtions prototype */
int startup(unsigned short *port);
static void *accept_request(void *arg);
static void process_request(int clifd);
int get_line(int fd, char *buf, int size);
void unimplemented(int fd);
void not_found(int fd);
void serve_file(int fd, const char *filename);
void headers(int fd, const char *filename);
void cat(int fd, FILE *f);
void execute_cgi(int fd, const char *path,
	const char *method, const char *query);
void bad_request(int fd);
void cannot_execute(int fd);


static void
sigpipe_handler(int sig)
{
	printf("in the signal handler\n");
}



/*
 * create socket, wait for client connect, then use multi
 * thread to process client request
 */
int
main(int argc, char *argv[])
{
	int sockfd, clifd;
	unsigned short port;
	struct sockaddr_in cli_addr;
	socklen_t cli_len;
	pthread_t newthread;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <port>\n", argv[0]);
		return -1;
	}


	port = (unsigned short) atoi(argv[1]);
	sockfd = startup(&port);
	if (sockfd == -1) {
		fprintf(stderr, "startup() failed\n");
		return -2;
	}
	printf("tiny httpd running on port %d\n", port);


	signal(SIGPIPE, sigpipe_handler);


	while (1) {
		cli_len = sizeof(cli_addr);
		clifd = accept(sockfd, (struct sockaddr *) &cli_addr, &cli_len);

		if (clifd == -1) {
			perror("accept() failed");
			continue;
		}

		/* accept_request(clifd); */
		if (pthread_create(&newthread, NULL, accept_request,
			(void *) &clifd) != 0)
		{
			perror("pthread_create() failed");
			close(clifd);
			continue;
		}

		/* detach thread，或者设置线程属性 */
		if (pthread_detach(newthread) != 0) {
			perror("pthread_detach() failed");
		}
	}

	close(sockfd);
	return 0;
}


/*
 * This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket
 *
 * avoid to use this function, because this socket can only deal with IPv4!
 */
int
startup(unsigned short *port)
{
	int sockfd;
	int on;
	struct sockaddr_in servaddr;
	socklen_t len;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		perror("socket() failed");
		return -1;
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(*port);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	on = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
		&on, sizeof(on)) == -1)
	{
		perror("setsockopt() failed");
		close(sockfd);
		return -1;
	}

	if (bind(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr))
		== -1)
	{
		perror("bind() failed");
		close(sockfd);
		return -1;
	}

	/* if dynamically allocating a port */
	if (*port == 0) {
		len = sizeof(servaddr);
		if (getsockname(sockfd, (struct sockaddr *) &servaddr, &len)
			== -1)
		{
			perror("getsockname() failed");
			close(sockfd);
			return -1;
		}

		*port = ntohs(servaddr.sin_port);
	}


	if (listen(sockfd, BAKLOG) == -1) {
		perror("listen() failed");
		close(sockfd);
		return -1;
	}

	return sockfd;
}


/*
 * A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client
 *
 * 服务完客户后一般由客户来关闭连接，而不是我方，因为有一个TIME_WAIT的问题，
 * 所以最好搞一个定时器，等待客户关闭，如果超时客户还不关闭，我们才强制关闭
 *
 * 如果在本地测试的话，连续运行webbench，最后会出错的，
 * 因为服务器上的端口都被占用来等待TIME_WAIT了，因为是服务器关闭连接的。
 * 所以每次运行webbench的结果相差是比较大的，往往第1次正常，第2次要比第1次
 * 差很多，第3次也是。
 *
 * 但是我看httpd(apache)好像也是服务器端关闭连接的，不确定（apache好像是多进程的）
 *
 *
 * HTTP/1.0是由服务器来断开连接的
 */
static void *
accept_request(void *arg)
{
	int clifd = *((int *) arg);

	process_request(clifd);
	/* wait for client closed */
	


	return (void *) NULL;
}


static void
process_request(int clifd)
{
	char buf[2048];
	int numchars;
	char method[255];
	char url[255];		/* url最长好像不止256 */
	char path[512];
	char *query_string = NULL;
	int i, j;
	struct stat st;
	int cgi = 0;	/* true if server is a cgi program */


	/* get HTTP method */
	numchars = get_line(clifd, buf, sizeof(buf));
	if (numchars == 0) {
		/* 原来这里没做好，如果客户断开连接，get_line就会返回0，如果
		 * 不返回的话就会继续向下执行，从而向断开的socket写数据导致
		 * 生成SIGPIPE错误 */
		printf("error in get_line()\n");
		return;
	}
	i = j = 0;
	while (!isspace(buf[j]) && (i < sizeof(method) - 1)) {
		method[i++] = buf[j++];
	}
	method[i] = '\0';

	/* can only deal with GET and POST */
	if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
		unimplemented(clifd);
		return;
	}

	if (strcasecmp(method, "POST") == 0)
		cgi = 1;

	/* get request url */
	i = 0;
	while (isspace(buf[j]) && (j < sizeof(buf)))
		++j;
	while (!isspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
		url[i++] = buf[j++];
	url[i] = '\0';

	if (strcasecmp(method, "GET") == 0) {
		query_string = url;

		/* 查看url是否含有'?'，有的话前半部分是url，
		 * 后半部分是传给服务器的参数 */
		/* url:
		 * <scheme>://<user>:<password>@<host>:<port>/<path>;<params>?<query>#<frag> */
		/* http://www.abc.com/abc.cgi?item=1234&color=red */
		while ((*query_string != '?') && (*query_string != '\0'))
			++query_string;
		if (*query_string == '?') {
			cgi = 1;
			*query_string = '\0';
			++query_string;
		}
	}

	/* www目录 */
	sprintf(path, "/var/www/html%s", url);
	/* test if the last character is '/' */
	if (path[strlen(path) - 1] == '/')
		strcat(path, "index.html");

	if (stat(path, &st) == -1) {
		/* error, cannot find the file on the path, or other error */
		/* read & discard headers */
		/* 这里应该是继续读取剩下的headers，然后返回404 */
		/* headers以"\r\n"结束，所以get_line()只返回"\n" */
		while ((numchars > 0) && strcmp("\n", buf))
			numchars = get_line(clifd, buf, sizeof(buf));
		not_found(clifd);
	} else {
		if ((st.st_mode & S_IFMT) == S_IFDIR)
			strcat(path, "/index.html");
		/* more simple way:
		 * if (S_ISDIR(st.st_mode))
		 */

		/* user, group, others can execute */
		/* 为什么??? 如果我的页面都设置为777呢? 这里有问题 */
		/* 而且原来如果path是文件夹的话，肯定是用户、组、其它都能
		 * 执行的，然后在经过上面的strcat()之后应该是该目录下的
		 * index.html，所以下面的if也是错误的 */
		if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP)
			|| (st.st_mode & S_IXOTH))
		{
			cgi = 1;
		}

		cgi = 0;
		//printf("cgi = %d, path = %s\n", cgi, path);
		if (cgi)
			execute_cgi(clifd, path, method, query_string);
		else
			serve_file(clifd, path);
	}

	close(clifd);
	return;
}


/*
 * Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null)
 *
 * 返回的buf末尾只有\n(没有\r\n)
 * 这个版本效率太低了
 */
int
get_line(int fd, char *buf, int size)
{
	int i = 0;
	char c = '\0';
	int n;

	while ((i < size - 1) && (c != '\n')) {
		n = recv(fd, &c, 1, 0);
		if (n > 0) {
			if (c == '\r') {
				n = recv(fd, &c, 1, MSG_PEEK);
				if ((n > 0) && (c == '\n'))
					recv(fd, &c, 1, 0);
				else
					c = '\n';
			}
			buf[i++] = c;
		} else {
			if (n < 0)
				perror("recv() failed");
			c = '\n';
		}
	}
	buf[i] = '\0';

	return i;
}


/*
 * Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket
 *
 * 这个效率也不行
 */
void
unimplemented(int fd)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	sprintf(buf, SERVER_STRING);
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(fd, buf, strlen(buf), 0);
}


/*
 * Give a client a 404 not found status message.
 */
void
not_found(int fd)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(fd, buf, strlen(buf), 0);
}


/*
 * Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve
 */
void
serve_file(int fd, const char *filename)
{
	FILE *f;
	int numchars = 1;
	char buf[1024];

	buf[0] = 'A';
	buf[1] = '\0';
	/* read & discard headers */
	while ((numchars > 0) && strcmp("\n", buf))
		numchars = get_line(fd, buf, sizeof(buf));

	f = fopen(filename, "r");
	/* should look for the error reason */
	if (f == NULL)
		not_found(fd);
	else {
		headers(fd, filename);
		cat(fd, f);
	}
	fclose(f);
}


/*
 * Return the informational HTTP headers about a file.
 * Parameters: the socket to print the headers on
 *             the name of the file
 */
void
headers(int fd, const char *filename)
{
	char buf[1024];

	/* 原文里的这一行不知是什么意思??? */
	//(void)filename;  /* could use filename to determine file type */

	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(fd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(fd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(fd, buf, strlen(buf), 0);
}



/*
 * Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat
 */
void
cat(int fd, FILE *f)
{
	char buf[1024];

	fgets(buf, sizeof(buf), f);
	while (!feof(f)) {
		send(fd,  buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), f);
	}
}


/*
 * Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script
 */
void
execute_cgi(int fd, const char *path, const char *method, const char *query)
{
	char buf[1024];
	int numchars = 1;
	int content_length = -1;
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;


	buf[0] = 'A';
	buf[1] = '\0';
	if (strcasecmp(method, "GET") == 0) {
		/* read & discard headers */
		while ((numchars > 0) && strcmp("\n", buf))
			numchars = get_line(fd, buf, sizeof(buf));
	} else {
		/* POST */
		numchars = get_line(fd, buf, sizeof(buf));
		while ((numchars > 0) && strcmp("\n", buf)) {
			/* 这样子获取content-length也太那个了吧 */
			buf[15] = '\0';
			if (strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&buf[16]);
			numchars = get_line(fd, buf, sizeof(buf));
		}

		if (content_length == -1) {
			bad_request(fd);
			return;
		}
	}

	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(fd, buf, strlen(buf), 0);

	if (pipe(cgi_input) < 0) {
		cannot_execute(fd);
		return;
	}
	if (pipe(cgi_output) < 0) {
		cannot_execute(fd);
		return;
	}

	if ((pid = fork()) < 0) {
		cannot_execute(fd);
		return;
	} else if (pid == 0) {
		/* child: CGI script */
		char meth_env[255];	/* method env */
		char query_env[255];
		char length_env[255];

		close(cgi_output[0]);	/* child write to parent */
		close(cgi_input[1]);	/* child read from parent */
		dup2(cgi_output[1], 1);
		dup2(cgi_input[0], 0);

		sprintf(meth_env, "REQUEST_METHOD=%s", meth_env);
		putenv(meth_env);

		if (strcasecmp(method, "GET") == 0) {
			sprintf(query_env, "QUERY_STRING=%s", query_env);
			putenv(query_env);
		} else {
			/* POST */
			sprintf(length_env, "CONTENT_LENGTH=%d",
				content_length);
			putenv(length_env);
		}

		/* execl(const char *path, const char *arg, ...); */
		/* 将参数全部放到环境变量上了 */
		execl(path, path, NULL);
		_exit(0);
	} else {
		/* parent */
		close(cgi_output[1]);	/* parent read from child */
		close(cgi_input[0]);	/* parent write to child */

		if (strcasecmp(method, "POST") == 0) {
			/* get content from client and write to child */
			for (i = 0; i < content_length; ++i) {
				recv(fd, &c, 1, 0);
				write(cgi_input[1], &c, i);
			}
		}

		/* get content from child and write to client */
		while (read(cgi_output[0], &c, 1) > 0)
			send(fd, &c, 1, 0);

		close(cgi_output[0]);
		close(cgi_input[1]);
		waitpid(pid, &status, 0);
	}
}


/*
 * Inform the client that a request it has made has a problem.
 * Parameters: client socket
 */
void
bad_request(int fd)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(fd, buf, sizeof(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(fd, buf, sizeof(buf), 0);
	sprintf(buf, "\r\n");
	send(fd, buf, sizeof(buf), 0);
	sprintf(buf, "<P>Your browser sent a bad request, ");
	send(fd, buf, sizeof(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(fd, buf, sizeof(buf), 0);
}


/*
 * Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor.
 */
void
cannot_execute(int fd)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(fd, buf, strlen(buf), 0);
	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(fd, buf, strlen(buf), 0);
}

