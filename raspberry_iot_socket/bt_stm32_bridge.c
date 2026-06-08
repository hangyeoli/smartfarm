/* Raspberry Pi <-> STM32 Bluetooth UART bridge */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>

#define BUF_SIZE 128
#define LOGIN_ID "SF_STM"
#define PASSWD   "PASSWD"

static int write_all(int fd, const char *buf, int len)
{
    int sent = 0;
    ssize_t ret;

    while(sent < len) {
        ret = write(fd, buf + sent, len - sent);
        if(ret < 0) {
            if(errno == EINTR) continue;
            return -errno;
        }
        if(ret == 0) return -EIO;
        sent += ret;
    }
    return sent;
}

static void print_write_result(const char *tag, int wr, int want, const char *buf)
{
    if(wr < 0) {
        printf("%s ERR(%d/%d) %s : %s", tag, wr, want, strerror(-wr), buf);
    } else {
        printf("%s(%d/%d) : %s", tag, wr, want, buf);
    }
}

static void error_handling(const char *msg)
{
    perror(msg);
    exit(1);
}

static int open_serial(const char *dev, int baud, int fatal)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        if(fatal) error_handling("open serial");
        printf("BT reopen failed: %s\n", strerror(errno));
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if(tcgetattr(fd, &tio) < 0) {
        if(fatal) error_handling("tcgetattr");
        printf("BT reopen tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);

    speed_t speed = B9600;
    if(baud == 38400) speed = B38400;
    else if(baud == 115200) speed = B115200;

    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;

    if(tcsetattr(fd, TCSANOW, &tio) < 0) {
        if(fatal) error_handling("tcsetattr");
        printf("BT reopen tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, 0);

    return fd;
}

static int reopen_serial(int oldfd, const char *dev, int baud)
{
    if(oldfd >= 0) {
        close(oldfd);
    }

    usleep(300000);
    int fd = open_serial(dev, baud, 0);
    if(fd >= 0) {
        printf("BT reopen OK: %s\n", dev);
    }
    return fd;
}

static int send_to_bt(int *btfd, const char *dev, int baud, const char *buf, int len)
{
    int wr;

    if(*btfd < 0) {
        *btfd = reopen_serial(*btfd, dev, baud);
        if(*btfd < 0) return -ENODEV;
    }

    wr = write_all(*btfd, buf, len);
    if(wr == -EIO || wr == -ENODEV || wr == -EBADF) {
        printf("BT write lost: %s, reopen...\n", strerror(-wr));
        *btfd = reopen_serial(*btfd, dev, baud);
        if(*btfd >= 0) {
            wr = write_all(*btfd, buf, len);
        }
    }

    return wr;
}

static int connect_server(const char *ip, int port)
{
    int sock;
    struct sockaddr_in serv_addr;
    char login[40];

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) error_handling("socket");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    serv_addr.sin_port = htons(port);

    if(connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error_handling("connect server");

    sprintf(login, "[%s:%s]", LOGIN_ID, PASSWD);
    write_all(sock, login, strlen(login));

    return sock;
}

int main(int argc, char *argv[])
{
    int sock, btfd;
    fd_set readfds;
    struct timeval tv;
    char buf[BUF_SIZE];
    char sendbuf[BUF_SIZE + 4];
    int len;
    int wr;
    time_t last_time_req = 0;
    const char *bt_dev;
    int baud;

    const char time_req[] = "[GETTIME]TIME\n";
    const char bt_test[] = "[SF_STM]SENSOR@123@24.0@50.0@7\n";

    if(argc != 5) {
        printf("Usage : %s <server_ip> <port> <bt_dev> <baud>\n", argv[0]);
        printf("ex) ./bt_stm32_bridge 127.0.0.1 5000 /dev/rfcomm0 9600\n");
        return 1;
    }

    bt_dev = argv[3];
    baud = atoi(argv[4]);

    sock = connect_server(argv[1], atoi(argv[2]));
    btfd = open_serial(bt_dev, baud, 1);

    setvbuf(stdout, NULL, _IONBF, 0);

    puts("BT bridge start: socket <-> STM32");

    wr = send_to_bt(&btfd, bt_dev, baud, bt_test, strlen(bt_test));
    print_write_result("BT TEST -> STM32", wr, strlen(bt_test), bt_test);

    write_all(sock, time_req, strlen(time_req));
    puts("TIME REQ -> server");
    last_time_req = time(NULL);

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        if(btfd >= 0) {
            FD_SET(btfd, &readfds);
        }

        int maxfd = (sock > btfd ? sock : btfd) + 1;

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if(select(maxfd, &readfds, NULL, NULL, &tv) < 0)
            error_handling("select");

        if(time(NULL) - last_time_req >= 30) {
            write_all(sock, time_req, strlen(time_req));
            puts("TIME REQ -> server");
            last_time_req = time(NULL);
        }

        if(FD_ISSET(sock, &readfds)) {
            memset(buf, 0, sizeof(buf));
            memset(sendbuf, 0, sizeof(sendbuf));

            len = read(sock, buf, sizeof(buf) - 1);
            if(len <= 0) break;

            memcpy(sendbuf, buf, len);

            if(len > 0 && buf[len - 1] != '\n' && buf[len - 1] != '\r') {
                sendbuf[len] = '\n';
                sendbuf[len + 1] = '\0';
                wr = send_to_bt(&btfd, bt_dev, baud, sendbuf, len + 1);
                print_write_result("S->BT", wr, len + 1, sendbuf);
            } else {
                sendbuf[len] = '\0';
                wr = send_to_bt(&btfd, bt_dev, baud, sendbuf, len);
                print_write_result("S->BT", wr, len, sendbuf);
            }
        }

        if(btfd >= 0 && FD_ISSET(btfd, &readfds)) {
            memset(buf, 0, sizeof(buf));

            len = read(btfd, buf, sizeof(buf) - 1);
            if(len > 0) {
                write_all(sock, buf, len);
                printf("BT->S : %s", buf);
            } else if(len < 0 && errno != EINTR) {
                printf("BT read ERR %s\n", strerror(errno));
                btfd = reopen_serial(btfd, bt_dev, baud);
            }
        }
    }

    if(btfd >= 0) close(btfd);
    close(sock);

    return 0;
}
