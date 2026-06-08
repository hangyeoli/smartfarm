/*
  SmartFarm Arduino UNO WiFi Client
  Date : 2026.05.29
  Developer : HEB
  version : v1.7 minimal patch from v1.1
  Sensor -> Raspberry Pi socket server -> MariaDB / STM32

  Fix point
  - duplicated secCount removed
  - getSensorTime / BTSerial / tempStr scope error fixed
  - DHT NaN check added
  - auto FAN control added when temp or humidity is high
*/

#include "WiFiEsp.h"
#include "SoftwareSerial.h"
#include <DHT.h>
#include <Servo.h>
Servo myServo;

//#define DEBUG_WIFI
#define DEBUG

#define AP_SSID "KCCI601"
#define AP_PASS "@kcci601@"
#define SERVER_NAME "10.10.16.90"   // Raspberry Pi IP
#define SERVER_PORT 5000
#define LOGID "SF_ARD"
#define PASSWD "PASSWD"

// PIN
#define WIFIRX 8        // Arduino D8 RX <- ESP8266 TX
#define WIFITX 9        // Arduino D9 TX -> ESP8266 RX
#define DHT_PIN 4       // DHT11
#define FAN_PIN 3       // PWM fan
#define LED_PIN 7       // low light LED
#define SERVO_PIN 10

// Analog Input PINs
#define WATER_PIN A0
#define CDS_PIN A1

#define CMD_SIZE 90
#define ARR_CNT 8
#define DHTTYPE DHT11

// 자동 제어 기준값
#define CDS_DARK_LEVEL 60          // CDS 값이 60 미만이면 저조도
#define TEMP_FAN_ON 30.0           // 온도 30도 이상이면 FAN 자동 ON
#define TEMP_FAN_OFF 26.0          // 온도 26도 이하이면 FAN 자동 OFF 조건
#define HUMI_FAN_ON 70.0           // 습도 70% 이상이면 FAN 자동 ON
#define HUMI_FAN_OFF 65.0          // 습도 65% 이하이면 FAN 자동 OFF 조건
#define FAN_AUTO_PWM 100           // 자동 동작 시 FAN PWM percent

// 블라인드 서보 제어
#define BLIND_UP_SERVO_ANGLE 0
#define BLIND_DOWN_SERVO_ANGLE 180
#define BLIND_MOVE_TIME 700

char sendBuf[CMD_SIZE];
char recvId[10] = "SF_LIN"; // SQL 저장 클라이이언트 ID

volatile bool timerIsrFlag = false;
bool blind_state = false;
unsigned long secCount = 0;
unsigned long prevOneSecMillis = 0;

int sensorTime = 5;
int cds = 0;
int waterlevel = 0;
float humi = 0.0;
float temp = 0.0;

bool cdsFlag = false;
bool fanAutoOn = false;
int manualFanPwm = 0;    // client에서 FAN@30을 보내면 30 저장
int currentFanPwm = 0;

SoftwareSerial wifiSerial(WIFIRX, WIFITX);
WiFiEspClient client;
DHT dht(DHT_PIN, DHTTYPE);

typedef struct {
  int year;
  int month;
  int day;
  int hour;
  int min;
  int sec;
} DATETIME;
DATETIME dateTime = { 26, 5, 29, 11, 0, 0 };

void wifi_Setup(void);
void wifi_Init(void);
int server_Connect(void);
void socketEvent(void);
void printWifiStatus(void);
void tickDateTime(void);
int clampPercent(int value);
void setFanPwm(int pwmPercent);
void autoFanControl(void);
void readSensors(void);
void sendSensorData(void);
void controlLowLightLamp(void);
void blindUp(void);
void blindDown(void);
void toggleBlind(void);
bool handleBlindCommand(const char *cmd);

