#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#define BACKLOG 10
#define MAX_DATA_SIZE 1024 
#define FILE_NAME "config.conf"
#define PROC_COUNT 4

//define global variables 

char PORTS[PROC_COUNT][16];
char IP[PROC_COUNT][16];

void sigchld_handler(int s)
{
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

void *get_in_addr(struct sockaddr *sa)
{
	if(sa->sa_family == AF_INET){
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void *do_chld(void *arg)
{
	int mysocfd = (int) arg;
	
	int i;

	//  read from the given socket 
	// read(mysocfd, data, 40);
	if (send(mysocfd, "Hello, world!", 13, 0) == -1)
				perror("send");

	/* simulate some processing */
	for (i=0;i<1000000;i++);

	/* close the socket and exit this thread */
	close(mysocfd);
	pthread_exit((void *)0);
}

void read_configure()
{
	char * line = NULL;
	size_t len = 0;
	FILE *fp = fopen (FILE_NAME, "r");
	int i = 0;
	if(fp!= NULL){
		while(i < 4){
			fscanf(fp, "%s %s", IP[i], PORTS[i]);
			printf("IP:%s\n",IP[i]);
			printf("PORT:%s\n", PORTS[i]);
			i++;
		}
		printf("read config file successfully.\n");
		fclose(fp);
	}

}

int getServer(struct addrinfo* servinfo, struct addrinfo* *p){
	int sockfd;

	for(*p = servinfo; *p!=NULL;*p= (*p)->ai_next){
		if((sockfd = socket((*p)->ai_family, (*p)->ai_socktype, 
			(*p)->ai_protocol)) == -1){
			perror("client:socket");
			continue;
		}
		if(connect(sockfd, (*p)->ai_addr, (*p)->ai_addrlen)==-1){
			close(sockfd);
			perror("client: connect");
			continue;
		}
		break;
	}

	return sockfd;
}

// void unicast_send(int dest, char* message){

// }

// void unicast_receive(int source, char*message){

// }



int main(int argc, char const *argv[]){

	struct addrinfo hints, *servinfo, *p;
	int rv;
	int sockfd, new_fd;
	int yes =1;
	struct sigaction sa;
	socklen_t sin_size;
	struct sockaddr_storage their_addr;
	int numbytes;
	char buf[MAX_DATA_SIZE];
	pthread_t chld_thr;
	char s[INET6_ADDRSTRLEN];
	

	if(argc != 2){
		fprintf(stderr, "usage: server portnumber\n");
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	//read-in configure file here
	read_configure();

	if((rv = getaddrinfo(NULL, PORTS[(int)(*argv[1])-48], &hints, &servinfo)) != 0){
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	for(p = servinfo; p!= NULL; p = p->ai_next){
		if((sockfd=socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1){
			perror("server: socket");
			continue;
		}

		if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1){
			perror("setsockopt");
			continue;
		}

		if(bind(sockfd, p->ai_addr, p->ai_addrlen) == -1){
			close(sockfd);
			perror("server: bind");
			continue;
		}

		break;
	}

	if(p == NULL){
		fprintf(stderr, "server: failed to bind\n");
		return 2;
	}

	freeaddrinfo(servinfo);

	/* set the level of thread concurrency we desire */
	pthread_setconcurrency(5);

	if(listen(sockfd, BACKLOG) == -1){
		perror("listen");
		exit(1);
	}

	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if(sigaction(SIGCHLD, &sa, NULL) == -1){
		perror("sigaction");
		exit(1);
	}

	printf("server: waiting for connections....\n");

	while(1){
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if(new_fd == -1){
			perror("accept");
			continue;
		}

		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);
		printf("server: got connection from %s\n", s);

		/* create a new thread to process the incomming request */
		pthread_create(&chld_thr, NULL, do_chld, (void *)new_fd);
	}

	return 0;
}