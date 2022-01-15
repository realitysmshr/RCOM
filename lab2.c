#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>


#define MAX_BUF 512
#define FTP "ftp://"
#define DEFAULT_USER "anonymous"
#define DEFAULT_PW "anonymous@"

#define MAX_DATA 1024
#define FTP_PORT 21

typedef struct {
  char user[MAX_BUF];
  char pw[MAX_BUF];
  char host[MAX_BUF];
  char path[MAX_BUF];
  char file[MAX_BUF];
  struct hostent* h;
} session;

int is_valid_fd(int fd) {
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

// sending commands
int command(int sockfd, char* cmd, char* retmsg) {

  int done = 0, res = 0, bad = 0, b = 0;
  char get;

  char tmp[MAX_BUF];

  if(cmd != NULL) {
    sprintf(tmp, "command '%s", cmd);
    tmp[strlen(tmp)-2] = '\'';
    tmp[strlen(tmp)-1] = '\0';
    if(send(sockfd, cmd, strlen(cmd), 0) < 0) {
      perror(tmp);
      return -1;
    }
  }

  struct timeval tv;
  fd_set readfds;

  int flag = 0;

  memset(retmsg, 0, MAX_BUF);
  while(!done) {
    if(bad >= 3) {
      fprintf(stderr, "ERROR: Reached attempt limit on %s\n", tmp);
      return -1;
    }
		/*** get byte ***/
    tv.tv_sec = 7; //"select modifies timeout to reflect the amount of time not slept"
    tv.tv_usec = 0;
    FD_ZERO(&readfds);  //select corrupts fdsets
    FD_SET(sockfd, &readfds);
		res = select(sockfd + 1, &readfds, NULL, NULL, &tv);
		if(res == -1) {   //some error occurred in select
			perror(tmp);
			return -1;
		} else if(res == 0) {   //timed out (3s)
      bad++;
      b = 0;
      memset(retmsg, 0, MAX_BUF);
      fprintf(stderr, "Waiting a little longer for a reply...\n\n");
      continue;
		} else if(FD_ISSET(sockfd, &readfds)) {
			//socket has data
			recv(sockfd, &get, 1, 0);
		}

		/*** by this stage function has returned err or get has 1 byte ***/
    if(get == '\n') {
      if(b > 0) {
        if(retmsg[b-1] == '\r') {  //reached \r\n - end of line
          retmsg[b++] = get;
          done = 1;
        } else {   //if b = 0 (first pos) we clean the buffer;
          b = 0;   //message can't start with \n because we only want the last line in a multi-line case
          memset(retmsg, 0, MAX_BUF);
        }
      } else {
        if(flag == 1) {  //receiving multiple \n
          break;
        } else {
          flag = 1;
        }
      }
    } else {
      retmsg[b++] = get;
    }

    if(done == 1) {
      if((retmsg[3] == ' ') && (retmsg[0] >= '1') && (retmsg[0] <= '5')) {
        switch(retmsg[0]) {
          case '1':   //ANOTHER REPLY IS COMMING, BUT FTP CAN QUEUE NEXT COMMAND
          case '2':   //SUCCESS
          case '3':   //WAITING FOR NEXT COMMAND (e.g. USER -> PASS)
            break;

          case '4':   //SEND CMD AGAIN
            if(cmd != NULL) {
              if(bad == 0) {
                fprintf(stderr, "Trying %s again.\n", tmp);
              }
              b = 0;
              bad++;
              done = 0;
              if(send(sockfd, cmd, strlen(cmd), 0) < 0) {
                perror(tmp);
                return -1;
              }
            } else {
              fprintf(stderr, "\t%s\n", retmsg);
              return -1;
            }
            break;

          case '5':   //ERROR
            fprintf(stderr, "\t%s\n", retmsg);
            return -1;
            break;

          default:
            break;
        }
      } else {
        memset(retmsg, 0, MAX_BUF);
        done = 0;
        b = 0;
      }
    }
	}

  return b;
}

// getting user, pw, host, path and file from url
int getInfo(char* url, session* sesh) {

  char* curr_pos;
  curr_pos = url;

  if(memcmp(url, FTP, strlen(FTP)) != 0) {
    fprintf(stderr, "ERROR: Argument must begin with «%s»\n", FTP);
    return -1;
  }

  curr_pos += strlen(FTP); // ftp://u ou ftp://: ou ftp://@
  char* ahead_pos;

  if((curr_pos[0] == '@' && strchr(curr_pos+1, '@') == NULL) || curr_pos[0] == ':') { //no user and pw || empty user and pw

    memcpy(sesh->user, DEFAULT_USER, strlen(DEFAULT_USER) + 1);   //anonymous
    memcpy(sesh->pw, DEFAULT_PW, strlen(DEFAULT_PW) + 1);   //anonymous@

    if((ahead_pos = strchr(url, '@')) == NULL) {
      fprintf(stderr, "ERROR: Argument must contain «@» separating password and host.\n");
      return -1;
    }

  } else {  //get user and pw

    ahead_pos = strchr(curr_pos, ':');   // ftp://user:
    if(ahead_pos == NULL) {
      fprintf(stderr, "ERROR: Argument must have a colon (:) separating username and password.\n");
      return -1;
    }
    memcpy(sesh->user, curr_pos, ahead_pos - curr_pos);   // ftp://user: - ftp://u = strlen(user)
    sesh->user[ahead_pos - curr_pos] = '\0';

    if((ahead_pos = strrchr(ahead_pos, '@')) == NULL) {  //strRchr() because password might contain '@'
      fprintf(stderr, "ERROR: Argument must contain «@» separating password and host.\n");
      return -1;
    }
    curr_pos = strchr(curr_pos, ':') + 1;  //already know it's not null and it's safe to go ahead of :

    memcpy(sesh->pw, curr_pos, ahead_pos - curr_pos);   // ftp://user:password@ - ftp://user:p = strlen(password)
    sesh->pw[ahead_pos - curr_pos] = '\0';  //NOTA: se pw vazia, sesh.pw fica apenas '\0'
  }

  curr_pos = ahead_pos;   // ftp://user:password@

  if((ahead_pos = strchr(ahead_pos, '/')) == NULL) {   // ftp://user:password@host/
    fprintf(stderr, "ERROR: Argument must at least contain a file: «path/file»\n");
    return -1;
  }

  if((curr_pos += 1)[0] == '/') {   // ftp://user:password@h
    fprintf(stderr, "ERROR: Argument must contain a host\n");
    return -1;
  }
  memcpy(sesh->host, curr_pos, ahead_pos - curr_pos);   // ftp://user:password@host/ - ftp://user:password@h
  sesh->host[ahead_pos - curr_pos] = '\0';

  curr_pos = ahead_pos;   // ftp://user:password@host/
  if((ahead_pos = strrchr(ahead_pos, '/')) == NULL) {   // ftp://user:password@host/path/
    fprintf(stderr, "ERROR: Argument must contain a file: «path/file»\n");
    return -1;
  }

  curr_pos++;   // ftp://user:password@host/p
  ahead_pos++;   // ftp://user:password@host/path/f

  memcpy(sesh->path, curr_pos, ahead_pos - curr_pos);   // ftp://user:password@host/path/f - ftp://user:password@host/p
  sesh->path[ahead_pos - curr_pos] = '\0';

  if(!strlen(ahead_pos)) {
    fprintf(stderr, "ERROR: Argument must include a file\n");
    return -1;
  }
  memcpy(sesh->file, ahead_pos, strlen(ahead_pos) + 1);

  if ((sesh->h = gethostbyname(sesh->host)) == NULL) {
    //herror("gethostbyname");
    return -2;
  }

  //show info obtained from input
  fprintf(stderr, "\n\tUsername: %s", sesh->user);
  fprintf(stderr, "\n\tPassword: ");
  int i = 0;
  for(i = 0; i < (int) strlen(sesh->pw); i++) {
    fprintf(stderr, "*");
  }
  fprintf(stderr, "\n\tHost: %s (%s)", sesh->host, inet_ntoa(*((struct in_addr *)sesh->h->h_addr)));
  fprintf(stderr, "\n\tPath: %s", sesh->path);
  fprintf(stderr, "\n\tFile: %s\n\n", sesh->file);

  return 0;
}

// establishing connection to server in FTP_PORT
int start_connection(char* host, int port) {

  struct sockaddr_in remote_server;
  int sockfd, res;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("start_connection()");
    return -1;
  }

  // IPv4
  remote_server.sin_family = AF_INET;
  // host to network short
  remote_server.sin_port = htons(port);
  // passing string host to IP address number
  res = inet_pton(remote_server.sin_family, host, &(remote_server.sin_addr));
  if(res == 0) {
    fprintf(stderr, "ERROR:  Invalid address in inet_pton()\n");
    return -1;
  } else if(res == -1) {
    perror("start_connection()");
    return -1;
  }

  memset(&remote_server.sin_zero, 0, 8);

  if ((connect(sockfd, (struct sockaddr*)&remote_server, sizeof(remote_server))) < 0) {
    perror("start_connection()");
    return -1;
  }

  return sockfd;
}

