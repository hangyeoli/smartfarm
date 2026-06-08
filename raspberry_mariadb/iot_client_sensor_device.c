#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <mysql/mysql.h>

#define BUF_SIZE 128
#define NAME_SIZE 20
#define ARR_CNT 8

void* send_msg(void* arg);
void* recv_msg(void* arg);
void error_handling(char* msg);

/* v1.7 Arduino format
 * [SF_SQL]SENSOR@illu@temp@humi@waterlevel
 * ex) [SF_SQL]SENSOR@86@26.2@41.0@0
 */
static void update_device_value(MYSQL* conn, const char* dev_name, const char* value);
static int get_device_value(MYSQL* conn, const char* dev_name, char* out, int out_size);

char name[NAME_SIZE] = "[Default]";
char msg[BUF_SIZE];

int main(int argc, char* argv[])
{
	int sock;
	struct sockaddr_in serv_addr;
	pthread_t snd_thread, rcv_thread;
	void* thread_return;

	if (argc != 4) {
		printf("Usage : %s <IP> <port> <name>\n", argv[0]);
		printf("ex) ./iot_client_sensor_device 127.0.0.1 5000 SF_SQL\n");
		exit(1);
	}

	sprintf(name, "%s", argv[3]);

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		error_handling("socket() error");

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
	serv_addr.sin_port = htons(atoi(argv[2]));

	if (connect(sock, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) == -1)
		error_handling("connect() error");

	sprintf(msg, "[%s:PASSWD]", name);
	write(sock, msg, strlen(msg));
	pthread_create(&rcv_thread, NULL, recv_msg, (void*)&sock);
	pthread_create(&snd_thread, NULL, send_msg, (void*)&sock);

	pthread_join(snd_thread, &thread_return);
	pthread_join(rcv_thread, &thread_return);

	if(sock != -1)
		close(sock);
	return 0;
}

void* send_msg(void* arg)
{
	int* sock = (int*)arg;
	int ret;
	fd_set initset, newset;
	struct timeval tv;
	char name_msg[NAME_SIZE + BUF_SIZE + 2];

	FD_ZERO(&initset);
	FD_SET(STDIN_FILENO, &initset);

	fputs("Input a message! [ID]msg (Default ID:ALLMSG)\n", stdout);
	while (1) {
		memset(msg, 0, sizeof(msg));
		name_msg[0] = '\0';
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		newset = initset;
		ret = select(STDIN_FILENO + 1, &newset, NULL, NULL, &tv);
		if (FD_ISSET(STDIN_FILENO, &newset))
		{
			fgets(msg, BUF_SIZE, stdin);
			if (!strncmp(msg, "quit\n", 5)) {
				*sock = -1;
				return NULL;
			}
			else if (msg[0] != '[')
			{
				strcat(name_msg, "[ALLMSG]");
				strcat(name_msg, msg);
			}
			else
				strcpy(name_msg, msg);
			if (write(*sock, name_msg, strlen(name_msg)) <= 0)
			{
				*sock = -1;
				return NULL;
			}
		}
		if (ret == 0)
		{
			if (*sock == -1)
				return NULL;
		}
	}
}

