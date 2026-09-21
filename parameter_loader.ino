// ============================================================
// EL CIHAZI - RS485/Modbus uzerinden surucuye (drive) parametre
// yukleyen ESP32-C3 tabanli tasiyici cihaz
// ============================================================

#include <ModbusMaster.h>     // Surucuye (Drive) Modbus MASTER olarak konusmak icin
#include <ModbusRTUSlave.h>   // PC/QModMaster tarafina Modbus SLAVE olarak gorunmek icin
#include <Preferences.h>      // ESP32 flash (NVS) hafizasina kalici veri yazmak icin
#include "esp_sleep.h"        // Deep sleep (derin uyku) fonksiyonlari icin

// ---------------- SURUCU (DRIVE) TARAFI AYARLARI ----------------
#define Drive_Port            (Serial1)      // Surucuyle konusulan donanim UART portu
#define Drive_Baud            (9600)         // Surucu haberlesme hizi
#define Drive_SerialConfig    (SERIAL_8N1)   // 8 veri biti, parite yok, 1 stop biti
#define Drive_UART_RxPin      (20)           // RS485 alici (RX) pini
#define Drive_UART_TxPin      (21)           // RS485 verici (TX) pini
#define Drive_ID              (1)            // Surucunun Modbus slave ID'si
#define Drive_GPIO_DR         (10)           // RS485 modulunun yon (DE/RE) pini

// ---------------- PC (QMODMASTER) TARAFI AYARLARI ----------------
#define PC_Port               (Serial)       // Bilgisayarla konusulan USB seri port
#define PC_Baud               (115200)       // PC haberlesme hizi

// ---------------- BUTON VE LED PINLERI ----------------
#define EXT_GPIO_Button       (4)            // Disaridaki ana kullanici butonu
#define DBG_GPIO_BootButton   (9)            // ESP32'nin uzerindeki BOOT butonu (silme icin)
#define BLUE_LED_GPIO         (8)            // Mavi durum LED'i (silme islemini gosterir)

#define RGB_RED_PIN           (1)            // RGB LED - kirmizi kanal
#define RGB_GREEN_PIN         (2)            // RGB LED - yesil kanal
#define RGB_BLUE_PIN          (5)            // RGB LED - mavi kanal

#define STATUS_POWER_LED_GPIO (6)            // Cihaz acikken surekli yanan güc LED'i

#define DEEP_SLEEP_TIMEOUT_MS (30000UL)      // Bu sure boyunca hic hareket olmazsa uykuya gec (ms)

#define MAX_PARAM             (100)          // Her bank icinde en fazla kac parametre tutulabilir

// RS485 modulunu VERICI (TX) moduna alir - surucuye veri gonderilecegi zaman cagrilir
void Drive_DR_Tx(){digitalWrite(Drive_GPIO_DR, HIGH);}
// RS485 modulunu ALICI (RX) moduna alir - surucudan cevap dinlenecegi zaman cagrilir
void Drive_DR_Rx(){digitalWrite(Drive_GPIO_DR, LOW);}

ModbusMaster mbDrive;            // Surucuyle konusan Modbus master nesnesi
ModbusRTUSlave mbPC(PC_Port);    // PC'ye karsi slave gibi davranan Modbus nesnesi
Preferences hafiza;              // Flash uzerindeki kalici hafiza (parametreler kayboldugunda burada durur)

// PC'den (QModMaster) gelen yazma istekleri bu diziye dusuyor (Modbus slave register haritasi).
// eskiRegisters ise "bir onceki dongude ne vardi" bilgisini tutar; ikisi karsilastirilarak
// "yeni bir yazma oldu mu" anlasilir.
// DEĞİŞİKLİK: artik {0} ile degil, setup() icinde 0xFFFF sentinel ile baslatiliyor
// (bir adrese ILK kez tam olarak 0 yazildiginda da bunun "degisiklik" oldugu anlasilsin diye)
uint16_t holdingRegisters[1000];
uint16_t eskiRegisters[1000];

const int bankSlaveIDs[3] = {1, 2, 3};   // Her bank, PC'ye karsi farkli bir Modbus slave ID'siyle gorunur

int activeBank = 0;   // Su an aktif olan bank (0=Bank1, 1=Bank2, 2=Bank3)