// control connection (give user + pass & get permission to Data Connection)
int permission(int sockfd, session sesh) {

  char user[MAX_BUF];
  char pass[MAX_BUF];
  char retval[MAX_BUF];

  // receiving connection confirmation
  if(command(sockfd, NULL, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  // passing USER to the server by command channel
  sprintf(user, "USER %s\r\n", sesh.user);
  if(command(sockfd, user, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  // passing PASS to the server by command channel
  sprintf(pass, "PASS %s\r\n", sesh.pw);
  if(command(sockfd, pass, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  return 0;
}

// entering passive mode (give PASV & get IP + Data Connection Port)
int getIntoPassiveMode(int sockfd, char* IP, int* port) {

  char pasv[MAX_BUF];
  char retval[MAX_BUF];

  sprintf(pasv, "PASV\r\n");
  if(command(sockfd, pasv, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  int ip_recv[6];
  char* data = strchr(retval, '(');
  sscanf(data, "(%d,%d,%d,%d,%d,%d)", &ip_recv[0],&ip_recv[1],&ip_recv[2],&ip_recv[3],&ip_recv[4],&ip_recv[5]);
  sprintf(IP, "%d.%d.%d.%d", ip_recv[0],ip_recv[1],ip_recv[2],ip_recv[3]);
  *port = ip_recv[4]*256 + ip_recv[5];

  return 0;
}

int retrieve(int sockfd, char* path, char* file) {

  char send[MAX_BUF];
  char retval[MAX_BUF];

  /* in RFC 959: "Image type is intended for the efficient storage and
   retrieval of files and for the transfer of binary data." */
  sprintf(send, "TYPE I\r\n");  //standardized, continuous transfer (e.g. new lines or GIF format files)
  if(command(sockfd, send, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  sprintf(send, "RETR %s%s\r\n", path, file);
  if(command(sockfd, send, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  return 0;
}

int getFile(int sockfd, char* file) {

  int fd, retval, bytes = 0;
  char tmp[MAX_DATA];

  fd = open(file, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
  if(fd < 0) {
    perror("readFile()");
    return -1;
  }

  struct timeval tv;
  fd_set readfds;

  do {
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    retval = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if(retval == -1) {
      perror("readFile()"); //some error occurred in select
      close(fd);
      return -1;
    } else if(retval == 0) {
      //timed out
      fprintf(stderr, "ERROR: timed out waiting for data\n");
      close(fd);
      return -1;
    } else if(FD_ISSET(sockfd, &readfds)) {
      //socket has data -> read
      if((bytes = recv(sockfd, tmp, MAX_DATA, 0)) < 0) {
        perror("readFile()");
        close(fd);
        return -1;
      }
      if(write(fd, tmp, bytes) < 0) {
        perror("readFile()");
        close(fd);
        return -1;
      }
    }

  } while(bytes > 0);  //bytes = 0 -> EOF

  if(close(fd) < 0) {
    perror("getfile()");
    return -1;
  }

  return 0;
}

int quit(int controlSocket, int dataSocket) {

  char retval[MAX_BUF];
  char quit[MAX_BUF];

  sprintf(quit, "QUIT\r\n");
  if(command(controlSocket, quit, retval) < 0) {
    return -1;
  }
  fprintf(stderr, "\t%s\n", retval);

  if(shutdown(dataSocket, SHUT_RDWR) < 0 || shutdown(controlSocket, 1) < 0) {
    perror("quit()");
    return -1;
  }
  while (command(controlSocket, NULL, retval) > 0) {
    fprintf(stderr, "\t%s\n", retval);
  }
  if(is_valid_fd(dataSocket)) {
    if(close(dataSocket) < 0) {
      perror("quit()");
      return -1;
    }
  }
  if(is_valid_fd(controlSocket)) {
    if(close(controlSocket) < 0) {
      perror("quit()");
      return -1;
    }
  }

  return 0;
}

int main(int argc, char** argv) {

  //argv[0] = ./download | argv[1] = ftp://user:password@host/path/file
  if(argc != 2) {
    fprintf(stderr, "ERROR: Usage: %s ftp://user:password@host/path/file\n\n", argv[0]);
    exit(-1);
  }

  int control_socket, data_socket, data_port, retval;
  char IP[MAX_BUF];
  session sesh;

  fprintf(stderr, "\nReading URL info...\n");

  retval = getInfo(argv[1], &sesh);
  if(retval < 0) {
    if(retval == -1) {
      fprintf(stderr, "\nERROR: failed to get info from user inserted url\n\n");
      exit(-1);
    } else if(retval == -2) {
      herror("\nERROR");
      fprintf(stderr, "\n\n");
      exit(-1);
    }
  }

  fprintf(stderr, "Printing server responses so user knows program state...\n\n");

  control_socket = start_connection(inet_ntoa(*((struct in_addr *)sesh.h->h_addr)), FTP_PORT);
  if(control_socket < 0) {
    fprintf(stderr, "\nERROR: failed to open control channel\n\n");
    exit(-1);
  }

  if(permission(control_socket, sesh) < 0) {
    fprintf(stderr, "\nERROR: Login is not correct\n\n");
    exit(-1);
  }

  if(getIntoPassiveMode(control_socket, IP, &data_port) < 0) {
    fprintf(stderr, "\nERROR: Failed to get into passive mode.\n\n");
    exit(-1);
  }

  data_socket = start_connection(inet_ntoa(*((struct in_addr *)sesh.h->h_addr)), data_port);
  if (data_socket < 0) {
    fprintf(stderr, "\nERROR: failed to open data channel\n\n");
    exit(-1);
  }

  if(retrieve(control_socket, sesh.path, sesh.file) < 0) {
    fprintf(stderr, "\nERROR: failed to send retrieve request for desired file\n\n");
    exit(-1);
  }

  if(getFile(data_socket, sesh.file) < 0) {
    fprintf(stderr, "\nERROR: failed to download file from server\n\n");
    exit(-1);
  }

  if(quit(control_socket, data_socket) < 0) {
    fprintf(stderr, "\nERROR: failed to correctly close connection\n\n");
    exit(-1);
  }

  return 0;
}