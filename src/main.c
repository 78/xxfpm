/*
 *  Xiaoxia FastCGI Process Manager
 *
 *  Copyright (C) 2008-2011  Huang Guan
 *
 *  Website: www.xiaoxia.org
 *  Email: gdxxhg@gmail.com
 * 
 *  2011/1/30 Created.
 *
 *  Description: This file mainly includes the functions about 
 *
 */

#ifdef __WIN32__

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500
#endif //_WIN32_WINNT

#include <windows.h>
#include <winsock.h>
#include <wininet.h>
#define SHUT_RDWR SD_BOTH

#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (0x2000)
#endif
HANDLE FcpJobObject;

#else

#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#define closesocket close

#endif //__WIN32__


#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#define MAX_PROCESSES	1024
static const char version[] = "$Revision: 0.01 $";
static char* prog_name;
int number = 1;
int port = 8000;
char *ip = "127.0.0.1";
char *user = "";
char *root = "";
char *path = "";
char *group = "";
int listen_fd;
struct sockaddr_in listen_addr;
int process_fp[MAX_PROCESSES];
int process_idx = 0;
pthread_t threads[MAX_PROCESSES];

static struct option longopts[] =
{
	{"help", no_argument, NULL, 'h'},
	{"version", no_argument, NULL, 'v'},
	{"number", required_argument, NULL, 'n'},
	{"ip", required_argument, NULL, 'i'},
	{"port", required_argument, NULL, 'p'},
	{"user", required_argument, NULL, 'u'},
	{"group", required_argument, NULL, 'g'},
	{"root", required_argument, NULL, 'r'},
	{NULL, 0, NULL, 0}
};

static char opts[] = "hvnipugr";

static void usage(FILE* where)
{
	fprintf(where, ""
		"Usage: %s path [-n number] [-i ip] [-p port]\n"
		"Manage FastCGI processes.\n"
		"\n"
		" -n, --number  number of processes to keep\n"
		" -i, --ip      ip address to bind\n"
		" -p, --port    port to bind, default is 8000\n"
		" -u, --user    start processes using specified linux user\n"
		" -g, --group   start processes using specified linux group\n"
		" -r, --root    change root direcotry for the processes\n"
		" -h, --help    output usage information and exit\n"
		" -v, --version output version information and exit\n"
		"", prog_name);
	exit(where == stderr ? 1:0);
}

static void print_version()
{
	printf("%s %s\n\
FastCGI Process Manager\n\
Copyright 2010 Xiaoxia.org\n\
Compiled on %s\n\
", prog_name, version, __DATE__);
	exit(0);
}

static int try_to_bind()
{
	listen_addr.sin_family = PF_INET;
	listen_addr.sin_addr.s_addr = inet_addr( ip );
	listen_addr.sin_port = htons( port );
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	
	if (-1 == bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(struct sockaddr_in)) ) {
		fprintf(stderr, "failed to bind %s:%d\n", ip, port );
		return -1;
	}
	
	listen(listen_fd, MAX_PROCESSES);
	return 0;
}