// Her bank kendi parametre listesini tutar: bankAdres[bank][index] = adres, bankDeger[bank][index] = deger
// bankSayisi[bank] = o bankta su an kayitli kac parametre oldugu (stack'in "dolu boy"u)
int bankAdres[3][MAX_PARAM];
uint16_t bankDeger[3][MAX_PARAM];
int bankSayisi[3] = {0, 0, 0};

bool wasPressed = false;        // Ana butonun bir onceki dongudeki basili durumu (kenar yakalamak icin)
unsigned long pressStart = 0;   // Ana butona basilmaya baslandigi an (millis())

const unsigned long MIN_VALID_MS  = 100;   // Bundan kisa basmalar "titreme" sayilir, yok sayilir
const unsigned long LONG_PRESS_MS = 600;   // Bundan uzun basma = bank degistir, kisa basma = yukle

bool bootLastState = HIGH;   // BOOT butonunun bir onceki durumu (kenar yakalamak icin)

unsigned long lastActivity = 0;   // En son ne zaman bir kullanici hareketi oldu (deep sleep sayaci icin)

// RGB LED'i istenen renge ayarlar (r,g,b: 0 = kapali, 1 = acik)
void setColor(int r, int g, int b)
{
  digitalWrite(RGB_RED_PIN, r ? HIGH : LOW);
  digitalWrite(RGB_GREEN_PIN, g ? HIGH : LOW);
  digitalWrite(RGB_BLUE_PIN, b ? HIGH : LOW);
}

// Aktif banka gore RGB LED'i sabit bir renge ayarlar, boylece hangi bankta oldugun her an belli olur
void bankRengineDon()
{
  if (activeBank == 0)      setColor(0, 0, 1); // Bank1 - Mavi
  else if (activeBank == 1) setColor(1, 0, 1); // Bank2 - Mor/Magenta
  else                       setColor(0, 1, 1); // Bank3 - Camgobegi/Cyan
}

// EKLEME (1. istenen ozellik): Aktif bank DOLU oldugunda (100 parametre doldugunda)
// kullaniciyi uyarmak icin aktif bankin KENDI rengiyle 5 saniye boyunca yanip soner,
// sure dolunca normal sabit renge geri doner. Boylece kullanici "bank dolu, yeni
// parametre kaydedilemiyor" durumunu LED'den anlar.
void bankDoluUyarisi()
{
  unsigned long baslangic = millis();   // 5 saniyelik uyari suresinin baslangic zamani
  bool ledAcik = false;                 // LED'in su anki acik/kapali durumu (yanip sonme icin)

  while (millis() - baslangic < 5000)   // Tam 5 saniye boyunca dongude kal
  {
    ledAcik = !ledAcik;                 // Her tur LED durumunu tersine cevir (yanip sonme efekti)
    if (ledAcik) bankRengineDon();      // Acik ise aktif bankin kendi rengini yak
    else setColor(0, 0, 0);             // Kapali ise LED'i tamamen sondur
    delay(250);                         // Her yanip-sonme adimi 250ms surer (saniyede 2 kez)
  }

  bankRengineDon();   // 5 saniye sonunda LED'i normal sabit aktif bank rengine geri dondur
}

// Aktif banki degistirir: yeni bankin rengini yakar ve PC'ye karsi o bankin slave ID'siyle yeniden baslar
void activateBank(int yeniBank)
{
  activeBank = yeniBank;   // Aktif bank numarasini guncelle (0, 1 veya 2)

  // EKLEME (2. istenen ozellik): hangi bankta oldugumuzu flash hafizaya da kaydet.
  // Boylece cihaz deep sleep'e girip uyandiginda (ki bu bir nevi reset'tir ve RAM
  // sifirlanir) en son kalinan bank hatirlanip otomatik acilir, tekrar Bank1'e donmez.
  hafiza.putInt("activeBank", activeBank);

  bankRengineDon();   // LED'i yeni aktif bankin rengine ayarla

  mbPC.begin(bankSlaveIDs[activeBank], PC_Baud);   // PC'ye karsi bu bankin kendi slave ID'siyle yeniden baslat
}

