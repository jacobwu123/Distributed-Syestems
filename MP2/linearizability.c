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
#include <time.h>


#define BACKLOG 10
#define MAX_DATA_SIZE 128 
#define FILE_NAME "config.conf"
#define PROC_COUNT 7
#define TTLMSGNUM 50

//define global variables 
char* holdBack[TTLMSGNUM];
int CurMsgNum = 0;
int numDelivered = 0;
int sequenceNum = 0;

char PORTS[PROC_COUNT][16];
char IP[PROC_COUNT][16];
int socketdrive[PROC_COUNT];
int process_id;
int delay[PROC_COUNT][PROC_COUNT];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int send_flag = 0;
char multi_message[512];

int keys[26];

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

void setup_delay(int min_delay, int max_delay){
	int i,j;
		// setting up delay 
	srand(time(NULL));
	while(i < PROC_COUNT){
		delay[i][i] = 0;
		for(j = i+1; j < PROC_COUNT; j++){
			delay[i][j] = rand()%(max_delay - min_delay) + min_delay;
			delay[j][i] = delay[i][j];
		}
		i ++;
	}
}

void read_configure()
{
	char * line = NULL;
	size_t len = 0;
	FILE *fp = fopen (FILE_NAME, "r");
	int i = 0;
	int min_delay, max_delay;
	if(fp!= NULL){
		printf("Setting up configuration...\n");

		fscanf(fp, "%d %d", &min_delay, &max_delay);
		printf("min_delay:%d, max_delay:%d\n",min_delay, max_delay);

		while(i < PROC_COUNT){
			fscanf(fp, "%s %s", IP[i], PORTS[i]);
			printf("Process %d: IP:%s, ",i,IP[i]);
			printf("PORT:%s\n", PORTS[i]);
			i++;
		}

		printf("Setting up configuration successfully.\n");
		fclose(fp);
	}
	setup_delay(min_delay, max_delay);
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

void *setupConnection(void* arg)
{
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int sockfd;
	int dest = (int) arg;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(IP[dest], PORTS[dest], &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		pthread_exit((void *)0);
	}

	sleep(10);//wait for other processes to open
	sockfd = getServer(servinfo, &p);
	if(p == NULL){
		fprintf(stderr, "client: fail to connect\n");
		pthread_exit((void *)0);
	}
	socketdrive[dest] = sockfd;
	freeaddrinfo(servinfo);
	pthread_exit((void *)0);

}

char* getTime(){
	time_t rawtime;
  	struct tm * timeinfo;

  	time ( &rawtime );
  	timeinfo = localtime ( &rawtime );
  	return asctime(timeinfo);
}

void unicast_send(int dest, char* message){
	char casted_message[strlen(message)+1];

	sprintf(casted_message,"%d",process_id);
	strcpy(&(casted_message[1]), message);

	sleep(delay[process_id][dest]);
	if(send(socketdrive[dest], casted_message, strlen(casted_message),0) == -1) 
		perror("send");
}

void unicast_receive(int source, char*message){
	// if(source != process_id)
	//if its a write command, write value into local
	char ack_msg[9];
	if(message[0] == 'p'){
		printf("Received \"%s\" from process %d, system time is ­­­­­­­­­­­­­%s\n", 
			message, source, getTime());		
		keys[(int)message[1]-97] = atoi(&(message[2]));
		//Return Ack() indicating completion of the Write(X,v) operation
		strcpy(ack_msg,"Ack:");
		strcat(ack_msg, message);
		unicast_send(source, ack_msg);
	}
	else if(message[0] == 'g'){
		/*
		When totally-ordered multicast of Read(X) initiated by pi is delivered to process pi, 
		read the value of local copy of X at pi.
		*/
		if(source == process_id){
			strcpy(ack_msg,"Ack:");
			strcat(ack_msg, message);
			ack_msg[6] = '=';
			sprintf(&(ack_msg[7]), "%d", keys[(int)message[1]-97]);
			ack_msg[8] = '\0';
			unicast_send(source, ack_msg);
		}
	} 
}

void *unicast(void *arg)
{
	int dest = (int) arg;
	unicast_send(dest, multi_message);
	pthread_exit((void *)0);
}

void *multicast(void* arg){
	char* message = (char*)arg;
	pthread_t *tid = malloc(PROC_COUNT* sizeof(pthread_t));
	strcpy(multi_message, message);
	int i = 0;
	for(i = 0; i < PROC_COUNT;i++){
		pthread_create(&tid[i], NULL, unicast,(void*)i);
	}
	free(tid);

	pthread_exit((void *)0);
}

void *send_message(void *arg){
	char command[16];
	char message[4];
	pthread_t chld_thr;
	while(fgets(command, sizeof(command),stdin) > 0){
		if(command[0] == 'p'){
			/*Perform a totally-ordered multicast of Write(X, v).*/
			message[0] = 'p';
			message[1] = command[4];
			message[2] = command[6];
			message[3] = '\0';
			pthread_create(&chld_thr,NULL,multicast,(void*)message);
		}
		else if(command[0] == 'g'){
			/*Perform a totally-ordered multicast of Read(X).*/
			message[0] = 'g';
			message[1] = command[4];
			message[2] = '\0';
			pthread_create(&chld_thr,NULL,multicast,(void*)message);
		}
		else if(strcmp(command, "quit") == 0)
			break;
	}
	pthread_exit((void *)0);
}

void *deliverMessage(void *arg){
	char* messages = (char*)arg;
	int i;
	//iterate through the holdback to check the coresponding message
	char seqNum[16];
	//put the message into deliver queue
	strcpy(seqNum, &(messages[3]));
	printf("seqNum: %s\n", seqNum);
	while(numDelivered < atoi(seqNum)){
		//wait for the numDelivered to be the same in seqNum
	}
	
	printf("Delivering the message...\n");

	pthread_mutex_lock(&mutex);
	if(numDelivered == atoi(seqNum)){
		for(i = 0; i < CurMsgNum;i++){
			// printf("CurMsgNum: %d\n", CurMsgNum);
			// printf("message:%s\n",messages);
			if(holdBack[i] != NULL && holdBack[i][0] == messages[2]){
				//put it to the delivery queue
				unicast_receive(atoi(&(holdBack[i][0])),&(holdBack[i][1]));
				free(holdBack[i]);
				holdBack[i] = holdBack[CurMsgNum-1];
				CurMsgNum --;
				numDelivered = atoi(seqNum)+1;
			}
		}
	}
	pthread_mutex_unlock(&mutex);
	free(messages);
	pthread_exit((void *)0);
}

void *do_chld(void *arg)
{
	int mysocfd = (int) arg;
	char* message = malloc(MAX_DATA_SIZE*sizeof(char));
	message[0] = '\0';	
	char* t_message;
	int i;
	int source;
	int numbytes;
	char act[10];
	int dest;
	pthread_t chld_thr, chld_thr2, chld_thr3;
	char* seqMessage = malloc(10*sizeof(char));

	// pthread_create(&chld_thr, NULL, send_message, NULL);
	while((numbytes = recv(mysocfd, message, MAX_DATA_SIZE-1, 0))>0){
		/* simulate some processing */
		message[numbytes] = '\0';
		printf("Received:%s\n", message);
		t_message = malloc(MAX_DATA_SIZE*sizeof(char));
		strcpy(t_message, message);
		for (i=0;i<1000000;i++);

		if(numbytes > 0){
			//if not sequencer:
			if(process_id != 0){
				//if the message from sequencer: use a thread to check, and deliver
				if(message[0] == '0' && message[1] == 'o'){
					printf("The message from the sequencer: %s\n", t_message);
					pthread_create(&chld_thr2, NULL, deliverMessage, (void*)t_message);
				}
				else if(message[1] == 'A'){
					printf("process %c completed:%s\n", message[0], &(message[5]));
				}
				else if(message[1] == 'p' || message[1] == 'g'){
					//else put the message in the hold back queue
					pthread_mutex_lock(&mutex);
					printf("Push the message into the holdBack queue...\n");
					holdBack[CurMsgNum] = (char*)malloc(strlen(message)*sizeof(char));
					strcpy(holdBack[CurMsgNum], message);
					CurMsgNum ++;
					pthread_mutex_unlock(&mutex);
					printf("Finish pushing the message.\n");
				}
			}	
			//if sequencer:
			else{
				//if the message is not an order:
				if(message[1] =='p' || message[1] == 'g'){
					//pthread_mutex_lock(&mutex);
					seqMessage[0] = 'o';
					strcpy(&(seqMessage[1]), &(message[0]));
					sprintf(&(seqMessage[2]), "%d", sequenceNum);
					printf("seqMessage:%s\n", seqMessage);
					pthread_create(&chld_thr3, NULL, multicast,(void*)seqMessage);
					sequenceNum ++;
					unicast_receive((int)message[0]-48, &message[1]);
					//pthread_mutex_unlock(&mutex);
				}
				else if(message[1] == 'A'){
					printf("process %c completed:%s\n", message[0], &(message[5]));
				}
			}
		}
		message[0] = '\0';

	}
	free(message);
	free(seqMessage);
	/* close the socket and exit this thread */
	close(mysocfd);
	pthread_exit((void *)0);
}


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
	pthread_t chld_thr, chld_thr1;
	pthread_t *tid = malloc( PROC_COUNT* sizeof(pthread_t) );
	char s[INET6_ADDRSTRLEN];
	int i;

	if(argc != 2){
		fprintf(stderr, "usage: server portnumber\n");
	}


	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	//read-in configure file here
	read_configure();
	process_id = (int)(*argv[1])-48;

	if((rv = getaddrinfo(NULL, PORTS[process_id], &hints, &servinfo)) != 0){
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

	for(i = 0; i < PROC_COUNT;i++){
		pthread_create(&tid[i], NULL, setupConnection,(void*)i);
	}

	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( tid[i], NULL);
	}
	pthread_create(&chld_thr1, NULL, send_message, NULL);

	//initialize key variables
	for(i = 0; i < 26; i++){
		keys[i] = 0;
	}

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

	for(i = 0; i < PROC_COUNT; i++){
		free(tid[i]);
	}

	return 0;
}