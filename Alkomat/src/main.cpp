#include <SPI.h>
#include <Wire.h>
#include <driver/adc.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_I2CDevice.h>
#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "1813"

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;
BLEAdvertising *pAdvertising = NULL;
bool deviceConnected = false;
uint32_t value = 0;

// Deklaracje dla sterownika SSD1306 (SPI)
#define OLED_MOSI 19
#define OLED_CLK 18
#define OLED_DC 22
#define OLED_CS 23
#define OLED_RESET 21
#define BUTTON_PIN 15
#define ADC 27
#define BUZZER 5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT,
                         OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);

//główna flaga zarządzająca przebiegiem programu, przełączana przerwaniem przycisku
int flag = 0;

//flagi zdarzeń przycisku
bool buttonIsHeld = false;
bool buttonWasHeld = false;
bool blockInput = false;

//zmienne odczytywanych wartości, zmienne kalibracyjne
int calibValue = 0;  //wartość kalibracyjna, x1
int measurement = 0; //wartość zwracana przez ADC
int alcValue = 0;    //wartość pomiaru w promilach * 1000

char const *text = ""; //zmienna tymczasowa wykorzystywana do druku tekstu na wyświetlacz
char *formattedValue;  //wartość pomiaru w promilach przekształcona na tablicę znaków
int temp = 0;          //liczbowa zmienna tymczasowa
int memValue = 0;      //zmienna przechowująca ostatnią odczytaną wartość z pamięci

//zmienne potrzebne do sformatowania wartości promili
char buffer[5];
int ret = 0;

unsigned long lastDebounceTime = 0; //czas wywołania ostatniego przerwania
unsigned long startPressed = 0;     //czas przytrzymania przycisku
unsigned long startMeasuring = 0;   //czas wykonywania pomiaru

//zmienna iteratora pamięci
int address = 0;

//parametry uruchomieniowe buzzera
int freq = 2000;
int resolution = 8;
const int ledChannel = 0;

//parametry czasowe
int heatTime = 10;              //czas nagrzewania, w sekundach
int measureTime = 5;            //czas pomiaru, w sekundach
int measurementDisplayTime = 5; //czas wyświetlania wyniku pomiaru, w sekundach

//współrzędne punktów, do aproksymacji
int ya1 = 0; //y1, nazwa inna bo tamta zarezerwowana

int x2 = 919;
float y2 = 0.13;

int x3 = 1302;
float y3 = 0.89;

//przerwanie przycisku, zabezpieczenie przez drganiami styków, obsługa zdarzeń: kliknięcie/przytrzymanie
void IRAM_ATTR buttonInterrupt()
{
  if (!blockInput)
  {
    if (digitalRead(BUTTON_PIN) == LOW)
    {
      if (startPressed == 0 && (millis() - lastDebounceTime) > 250)
      {
        startPressed = millis();
        buttonIsHeld = true;
        Serial.println("COUNTING");
      }
    }
    else
    {
      if (buttonWasHeld)
      {
        buttonWasHeld = false;
      }
      else if ((millis() - lastDebounceTime) > 100)
      {
        if (flag >= 2)
        {
          flag--;
        }
        else
        {
          flag++;
        }

        blockInput = true; //blokada przycisku
        Serial.print("FLAG SET TO ");
        Serial.println(flag);
      }

      startPressed = 0;
      buttonIsHeld = false;
    }
    lastDebounceTime = millis();
  }
}

//fukcja czyzscząca pamięć zapisanych pomiarów
void eepromClear()
{
  for (int i = 0; i < 10 * sizeof(int); i += sizeof(int))
  {
    EEPROM.get(i, memValue);
    if (memValue != 0) //pominięcie pustych adresów
    {
      EEPROM.put(i, 0); //zadpisywanie pamięci wartością 0
      EEPROM.commit();
    }
  }
  Serial.println("EEPROM erased");
  address = 0; //zerowanie iteratora pamięci
}

//fukcja zapisu podanej wartości value do pamięci pod następny wolny adres
void eepromSave(int value)
{
  EEPROM.put(address, value); //zapis
  EEPROM.commit();
  Serial.print("Sensor value ");
  Serial.print(value);
  Serial.print(" stored at index ");
  Serial.println(address / sizeof(int));

  address += sizeof(int); //inkrementacja iteratora pamięci

  // w przypadku zapełnienia pamięci, następne pomiary zostają zapisane na początku pamięci
  if (address == 10 * sizeof(int))
  {
    address = 0;
  }
}

//fukcja (przeciążona) do wyświetlania tekstu
void OLED(int x, int y, const char *text, int font_size)
{
  display.setCursor(x, y);
  display.setTextSize(font_size);
  display.setTextColor(SSD1306_WHITE);
  display.println(text);
}

//fukcja (przeciążona) do wyświetlania liczby o podstawie dziesiątkowej
void OLED(int x, int y, int value_decimal, int font_size)
{
  display.setCursor(x, y);
  display.setTextSize(font_size);
  display.setTextColor(SSD1306_WHITE);
  display.println(value_decimal, 10);
}

class myServerCallback : public BLEServerCallbacks
{