// Su an aktif olan bankin TUM parametrelerini hem RAM'den hem kalici hafizadan siler
void deleteActiveBankMemory()
{
  int adet = bankSayisi[activeBank];   // Silinecek toplam parametre sayisi

  for (int k = 0; k < adet; k++)   // Bankin icindeki her parametre icin tek tek
  {
    int adr = bankAdres[activeBank][k];   // Bu parametrenin Modbus adresi

    // Bu parametrenin flash'taki kayitlarini (adres + deger) sil
    String prefix = "b" + String(activeBank) + "_" + String(k);
    hafiza.remove((prefix + "a").c_str());
    hafiza.remove((prefix + "d").c_str());

    if (adr >= 0 && adr < 1000) 
    {
      // DEĞİŞİKLİK: silinen adresler de 0 yerine sentinel'e donuyor.
      // Boylece bu adrese ileride tekrar 0 yazilsa bile "yeni bir yazma" olarak dogru yakalanir.
      holdingRegisters[adr] = 0xFFFF;
      eskiRegisters[adr] = 0xFFFF;
    }
  }

  // Bank sayacini sifirla ve bu durumu flash'a da kaydet
  bankSayisi[activeBank] = 0;
  hafiza.putInt(("n" + String(activeBank)).c_str(), 0);

  // Kullaniciya silme islemini serial monitorden bildir
  Serial.println("============================");
  Serial.println("AKTIF BANK SILME ISLEMI");
  Serial.print("Aktif Bank: ");
  Serial.println(activeBank + 1);
  Serial.print("Silinen Parametre Sayisi: ");
  Serial.println(adet);
  Serial.println("============================");

  // Mavi LED'i kisa sure kapatip acarak silme islemini gorsel olarak da onayla
  digitalWrite(BLUE_LED_GPIO, LOW);
  delay(300);
  digitalWrite(BLUE_LED_GPIO, HIGH);
}

// Tek bir parametreyi surucuye yazmayi dener. Basarisiz olursa 3 kere tekrar dener.
// Basarili olursa true, 3 denemede de basarisiz olursa false doner.
bool yazTekParametre(int adres, uint16_t deger)
{
  delay(15);   // Surucunun onceki islemi toparlamasi icin kisa bekleme

  for (int deneme = 0; deneme < 3; deneme++)   // En fazla 3 kez dene
  {
    unsigned long t0 = millis();   // Bu denemenin basladigi an (sure olcumu icin)
    uint8_t sonuc = mbDrive.writeSingleRegister(adres, deger);   // Surucuye Modbus yazma komutu gonder

    // Her denemenin detayini (adres, kacinci deneme, sonuc kodu, sure) serial monitore bas
    Serial.print("YAZMA adres="); Serial.print(adres);
    Serial.print(" deneme="); Serial.print(deneme);
    Serial.print(" sonuc="); Serial.print(sonuc);
    Serial.print(" sure(ms)="); Serial.println(millis() - t0);

    if (sonuc == 0) return true;   // sonuc==0 => Modbus yazma basarili (ACK alindi), fonksiyondan cik
  }
  return false;   // 3 deneme de basarisizsa false dondur
}

// Tek bir parametreyi surucuden geri okuyup beklenen degerle karsilastirir (dogrulama).
// Basarili olursa true, 3 denemede de basarisiz/uyusmuyor olursa false doner.
bool dogrulaTekParametre(int adres, uint16_t deger)
{
  delay(15);   // Surucunun onceki islemi toparlamasi icin kisa bekleme

  for (int deneme = 0; deneme < 3; deneme++)   // En fazla 3 kez dene
  {
    unsigned long t0 = millis();   // Bu denemenin basladigi an (sure olcumu icin)
    uint8_t sonuc = mbDrive.readHoldingRegisters(adres, 1);   // Surucuden bu adresi geri oku

    // Her denemenin detayini (adres, kacinci deneme, sonuc kodu, sure) serial monitore bas
    Serial.print("OKUMA adres="); Serial.print(adres);
    Serial.print(" deneme="); Serial.print(deneme);
    Serial.print(" sonuc="); Serial.print(sonuc);
    Serial.print(" sure(ms)="); Serial.println(millis() - t0);

    // sonuc==0 => okuma basarili; ayrica okunan deger beklenenle ayni olmali
    if (sonuc == 0 && mbDrive.getResponseBuffer(0) == deger) return true;
  }
  return false;   // 3 deneme de basarisiz/uyusmuyorsa false dondur
}

