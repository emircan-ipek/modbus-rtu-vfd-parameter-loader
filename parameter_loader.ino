#include <ModbusMaster.h>
#include <ModbusRTUSlave.h>
#include <Preferences.h>
#include "esp_sleep.h"

#define Drive_Port            (Serial1)
#define Drive_Baud            (9600)
#define Drive_SerialConfig    (SERIAL_8N1)
#define Drive_UART_RxPin      (20)
#define Drive_UART_TxPin      (21)
#define Drive_ID              (1)
#define Drive_GPIO_DR         (10)

#define PC_Port               (Serial)
#define PC_Baud               (115200)

#define EXT_GPIO_Button       (4)
#define DBG_GPIO_BootButton   (9)
#define BLUE_LED_GPIO         (8)

#define RGB_RED_PIN           (1)
#define RGB_GREEN_PIN         (2)
#define RGB_BLUE_PIN          (5)

#define STATUS_POWER_LED_GPIO (6)

#define DEEP_SLEEP_TIMEOUT_MS (30000UL)

#define MAX_PARAM             (100)

void Drive_DR_Tx(){digitalWrite(Drive_GPIO_DR, HIGH);}
void Drive_DR_Rx(){digitalWrite(Drive_GPIO_DR, LOW);}

ModbusMaster mbDrive;
ModbusRTUSlave mbPC(PC_Port);
Preferences hafiza;

// DEĞİŞİKLİK: artik {0} ile degil, setup() icinde 0xFFFF sentinel ile baslatiliyor
uint16_t holdingRegisters[1000];
uint16_t eskiRegisters[1000];

const int bankSlaveIDs[3] = {1, 2, 3};

int activeBank = 0;

int bankAdres[3][MAX_PARAM];
uint16_t bankDeger[3][MAX_PARAM];
int bankSayisi[3] = {0, 0, 0};

bool wasPressed = false;
unsigned long pressStart = 0;

const unsigned long MIN_VALID_MS  = 100;
const unsigned long LONG_PRESS_MS = 600;

bool bootLastState = HIGH;

unsigned long lastActivity = 0;

void setColor(int r, int g, int b)
{
  digitalWrite(RGB_RED_PIN, r ? HIGH : LOW);
  digitalWrite(RGB_GREEN_PIN, g ? HIGH : LOW);
  digitalWrite(RGB_BLUE_PIN, b ? HIGH : LOW);
}

void bankRengineDon()
{
  if (activeBank == 0)      setColor(0, 0, 1); // Bank1 - Mavi
  else if (activeBank == 1) setColor(1, 0, 1); // Bank2 - Mor/Magenta
  else                       setColor(0, 1, 1); // Bank3 - Camgobegi/Cyan
}

void activateBank(int yeniBank)
{
  activeBank = yeniBank;
  bankRengineDon();

  mbPC.begin(bankSlaveIDs[activeBank], PC_Baud);
}

void deleteActiveBankMemory()
{
  int adet = bankSayisi[activeBank];

  for (int k = 0; k < adet; k++)
  {
    int adr = bankAdres[activeBank][k];

    String prefix = "b" + String(activeBank) + "_" + String(k);
    hafiza.remove((prefix + "a").c_str());
    hafiza.remove((prefix + "d").c_str());

    if (adr >= 0 && adr < 1000) 
    {
      // DEĞİŞİKLİK: silinen adresler de 0 yerine sentinel'e donuyor
      holdingRegisters[adr] = 0xFFFF;
      eskiRegisters[adr] = 0xFFFF;
    }
  }

  bankSayisi[activeBank] = 0;
  hafiza.putInt(("n" + String(activeBank)).c_str(), 0);

  Serial.println("============================");
  Serial.println("AKTIF BANK SILME ISLEMI");
  Serial.print("Aktif Bank: ");
  Serial.println(activeBank + 1);
  Serial.print("Silinen Parametre Sayisi: ");
  Serial.println(adet);
  Serial.println("============================");

  digitalWrite(BLUE_LED_GPIO, LOW);
  delay(300);
  digitalWrite(BLUE_LED_GPIO, HIGH);
}

bool yazTekParametre(int adres, uint16_t deger)
{
  delay(15);

  for (int deneme = 0; deneme < 3; deneme++)
  {
    unsigned long t0 = millis();
    uint8_t sonuc = mbDrive.writeSingleRegister(adres, deger);

    Serial.print("YAZMA adres="); Serial.print(adres);
    Serial.print(" deneme="); Serial.print(deneme);
    Serial.print(" sonuc="); Serial.print(sonuc);
    Serial.print(" sure(ms)="); Serial.println(millis() - t0);

    if (sonuc == 0) return true;
  }
  return false;
}