  void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param)
  {

    char remoteAddress[18];

    sprintf(
        remoteAddress,
        "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X",
        param->connect.remote_bda[0],
        param->connect.remote_bda[1],
        param->connect.remote_bda[2],
        param->connect.remote_bda[3],
        param->connect.remote_bda[4],
        param->connect.remote_bda[5]);

    Serial.print("Device connected, MAC: ");
    Serial.println(remoteAddress);

    pAdvertising->stop();
    pCharacteristic->notify();

    deviceConnected = true;
  }

  void onDisconnect(BLEServer *pServer)
  {

    Serial.println("Device disconnected");

    pAdvertising->start();

    deviceConnected = false;

    flag = 0;
  }
};

void setup()
{
  Serial.begin(115200);

  Serial.begin(115200);
  Serial.println("Starting BLE work!");

  BLEDevice::init("Breathalyzer");
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new myServerCallback());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE);

  pCharacteristic->setValue("BLOCK");
  Serial.println("Set to BLOCK");
  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  //konfiguracja buzzera
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledcSetup(ledChannel, freq, resolution);
  ledcAttachPin(BUZZER, ledChannel);

  //konfiguracja wyświetlacza
  if (!display.begin(SSD1306_SWITCHCAPVCC))
  {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }

  //konfiguracja pamięci
  if (!EEPROM.begin(512))
  {
    Serial.println("failed to initialise EEPROM");
    for (;;)
      ;
  }

  //konfiguracja ADC
  adc1_config_width(ADC_WIDTH_12Bit);
  adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_11db);

  //opóźnienie, proces nagrzewania czujnika
  for (int i = heatTime; i > 0; --i)
  {
    text = "HEATING";

    display.clearDisplay();
    OLED(0, 0, i, 1);
    OLED((SCREEN_WIDTH - (strlen(text) * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, text, 2);
    display.display();

    delay(1000);
  }

  //pobranie wartości kalibracyjnej
  calibValue = analogRead(ADC);
  Serial.print("Calibration value: ");
  Serial.println(calibValue);

  text = "READY";

  display.clearDisplay();
  OLED((SCREEN_WIDTH - (strlen(text) * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, text, 2);
  display.display();

  //włączenie przerwania przycisku
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonInterrupt, CHANGE);
}

void loop()
{
  if (deviceConnected)
  {
    if (flag == 0)
    {
      blockInput = false;

      text = "READY";

      display.clearDisplay();
      OLED(0, 0, "CONNECTED", 1);
      OLED((SCREEN_WIDTH - (strlen(text) * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, text, 2);
      display.display();

      delay(200);
    }

    while (flag == 1)
    {
      if (startMeasuring == 0)
      {
        measurement = 0; //zerowanie wartości odczytu
        Serial.println("Started measuring");
        delay(1000);
        ledcWriteTone(ledChannel, 1000);
        startMeasuring = millis();
      }

      //odczyt wartości z ADC
      int temp = analogRead(ADC);
      if (temp > measurement)
      {
        measurement = temp;
        Serial.println(measurement);
      }

      //aproksymacja
      if (measurement < 944)
      {
        alcValue = 1000 * ((((ya1 - y2) / (calibValue - x2)) * measurement) + (ya1 - ((ya1 - y2) / (calibValue - x2) * calibValue)));
      }
      else
      {
        alcValue = 1000 * ((((y2 - y3) / (x2 - x3)) * measurement) + (y2 - ((y2 - y3) / (x2 - x3)) * x2));
      }
      if (alcValue < 0)
      {
        alcValue = 0;
      }

      display.clearDisplay();
      ret = snprintf(buffer, sizeof buffer, "%f", alcValue / 1000.0);
      OLED((SCREEN_WIDTH - (4 * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, buffer, 2);
      display.display();

      //zdarzenie uruchamia się po ustalonym czasie od rozpoczęcia pomiaru
      if (startMeasuring + (measureTime * 1000) < millis())
      {
        ledcWriteTone(ledChannel, 0);
        for (int i = measurementDisplayTime * 2; i > 0; --i)
        {
          display.clearDisplay();
          OLED(0, 0, ceil(i / 2), 1);

          //konwersja wartości promili na tekst
          ret = snprintf(buffer, sizeof buffer, "%f", alcValue / 1000.0);
          OLED((SCREEN_WIDTH - (4 * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, buffer, 2);
          display.display();
          delay(250);

          display.clearDisplay();
          OLED(0, 0, ceil(i / 2), 1);
          display.display();
          delay(250);
        }
        display.clearDisplay();

        //końcowy druk pomiaru
        ret = snprintf(buffer, sizeof buffer, "%f", alcValue / 1000.0);
        OLED((SCREEN_WIDTH - (4 * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, buffer, 2);
        display.display();

        Serial.print("Zawartość alkoholu wynosi: ");
        Serial.println(alcValue);

        if (alcValue < 100)
        {
          pCharacteristic->setValue("UNLOCK");
          pCharacteristic->notify();
          Serial.println("Sent UNLOCK");
        }

        flag++;

        //zerowanie czasu pomiaru
        startMeasuring = 0;

        //wyłączenie blokady przycisku
        blockInput = false;

        //zerowanie flagi przytrzymania przycisku
        //buttonWasHeld = false;
      }
    }
  }
  else
  {
    blockInput = true;
    text = "READY";

    display.clearDisplay();
    OLED(0, 0, "NOT CONNECTED", 1);
    OLED((SCREEN_WIDTH - (strlen(text) * 11)) / 2, (SCREEN_HEIGHT - 16) / 2, text, 2);
    display.display();

    delay(500);
  }
}