// Aktif banktaki TUM parametreleri sirayla surucuye yazar, dogrular ve genel basari durumunu dondurur.
// Butona kisa basildiginda cagrilir.
bool yazVeDogrula()
{
  int adet = bankSayisi[activeBank];   // Aktif bankta kac parametre var
  if (adet == 0) return false;   // Bankta hic parametre yoksa yapacak bir sey yok

  // Once tek bir deneme yazimla hat/baglanti gercekten calisiyor mu diye bak.
  // Bu basarisiz olursa surucu hic cevap vermiyor demektir, tum islemi iptal et.
  uint8_t baglantiTest = mbDrive.writeSingleRegister(bankAdres[activeBank][0], bankDeger[activeBank][0]);

  if (baglantiTest != 0)
  {
    Serial.println("BAGLANTI YOK - hat kontrolu basarisiz, islem iptal edildi");
    return false;
  }

  unsigned long toplamBaslangic = millis();   // Tum islemin toplam suresini olcmek icin baslangic zamani

  bool girisSonucu[MAX_PARAM];   // Her parametrenin nihai basarili/basarisiz durumu
  bool sonGirisMi[MAX_PARAM];    // Bu giris, ayni adresin listedeki EN SON (kalici) girisi mi?

  // Ayni adres listede birden fazla kez gecebilir (once 5, sonra 8 yazilmis gibi).
  // Surucude sadece EN SON yazilan deger kalici olarak durur. Bu yuzden her giris icin,
  // "benden sonra ayni adrese baska bir yazim var mi" diye bakip sonGirisMi'yi belirliyoruz.
  // Boylece asagida sadece kalici (son) degerler surucuden geri okunarak dogrulanacak;
  // uzerine yazilmis eski girisler icin geri okuma yapmiyoruz (zaten anlamsiz olurdu).
  for (int k = 0; k < adet; k++)
  {
    sonGirisMi[k] = true;   // Baslangicta "bu son giris" varsay
    for (int j = k + 1; j < adet; j++)   // Bu giristen SONRAKI tum girislere bak
    {
      if (bankAdres[activeBank][j] == bankAdres[activeBank][k])   // Ayni adrese sonradan baska yazim varsa
      {
        sonGirisMi[k] = false;   // Bu giris son degil, uzerine yazilmis demektir
        break;
      }
    }
  }

  // Yuklemeye baslamadan once, yuklenecek tam listeyi (sira, adres, deger) serial monitore bas
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
      girisSonucu[k] = false;   // Dogrulama basarisizsa bu girisi de basarisiz isaretle
    }
  }

  // 3) Sonuc tablosu ve genel karar
  bool hepsiBasarili = true;   // Genel basari bayragi, herhangi biri basarisizsa false olacak
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

    if (!girisSonucu[k]) hepsiBasarili = false;   // Bir tanesi bile basarisizsa genel sonuc basarisiz olur
  }

  Serial.print("TOPLAM SURE (ms): ");
  Serial.println(millis() - toplamBaslangic);

  return hepsiBasarili;   // Butonu basan yerdeki kod bu deger uzerinden green/red flash karar verir
}

// Yukleme basarili oldugunda 5 kez yesil yanip soner, sonra aktif bank rengine geri doner
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

// Yukleme basarisiz oldugunda 5 kez kirmizi yanip soner, sonra aktif bank rengine geri doner
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

// Cihazi derin uykuya alir. Sadece EXT_GPIO_Button pinine basilinca (LOW oldugunda) uyanir.
void deepSleepeGir()
{
  digitalWrite(STATUS_POWER_LED_GPIO, LOW);   // Uykuya girerken guc LED'ini sondur
  setColor(0, 0, 0);                          // RGB LED'i de kapat

  esp_deep_sleep_enable_gpio_wakeup(1ULL << EXT_GPIO_Button, ESP_GPIO_WAKEUP_GPIO_LOW);   // Sadece bu butonla uyanmaya izin ver
  esp_deep_sleep_start();   // Derin uykuya gir (bu satirdan sonrasi calismaz, cihaz uyanınca setup() bastan calisir)
}