bool dogrulaTekParametre(int adres, uint16_t deger)
{
  delay(15);

  for (int deneme = 0; deneme < 3; deneme++)
  {
    unsigned long t0 = millis();
    uint8_t sonuc = mbDrive.readHoldingRegisters(adres, 1);

    Serial.print("OKUMA adres="); Serial.print(adres);
    Serial.print(" deneme="); Serial.print(deneme);
    Serial.print(" sonuc="); Serial.print(sonuc);
    Serial.print(" sure(ms)="); Serial.println(millis() - t0);

    if (sonuc == 0 && mbDrive.getResponseBuffer(0) == deger) return true;
  }
  return false;
}

bool yazVeDogrula()
{
  int adet = bankSayisi[activeBank];
  if (adet == 0) return false;

  uint8_t baglantiTest = mbDrive.writeSingleRegister(bankAdres[activeBank][0], bankDeger[activeBank][0]);

  if (baglantiTest != 0)
  {
    Serial.println("BAGLANTI YOK - hat kontrolu basarisiz, islem iptal edildi");
    return false;
  }

  unsigned long toplamBaslangic = millis();

  bool girisSonucu[MAX_PARAM];
  bool sonGirisMi[MAX_PARAM];

  // Her adresin listede EN SON gectigi index'i bul (o girisin kalici oldugu anlamina gelir)
  for (int k = 0; k < adet; k++)
  {
    sonGirisMi[k] = true;
    for (int j = k + 1; j < adet; j++)
    {
      if (bankAdres[activeBank][j] == bankAdres[activeBank][k])
      {
        sonGirisMi[k] = false;
        break;
      }
    }
  }

  Serial.println("============================");
  Serial.println("YUKLENECEK PARAMETRE LISTESI:");
  for (int k = 0; k < adet; k++)
  {
    Serial.print("  [");
    Serial.print(k);
    Serial.print("] adres=");
    Serial.print(bankAdres[activeBank][k]);
    Serial.print(" deger=");
    Serial.print(bankDeger[activeBank][k]);
    Serial.println(sonGirisMi[k] ? "  (kalici deger)" : "  (sonradan degistirildi)");
  }
  Serial.println("============================");

  // 1) Sirasiyla hepsini yaz, her birinin yazma onayini (ACK) kaydet
  for (int k = 0; k < adet; k++)
  {
    girisSonucu[k] = yazTekParametre(bankAdres[activeBank][k], bankDeger[activeBank][k]);
  }

  // 2) Sadece her adresin KALICI (en son) girisini geri okuyup dogrula
  for (int k = 0; k < adet; k++)
  {
    if (!sonGirisMi[k]) continue; // uzerine yazilmis giris, okuma anlamsiz

    if (!dogrulaTekParametre(bankAdres[activeBank][k], bankDeger[activeBank][k]))
    {
      girisSonucu[k] = false;
    }
  }

  // 3) Sonuc tablosu ve genel karar
  bool hepsiBasarili = true;
  Serial.println("SONUC TABLOSU:");
  for (int k = 0; k < adet; k++)
  {
    Serial.print("  [");
    Serial.print(k);
    Serial.print("] adres=");
    Serial.print(bankAdres[activeBank][k]);
    Serial.print(" deger=");
    Serial.print(bankDeger[activeBank][k]);
    Serial.println(girisSonucu[k] ? "  -> BASARILI" : "  -> BASARISIZ");

    if (!girisSonucu[k]) hepsiBasarili = false;
  }

  Serial.print("TOPLAM SURE (ms): ");
  Serial.println(millis() - toplamBaslangic);

  return hepsiBasarili;
}

void greenFlash()
{
  for (int i = 0; i < 5; i++)
  {
    setColor(0, 1, 0);
    delay(200);
    setColor(0, 0, 0);
    delay(200);
  }
  bankRengineDon();
}

void redFlash()
{
  for (int i = 0; i < 5; i++)
  {
    setColor(1, 0, 0);
    delay(200);
    setColor(0, 0, 0);
    delay(200);
  }
  bankRengineDon();
}

