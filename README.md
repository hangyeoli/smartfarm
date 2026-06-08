# IoT SmartFarm

구성도 흐름은 `구성도.jpg` 기준입니다.

## 실행 순서

```bash
cd /srv/samba/smartfarm/raspberry_iot_socket
make
./iot_server 5000
```

```bash
cd /srv/samba/smartfarm/raspberry_mariadb/db_query_c
./build.sh

cd ../sql_client
make
./iot_client_sensor_device 127.0.0.1 5000 SF_SQL
```

`mysql_config` 또는 `mysql/mysql.h`가 없다고 나오면 라즈베리파이에서 `sudo apt install libmariadb-dev-compat` 설치 후 다시 `make` 하세요.

### Bluetooth STM32 페어링 (최초 1회만)

```bash
bluetoothctl
# 다음 명령들을 bluetoothctl 대화형 모드에서 실행:
power on
scan on
# 위의 MAC 주소 98:DA:60:08:1F:6B를 발견한 후:
pair 98:DA:60:08:1F:6B
trust 98:DA:60:08:1F:6B
connect 98:DA:60:08:1F:6B
quit

# RFCOMM 바인드 (커널 모듈 필요)
sudo rfcomm bind /dev/rfcomm0 98:DA:60:08:1F:6B
```

### Bluetooth 브릿지 실행

```bash
cd /srv/samba/smartfarm/raspberry_iot_socket
./bt_stm32_bridge 127.0.0.1 5000 /dev/rfcomm0 9600
```

STM32 시간 표시는 `main.c`가 5초마다 Bluetooth UART6로 `[GETTIME]TIME`을 보내면, `bt_stm32_bridge`가 그 요청을 `iot_server`로 전달하고 서버가 현재 시간을 다시 STM32로 보내는 방식입니다.

```bash
cd /srv/samba/smartfarm/raspberry_iot_socket
./iot_client 127.0.0.1 5000 SF_CLI
```

## Client 명령

```text
GETDB@WATERLEVEL
GETDB@TEMP
GETDB@CDS
FAN@30
LED@ON
LED@OFF
GETSENSOR@5
```

`GETDB@WATERLEVEL`을 입력하면 `iot_client`가 `[SF_SQL]GETDB@WATERLEVEL`로 보내고, SQL client가 MariaDB의 `device` 테이블에서 값을 읽어 응답합니다.