void* recv_msg(void* arg)
{
	MYSQL* conn;
	int res;
	char sql_cmd[512] = { 0 };
	char* host = "localhost";
	char* user = "iot";
	char* pass = "pwiot";
	char* dbname = "iotdb";

	int* sock = (int*)arg;
	int i;
	char* pToken;
	char* pArray[ARR_CNT] = { 0 };

	char name_msg[NAME_SIZE + BUF_SIZE + 1];
	int str_len;

	int illu;
	int waterlevel;
	float temp;
	float humi;
	char value_buf[64];
	char illu_str[16];
	char temp_str[16];
	char humi_str[16];
	char water_str[16];

	conn = mysql_init(NULL);

	puts("MYSQL startup");
	if (!(mysql_real_connect(conn, host, user, pass, dbname, 0, NULL, 0)))
	{
		fprintf(stderr, "ERROR : %s[%d]\n", mysql_error(conn), mysql_errno(conn));
		exit(1);
	}
	else
		printf("Connection Successful!\n\n");

	while (1) {
		memset(name_msg, 0x0, sizeof(name_msg));
		memset(pArray, 0x0, sizeof(pArray));

		str_len = read(*sock, name_msg, NAME_SIZE + BUF_SIZE);
		if (str_len <= 0)
		{
			*sock = -1;
			return NULL;
		}
		name_msg[str_len] = '\0';
		fputs(name_msg, stdout);
		name_msg[strcspn(name_msg, "\n")] = '\0';

		pToken = strtok(name_msg, "[:@]");
		i = 0;
		while (pToken != NULL)
		{
			pArray[i] = pToken;
			if (++i >= ARR_CNT)
				break;
			pToken = strtok(NULL, "[:@]");
		}

		if (i < 2 || pArray[1] == NULL)
			continue;

		/* v1.7 Arduino sensor data
		 * [SF_SQL]SENSOR@illu@temp@humi@waterlevel
		 */
		if (!strcmp(pArray[1], "SENSOR") && (i == 6)) {
			illu = atoi(pArray[2]);
			temp = atof(pArray[3]);
			humi = atof(pArray[4]);
			waterlevel = atoi(pArray[5]);

			sprintf(sql_cmd,
				"insert into sensor(name, date, time, illu, temp, humi, waterlevel) "
				"values('%s', now(), now(), %d, %f, %f, %d)",
				pArray[0], illu, temp, humi, waterlevel);

			res = mysql_query(conn, sql_cmd);
			if (!res)
				printf("inserted %lu rows\n", (unsigned long)mysql_affected_rows(conn));
			else
				fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));

			/* GETDB@illu / TEMP / HUMI / WATERLEVEL 명령용 현재값 저장 */
			sprintf(illu_str, "%d", illu);
			sprintf(temp_str, "%.1f", temp);
			sprintf(humi_str, "%.1f", humi);
			sprintf(water_str, "%d", waterlevel);

			update_device_value(conn, "illu", illu_str);
			update_device_value(conn, "TEMP", temp_str);
			update_device_value(conn, "HUMI", humi_str);
			update_device_value(conn, "WATERLEVEL", water_str);
		}

		/* Client command examples
		 * [SF_SQL]GETDB@WATERLEVEL
		 * [SF_SQL]GETDB@TEMP
		 */
		else if (!strcmp(pArray[1], "GETDB") && i == 3)
		{
			if (get_device_value(conn, pArray[2], value_buf, sizeof(value_buf)) == 0)
				sprintf(sql_cmd, "[%s]%s@%s@%s\n", pArray[0], pArray[1], pArray[2], value_buf);
			else
				sprintf(sql_cmd, "[%s]%s@%s@NULL\n", pArray[0], pArray[1], pArray[2]);

			write(*sock, sql_cmd, strlen(sql_cmd));
		}

		/* Client command examples
		 * [SF_SQL]SETDB@BLIND@UP
		 * [SF_SQL]SETDB@FAN@30
		 * [SF_SQL]SETDB@FAN@30@SF_ARD  -> [SF_ARD]FAN@30 전달
		 */
		else if (!strcmp(pArray[1], "SETDB") && (i == 4 || i == 5)) {
			update_device_value(conn, pArray[2], pArray[3]);

			if (i == 4)
				sprintf(sql_cmd, "[%s]%s@%s@%s\n", pArray[0], pArray[1], pArray[2], pArray[3]);
			else
				sprintf(sql_cmd, "[%s]%s@%s\n", pArray[4], pArray[2], pArray[3]);

			write(*sock, sql_cmd, strlen(sql_cmd));
		}
	}
	mysql_close(conn);
	return NULL;
}

static void update_device_value(MYSQL* conn, const char* dev_name, const char* value)
{
	char sql_cmd[256];

	/* device 테이블에 값이 없으면 insert, 있으면 update */
	sprintf(sql_cmd,
		"insert into device(name, value, date, time) values('%s', '%s', now(), now()) "
		"on duplicate key update value='%s', date=now(), time=now()",
		dev_name, value, value);

	if (mysql_query(conn, sql_cmd))
		fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
}

static int get_device_value(MYSQL* conn, const char* dev_name, char* out, int out_size)
{
	MYSQL_RES* result;
	MYSQL_ROW sqlrow;
	char sql_cmd[256];

	sprintf(sql_cmd, "select value from device where name='%s'", dev_name);

	if (mysql_query(conn, sql_cmd))
	{
		fprintf(stderr, "%s\n", mysql_error(conn));
		return -1;
	}

	result = mysql_store_result(conn);
	if (result == NULL)
	{
		fprintf(stderr, "%s\n", mysql_error(conn));
		return -1;
	}

	sqlrow = mysql_fetch_row(result);
	if (sqlrow == NULL || sqlrow[0] == NULL)
	{
		mysql_free_result(result);
		return -1;
	}

	snprintf(out, out_size, "%s", sqlrow[0]);
	mysql_free_result(result);
	return 0;
}

void error_handling(char* msg)
{
	fputs(msg, stderr);
	fputc('\n', stderr);
	exit(1);
}
