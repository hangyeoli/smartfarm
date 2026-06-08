/* SmartFarm IoT Client - example code format based on raspberry_iot_socket/iot_client.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#define BUF_SIZE 100
#define NAME_SIZE 20
#define ARR_CNT 8

#define SQL_ID "SF_SQL"
#define ARD_ID "SF_ARD"
#define STM_ID "SF_STM"

void * send_msg(void * arg);
void * recv_msg(void * arg);
void error_handling(char * msg);
static void make_auto_route_msg(char *out, size_t outsz, const char *in);

char name[NAME_SIZE]="[Default]";
char msg[BUF_SIZE];

int main(int argc, char *argv[])
{
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t snd_thread, rcv_thread;
    void * thread_return;

    if(argc != 4) {
        printf("Usage : %s <IP> <port> <name>\n",argv[0]);
        printf("ex) ./iot_client 127.0.0.1 5000 SF_CLI\n");
        exit(1);
    }

    sprintf(name, "%s",argv[3]);

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock == -1)
        error_handling("socket() error");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if(connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("connect() error");

    sprintf(msg,"[%s:PASSWD]",name);
    write(sock, msg, strlen(msg));
    pthread_create(&rcv_thread, NULL, recv_msg, (void *)&sock);
    pthread_create(&snd_thread, NULL, send_msg, (void *)&sock);

    pthread_join(snd_thread, &thread_return);
    close(sock);
    return 0;
}

static void make_auto_route_msg(char *out, size_t outsz, const char *in)
{
    char buf[BUF_SIZE];
    strncpy(buf, in, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    buf[strcspn(buf,"\r\n")] = '\0';

    if(buf[0] == '[') {
        snprintf(out, outsz, "%s\n", buf);                /* original example style */
    }
    else if(!strncmp(buf,"GETDB@",6) || !strncmp(buf,"SETDB@",6)) {
        snprintf(out, outsz, "[%s]%s\n", SQL_ID, buf);    /* ex) GETDB@WATERLEVEL */
    }
    else if(!strncmp(buf,"FAN@",4) || !strncmp(buf,"LED@",4) || !strncmp(buf,"GETSENSOR@",10)) {
        snprintf(out, outsz, "[%s]%s\n", ARD_ID, buf);    /* ex) FAN@30 */
    }
    else if(!strncmp(buf,"STM@",4)) {
        snprintf(out, outsz, "[%s]%s\n", STM_ID, buf+4);  /* ex) STM@LCD@HELLO */
    }
    else if(!strncmp(buf,"GETTIME",7)) {
        snprintf(out, outsz, "[GETTIME]TIME\n");
    }
    else if(!strncmp(buf,"IDLIST",6)) {
        snprintf(out, outsz, "[IDLIST]LIST\n");
    }
    else {
        snprintf(out, outsz, "[ALLMSG]%s\n", buf);
    }
}

void * send_msg(void * arg)
{
    int *sock = (int *)arg;
    int ret;
    fd_set initset, newset;
    struct timeval tv;
    char name_msg[NAME_SIZE + BUF_SIZE + 4];

    FD_ZERO(&initset);
    FD_SET(STDIN_FILENO, &initset);

    fputs("Input message\n",stdout);
    fputs("  GETDB@WATERLEVEL  -> DB value read\n",stdout);
    fputs("  FAN@30            -> Arduino FAN PWM 30%\n",stdout);
    fputs("  [SF_ARD]FAN@30    -> original direct format\n",stdout);

    while(1) {
        memset(msg,0,sizeof(msg));
        name_msg[0] = '\0';
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        newset = initset;
        ret = select(STDIN_FILENO + 1, &newset, NULL, NULL, &tv);
        if(FD_ISSET(STDIN_FILENO, &newset))
        {
            fgets(msg, BUF_SIZE, stdin);
            if(!strncmp(msg,"quit\n",5)) {
                *sock = -1;
                return NULL;
            }
            make_auto_route_msg(name_msg, sizeof(name_msg), msg);
            if(write(*sock, name_msg, strlen(name_msg))<=0)
            {
                *sock = -1;
                return NULL;
            }
        }
        if(ret == 0 && *sock == -1)
            return NULL;
    }
}

void * recv_msg(void * arg)
{
    int * sock = (int *)arg;
    char name_msg[NAME_SIZE + BUF_SIZE +1];
    int str_len;
    while(1) {
        memset(name_msg,0x0,sizeof(name_msg));
        str_len = read(*sock, name_msg, NAME_SIZE + BUF_SIZE );
        if(str_len <= 0)
        {
            *sock = -1;
            return NULL;
        }
        name_msg[str_len] = 0;
        fputs(name_msg, stdout);
    }
}

void error_handling(char * msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}