static void* spawn_process(void* arg)
{
	int idx = process_idx ++, ret;
	while(1){
#ifdef __WIN32__
		STARTUPINFO si={0};
		PROCESS_INFORMATION pi={0};
		ZeroMemory(&si,sizeof(STARTUPINFO));
		si.cb = sizeof(STARTUPINFO);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput  = (HANDLE)listen_fd;
		si.hStdOutput = INVALID_HANDLE_VALUE;
		si.hStdError  = INVALID_HANDLE_VALUE;
		if(0 == (ret=CreateProcess(NULL, path,
			NULL,NULL,
			TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB,
			NULL,NULL,
			&si,&pi)) ){
			fprintf(stderr, "failed to create process %s, ret=%d\n", path, ret);
			return NULL;
		}
		
		/* Use Job Control System */
		if(!AssignProcessToJobObject(FcpJobObject, pi.hProcess)){
			TerminateProcess(pi.hProcess, 1);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return NULL;
		}
		
		if(!ResumeThread(pi.hThread)){
			TerminateProcess(pi.hProcess, 1);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return NULL;
		}
		
		process_fp[idx] = (int)pi.hProcess;
		WaitForSingleObject(pi.hProcess, INFINITE);
		process_fp[idx] = 0;
		CloseHandle(pi.hThread);
#else
		ret = fork();
		switch(ret){
		case 0:{	//child
			/* change uid from root to other user */
			if(getuid()==0){ 
                struct group *grp = NULL;
                struct passwd *pwd = NULL;
				if (*user) {
					if (NULL == (pwd = getpwnam(user))) {
						fprintf(stderr, "[fcgi] %s %s\n", "can't find username", user);
						exit(-1);
					}

					if (pwd->pw_uid == 0) {
						fprintf(stderr, "[fcgi] %s\n", "what? dest uid == 0?" );
						exit(-1);
					}
				}

				if (*group) {
					if (NULL == (grp = getgrnam(group))) {
						fprintf(stderr, "[fcgi] %s %s\n", "can't find groupname", group);
						exit(1);
					}

					if (grp->gr_gid == 0) {
						fprintf(stderr, "[fcgi] %s\n", "what? dest gid == 0?" );
						exit(1);
					}
					/* do the change before we do the chroot() */
					setgid(grp->gr_gid);
					setgroups(0, NULL);

					if (user) {
						initgroups(user, grp->gr_gid);
					}
				}
				if (*root) {
					if (-1 == chroot(root)) {
						fprintf(stderr, "[fcgi] %s %s\n", "can't change root", root);
						exit(1);
					}
					if (-1 == chdir("/")) {
						fprintf(stderr, "[fcgi] %s %s\n", "can't change dir to", root);
						exit(1);
					}
				}

				/* drop root privs */
				if (*user) {
					setuid(pwd->pw_uid);
				}
			}
			
			int max_fd = 0, i=0;
			// Set stdin to listen_fd
			close(STDIN_FILENO);
			dup2(listen_fd, STDIN_FILENO);
			close(listen_fd);
			// Set stdout and stderr to dummy fd
			max_fd = open("/dev/null", O_RDWR);
			close(STDERR_FILENO);
			dup2(max_fd, STDERR_FILENO);
			close(max_fd);
			max_fd = open("/dev/null", O_RDWR);
			close(STDOUT_FILENO);
			dup2(max_fd, STDOUT_FILENO);
			close(max_fd);
			// close other handles
			for(i=3; i<max_fd; i++)
				close(i);
			char *b = malloc(strlen("exec ") + strlen(path) + 1);
			strcpy(b, "exec ");
			strcat(b, path);
			
			/* exec the cgi */
			execl("/bin/sh", "sh", "-c", b, (char *)NULL);
			exit(errno);
			break;
		}
		case -1:
			fprintf(stderr, "[fcgi] fork failed\n");
			return NULL;
		default:{
			struct timeval tv = { 0, 100 * 1000 };
			int status;
			select(0, NULL, NULL, NULL, &tv);
			switch(waitpid(ret, &status, WNOHANG)){
			case 0:
				printf("[fcg] spawned process %s: %d\n", path, ret);
				break;
			case -1:
				fprintf(stderr, "[fcgi] waitpid failed\n");
				return NULL;
			default:
				if (WIFEXITED(status)) {
						fprintf(stderr, "[fcgi] child exited with: %d\n", WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
						fprintf(stderr, "[fcgi] child signaled: %d\n", WTERMSIG(status));
				} else {
						fprintf(stderr, "[fcgi] child died somehow: %d\n", status);
				}
				return NULL;
			}
			//wait for child process to exit
			process_fp[idx] = ret;
			waitpid(ret, &status, 0);
			process_fp[idx] = 0;
		}
		}
#endif
	}
}

static int start_processes()
{
	int i;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64*1024); //64KB
	for(i=0; i<number; i++){
		if( pthread_create( &threads[i], &attr, spawn_process, NULL ) == -1 ){
			fprintf(stderr, "failed to create thread %d\n", i);
		}
	}
	
	for(i=0; i<number; i++){
		pthread_join(threads[i], NULL);
	}
	return 0;
}

#ifdef __WIN32__
void init_win32()
{
	/* init win32 socket */
	static WSADATA wsa_data; 
	if(WSAStartup((WORD)(1<<8|1), &wsa_data) != 0)
		exit(1);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
	FcpJobObject = (HANDLE)CreateJobObject(NULL, NULL);
	if(FcpJobObject == NULL) 
		exit(1);
	
	/* let all processes assigned to this job object
	 * being killed when the job object closed */
	if (!QueryInformationJobObject(FcpJobObject, JobObjectExtendedLimitInformation, &limit, sizeof(limit), NULL)) {
		CloseHandle(FcpJobObject);
		exit(1);
	}

	limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

	if (!SetInformationJobObject(FcpJobObject, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
		CloseHandle(FcpJobObject);
		exit(1);
	}
}
#endif //__WIN32__

#ifndef __WIN32__
void before_exit(int sig)
{
	signal(SIGTERM, SIG_DFL);
	/* call child processes to exit */
	kill(0, SIGTERM);
}
#endif

int main(int argc, char **argv)
{
	prog_name = strrchr(argv[0], '/');
	if(prog_name == NULL)
		prog_name = strrchr(argv[0], '\\');
	if(prog_name == NULL)
		prog_name = argv[0];
	else
		prog_name++;
	
	if(argc == 1)
		usage(stderr);
	
	path = argv[1];
	
	opterr = 0;
	
	char* p;

	for(;;){
		int ch;
		if((ch = getopt_long(argc, argv, opts, longopts, NULL)) == EOF)
			break;
		char *av = argv[optind];
		switch(ch){
		case 'h':
			usage(stdout);
			break;
		case 'v':
			print_version();
			break;
		case 'n':
			number = atoi(av);
			if(number > MAX_PROCESSES){
				fprintf(stderr, "exceeds MAX_PROCESSES!\n");
				number = MAX_PROCESSES;
			}
			break;
		case 'u':
			user = av;
			break;
		case 'r':
			root = av;
			break;
		case 'g':
			group = av;
			break;
		case 'i':
			ip = av;
			break;
		case 'p':
			port = atoi(av);
			break;
		default:
			usage(stderr);
			break;
		}
	}

#ifdef __WIN32__
	init_win32();
#else
	/* call child processes to exit */
	signal(SIGTERM, before_exit);
	signal(SIGINT, before_exit);
	signal(SIGABRT, before_exit);
#endif
	
	int ret;
	ret = try_to_bind();
	if(ret != 0)
		return ret;
	ret = start_processes();
	if(ret !=0)
		return ret;
	
	
#ifdef __WIN32__
	CloseHandle(FcpJobObject);
	WSACleanup();
#endif
	return 0;
}
