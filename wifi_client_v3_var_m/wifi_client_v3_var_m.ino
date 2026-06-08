/* SmartFarm Arduino UNO WiFi Client
 * Based on wifi_client_v3_var_m.ino format.
 * Sensor -> Raspberry Pi socket server -> MariaDB / STM32
 */
#define DEBUG
#define AP_SSID "KCCI601"
#define AP_PASS "@kcci601@"
#define SERVER_NAME "10.10.16.78"   // Raspberry Pi IP
#define SERVER_PORT 5000
#define LOGID "SF_ARD"
#define PASSWD "PASSWD"

#define DHT_PIN 4
#define WIFIRX 6      // Arduino D6 RX  <- ESP8266 TX
#define WIFITX 7      // Arduino D7 TX  -> ESP8266 RX
#define FAN_PIN 11    // PWM fan
#define LED_PIN 13    // low light LED
#define CDS_PIN A0
#define WATER_PIN A1

#define CMD_SIZE 90
#define ARR_CNT 8
#define DHTTYPE DHT11

#include "WiFiEsp.h"
#include "SoftwareSerial.h"
#include <TimerOne.h>
#include <DHT.h>

SoftwareSerial wifiSerial(WIFIRX, WIFITX);
WiFiEspClient client;
DHT dht(DHT_PIN, DHTTYPE);

char sendBuf[CMD_SIZE];
bool timerIsrFlag = false;
unsigned long secCount;
int sensorTime = 5;

int cds;
int waterlevel;
float humi;
float temp;

void wifi_Setup(void);
void wifi_Init(void);
int server_Connect(void);
void socketEvent(void);
void timerIsr(void);
void printWifiStatus(void);

void setup() {
  pinMode(FAN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(CDS_PIN, INPUT);
  pinMode(WATER_PIN, INPUT);

  Serial.begin(115200);
  wifi_Setup();
  Timer1.initialize(1000000);
  Timer1.attachInterrupt(timerIsr);
  dht.begin();
}

void loop() {
  if (client.available()) {
    socketEvent();
  }

  if (timerIsrFlag) {
    timerIsrFlag = false;

    if (!client.connected()) {
      server_Connect();
    }

    if (!(secCount % sensorTime)) {
      char tempStr[8];
      char humiStr[8];

      cds = analogRead(CDS_PIN);
      cds = map(cds, 0, 1023, 0, 100);

      waterlevel = analogRead(WATER_PIN);
      waterlevel = map(waterlevel, 0, 1023, 0, 100);

      humi = dht.readHumidity();
      temp = dht.readTemperature();
      if (isnan(humi) || isnan(temp)) {
        humi = 0;
        temp = 0;
      }

      dtostrf(temp, 4, 1, tempStr);
      dtostrf(humi, 4, 1, humiStr);

      // DB insert and graph (SQL client will forward to STM32)
      sprintf(sendBuf, "[SF_SQL]SENSOR@%d@%s@%s@%d\n", cds, tempStr, humiStr, waterlevel);
      client.write(sendBuf, strlen(sendBuf));
      client.flush();

#ifdef DEBUG
      Serial.print("SENSOR cds:"); Serial.print(cds);
      Serial.print(" temp:"); Serial.print(tempStr);
      Serial.print(" humi:"); Serial.print(humiStr);
      Serial.print(" water:"); Serial.println(waterlevel);
#endif
    }
  }
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
    if (++i >= ARR_CNT)
      break;
    pToken = strtok(NULL, "[@]");
  }

  if (i < 2) return;

  if (!strncmp(pArray[1], " New connected", 4)) {
    Serial.write('\n');
    return;
  } else if (!strncmp(pArray[1], " Alr", 4)) {
    Serial.write('\n');
    client.stop();
    server_Connect();
    return;
  } else if (!strcmp(pArray[1], "LED")) {
    if (!strcmp(pArray[2], "ON"))
      digitalWrite(LED_PIN, HIGH);
    else if (!strcmp(pArray[2], "OFF"))
      digitalWrite(LED_PIN, LOW);

    sprintf(sendBuf, "[%s]LED@%s\n", pArray[0], digitalRead(LED_PIN) ? "ON" : "OFF");
  } else if (!strcmp(pArray[1], "FAN")) {
    int pwm = atoi(pArray[2]);
    if(pwm < 0) pwm = 0;
    if(pwm > 100) pwm = 100;
    analogWrite(FAN_PIN, map(pwm, 0, 100, 0, 255));
    sprintf(sendBuf, "[%s]FAN@%d\n", pArray[0], pwm);
  } else if (!strcmp(pArray[1], "GETSENSOR")) {
    sensorTime = atoi(pArray[2]);
    if(sensorTime <= 0) sensorTime = 5;
    sprintf(sendBuf, "[%s]GETSENSOR@%d\n", pArray[0], sensorTime);
  } else if (!strcmp(pArray[1], "GETSTATE")) {
    sprintf(sendBuf, "[%s]STATE@LED_%s@FAN_PWM\n", pArray[0], digitalRead(LED_PIN) ? "ON" : "OFF");
  } else {
    return;
  }

  client.write(sendBuf, strlen(sendBuf));
  client.flush();

#ifdef DEBUG
  Serial.print(", send : ");
  Serial.print(sendBuf);
#endif
}

void timerIsr() {
  timerIsrFlag = true;
  secCount++;
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
  } else {
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