void setup() {
#ifdef DEBUG
  Serial.begin(115200);
  Serial.println("setup() start!!");
#endif

  pinMode(FAN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(CDS_PIN, INPUT);
  pinMode(WATER_PIN, INPUT);

  // SG90은 setup에서 계속 attach하지 않습니다.
  // BLIND 명령이 들어올 때만 attach -> write -> delay -> detach 합니다.

  digitalWrite(LED_PIN, LOW);
  setFanPwm(0);

  dht.begin();
  wifi_Setup();

  prevOneSecMillis = millis();
}

void loop() {
  if (client.available()) {
    socketEvent();
  }

  // TimerOne 제거: v1.1의 timerIsrFlag 구조는 유지하고, 1초 tick만 millis()로 만듭니다.
  if (millis() - prevOneSecMillis >= 1000UL) {
    prevOneSecMillis += 1000UL;
    timerIsrFlag = true;
    secCount++;
  }

  if (timerIsrFlag) {
    timerIsrFlag = false;
    tickDateTime();

    if (!client.connected()) {
      server_Connect();
    }

    // sensorTime초마다 센서 측정/전송/자동제어
    if (sensorTime > 0 && !(secCount % sensorTime)) {
      readSensors();
      controlLowLightLamp();
      autoFanControl();
      sendSensorData();
    }

  }
}

void readSensors() {
  char tempStr[8];
  char humiStr[8];

  waterlevel = analogRead(WATER_PIN);
  waterlevel = map(waterlevel, 0, 1023, 0, 100);
  waterlevel = clampPercent(waterlevel);

  cds = map(analogRead(CDS_PIN), 50, 1000, 0, 100);
  cds = clampPercent(cds);

  float newHumi = dht.readHumidity();
  float newTemp = dht.readTemperature();

  // DHT11 읽기 실패 시 NaN이 나올 수 있으므로 이전 값을 유지
  if (!isnan(newHumi)) humi = newHumi;
  if (!isnan(newTemp)) temp = newTemp;

  dtostrf(temp, 4, 1, tempStr);
  dtostrf(humi, 4, 1, humiStr);

#ifdef DEBUG
  Serial.print("cds:"); Serial.print(cds);
  Serial.print(" temp:"); Serial.print(tempStr);
  Serial.print(" humi:"); Serial.print(humiStr);
  Serial.print(" water:"); Serial.print(waterlevel);
  Serial.print(" fan:"); Serial.println(currentFanPwm);
#endif
}

void sendSensorData() {
  char tempStr[8];
  char humiStr[8];

  dtostrf(temp, 4, 1, tempStr);
  dtostrf(humi, 4, 1, humiStr);

  // DB insert and graph
  sprintf(sendBuf, "[SF_SQL]SENSOR@%d@%s@%s@%d\n", cds, tempStr, humiStr, waterlevel);
  client.write(sendBuf, strlen(sendBuf));
  client.flush();

  /* STM32 직접 전송 삭제 (SQL 클라이언트가 지연을 두고 대신 전달하여 TCP/UART 패킷 뭉침/유실 방지) 
  sprintf(sendBuf, "[SF_STM]SENSOR@%d@%s@%s@%d\n", cds, tempStr, humiStr, waterlevel);
  client.write(sendBuf, strlen(sendBuf));
  client.flush();
  */
}

void controlLowLightLamp() {
  if ((cds >= CDS_DARK_LEVEL) && cdsFlag) {
    cdsFlag = false;
    digitalWrite(LED_PIN, LOW);
    sprintf(sendBuf, "[%s]LAMP@OFF\n", LOGID);
    client.write(sendBuf, strlen(sendBuf));
    client.flush();
  }
  else if ((cds < CDS_DARK_LEVEL) && !cdsFlag) {
    cdsFlag = true;
    digitalWrite(LED_PIN, HIGH);
    sprintf(sendBuf, "[%s]LAMP@ON\n", LOGID);
    client.write(sendBuf, strlen(sendBuf));
    client.flush();
  }
}

void autoFanControl() {
  // 온도 또는 습도가 높으면 자동으로 강하게 동작
  if (temp >= TEMP_FAN_ON || humi >= HUMI_FAN_ON) {
    fanAutoOn = true;
  }
  // 둘 다 충분히 내려가면 자동 동작 해제
  else if (temp <= TEMP_FAN_OFF && humi <= HUMI_FAN_OFF) {
    fanAutoOn = false;
  }

  // 수동 FAN@30 값이 있어도 고온/고습이면 자동값이 우선됨
  if (fanAutoOn) {
    if (manualFanPwm < FAN_AUTO_PWM) {
      setFanPwm(FAN_AUTO_PWM);
    } else {
      setFanPwm(manualFanPwm);
    }
  } else {
    setFanPwm(manualFanPwm);
  }
}

void blindUp() {
  // SG90 일반 서보: UP 위치로 이동 후 detach
  if (blind_state == false) {
    myServo.attach(SERVO_PIN);
    myServo.write(BLIND_UP_SERVO_ANGLE);
    delay(BLIND_MOVE_TIME);
    myServo.detach();

    blind_state = true;
#ifdef DEBUG
    Serial.println("BLIND STATE: UP");
#endif
  }
}

void blindDown() {
  // SG90 일반 서보: DOWN 위치로 이동 후 detach
  if (blind_state == true) {
    myServo.attach(SERVO_PIN);
    myServo.write(BLIND_DOWN_SERVO_ANGLE);
    delay(BLIND_MOVE_TIME);
    myServo.detach();

    blind_state = false;
#ifdef DEBUG
    Serial.println("BLIND STATE: DOWN");
#endif
  }
}

void toggleBlind() {
  if (blind_state == false) {
    blindUp();
  } else {
    blindDown();
  }
}

bool handleBlindCommand(const char *cmd) {
  // BLIND@T : 현재 상태 반대로 토글
  if (!strcmp(cmd, "T")) {
    toggleBlind();
    return true;
  }
  // BLIND@UP : 블라인드 올림
  else if (!strcmp(cmd, "UP")) {
    blindUp();
    return true;
  }
  // BLIND@DOWN : 블라인드 내림
  else if (!strcmp(cmd, "DOWN")) {
    blindDown();
    return true;
  }

  return false;
}

void socketEvent() {
  int i = 0;
  char* pToken;
  char* pArray[ARR_CNT] = { 0 };
  char recvBuf[CMD_SIZE] = { 0 };
  int len;

  len = client.readBytesUntil('\n', recvBuf, CMD_SIZE - 1);
  recvBuf[len] = '\0';
  client.flush();

#ifdef DEBUG
  Serial.print("recv : ");
  Serial.print(recvBuf);
#endif

  pToken = strtok(recvBuf, "[@]");
  while (pToken != NULL) {
    pArray[i] = pToken;
    if (++i >= ARR_CNT) break;
    pToken = strtok(NULL, "[@]");
  }

  if (i < 2) return;

  // 혹시 서버가 ID 없이 BLIND@T 그대로 보내는 경우도 처리
  if (!strcmp(pArray[0], "BLIND")) {
    if (!handleBlindCommand(pArray[1])) return;
    sprintf(sendBuf, "[%s]BLIND@%s\n", LOGID, blind_state ? "UP" : "DOWN");
  }
  else if (!strncmp(pArray[1], " New connected", 4)) {
    Serial.write('\n');
    return;
  }
  else if (!strncmp(pArray[1], " Alr", 4)) {
    Serial.write('\n');
    client.stop();
    server_Connect();
    return;
  }
  else if (!strcmp(pArray[1], "LED")) {
    if (i < 3) return;

    if (!strcmp(pArray[2], "ON")) {
      digitalWrite(LED_PIN, HIGH);
      cdsFlag = true;
    }
    else if (!strcmp(pArray[2], "OFF")) {
      digitalWrite(LED_PIN, LOW);
      cdsFlag = false;
    }

    sprintf(sendBuf, "[%s]LED@%s\n", pArray[0], digitalRead(LED_PIN) ? "ON" : "OFF");
  }
  else if (!strcmp(pArray[1], "FAN")) {
    if (i < 3) return;

    manualFanPwm = clampPercent(atoi(pArray[2]));
    autoFanControl();

    sprintf(sendBuf, "[%s]FAN@%d\n", pArray[0], currentFanPwm);
  }
  else if (!strcmp(pArray[1], "BLIND")) {
    if (i < 3) return;

    // 다른 클라이언트에서 BLIND@T 입력 시 블라인드 토글
    if (!handleBlindCommand(pArray[2])) return;

    sprintf(sendBuf, "[%s]BLIND@%s\n", pArray[0], blind_state ? "UP" : "DOWN");
  }
  else if (!strcmp(pArray[1], "GETSENSOR")) {
    if (i < 3) return;

    sensorTime = atoi(pArray[2]);
    if (sensorTime <= 0) sensorTime = 5;

    sprintf(sendBuf, "[%s]GETSENSOR@%d\n", pArray[0], sensorTime);
  }
  else if (!strcmp(pArray[1], "GETSTATE")) {
    sprintf(sendBuf, "[%s]STATE@LED_%s@FAN_%d@AUTO_%s@BLIND_%s\n",
            pArray[0],
            digitalRead(LED_PIN) ? "ON" : "OFF",
            currentFanPwm,
            fanAutoOn ? "ON" : "OFF",
            blind_state ? "UP" : "DOWN");
  }
  else {
    return;
  }

  client.write(sendBuf, strlen(sendBuf));
  client.flush();

#ifdef DEBUG
  Serial.print(", send : ");
  Serial.print(sendBuf);
#endif
}

void wifi_Setup() {
  wifiSerial.begin(38400);
  wifi_Init();
  server_Connect();
}

void wifi_Init() {
  do {
    WiFi.init(&wifiSerial);
    if (WiFi.status() == WL_NO_SHIELD) {
#ifdef DEBUG
      Serial.println("WiFi shield not present");
#endif
    } else {
      break;
    }
  } while (1);

  while (WiFi.begin(AP_SSID, AP_PASS) != WL_CONNECTED) {
#ifdef DEBUG
    Serial.println("Attempting to connect to WPA SSID...");
#endif
  }

#ifdef DEBUG
  Serial.println("You're connected to the network");
  printWifiStatus();
#endif
}

int server_Connect() {
#ifdef DEBUG
  Serial.println("Starting connection to server...");
#endif

  if (client.connect(SERVER_NAME, SERVER_PORT)) {
#ifdef DEBUG
    Serial.println("Connected to server");
#endif
    client.print("[" LOGID ":" PASSWD "]");
    return 1;
  }
  else {
#ifdef DEBUG
    Serial.println("server connection failure");
#endif
    return 0;
  }
}

void printWifiStatus() {
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);
}


void tickDateTime() {
  dateTime.sec++;

  if (dateTime.sec >= 60) {
    dateTime.sec = 0;
    dateTime.min++;
  }
  if (dateTime.min >= 60) {
    dateTime.min = 0;
    dateTime.hour++;
  }
  if (dateTime.hour >= 24) {
    dateTime.hour = 0;
    dateTime.day++;
  }
}

int clampPercent(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

void setFanPwm(int pwmPercent) {
  pwmPercent = clampPercent(pwmPercent);
  currentFanPwm = pwmPercent;
  analogWrite(FAN_PIN, map(pwmPercent, 0, 100, 0, 255));
}
