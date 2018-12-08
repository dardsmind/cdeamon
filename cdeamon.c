#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define CAST_PORT			8080	// defines the port on which the daemon will listen
#define PHP_PATH "/usr/bin/php"

#define MAX_REQUEST_LENGTH	4096
#define MAX_RESPONSE_LENGTH 1024
#define MAX_PATH_LENGTH		1024
#define MAX_CCM_LENGTH		1536
#define MAX_PENDING			10
#define MAX_CMD_LENGTH		MAX_REQUEST_LENGTH*2 + MAX_PATH_LENGTH + MAX_CCM_LENGTH + 4

#define PHP_PATH_COUNT 3
#define PHP_PATHS { "/usr/local/bin/php", "/usr/bin/php", "/bin/php" }

int handle_request(int client_sock, char *pphp, char *pccmanage);
int findphp(char *buf, int bufsize);
void handle_sigchld();

int main(int argc, char **argv) {
	int serv_sock;
	int client_sock;
	int client_len;
	pid_t client_pid;
	struct sockaddr_in serv_addr;
	struct sockaddr_in client_addr;
	
	char php[MAX_PATH_LENGTH+1];
	char *pphp = php;

	char cwd[MAX_PATH_LENGTH+1];
	char *pcwd = cwd;
	char ccmanage[MAX_CCM_LENGTH+1];
	char *pccmanage = ccmanage;
	char *p = NULL;
	
	int one = 1;

	// try to set real UID to EUID
	if (setreuid(geteuid(),-1) == -1) {
		fprintf(stderr,"Cannot set real UID to %d\n",(int) geteuid());
		exit(EXIT_FAILURE);
	}
	
	// find ccmanage.php
	if (getcwd(pcwd,MAX_PATH_LENGTH)==NULL) {
		fprintf(stderr,"Path too long\n");
		exit(EXIT_FAILURE);
	}
	if (argv[0][0]=='/') {
		snprintf(pccmanage,MAX_CCM_LENGTH-32,"%s",argv[0]);
	} else {
		snprintf(pccmanage,MAX_CCM_LENGTH-32,"%s/%s",pcwd,argv[0]);
	}
	p = strrchr(pccmanage,'/');
	if (p != NULL) p[0] = '\0';
	pccmanage = "/home/wcast/system/wcommand.php";   // php file that can be use to execute for the given command.

	// find php
	if (findphp(pphp,sizeof(php)) == -1) {
		fprintf(stderr,"Could not locate the PHP CLI binary in any standard locations.\nYou may need to explicitly set the PHP_PATH constant and recompile this program.\n");
		exit(EXIT_FAILURE);
	}

	signal(SIGCHLD, handle_sigchld);

	// setup our server socket
	serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (serv_sock == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	if ((setsockopt(serv_sock,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one))) == -1) {
    	perror("setsockopt(SO_REUSEADDR)");
    	exit(EXIT_FAILURE);
    }
    
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(CAST_PORT);

	// bind to socket and listen
	if (bind(serv_sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	if (listen(serv_sock, MAX_PENDING) == -1) {
		perror("listen");
		exit(EXIT_FAILURE);
	}
	
	fprintf(stderr,"WCast daemon listening on port %d\n",CAST_PORT);

	// wait for connections
	while (1) {
		client_len = sizeof(client_addr);

		client_sock = accept(serv_sock, (struct sockaddr *) &client_addr, &client_len);

		if (client_sock == -1) {
			perror("accept");
			exit(EXIT_FAILURE);
		}
	
		fprintf(stderr,"Connection from %s\n", inet_ntoa(client_addr.sin_addr));

		if ( (client_pid = fork()) == -1) {
			// fork failed
			perror("fork");
			exit(EXIT_FAILURE);
			
		} else if (client_pid == 0) {
			// we are the child
			close(serv_sock);

			handle_request(client_sock,pphp,pccmanage);
			close(client_sock);
			
			// child process terminates here
			exit(0);
		}
		// we are the server
		
		fprintf(stderr,"child process %d\n",client_pid);

		// done with child socket
		close(client_sock);
	}

	close(serv_sock);
	exit(0);
}

// reads a line from sock and writes it to buf
int read_line(int sock, char *buf, int bufsize) {
	char *bp = buf;
	char c;
	int n;
	
	while ( ((bp-buf) < MAX_REQUEST_LENGTH) && (n = recv(sock,bp,1,0)) > 0 ) {
		if(*bp++ == '\n') return (bp - 1 - buf);
		if (n<0) return -1;
	}
	
	// if we ran out of space, clear up to the next newline (or EOF)
	if ( (bp-buf) == bufsize ) {
		while ( (recv(sock, &c, 1, 0) > 0) && (c != '\n') );
	}
	
	return (bp - buf);
}

// reads all data (up to EOF) from the specified pipe and writes it to the specified socket
void read_pipe_to_socket (int ccpipe, int sock) {
	FILE *stream;
	int c;
	stream = fdopen (ccpipe, "r");
	while ((c = fgetc (stream)) != EOF) {
		write(sock,&c,1);
	}
	fclose (stream);
}

// writes a string to a pipe
void write_pipe(int ccpipe, char *buf) {
	FILE *stream;
	stream = fdopen (ccpipe, "w");
	fprintf (stream, buf);
	fclose (stream);
}

// Executes the PHP script pccmanage using PHP interpreter pphp with the specified command, username,
// and arguments on the commandline, and passes ppassword on the script's stdin.  All output is
// redirected to the socket sock.
int exec_write_socket(int sock, char *pphp, char *pccmanage, char *pcommand, char *pusername, char *ppassword, char *parguments) {
	pid_t pid;
	
	#define	PARENT_READ		readpipe[0]
	#define	CHILD_WRITE		readpipe[1]
	#define CHILD_READ		writepipe[0]
	#define PARENT_WRITE	writepipe[1]	
	
	int readpipe[2], writepipe[2];
	
	// setup our pipes
	if (pipe(readpipe)) {
		perror("readpipe");
		return EXIT_FAILURE;
	}
	if (pipe(writepipe)) {
		perror("writepipe");
		return EXIT_FAILURE;
	}
	
	// fork
	if ( (pid = fork()) == -1) {
		// fork failed
		perror("fork");
		return EXIT_FAILURE;
		
	} else if (pid == 0) {
		// child process
		//close(ccpipe[1]);
		
		// close parent ends of the pipes
		close(PARENT_WRITE);
		close(PARENT_READ);
		
		// duplicate and close our descriptors
		dup2(CHILD_READ,0);
		dup2(CHILD_WRITE,1);
		
		close(CHILD_READ);
		close(CHILD_WRITE);
		
		// execute ccmanage
		execlp(pphp,pphp,"-q",pccmanage,"daemon",pusername,pcommand,parguments,(char *)NULL);
		
		perror("exec");
		_exit(EXIT_FAILURE);
		
		// should never get here
		return EXIT_SUCCESS;
	}
	
	// close the child's ends of the pipes
	close(CHILD_READ);
	close(CHILD_WRITE); 
	
	// write the password to the script's stdin
	write_pipe(PARENT_WRITE,ppassword);
	close(PARENT_WRITE);
	
	// read all output from the script's stdout and send it to the socket
	read_pipe_to_socket(PARENT_READ,sock);
	close(PARENT_READ);
	
	return EXIT_SUCCESS;
}

// shifts the first space-delimited token off the beginning of pcommand
// and returns a pointer to the next token in the string
char *get_cmd_token(char *pcommand,char *token, int size) {
	int len = 0;
	
	while (*pcommand == ' ') pcommand++; // remove leading whitespace
	
	// copy up to first space or null
	while ( (*pcommand != ' ') && (*pcommand != '\0') && (len<size-1) ) {
		token[len++] = *pcommand++;
	}
	token[len] = 0;
	
	return pcommand;
}


// handles a request on the specified client socket
int handle_request(int client_sock, char *pphp, char *pccmanage) {
	char reqbuf[MAX_REQUEST_LENGTH+1];
	char response[MAX_RESPONSE_LENGTH];

	char password[MAX_REQUEST_LENGTH];
	char *ppassword = password;
	char username[MAX_REQUEST_LENGTH];
	char *pusername = username;
	char command[MAX_REQUEST_LENGTH];
	char *pcommand = command;
	
	char *prequest;
	int len;
	int rlen;
	int ret;

	// read the command from the socket
	if ( (len = read_line(client_sock,reqbuf,MAX_REQUEST_LENGTH)) == -1) {
		perror("read_line");
	}
	

       fprintf(stderr,reqbuf);

	// make sure that it's an EXEC command
	prequest = reqbuf;
	if (strstr(prequest,"EXEC ")!=prequest) {
		rlen = snprintf(response,MAX_RESPONSE_LENGTH,"CCD-ERR bad request\n");
		write(client_sock,response,rlen);
		fprintf(stderr,"Does not start with exec\n");
		return -1;
	}
	
	// strip EXEC from the beginning of the string
	prequest += 5;

	// grab the request information from the string
	prequest = get_cmd_token(prequest,pcommand,MAX_REQUEST_LENGTH);
	prequest = get_cmd_token(prequest,pusername,MAX_REQUEST_LENGTH);
	prequest = get_cmd_token(prequest,ppassword,MAX_REQUEST_LENGTH);

	fprintf(stderr,"Will execute with password [%s] command %s -q %s daemon %s %s %s\n",ppassword,pphp,pccmanage,pusername,pcommand,prequest);
	
	// execute the request
	ret = exec_write_socket(client_sock,pphp,pccmanage,pcommand,pusername,ppassword,prequest);
	
	fprintf(stderr,"Child process exiting\n\n");
	
	return 0;
}

// locates the php cli binary and returns its path in buf
int findphp(char *buf, int bufsize) {
	int i;
	struct stat *filestat;
	char *paths[PHP_PATH_COUNT] = PHP_PATHS;
	
	if (strlen(PHP_PATH)>0) {
		snprintf(buf,bufsize,"%s",PHP_PATH);
		return 0;
	}
	
	for (i=0; i<PHP_PATH_COUNT; i++) {
		filestat = malloc(sizeof (struct stat));
		if (stat(paths[i], filestat)==0) {
			
			snprintf(buf,bufsize,"%s",paths[i]);
			free(filestat);
				
			return 0;
		}
		free(filestat);
	}
	
	return -1;
	
}

void handle_sigchld() {
	while(wait3(NULL,WNOHANG,NULL) > 0) ; 
}