void deepSleepeGir()
{
  digitalWrite(STATUS_POWER_LED_GPIO, LOW);
  setColor(0, 0, 0);

  esp_deep_sleep_enable_gpio_wakeup(1ULL << EXT_GPIO_Button, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void setup()
{
  pinMode(RGB_RED_PIN, OUTPUT);
  pinMode(RGB_GREEN_PIN, OUTPUT);
  pinMode(RGB_BLUE_PIN, OUTPUT);
  setColor(0, 0, 0);

  pinMode(STATUS_POWER_LED_GPIO, OUTPUT);
  digitalWrite(STATUS_POWER_LED_GPIO, HIGH);

  pinMode(EXT_GPIO_Button, INPUT_PULLUP);
  pinMode(DBG_GPIO_BootButton, INPUT_PULLUP);

  pinMode(BLUE_LED_GPIO, OUTPUT);
  digitalWrite(BLUE_LED_GPIO, HIGH);

  pinMode(Drive_GPIO_DR, OUTPUT);
  digitalWrite(Drive_GPIO_DR, LOW);

  PC_Port.begin(PC_Baud);

  Drive_Port.begin(Drive_Baud, Drive_SerialConfig, Drive_UART_RxPin, Drive_UART_TxPin);

  mbDrive.begin(Drive_ID, Drive_Port);
  mbDrive.preTransmission(Drive_DR_Tx);
  mbDrive.postTransmission(Drive_DR_Rx);

  // EKLEME: dizileri "hic yazilmadi" anlamina gelen sentinel deger ile baslat.
  // 0 yerine 0xFFFF kullaniliyor ki bir adrese ILK kez 0 yazildiginda da
  // bu bir "degisiklik" olarak algilansin (0==0 karsilastirmasi yanilmasin).
  for (int i = 0; i < 1000; i++)
  {
    holdingRegisters[i] = 0xFFFF;
    eskiRegisters[i] = 0xFFFF;
  }

  mbPC.configureHoldingRegisters(holdingRegisters, 1000);

  hafiza.begin("ayarlar", false);

  for (int b = 0; b < 3; b++)
  {
    bankSayisi[b] = hafiza.getInt(("n" + String(b)).c_str(), 0);

    for (int k = 0; k < bankSayisi[b]; k++)
    {
      String prefix = "b" + String(b) + "_" + String(k);
      bankAdres[b][k] = hafiza.getInt((prefix + "a").c_str(), 0);
      bankDeger[b][k] = hafiza.getInt((prefix + "d").c_str(), 0);

      if (b == activeBank)
      {
        holdingRegisters[bankAdres[b][k]] = bankDeger[b][k];
        eskiRegisters[bankAdres[b][k]] = bankDeger[b][k];
      }
    }
  }

  bankRengineDon();

  mbPC.begin(bankSlaveIDs[activeBank], PC_Baud);

  lastActivity = millis();
}

void loop()
{
  mbPC.poll();

  for (int i = 0; i < 1000; i++)
  {
    if (holdingRegisters[i] != eskiRegisters[i])
    {
      eskiRegisters[i] = holdingRegisters[i];
      uint16_t yeniDeger = holdingRegisters[i];

      int hedefIndex;

      if (bankSayisi[activeBank] < MAX_PARAM)
      {
        hedefIndex = bankSayisi[activeBank];
        bankSayisi[activeBank]++;
        bankAdres[activeBank][hedefIndex] = i;

        hafiza.putInt(("n" + String(activeBank)).c_str(), bankSayisi[activeBank]);
      }
      else
      {
        continue;
      }

      bankDeger[activeBank][hedefIndex] = yeniDeger;

      String prefix = "b" + String(activeBank) + "_" + String(hedefIndex);
      hafiza.putInt((prefix + "a").c_str(), i);
      hafiza.putInt((prefix + "d").c_str(), yeniDeger);
    }
  }

  bool basiliMi = (digitalRead(EXT_GPIO_Button) == LOW);

  if (basiliMi && !wasPressed)
  {
    wasPressed = true;
    pressStart = millis();
    lastActivity = millis();
  }

  if (!basiliMi && wasPressed)
  {
    wasPressed = false;

    unsigned long sure = millis() - pressStart;

    if (sure < MIN_VALID_MS)
    {
      // HICBIR SEY YAPMA
    }
    else if (sure >= LONG_PRESS_MS)
    {
      int siradaki = (activeBank + 1) % 3;
      activateBank(siradaki);
    }
    else
    {
      bool usbBagli = (bool)Serial;
      bool bankBos = (bankSayisi[activeBank] == 0);

      if (usbBagli || bankBos)
      {
        redFlash();
      }
      else
      {
        bool basarili = yazVeDogrula();
        if (basarili) greenFlash();
        else redFlash();
      }
    }
  }

  bool bootState = digitalRead(DBG_GPIO_BootButton);

  if (bootState == LOW && bootLastState == HIGH)
  {
    lastActivity = millis();
    deleteActiveBankMemory();
  }

  bootLastState = bootState;

  if (millis() - lastActivity >= DEEP_SLEEP_TIMEOUT_MS)
  {
    deepSleepeGir();
  }
}
