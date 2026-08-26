/*
  Weather Station with STM32 Nucleo, BME280, OLED SSD1306, 
  PAJ7620 Gesture Sensor, and RGB Comfort Indicator.
  
  Hardware Connections:
  - BME280 & SSD1306 & PAJ7620: Connected via I2C
  - RGB LED: Connected to PWM pins (pinR, pinG, pinB)
  - Button: Connected to pinPrzycisk (USER_BTN)
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h> 
#include <math.h> 
#include <TimeLib.h> 
#include "paj7620.h" // Gesture sensor library

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

Adafruit_BME280 sensor; 

// --- PIN CONFIGURATION ---
const int pinR = D3;
const int pinG = D5;
const int pinB = D6;
const int pinPrzycisk = USER_BTN; 

// --- MENU CONTROL VARIABLES ---
int aktualnaStrona = 1;
bool stanPrzyciskuPoprzedni = HIGH;
unsigned long czasWcisnieciaPrzycisku = 0;
bool przyciskBylWcisniety = false;

// LED blinking logic for high humidity alert
unsigned long poprzedniCzasMigania = 0;
bool stanDiodyMiganie = false;

// --- SCREENSAVER VARIABLES ---
unsigned long ostatniaAktywnosc = 0;          
const unsigned long CZAS_DO_WYGASZENIA = 120000; // 2 minutes timeout
bool wygaszaczAktywny = false;

// Bouncing clock position and speed for screensaver
int saverX = 10;
int saverY = 20;
int saverDX = 1; 
int saverDY = 1;
unsigned long poprzedniRuchSaver = 0;

// --- WEATHER TREND INDICATOR VARIABLES ---
float poprzedniaTemp = 0.0;
float poprzednieCisnienie = 0.0;
float poprzedniaWilgoc = 0.0; 
unsigned long ostatniZapisTendencji = 0;
const unsigned long INTERWAL_TENDENCJI = 60000; 
bool pierwszeUruchomienieTendencji = true;

void setup() {
  Serial.begin(9600); 
  Wire.begin();

  pinMode(pinR, OUTPUT);
  pinMode(pinG, OUTPUT);
  pinMode(pinB, OUTPUT);
  pinMode(pinPrzycisk, INPUT); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    while(1);
  }
  if (!sensor.begin(0x76)) {  
    while (1);
  }

  // PAJ7620 Gesture sensor initialization
  uint8_t error = paj7620Init();
  if (error) {
    Serial.print("Blad inicjalizacji PAJ7620: ");
    Serial.println(error);
  } else {
    Serial.println("PAJ7620 zainicjalizowany poprawnie!");
  }

  setTime(0, 0, 0, 1, 1, 1970); 

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  ostatniaAktywnosc = millis(); 
}

void sprawdzSynchronizacjeSerial() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'T') { 
      unsigned long pctime = Serial.parseInt();
      if (pctime > 1357041600UL) { 
        unsigned long czasCEST = pctime + 7200; // CEST offset (UTC+2)
        setTime(czasCEST); 
      }
    }
  }
}

// --- BREATHING / PULSING EFFECT GENERATOR ---
float generujOddech() {
  // Converts milliseconds into radians (2000ms divisor = ~6.28 sec breath cycle)
  float faza = millis() / 2000.0;
  
  // Maps sin (-1.0 to 1.0) to a smooth 0.0 to 1.0 range
  return (sin(faza) + 1.0) / 2.0;
}

void plynnyKomfortRGB(float t, float h) {
  // Critical alert (>65% humidity) overrides breathing effect with rapid blinking
  if (h > 65.0) {
    if (millis() - poprzedniCzasMigania >= 400) {
      poprzedniCzasMigania = millis();
      stanDiodyMiganie = !stanDiodyMiganie;
    }
    if (stanDiodyMiganie) {
      analogWrite(pinR, 255); analogWrite(pinG, 0); analogWrite(pinB, 0);
    } else {
      analogWrite(pinR, 0); analogWrite(pinG, 0); analogWrite(pinB, 0);
    }
    return;
  }

  // Retrieve current brightness factor from breathing generator
  float wspolczynnikOddechu = generujOddech();

  int r = 0, g = 0, b = 0;
  if (t <= 18.0) b = 255;
  else if (t > 18.0 && t <= 23.0) {
    g = map(t * 10, 180, 230, 0, 255);
    b = map(t * 10, 180, 230, 255, 0);
  } 
  else if (t > 23.0 && t < 28.0) {
    r = map(t * 10, 230, 280, 0, 255);
    g = map(t * 10, 230, 280, 255, 0);
  } 
  else r = 255;

  // Apply breathing factor to target RGB values
  r = (int)(r * wspolczynnikOddechu);
  g = (int)(g * wspolczynnikOddechu);
  b = (int)(b * wspolczynnikOddechu);

  analogWrite(pinR, r);
  analogWrite(pinG, g);
  analogWrite(pinB, b);
}

float obliczPunktRosy(float t, float h) {
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * t) / (b + t)) + log(h / 100.0);
  return (b * alpha) / (a - alpha);
}

void wyswietlDwucyfrowo(int static_liczba) {
  if (static_liczba < 10) display.print('0');
  display.print(static_liczba);
}

void animacjaPrzejscia(bool wPrawo) {
  if (wPrawo) {
    display.startscrollright(0x00, 0x0F);
  } else {
    display.startscrollleft(0x00, 0x0F);
  }
  delay(400); 
  display.stopscroll();
}

void loop() {
  unsigned long obecnyCzas = millis();

  sprawdzSynchronizacjeSerial();

  float temp = sensor.readTemperature();
  float pres = sensor.readPressure() / 100.0F;
  float hum = sensor.readHumidity();
  float punktRosy = obliczPunktRosy(temp, hum);

  plynnyKomfortRGB(temp, hum);

  // --- WEATHER TREND DATA LOGIC ---
  if (pierwszeUruchomienieTendencji) {
    poprzedniaTemp = temp;
    poprzednieCisnienie = pres;
    poprzedniaWilgoc = hum;
    ostatniZapisTendencji = obecnyCzas;
    pierwszeUruchomienieTendencji = false;
  }
  
  if (obecnyCzas - ostatniZapisTendencji >= INTERWAL_TENDENCJI) {
    poprzedniaTemp = temp;
    poprzednieCisnienie = pres;
    poprzedniaWilgoc = hum;
    ostatniZapisTendencji = obecnyCzas;
    Serial.println("Zaktualizowano baze tendencji pogodowych.");
  }

  // --- GESTURE HANDLING (PAJ7620) ---
  uint8_t data = 0;
  paj7620ReadReg(0x43, 1, &data); 

  if (data) {
    ostatniaAktywnosc = obecnyCzas; 
    
    if (wygaszaczAktywny) {
      wygaszaczAktywny = false; 
    } else {
      switch (data) {
        case GES_RIGHT_FLAG: 
          animacjaPrzejscia(true);
          aktualnaStrona++;
          if (aktualnaStrona > 4) aktualnaStrona = 1; 
          delay(100); 
          break;
          
        case GES_LEFT_FLAG: 
          animacjaPrzejscia(false);
          aktualnaStrona--;
          if (aktualnaStrona < 1) aktualnaStrona = 4;
          delay(100);
          break;
      }
    }
  }

  // --- PHYSICAL BUTTON FALLBACK ---
  bool stanPrzycisku = digitalRead(pinPrzycisk);

  if (stanPrzycisku == LOW && stanPrzyciskuPoprzedni == HIGH) {
    czasWcisnieciaPrzycisku = obecnyCzas;
    przyciskBylWcisniety = true;
    ostatniaAktywnosc = obecnyCzas; 
  }

  if (stanPrzycisku == HIGH && stanPrzyciskuPoprzedni == LOW) {
    if (przyciskBylWcisniety) {
      if (wygaszaczAktywny) {
        wygaszaczAktywny = false;
      } else {
        if (obecnyCzas - czasWcisnieciaPrzycisku < 1500) {
          animacjaPrzejscia(true);
          aktualnaStrona++;
          if (aktualnaStrona > 4) aktualnaStrona = 1; 
        }
      }
      przyciskBylWcisniety = false;
    }
  }
  stanPrzyciskuPoprzedni = stanPrzycisku;

  if (obecnyCzas - ostatniaAktywnosc >= CZAS_DO_WYGASZENIA) {
    wygaszaczAktywny = true;
  }

  // --- DISPLAY RENDERING ---
  display.clearDisplay();

  if (wygaszaczAktywny) {
    // --- SCREENSAVER MODE ---
    if (obecnyCzas - poprzedniRuchSaver >= 1000) { 
      poprzedniRuchSaver = obecnyCzas;
      
      saverX += saverDX * 4; 
      saverY += saverDY * 2; 
      
      if (saverX <= 0 || saverX >= 128 - 64) saverDX = -saverDX;
      if (saverY <= 0 || saverY >= 64 - 16) saverDY = -saverDY;
    }

    display.setTextSize(2);
    display.setCursor(saverX, saverY);
    if (year() == 1970) {
      display.print("--:--");
    } else {
      wyswietlDwucyfrowo(hour());
      display.print(":");
      wyswietlDwucyfrowo(minute());
    }

  } else {
    // --- NORMAL OPERATION MODE ---
    if (aktualnaStrona == 1) {
      // PAGE 1: Temperature, Humidity, Pressure
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("TEMPERATURA:");

      display.setTextSize(3); 
      display.setCursor(0, 9); 
      display.print(temp, 1);
      display.setTextSize(2); 
      display.print(" C");

      display.drawFastHLine(0, 34, 128, SSD1306_WHITE);
      
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print("Wilgoc:    "); display.print(hum, 1); display.print(" %");
      
      display.setCursor(0, 52);
      display.print("Cisnienie: "); display.print(pres, 1); display.print(" hPa");

    } else if (aktualnaStrona == 2) {
      // PAGE 2: Dew Point & Comfort Level
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("PUNKT ROSY:");

      display.setTextSize(3); 
      display.setCursor(0, 9); 
      display.print(punktRosy, 1);
      display.setTextSize(2); 
      display.print(" C");

      display.drawFastHLine(0, 34, 128, SSD1306_WHITE);
      
      float odchylenieOdIdealu = abs(punktRosy - 13.0); 
      if (odchylenieOdIdealu > 8.0) odchylenieOdIdealu = 8.0; 
      
      int dlugoscPaska = map(odchylenieOdIdealu * 10, 0, 80, 68, 0); 
      
      display.setTextSize(1);
      display.setCursor(0, 38);
      display.print("Komfort:");
      display.drawRect(54, 38, 72, 7, SSD1306_WHITE); 
      display.fillRect(56, 40, dlugoscPaska, 3, SSD1306_WHITE); 

      display.setCursor(0, 51);
      display.print("Stan: ");
      if (punktRosy < 10.0) display.print("Sucho");
      else if (punktRosy >= 10.0 && punktRosy < 16.0) display.print("Komfortowo");
      else if (punktRosy >= 16.0 && punktRosy < 20.0) display.print("Wilgotno");
      else display.print("Bardzo parno!");

    } else if (aktualnaStrona == 3) {
      // PAGE 3: Real-Time Clock & Date
      if (year() == 1970) {
        display.setTextSize(1);
        display.setCursor(10, 5);
        display.print("Zsynchronizuj czas!");
        
        display.setTextSize(2);
        display.setCursor(12, 22);
        display.print("WYSLIJ T");
        display.setCursor(12, 42);
        display.print("W SERIALU");
      } else {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("AKTUALNY CZAS:");

        display.setTextSize(3);
        display.setCursor(10, 18);
        wyswietlDwucyfrowo(hour());
        display.print(":");
        wyswietlDwucyfrowo(minute());

        display.drawFastHLine(0, 46, 128, SSD1306_WHITE);

        display.setTextSize(1);
        display.setCursor(30, 53);
        wyswietlDwucyfrowo(day());
        display.print(".");
        wyswietlDwucyfrowo(month());
        display.print(".");
        display.print(year());
      }
    } else if (aktualnaStrona == 4) {
      // PAGE 4: Weather Trends
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("TENDENCJE POGODY:");

      display.drawFastVLine(4, 12, 8, SSD1306_WHITE);    
      display.drawCircle(4, 21, 2, SSD1306_WHITE);       
      
      float roznicaTemp = temp - poprzedniaTemp;
      display.setCursor(16, 16); 
      display.print(temp, 1); display.print(" C ");
      
      if (roznicaTemp > 0.2) display.print("[+]");
      else if (roznicaTemp < -0.2) display.print("[-]");
      else display.print("[=]");

      display.drawFastHLine(0, 28, 128, SSD1306_WHITE);

      display.drawCircle(4, 37, 4, SSD1306_WHITE);       
      display.drawLine(4, 37, 6, 35, SSD1306_WHITE);     
      
      float roznicaPres = pres - poprzednieCisnienie;
      display.setCursor(16, 34);
      display.print(pres, 0); display.print(" hPa ");
      
      if (roznicaPres > 0.5) display.print("[+]");
      else if (roznicaPres < -0.5) display.print("[-]");
      else display.print("[=]");

      display.drawFastHLine(0, 45, 128, SSD1306_WHITE);

      display.fillCircle(4, 54, 3, SSD1306_WHITE);       
      display.drawTriangle(1, 54, 7, 54, 4, 49, SSD1306_WHITE); 
      
      float roznicaHum = hum - poprzedniaWilgoc;
      display.setCursor(16, 51);
      display.print(hum, 0); display.print(" % ");
      
      if (roznicaHum > 1.0) display.print("[+]");
      else if (roznicaHum < -1.0) display.print("[-]");
      else display.print("[=]");
    }
  }

  display.display();
}