void setup()
{
  // --- Pin yonlerini ayarla ---
  pinMode(RGB_RED_PIN, OUTPUT);
  pinMode(RGB_GREEN_PIN, OUTPUT);
  pinMode(RGB_BLUE_PIN, OUTPUT);
  setColor(0, 0, 0);   // Baslangicta RGB LED sonuk

  pinMode(STATUS_POWER_LED_GPIO, OUTPUT);
  digitalWrite(STATUS_POWER_LED_GPIO, HIGH);   // Cihaz uyanikken bu LED surekli yanik kalir

  pinMode(EXT_GPIO_Button, INPUT_PULLUP);      // Ana kullanici butonu
  pinMode(DBG_GPIO_BootButton, INPUT_PULLUP);  // Silme icin kullanilan BOOT butonu

  pinMode(BLUE_LED_GPIO, OUTPUT);
  digitalWrite(BLUE_LED_GPIO, HIGH);

  pinMode(Drive_GPIO_DR, OUTPUT);
  digitalWrite(Drive_GPIO_DR, LOW);   // Baslangicta RS485 alici (dinleme) modunda

  // --- Seri portlari baslat ---
  PC_Port.begin(PC_Baud);   // USB uzerinden QModMaster ile konusulacak port

  Drive_Port.begin(Drive_Baud, Drive_SerialConfig, Drive_UART_RxPin, Drive_UART_TxPin);

  // --- Surucuyle Modbus master olarak baglanti kur ---
  mbDrive.begin(Drive_ID, Drive_Port);
  mbDrive.preTransmission(Drive_DR_Tx);    // Veri gondermeden once RS485'i verici moda al
  mbDrive.postTransmission(Drive_DR_Rx);   // Veri gonderdikten sonra RS485'i alici moda al

  // EKLEME: dizileri "hic yazilmadi" anlamina gelen sentinel deger ile baslat.
  // 0 yerine 0xFFFF kullaniliyor ki bir adrese ILK kez 0 yazildiginda da
  // bu bir "degisiklik" olarak algilansin (0==0 karsilastirmasi yanilmasin).
  for (int i = 0; i < 1000; i++)
  {
    holdingRegisters[i] = 0xFFFF;
    eskiRegisters[i] = 0xFFFF;
  }

  // PC tarafina, yazma istekleri dogrudan holdingRegisters dizisine islenecek sekilde Modbus slave kur
  mbPC.configureHoldingRegisters(holdingRegisters, 1000);

  hafiza.begin("ayarlar", false);   // Flash (NVS) hafizasini "ayarlar" isimli alanda ac

  // EKLEME (1. istenen ozellik): en son hangi banktaydiysak flash'tan onu geri oku.
  // Bu satir, asagidaki bank-yukleme donugusunden ONCE calismali ki dogru banka gore
  // holdingRegisters dizisi dolsun. Kayit yoksa (ilk calistirma) varsayilan olarak 0 (Bank1) doner.
  activeBank = hafiza.getInt("activeBank", 0);

  // --- Onceden kaydedilmis 3 bankin tum parametrelerini flash'tan RAM'e geri yukle ---
  for (int b = 0; b < 3; b++)
  {
    bankSayisi[b] = hafiza.getInt(("n" + String(b)).c_str(), 0);   // O bankta kayitli parametre sayisi

    for (int k = 0; k < bankSayisi[b]; k++)
    {
      String prefix = "b" + String(b) + "_" + String(k);
      bankAdres[b][k] = hafiza.getInt((prefix + "a").c_str(), 0);
      bankDeger[b][k] = hafiza.getInt((prefix + "d").c_str(), 0);

      // Sadece aktif bankin degerlerini Modbus register dizisine de yansit,
      // ki PC baglaninca bu bankin guncel degerlerini gorebilsin
      if (b == activeBank)
      {
        holdingRegisters[bankAdres[b][k]] = bankDeger[b][k];
        eskiRegisters[bankAdres[b][k]] = bankDeger[b][k];
      }
    }
  }

  bankRengineDon();   // Baslangictaki aktif bankin rengini LED'de goster

  mbPC.begin(bankSlaveIDs[activeBank], PC_Baud);   // PC'ye karsi aktif bankin slave ID'siyle baslat

  lastActivity = millis();   // Deep sleep sayacini sifirla
}

