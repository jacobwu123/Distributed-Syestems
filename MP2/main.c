/*
** main.c -- stream (TCP) base
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <linux/netdevice.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>
#include <stropts.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/time.h>

#define BACKLOG 	10	   // how many pending connections queue will hold
#define MAXDATASIZE 128    // max number of bytes we can get at once
#define MAXBUFLEN 	128	   // max number of bytes we will store at once
#define ASCII_OFFSET 97    // offset to calculate index from variable
#define SETUP_WAIT  10      // wait time to allow other processes to set up
#define ALPHA_LEN   26	   // length of alphabet
#define THOUSAND   1000    // used to convert milliseconds to microsends for delay
#define SHAREDBUF_LEN 112  // length of shared buffer for comparing up to 7 messages
#define ID_OFFSET   5	   // used to calculate id offset into shared buffer
#define T_OFFSET    16     // used to calculate timestamp offset into shared buffer
#define TSTAMP_LEN  10	   // length of timestamp char buffer
#define BASE_TEN    10	   // used to denote base ten in format conversion
#define IPADDR_LEN  16     // length of char array to hold ip address
#define PORT_LEN    6	   // length of char array to hold port number
#define REPLICAS	7 	   // potential number of replicas in system

/* Host Machine Info */
char my_ipaddr[IPADDR_LEN];
char my_port[PORT_LEN];
int my_pid;

/* Array of Socket Descriptors */
int sockets[REPLICAS];

/* p_count Holds # of Processes in System */
int p_count;

/* State variable & Flags */
bool server_created = false;
bool rcomp_done = false;
int r;
int w;
int r_count = 0;
int w_count= 0;
int sent_count = 0;
bool read_op = false;
bool write_op = false;
char op_var;
char op_val;
int min_delay;
int max_delay;

/* Synchronization of buffers */
pthread_mutex_t socket_mutex;
pthread_mutex_t file_mutex;

/* Multicast Buffer */
char mc_buf[1024];

/* Compare Buffer */
char rcomp_buf[SHAREDBUF_LEN];

/* Local info of Distributed System */
int local_vars[ALPHA_LEN];
long int local_times[ALPHA_LEN];

//  File to be written 
// FILE *fp;
char title[9];

/* Struct to pass to thread to create server */
typedef struct Config_info{
	long *pids;
	char *ip_addresses;
	long *ports;
} Config_info;

/* Struct to pass to threads to create clients */
typedef struct Client_info{
	long port;
	char* ip_addr;
} Client_info;

/* Reap all dead processes */
void sigchld_handler(int s){
	(void)s; // quiet unused variable warning

	// waitpid() might overwrite errno, so we save and restore it:
	int saved_errno = errno;

	while(waitpid(-1, NULL, WNOHANG) > 0);

	errno = saved_errno;
}

