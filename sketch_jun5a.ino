#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include "time.h"
#include <vector>
#include <SPIFFS.h>

// ========== Wi-Fi ==========
const char* ssid = "SEU_WIFI";
const char* password = "SUA_SENHA";

// ========== Servidor ==========
const char* serverName = "https://script.google.com"; 
const char* apiToken = ".....";

// ========== Pinos ==========
const int LED_VERDE = 25;
const int LED_VERMELHO = 26;

// ========== Sensor ==========
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
bool modoAdmin = false;

// ========== Horário ==========
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;
const int daylightOffset_sec = 0;

// ========== Aula ==========
const char* horaInicio = "08:00";
const char* horaFim = "11:00";
const char* nomeArquivoCSV = "/presencas.csv";

// ========== Feriados ==========
std::vector<String> feriados = {
  "2025-01-01", "2025-04-21", "2025-05-01",
  "2025-09-07", "2025-10-12", "2025-11-02",
  "2025-11-15", "2025-12-25"
};

// ========== Controle de Leitura ==========
int ultimoIDLido = -1;
unsigned long ultimoTempoLeitura = 0;
const unsigned long intervaloMinimo = 30000; // 30 segundos

// ========== Wi-Fi ==========
void connectWiFi() {
  Serial.println("Conectando ao Wi-Fi...");
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 10) {
    delay(1000);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n Wi-Fi conectado!");
    Serial.print("IP do ESP32: ");
    Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  } else {
    Serial.println("\n Falha ao conectar no Wi-Fi.");
  }
}

// ========== Utilitários ==========
int horaParaMinutos(const char* hora) {
  int h, m;
  sscanf(hora, "%d:%d", &h, &m);
  return h * 60 + m;
}

bool dentroDoHorario(struct tm timeinfo) {
  char agora[6];
  strftime(agora, sizeof(agora), "%H:%M", &timeinfo);
  int minutosAgora = horaParaMinutos(agora);
  return minutosAgora >= horaParaMinutos(horaInicio) && minutosAgora <= horaParaMinutos(horaFim);
}

bool isFeriado(struct tm t) {
  char data[11];
  strftime(data, sizeof(data), "%Y-%m-%d", &t);
  for (const String& f : feriados) {
    if (f == String(data)) return true;
  }
  return false;
}

// ========== Falta Automática ==========
void verificarFaltasAutomaticas() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  int hora = timeinfo.tm_hour;
  int minuto = timeinfo.tm_min;
  int diaSemana = timeinfo.tm_wday;
  if (diaSemana == 0 || diaSemana == 6 || isFeriado(timeinfo)) return;
  if (hora == 11 && minuto == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverName);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", "Bearer " + String(apiToken));
      String json = "{\"falta_auto\": true}";
      http.POST(json);
      http.end();
    }
  }
}

// ========== LEDs ==========
void acendeLedVerde() {
  digitalWrite(LED_VERDE, HIGH);
  digitalWrite(LED_VERMELHO, LOW);
  delay(1500);
  digitalWrite(LED_VERDE, LOW);
}

void acendeLedVermelho() {
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_VERMELHO, HIGH);
  delay(1500);
  digitalWrite(LED_VERMELHO, LOW);
}

// ========== SPIFFS ==========
void salvarPresencaOffline(int id, const char* hora) {
  File arquivo = SPIFFS.open(nomeArquivoCSV, FILE_APPEND);
  if (!arquivo) return;
  arquivo.printf("%d,%s\n", id, hora);
  arquivo.close();
}

void sincronizarCSV() {
  if (WiFi.status() != WL_CONNECTED) return;

  File arquivo = SPIFFS.open(nomeArquivoCSV, FILE_READ);
  if (!arquivo) return;

  String conteudo;
  while (arquivo.available()) {
    conteudo += arquivo.readStringUntil('\n') + "\n";
  }
  arquivo.close();

  if (conteudo.length() > 0) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(apiToken));
    String json = "{\"csv\": \"" + conteudo + "\"}";
    int httpCode = http.POST(json);
    http.end();

    if (httpCode == 200) {
      SPIFFS.remove(nomeArquivoCSV);
    }
  }
}

// ========== Biometria ==========
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