void loop()
{
  mbPC.poll();   // PC'den gelen Modbus isteklerini isle (varsa holdingRegisters'a yazar)

  // --- Yeni yazilan parametreleri yakala (capture) ---
  // holdingRegisters ile eskiRegisters'i karsilastirarak PC'nin hangi adrese
  // yeni bir deger yazdigini tespit ediyoruz.
  for (int i = 0; i < 1000; i++)
  {
    if (holdingRegisters[i] != eskiRegisters[i])   // Bu adreste bir degisiklik var mi?
    {
      eskiRegisters[i] = holdingRegisters[i];   // Bir sonraki karsilastirma icin "eski deger"i guncelle
      uint16_t yeniDeger = holdingRegisters[i];

      int hedefIndex;   // Bu yeni degerin bank listesinde hangi index'e yazilacagi

      // Aktif bankta hala bos yer varsa, bu yeni degeri listenin BIR SONRAKI bos
      // index'ine ekle (adres daha once kullanilmis olsa bile UZERINE YAZMADAN,
      // stack gibi sirayla ekleniyor).
      if (bankSayisi[activeBank] < MAX_PARAM)
      {
        hedefIndex = bankSayisi[activeBank];
        bankSayisi[activeBank]++;
        bankAdres[activeBank][hedefIndex] = i;

        // Guncel eleman sayisini hemen flash'a da yaz (elektrik kesilse bile kaybolmasin)
        hafiza.putInt(("n" + String(activeBank)).c_str(), bankSayisi[activeBank]);
      }
      else
      {
        // EKLEME (2. istenen ozellik): Bank doluysa (100 parametreye ulasilmissa),
        // yeni degeri kaydetmeden once kullaniciyi LED ile uyar (5 sn yanip soner).
        bankDoluUyarisi();
        continue;   // Bank doluysa (100 parametre) bu yeni degeri yok say
      }

      bankDeger[activeBank][hedefIndex] = yeniDeger;

      // Bu yeni parametreyi (adres + deger) kalici olarak flash'a kaydet
      String prefix = "b" + String(activeBank) + "_" + String(hedefIndex);
      hafiza.putInt((prefix + "a").c_str(), i);
      hafiza.putInt((prefix + "d").c_str(), yeniDeger);
    }
  }

  // --- Ana buton mantigi (basma suresine gore farkli islemler) ---
  bool basiliMi = (digitalRead(EXT_GPIO_Button) == LOW);

  // Butona yeni basildi (basilmamis -> basili gecisi): basilma anini kaydet
  if (basiliMi && !wasPressed)
  {
    wasPressed = true;
    pressStart = millis();
    lastActivity = millis();
  }

  // Buton birakildi (basili -> birakilmis gecisi): ne kadar basili kaldigina gore karar ver
  if (!basiliMi && wasPressed)
  {
    wasPressed = false;

    unsigned long sure = millis() - pressStart;   // Butonun ne kadar sure basili kaldigi

    if (sure < MIN_VALID_MS)
    {
      // Cok kisa surdu (titreme/gurultu), hicbir sey yapma
    }
    else if (sure >= LONG_PRESS_MS)
    {
      // Uzun basma: bir sonraki banka gec (Bank1 -> Bank2 -> Bank3 -> Bank1 ...)
      int siradaki = (activeBank + 1) % 3;
      activateBank(siradaki);
    }
    else
    {
      // Kisa basma: aktif banktaki parametreleri surucuye yukle
      bool usbBagli = (bool)Serial;   // USB baglantisi var mi (guvenlik kontrolu icin)
      bool bankBos = (bankSayisi[activeBank] == 0);   // Aktif bankta hic parametre yok mu

      if (usbBagli || bankBos)
      {
        // USB baglantidayken (guvenlik onlemi olarak) veya bankta hic parametre yoksa yukleme yapma
        redFlash();
      }
      else
      {
        bool basarili = yazVeDogrula();   // Tum parametreleri sirayla yaz ve dogrula
        if (basarili) greenFlash();
        else redFlash();
      }
    }
  }

  // --- BOOT butonu: aktif bankin tum parametrelerini siler ---
  bool bootState = digitalRead(DBG_GPIO_BootButton);

  if (bootState == LOW && bootLastState == HIGH)   // Basilma anini yakala (kenar tetiklemesi)
  {
    lastActivity = millis();
    deleteActiveBankMemory();
  }

  bootLastState = bootState;

  // --- Hareketsizlik kontrolu: belirli sure hic islem olmazsa derin uykuya gec ---
  if (millis() - lastActivity >= DEEP_SLEEP_TIMEOUT_MS)
  {
    deepSleepeGir();
  }
}