/* Get sockaddr, IPv4 or IPv6: */
void *get_in_addr(struct sockaddr *sa){
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/* Get host machine's own IP Address */
void get_ip_addr(char * ip_addr){
    int sock, i;
    struct ifreq ifreqs[20];
    struct ifconf ic;

    ic.ifc_len = sizeof ifreqs;
    ic.ifc_req = ifreqs;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    if (ioctl(sock, SIOCGIFCONF, &ic) < 0) {
        perror("SIOCGIFCONF");
        exit(1);
    }

    //printf("%s\n", inet_ntoa(((struct sockaddr_in*)&ifreqs[1].ifr_addr)->sin_addr));
    char *ip = inet_ntoa(((struct sockaddr_in*)&ifreqs[1].ifr_addr)->sin_addr);
    strncpy(ip_addr, ip, IPADDR_LEN);
    return;
}

/* Unicast Functionality */
void unicast(void * arg){
	char * casted_message = (char*) arg;
	int sd = atoi(&casted_message[0]);

	if (send(sockets[sd], &casted_message[1], strlen(casted_message)-1, 0) == -1)
				perror("send");
	return;
}

/* Thread to add Unicast Functionality Delay */
void * unicast_delay(void* arg){
	char * msg = (char*) arg;
	sleep(min_delay + rand()%(max_delay+1 -min_delay));
	unicast(msg);
	//free();
	pthread_exit(NULL);
}

/* Multicast Functionality */
void multicast(void *arg){
	char * message = (char*) arg;
	char * casted_message;

	pthread_t u_delay[p_count];
	for(int i = 0; i < p_count; i++){
		
		casted_message = malloc(sizeof(char)* (strlen(message)+2));
		sprintf(casted_message, "%d", i);
		strcpy(&(casted_message[1]), message);


		int rc;
		rc = pthread_create(&u_delay[i], NULL, unicast_delay, (void*)casted_message);
		if(rc){
			printf("ERROR W/ THREAD CREATION.\n");
			exit(-1);
		}		
	}

	for(int i = 0; i <p_count; i++){
		pthread_join(u_delay[i], NULL);
	}
	return;
}

/* Thread used for read/get and write/put operations */
void * handle_rw(){
	while(true){
		if(read_op){
			/* 
			 * Read Operation: Compare received read request messages
			 *				   to select message with most recent timestamp
			 */

			// Wait for number of received messages to be at least R
			if(r_count >= r){
				// Buffer now has at least r messages for read/get operation
				pthread_mutex_lock(&socket_mutex);

				// Declare Arrays to hold info of received messages
				char id_list[r_count];
				char value_list[r_count];
				char key_list[r_count];
				char time_s[r_count][11];
				int time_l[r_count];

				// This variable will hold index of latest timestamp
				int res_i = 0;

				// Initialize array values with ID's and Timestamps
				for(int i = 0; i < r_count; i++){
					id_list[i] = rcomp_buf[0 + (T_OFFSET*i)];
					key_list[i] = rcomp_buf[2 + (T_OFFSET*i)];
					value_list[i] = rcomp_buf[3 + (T_OFFSET*i)];
					strncpy(time_s[i], &rcomp_buf[ID_OFFSET+(T_OFFSET*i)], TSTAMP_LEN);
					time_s[i][TSTAMP_LEN] = '\0';
					time_l[i] = atoi(time_s[i]);

					printf("time_l[%d]:%d\n", i, time_l[i]);

					// Compare timestamps and set index accordingly
					if(i > 0){
						if(time_l[res_i] < time_l[i])
							res_i = i;
					}
				}
				
				// Message with latest timestamp
				printf("-->Latest timestamp from PID:%c, time:%d, value: %c\n\n", id_list[res_i], time_l[res_i],value_list[res_i] );

				// Write to file
				char tstamp[11];
				struct timeval start;
				gettimeofday(&start, NULL);	
				long int issued_time = start.tv_sec;
				sprintf(tstamp, "%lu", issued_time);
				pthread_mutex_lock(&file_mutex);
				FILE *fp;
				fp = fopen(title, "a+");
				fprintf(fp, "555,%d,get,%c,%s,resp,%c\n", my_pid,key_list[res_i],tstamp, value_list[res_i]);
				//fflush(fp);
				fclose(fp);
				pthread_mutex_unlock(&file_mutex);


				// Reset state/control variables and flags 
				read_op = false;
				r_count = 0;
				rcomp_buf[0] = '\0';
				rcomp_done = true;
				pthread_mutex_unlock(&socket_mutex);
			}
		}
		else if(write_op){
			/* 
			 * Write Operation: Here wait to receive W acks from write operation
			 *					multicasted to clients in system
			 */

			if(w_count >= w){
				// Buffer now has at least w ack messages from put operation
				pthread_mutex_lock(&socket_mutex);
				
				// Indicate Write/Put operation complete
				// printf("Write OP Complete\n\n");

				// Write to file
				// Retrieve value and timestamp to write
				char tstamp[11];
				struct timeval start;
				gettimeofday(&start, NULL);	
				long int issued_time = start.tv_sec;
				sprintf(tstamp, "%lu", issued_time);
				pthread_mutex_lock(&file_mutex);
				FILE *fp;
				fp = fopen(title, "a+");
				fprintf(fp, "555,%d,put,%c,%s,req,%c\n", my_pid,op_var,tstamp, op_val);
				// fflush(fp);
				fclose(fp);
				pthread_mutex_unlock(&file_mutex);


				// Reset state/control variables and flags 
				write_op = false;
				w_count = 0;
				rcomp_buf[0] = '\0';
				rcomp_done = true;
				pthread_mutex_unlock(&socket_mutex);
			}
		}
	}
	pthread_exit(NULL);
}

/* Thread to handle receiving messages from clients of local server */
void * handle_connection(void* sd){
	int numbytes;
	char buf[MAXDATASIZE];
	int new_fd = *(int*) sd;
	static int messages_counter = 0;

	while((numbytes = recv(new_fd, buf, MAXDATASIZE-1, 0)) > 0)
	{
		messages_counter++;
		buf[numbytes] = '\0';
		
		// printf("-- Server: received '%s' --\n",buf);
		// printf("-- Messages counter: %d  --\n", messages_counter);
		// printf("r_count: %d\nread_op: %d\n", r_count, read_op);
		// printf("w_count: %d\nwrite_op: %d\n", w_count, write_op);

		if(read_op){
			// Increment number of received messages for read/get operation
			// 	and add received messages to shared buffer
			pthread_mutex_lock(&socket_mutex);
			r_count++;
			strcat(rcomp_buf, buf);
			strcat(rcomp_buf, " ");
			pthread_mutex_unlock(&socket_mutex);

			// Wait until timestamp comparisons are done
			while(!rcomp_done);
		}
		else if (write_op){
			// Increment number of received messages for write/put operation
			// 	if get ack from clients indicating write operation received
			pthread_mutex_lock(&socket_mutex);		
			if(strcmp(buf, "WRITE_ACK") == 0)
				w_count++;
			pthread_mutex_unlock(&socket_mutex);

			// Wait until write/put operation is done
			while(!rcomp_done);
		}

	}

	close(new_fd);
	// printf("--CONNECTION HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
}

/* Thread for creating a Server socket per Process ID in config file */
void * thread_create_server(void * cinfo){
	struct Config_info *data = (struct Config_info *) cinfo;
	printf("THREAD: Server Creation initiated...\n");

	// Parse IP Addresses from Struct passed as Argument
	char sep_ips[p_count][IPADDR_LEN];
	int i = 0;
	int x;
	for(x = 0; x < p_count; x++){
		int j = 0;
		while(data->ip_addresses[i] != ' ' && data->ip_addresses[i] != '\0'){
			sep_ips[x][j] = data->ip_addresses[i];
			j++;
			i++;
		}
		sep_ips[x][j] = '\0';
		i++;
	}

	int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	// Select Available Port to Bind to.
	for(x = 0; x < p_count; x++){
		// Ensure IP address in config file is own for server setup.
		if(strcmp(sep_ips[x], my_ipaddr) != 0){
			continue;
		}

		// Bind to first unused port on local machine in config file
		sprintf(my_port, "%ld",data->ports[x]);

		if ((rv = getaddrinfo(NULL, my_port, &hints, &servinfo)) != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
			//return 1;
			pthread_exit(NULL);
		}

		// loop through all the results and bind to the first we can
		for(p = servinfo; p != NULL; p = p->ai_next) {
			if ((sockfd = socket(p->ai_family, p->ai_socktype,
					p->ai_protocol)) == -1) {
				perror("server: socket");
				continue;
			}

			if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
					sizeof(int)) == -1) {
				perror("setsockopt");
				//exit(1);
				pthread_exit(NULL);
			}

			if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
				close(sockfd);
				perror("server: bind");
				continue;
				//break;
			}

			break;
		}

		// If did not bind because pointer is NULL, then continue.
		if(p == NULL)
			continue;
		else
			break;
	}

	printf("Binded to port: %s\n", my_port);
	my_pid = (int) data->pids[x];
	server_created = true;

	freeaddrinfo(servinfo); // all done with this structure

	if (p == NULL)  {
		fprintf(stderr, "server: failed to bind\n");
		//exit(1);
		pthread_exit(NULL);
	}

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		//exit(1);
		pthread_exit(NULL);
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		//exit(1);
		pthread_exit(NULL);
	}

	printf("[PID:%lu] - Server now waiting for connections...\n\n", data->pids[x]);

	// handle_this thread will handle connections accepted by local server
	pthread_t handle_this;
	int socket_idx;
	while(1) {
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		// Add to array of Server-Client sockets
		sockets[socket_idx] = new_fd;
		socket_idx++;

		// Print connector info
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);

		printf("[Server id(%lu)]: Got connection from %s.\n", data->pids[x], s);

		// Have thread handle the new connection
		int rc;
		rc = pthread_create(&handle_this, NULL, handle_connection, &new_fd);
		if(rc){
			printf("ERROR W/ THREAD CREATION.\n");
			exit(-1);
		}
	
	}

	// free(cinfo);
	pthread_exit(NULL);
}