void enviarPresenca(int id) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  if (!dentroDoHorario(timeinfo)) return;

  char hora[9];
  strftime(hora, sizeof(hora), "%H:%M:%S", &timeinfo);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(apiToken));
    String json = "{\"id_aluno\": " + String(id) + ", \"hora\": \"" + String(hora) + "\", \"token\": \"" + String(apiToken) + "\"}";
    int httpCode = http.POST(json);
    http.end();
    if (httpCode != 200) salvarPresencaOffline(id, hora);
  } else {
    salvarPresencaOffline(id, hora);
  }
}

// ========== Cadastro ==========
bool enrollFingerprint(int id) {
  int p = -1;
  Serial.println("Coloque o dedo...");
  while ((p = finger.getImage()) != FINGERPRINT_OK);
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) return false;
  Serial.println("Remova o dedo...");
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER);
  Serial.println("Coloque o mesmo dedo novamente...");
  while ((p = finger.getImage()) != FINGERPRINT_OK);
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) return false;
  p = finger.createModel();
  if (p != FINGERPRINT_OK) return false;
  p = finger.storeModel(id);
  return p == FINGERPRINT_OK;
}

void modoAdministrador() {
  finger.getTemplateCount();
  int novoID = finger.templateCount + 1;
  Serial.print("Novo ID: ");
  Serial.println(novoID);

  Serial.println("Digite o nome do aluno:");
  String nomeAluno = "";
  while (nomeAluno.length() == 0) {
    if (Serial.available()) {
      nomeAluno = Serial.readStringUntil('\n');
      nomeAluno.trim();
    }
  }

  if (enrollFingerprint(novoID)) {
    acendeLedVerde();

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverName);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", "Bearer " + String(apiToken));

      String json = "{\"novo_aluno\": true, \"id\": " + String(novoID) +
                    ", \"nome\": \"" + nomeAluno + "\", \"token\": \"" + String(apiToken) + "\"}";

      int httpCode = http.POST(json);
      String resposta = http.getString();

      Serial.print("Resposta do servidor: ");
      Serial.println(resposta);
      http.end();
    }
  } else {
    acendeLedVermelho();
  }

  delay(2000);
}

void criarPlanilhaSeNecessario() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(apiToken));
    String json = "{\"criar_planilha\": true}";
    http.POST(json);
    http.end();
  }
}

// ========== Setup & Loop ==========
void setup() {
  Serial.begin(115200);
  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  connectWiFi();
  SPIFFS.begin(true);
  finger.begin(57600);
  if (finger.verifyPassword()) Serial.println("Sensor OK!");
  else while (1) delay(1);
  finger.getTemplateCount();
  criarPlanilhaSeNecessario();
}

void loop() {
  // Verificar e reconectar Wi-Fi se necessário
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi desconectado! Tentando reconectar...");
    connectWiFi();
  }

  verificarFaltasAutomaticas();
  sincronizarCSV();

  if (Serial.available()) {
    String comando = Serial.readStringUntil('\n');
    comando.trim();

    if (comando == "admin") {
      modoAdmin = true;
      modoAdministrador();
      modoAdmin = false;
    } else if (comando == "baixar") {
      File arquivo = SPIFFS.open(nomeArquivoCSV, FILE_READ);
      if (arquivo) {
        Serial.println("=== PRESENCAS SALVAS OFFLINE ===");
        while (arquivo.available()) {
          Serial.write(arquivo.read());
        }
        Serial.println("\n=== FIM DO ARQUIVO ===");
        arquivo.close();
      } else {
        Serial.println("Arquivo não encontrado.");
      }
    } else if (comando == "apagar") {
      Serial.println("Apagando todas as digitais...");
      for (int i = 1; i <= finger.templateCount; i++) {
        if (finger.deleteModel(i) == FINGERPRINT_OK) {
          Serial.print("Apagado ID ");
          Serial.println(i);
        }
      }
      Serial.println("Todas as digitais foram apagadas.");
    }
  }

  if (!modoAdmin) {
    int id = getFingerprintID();
    unsigned long agora = millis();

    if (id >= 0) {
      if (id != ultimoIDLido || (agora - ultimoTempoLeitura) > intervaloMinimo) {
        acendeLedVerde();
        enviarPresenca(id);
        ultimoIDLido = id;
        ultimoTempoLeitura = agora;
      } else {
        Serial.println("Leitura ignorada: ID duplicado recente.");
      }
    } else {
      acendeLedVermelho();
    }

    delay(2000);
  }
}