/* Thread for creating a Client socket per Process ID */
void * thread_create_client(void * cl_info){
	struct Client_info *data = (struct Client_info *) cl_info;
	printf("THREAD: Client Creation initiated to connect to %s...\n", data->ip_addr);

	int sockfd, numbytes;  
	char buf[MAXDATASIZE];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char connection_port[PORT_LEN];
	sprintf(connection_port, "%ld",data->port);

	if ((rv = getaddrinfo(data->ip_addr, connection_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		//return 1;
		pthread_exit(NULL);
	}

	// Sleep temporarily to allow other machines to finish setup
	sleep(SETUP_WAIT);	

	// Loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			//perror("client: connect");
			close(sockfd);
			continue;
		}
		break;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			s, sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure


	char * token;
	char * one_msg;
	while(((numbytes = recv(sockfd, buf, MAXDATASIZE-1, 0)) > 0))
	{
		buf[numbytes] = '\0';
		one_msg = malloc(MAXDATASIZE * sizeof(char));
		strcpy(one_msg, buf);

		// printf("client: received '%s'\n",buf);
		// Split received concatenated messages from TCP
		token = strtok(one_msg, " ");
		// printf("client: token '%s'\n", token);

		while(token != NULL){
			one_msg = malloc(MAXDATASIZE * sizeof(char));
			strcpy(one_msg, token);

			printf("one_msg: %s\n", one_msg);

			// Following Conditional Statements handle how client in system
			// 	responds to received get/put operations
			if(one_msg[0] == 'g'){
				// Get Operation; client responds with PID, Variable Value, and Timestamp

				int var_idx = one_msg[1] - ASCII_OFFSET;
				printf("Read operation requested for variable %c.\n", one_msg[1]);

				// Retrieve relevant information for get operation
				char send_buf[16];
				char value[2];
				char timestamp[11];
				char identifier[2];

				pthread_mutex_lock(&socket_mutex);
				// printf("LOCKED\n");
				sprintf(value, "%d", local_vars[var_idx]);
				// printf("-----VALUE: %s, value[0]: %c,local_var: %d\n", value,value[0], local_vars[var_idx]);
				sprintf(timestamp, "%lu", local_times[var_idx]);
				sprintf(identifier, "%d", my_pid);
				pthread_mutex_unlock(&socket_mutex);
				// printf("UNLOCKED\n");

				// Concatentate strings of information
				send_buf[0] = '\0';
				strcat(send_buf, identifier);    // process identifier
				strcat(send_buf, " ");
				strcat(send_buf, &one_msg[1]);	 // variable key
				// strcat(send_buf, value);         // variable value
				send_buf[3] = value[0];
				strcat(send_buf, " "); 			 //  space
				strcat(send_buf, timestamp);	 // variable timestamp

				// printf("send_buf: %s\n", send_buf);
				
				// Send message back to sender
				// min + rand() % (max+1 - min);
				//usleep(min_delay* THOUSAND + rand()%(max_delay*THOUSAND+THOUSAND-min_delay*THOUSAND));
				sleep(min_delay + rand()%(max_delay+1 -min_delay));
				if (send(sockfd, &send_buf, strlen(send_buf), 0) == -1)
					perror("send");

				// printf("sent read message requested\n");
			}
			else if(one_msg[0] == 'p'){
				// Put Operation; Client decides whether or not to write new value based on timestamp

				int var_idx = one_msg[1] - ASCII_OFFSET;
				printf("Write operation requested for variable '%c'...\n", one_msg[1]);

				// Retrieve relevant information for put operation (i.e., value & timestamp)
				char val_char = one_msg[2];
				int val_int = atoi(&val_char);
				char tstamp_string[11];
				strncpy(tstamp_string, &one_msg[3], TSTAMP_LEN);
				tstamp_string[TSTAMP_LEN] = '\0';
				int tstamp_int = atoi(tstamp_string);

				// Last-write wins 
				// printf("-received timestamp: %d\n", tstamp_int);
				// printf("-received new value: %d\n", val_int);
				pthread_mutex_lock(&socket_mutex);
				if(local_times[var_idx] < tstamp_int){
					local_vars[var_idx] = val_int;
					local_times[var_idx] = tstamp_int;

					printf("**updated value!**\n");
				}
				pthread_mutex_unlock(&socket_mutex);

				// Send ACK back to sender
				char *send_buf = "WRITE_ACK";

				// min + rand() % (max+1 - min);
				//usleep(min_delay* THOUSAND + rand()%(max_delay*THOUSAND+THOUSAND-min_delay*THOUSAND));
				sleep(min_delay + rand()%(max_delay+1 -min_delay));
				if (send(sockfd, send_buf, strlen(send_buf), 0) == -1)
					perror("send");
			}

			token = strtok(NULL, " ");
		}
		

	}

	close(sockfd);
	// printf("--CONNECTION HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
}

// /* Thread to keep multicasting when Multicast buffer has message */
void * thread_mcast(void* m){
	char * mcast_msg = (char *)m;
	char * temp_msg = malloc(strlen(mcast_msg) * sizeof(char));
	strcpy(temp_msg, mcast_msg);
	multicast(temp_msg);
	pthread_exit(NULL);
}

/* Thread for taking commands and multicasting */
void * thread_get_stdin(void * ptr){
	while(1){
		// Store commands in buffer
		char input_buf[MAXBUFLEN];
		fgets(input_buf, MAXBUFLEN, stdin);
		input_buf[strlen(input_buf) -1] = '\0';

		// Parse through received command
		char op[4];
		char var = input_buf[4];
		memset(op, '\0', sizeof(op));
		strncpy(op, input_buf, 3);
		if(strcmp(op, "get") == 0){
			// Read operation
			op_var = var;
			read_op = true;
			rcomp_done = false;

			// File write
			char tstamp[11];
			struct timeval start;
			gettimeofday(&start, NULL);	
			long int issued_time = start.tv_sec;
			sprintf(tstamp, "%lu", issued_time);
			// printf("issued timestamp: %s\n", tstamp);
			pthread_mutex_lock(&file_mutex);
			FILE *fp;
			fp = fopen(title, "a+");
			fprintf(fp, "555,%d,get,%c,%s,req,\n", my_pid,var,tstamp);
			// fflush(fp);
			fclose(fp);
			pthread_mutex_unlock(&file_mutex);

			// Set up Multicast buffer
			mc_buf[0] = '\0';
			strcat(mc_buf, " ");
			strcat(mc_buf, "g");
			strcat(mc_buf, &op_var);
			strcat(mc_buf, " ");
			// multicast(mc_buf);
			pthread_t mcast_me;
			int rc = pthread_create(&mcast_me, NULL, thread_mcast, (void*)mc_buf);
			if(rc){
				printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
				exit(-1);
			}

		}
		else if(strcmp(op, "put") == 0){

			// Write operation 
			write_op = true;
			op_var = var;
			op_val = input_buf[6];
			
			// Retrieve value and timestamp to write
			char tstamp[11];
			struct timeval start;
			gettimeofday(&start, NULL);	
			long int issued_time = start.tv_sec;
			sprintf(tstamp, "%lu", issued_time);
			// printf("issued timestamp: %s\n", tstamp);

			// File write
			pthread_mutex_lock(&file_mutex);
			FILE *fp;
			fp = fopen(title, "a+");
			fprintf(fp, "555,%d,put,%c,%s,req,%c\n", my_pid,var,tstamp, op_val);
			// fflush(fp);
			fclose(fp);
			pthread_mutex_unlock(&file_mutex);

			// Set up Multicast buffer
			mc_buf[0] = '\0';
			strcat(mc_buf, " ");
			strcat(mc_buf, "p");
			strcat(mc_buf, &op_var);
			strcat(mc_buf, tstamp);
			strcat(mc_buf, " ");

			// printf("----mc_buf: %s\n", mc_buf);
			// multicast(mc_buf);
			// do_mcast = true;

			pthread_t mcast_me;
			int rc = pthread_create(&mcast_me, NULL, thread_mcast, (void*)mc_buf);
			if(rc){
				printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
				exit(-1);
			}
		}
		else if(input_buf[0] == 'd' && input_buf[1] == 'u'){
			// Dump operation
			pthread_mutex_lock(&socket_mutex);
			for(int i = 0; i < ALPHA_LEN; i++){
				printf("%c %d\n", (i+ASCII_OFFSET),local_vars[i]);
			}
			pthread_mutex_unlock(&socket_mutex);
		}
		else if(input_buf[0] == 'd' && input_buf[1] == 'e'){
			// Delay operation
			//  in milliseconds given
			char *ptr = &input_buf[6];

			if(isdigit(*ptr)){
				long val = strtol(ptr, &ptr, BASE_TEN);
				printf("Delaying %lu milliseconds...\n", val);
				usleep(val * THOUSAND);
			}
		}
		else if(input_buf[0] == 'r'){
			// Debugging purposes
			printf("HELPER: rcomp_buf:%s, read_op:%d\n", rcomp_buf, read_op);
		}

		// Exit STDIN Thread
		if(strcmp(input_buf, "quit") == 0){
			break;
		}
	}

	pthread_exit(NULL);	
}

/* MAIN THREAD */
int main(int argc, char *argv[])
{
	// Initialize timestamps for local variables
	struct timeval start;
	gettimeofday(&start, NULL);
	for(int i = 0; i < ALPHA_LEN; i++){
		local_times[i] = start.tv_sec;
	}

	// Set R, W values for Eventual Consistency
	r = atoi(argv[2]);
	w = atoi(argv[3]);
	printf("R:%d\n",r);
	printf("W:%d\n",w);

	/********  CONFIG FILE PARSING ********/
	FILE *file = fopen("config.txt", "r");
	char *file_buf = NULL;
	char *str = NULL;
	int file_len;
	p_count = 0;
	int parse_sel= 0;
	int i = 0;
	get_ip_addr(my_ipaddr);
	printf("My IP Address: %s\n", my_ipaddr);
	
	if(file != NULL){
		// Get size of file
		if(fseek(file, 0L, SEEK_END) == 0){
			long size = ftell(file);
			if(size == -1){
				printf("Error with file size.\n");
				return -1;
			}

			file_buf = malloc(sizeof(char) * (size+1));
			if(fseek(file, 0L, SEEK_SET) != 0){
				printf("Error with file seek_set.\n");
				return -1;
			}

			// Read file into buffer
			size_t new_size = fread(file_buf, sizeof(char), size, file);
			file_buf[new_size++] = '\0';
		}
		fclose(file);
	}
	str = file_buf;
	file_len = sizeof(file_buf) / sizeof(file_buf[0]);

	// Count number of processes in config file
	while(*str != '\0'){
		if(*str == '\n')
			p_count++;
		str++;
	}

	// Arrays to hold config file's 
	long pids[p_count];
	char ip_addresses[p_count * IPADDR_LEN];
	long ports[p_count];
	char *str_ip;
	// Reset values to re-read file buffer
	str = file_buf;
	str_ip = ip_addresses;
	i = 0;

	while(*str != '\0'){
		// Parsing
		if(parse_sel == 0){
			// Parse Channel Delays
			if(isdigit(*str)){
				min_delay = strtol(str, &str, BASE_TEN);
			}
			str++;

			if(isdigit(*str)){
				max_delay = strtol(str, &str, BASE_TEN);
			}
			str++;

			parse_sel++;
		}
		else if(parse_sel == 1){
			// Parse Process ID
			if(isdigit(*str)){
				long val = strtol(str, &str, BASE_TEN);
				pids[i] = val;
			}
			str++;

			// Parse IP Address
			while(*str != ' '){
				*str_ip = *str;
				str++;
				str_ip++;
			}
			if(i == p_count-1)
				*str_ip = '\0';
			else
				*str_ip = ' ';
			str_ip++;
			str++;
			
			// Parse Port Number
			if(isdigit(*str)){
				long val = strtol(str, &str, BASE_TEN);
				ports[i] = val;
			}

			if(*str == '\n'){
				i++;
				str++;
			}
		}
	}
	free(file_buf);
	/******** END CONFIG FILE PARSING ********/

	// Parse through and separate IP Addresses
	char sep_ips[p_count][IPADDR_LEN];
	i = 0;
	for(int x = 0; x < p_count; x++){
		int j = 0;
		while(ip_addresses[i] != ' ' && ip_addresses[i] != '\0'){
			sep_ips[x][j] = ip_addresses[i];
			j++;
			i++;
		}
		sep_ips[x][j] = '\0';
		i++;
	}

	// for(int x = 0; x< p_count; x++)
	// 	printf("sep_ips[%d] = %s\n", x, sep_ips[x]);

	/* THREAD CREATIONS */
	pthread_t proc_server;
	pthread_t proc_client[p_count];
	pthread_t proc_stdin;
	int rc;

	// Create Server Thread
	struct Config_info *cinfo;
	cinfo =  malloc(sizeof(struct Config_info));
	cinfo->pids = pids;
	cinfo->ip_addresses = ip_addresses;
	cinfo->ports = ports;
	rc = pthread_create(&proc_server, NULL, thread_create_server, (void*)cinfo);
	if(rc){
		printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
		exit(-1);
	}
	
	// Create Client Threads
	struct Client_info *cl_info;
	while(!server_created);
	for(int x = 0; x < p_count; x++){
		cl_info = malloc(sizeof(struct Client_info));
		cl_info->port = ports[x];
		cl_info->ip_addr = sep_ips[x];

		rc = pthread_create(&proc_client[x], NULL, thread_create_client, (void*)cl_info);			
		if(rc){
			printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
			exit(-1);
		}
	}

	// Create Stdin Thread
	rc = pthread_create(&proc_stdin, NULL, thread_get_stdin, NULL);			
	if(rc){
			printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
			exit(-1);
	}

	// Create Thread to Handle Get/Put Operation
	pthread_t rw_op;
	rc = pthread_create(&rw_op, NULL, handle_rw, NULL);
	if(rc){
			printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
			exit(-1);
	}

	// Open File to be written into
	title[0] = '\0';
	strcat(title, "log");  
	char id[2];
	sprintf(id, "%d", my_pid);
	strcat(title, id);
	strcat(title, ".txt");
	printf("title: %s\n", title);
	
	FILE *fp;
	fp = fopen(title, "w");
	if (fp == NULL)
	{
	    printf("Error opening file!\n");
	    exit(1);
	}
	fclose(fp);

	pthread_exit(NULL);
	free(rcomp_buf);
	return 0;
}

