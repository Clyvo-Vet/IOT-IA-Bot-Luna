/*
 * ============================================================================
 *  KURA IoT — Monitoramento de Câmara Fria de Vacinas
 * ============================================================================
 *  Hardware:  ESP32 DevKit v1 + DHT22 + LDR (módulo com resistor pull-down)
 *  Protocolo: MQTT QoS 1 (garantia de entrega ao Node-RED)
 *  Tópico:    kura/clinica01/camarafria/telemetria
 *
 *  Regra de Negócio (ANVISA RDC 197/2017):
 *    - Faixa crítica de armazenamento de vacinas: 2°C a 8°C
 *    - Temperaturas fora dessa faixa devem gerar alerta imediato
 *
 *  Decisão de Arquitetura:
 *    - ESP32 é pura borda (edge). NÃO toma decisões de negócio.
 *    - Toda lógica de alerta (ALERTA_TEMPERATURA) fica no Node-RED.
 *    - ESP32 apenas coleta, classifica minimamente e publica.
 * ============================================================================
 *  Libs necessárias (Library Manager da Arduino IDE):
 *    - PubSubClient  by Nick O'Leary
 *    - ArduinoJson   by Benoit Blanchon
 *    - DHT sensor library by Adafruit
 *    - Adafruit Unified Sensor by Adafruit
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =============================================================================
// CONFIGURAÇÃO — Edite estes valores conforme seu ambiente
// =============================================================================

// Wi-Fi
const char* WIFI_SSID     = "SEU_SSID";
const char* WIFI_PASSWORD = "SUA_SENHA";

// MQTT — HiveMQ Cloud ou Mosquitto local
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";         // preenchido apenas se o broker exigir auth
const char* MQTT_PASS     = "";
// ID único do cliente. Em produção, usar o MAC address do chip.
// Deve coincidir com DISPOSITIVO_IOT.DS_IDENTIFICADOR no banco.
const char* MQTT_CLIENT_ID = "kura_esp32_clinica01_camarafria";

// Tópicos MQTT
const char* TOPIC_TELEMETRIA = "kura/clinica01/camarafria/telemetria";
const char* TOPIC_STATUS     = "kura/clinica01/camarafria/status";

// Pins de Hardware
#define DHT_PIN     4    // GPIO4 → DATA do DHT22
#define DHT_TYPE    DHT22
#define LDR_PIN    34    // GPIO34 (ADC input-only) → sinal do divisor de tensão LDR

// =============================================================================
// PARÂMETROS OPERACIONAIS (Regra ANVISA)
// =============================================================================

// Intervalo de leitura: 10 segundos (sem delay() — usando millis())
const unsigned long INTERVALO_LEITURA_MS   = 10000UL;
// Intervalo de reconexão MQTT: não tenta mais de 1x a cada 5s
const unsigned long INTERVALO_RECONEXAO_MS = 5000UL;
// Verificação periódica do Wi-Fi
const unsigned long INTERVALO_WIFI_MS      = 30000UL;

// Faixa ANVISA para vacinas (usada para pré-classificação no ESP32)
// O Node-RED recalcula usando os valores de DISPOSITIVO_IOT.NR_TEMP_MINIMA/MAXIMA
const float TEMP_MIN_ANVISA = 2.0;
const float TEMP_MAX_ANVISA = 8.0;

// Threshold do LDR: acima deste valor = luz detectada = porta provavelmente aberta
// ADC do ESP32 é 12-bit (0-4095). Ambiente claro = ~2000+
// Ajuste conforme a sensibilidade do seu LDR e a iluminação do ambiente.
const int LDR_THRESHOLD_PORTA_ABERTA = 1500;

// =============================================================================
// OBJETOS GLOBAIS
// =============================================================================

WiFiClient    espClient;
PubSubClient  mqtt(espClient);
DHT           dht(DHT_PIN, DHT_TYPE);

// Controle de tempo (non-blocking com millis())
unsigned long ultimaLeitura   = 0;
unsigned long ultimaReconexao = 0;
unsigned long ultimoWifiCheck = 0;

// Contador sequencial de mensagens para rastreamento/debug
unsigned long contadorMsg = 0;

// =============================================================================
// ESTRUTURA DE DADOS DO SENSOR
// =============================================================================

/*
 * Encapsula os valores de uma leitura. 'valido' é false se o DHT22 falhar.
 * O LDR nunca falha eletricamente (leitura analógica bruta), mas pode ser
 * impreciso em condições extremas — por isso tratamos como indicativo.
 */
struct LeituraSensor {
  float temperatura;    // °C — DHT22
  float umidade;        // %  — DHT22 (bônus: não está no escopo principal mas é grátis)
  int   luzADC;         // 0-4095 — LDR via divisor de tensão
  bool  portaAberta;    // true se luzADC > threshold
  bool  dentroFaixa;    // true se 2°C ≤ temp ≤ 8°C (ANVISA RDC 197/2017)
  bool  valido;         // false se DHT22 retornar NaN
};

// =============================================================================
// FUNÇÕES DE CONECTIVIDADE
// =============================================================================

/*
 * setupWifi: bloqueante apenas no boot (aceitável — o sistema não opera sem Wi-Fi).
 * Após o boot, reconexões são non-blocking via checkWifi().
 */
void setupWifi() {
  Serial.printf("[WiFi] Conectando a '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativa = 0;
  while (WiFi.status() != WL_CONNECTED && tentativa < 40) {
    delay(500);
    Serial.print(".");
    tentativa++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] OK — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Falha no boot. Será tratado no loop().");
  }
}

/*
 * checkWifi: chamado periodicamente pelo loop() para detectar e tratar
 * quedas de sinal. Não bloqueia o loop.
 */
void checkWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Desconectado. Reconectando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

/*
 * conectarMqtt: tentativa única de conexão.
 * Usa LWT (Last Will and Testament): se o ESP32 perder conexão abruptamente,
 * o broker publica "offline" no tópico de status automaticamente.
 * O Node-RED pode monitorar este tópico para detectar dispositivo offline.
 */
bool conectarMqtt() {
  if (mqtt.connected()) return true;

  Serial.printf("[MQTT] Conectando ao broker %s:%d...\n", MQTT_BROKER, MQTT_PORT);

  // LWT — Node-RED usa isso para gerar ALERTA 'SENSOR_OFFLINE' no banco
  const char* lwtPayload = "{\"status\":\"offline\",\"dispositivo\":\"kura_esp32_clinica01_camarafria\"}";

  bool ok = mqtt.connect(
    MQTT_CLIENT_ID,
    MQTT_USER,
    MQTT_PASS,
    TOPIC_STATUS,   // tópico do LWT
    1,              // QoS 1 do LWT
    true,           // retain: o último status fica guardado no broker
    lwtPayload
  );

  if (ok) {
    Serial.println("[MQTT] Conectado!");
    // Publica status online (retained = mantido no broker até novo publish)
    mqtt.publish(
      TOPIC_STATUS,
      "{\"status\":\"online\",\"dispositivo\":\"kura_esp32_clinica01_camarafria\"}",
      true
    );
  } else {
    // rc codes: -4=timeout, -3=conn_lost, -2=conn_fail, 1=bad_proto,
    // 2=bad_clientid, 4=bad_credentials, 5=unauthorized
    Serial.printf("[MQTT] Falha. rc=%d\n", mqtt.state());
  }

  return ok;
}

// =============================================================================
// LEITURA DOS SENSORES
// =============================================================================

LeituraSensor lerSensores() {
  LeituraSensor dados;

  // -- DHT22 --
  dados.temperatura = dht.readTemperature();   // Celsius
  dados.umidade     = dht.readHumidity();

  // DHT22 retorna NaN em falha de leitura (bad CRC, cabo, etc.)
  dados.valido = !isnan(dados.temperatura) && !isnan(dados.umidade);

  if (!dados.valido) {
    Serial.println("[SENSOR] ERRO: DHT22 retornou NaN. Verifique o cabeamento.");
    dados.temperatura = -999.0; // sentinel — Node-RED deve ignorar esta leitura
    dados.umidade     = -999.0;
  }

  // -- LDR --
  // analogRead no GPIO34 (ADC1_CH6). GPIO34 é input-only, sem pull interno.
  // 0 = escuro (câmara fechada), 4095 = muito iluminado (câmara aberta)
  dados.luzADC    = analogRead(LDR_PIN);
  dados.portaAberta = (dados.luzADC > LDR_THRESHOLD_PORTA_ABERTA);

  // -- Classificação ANVISA (pré-processada no edge para otimizar payload) --
  // O Node-RED re-verifica usando os limites do banco (DISPOSITIVO_IOT),
  // mas esta flag permite ao broker/dashboard filtrar rapidamente.
  dados.dentroFaixa = dados.valido &&
                      (dados.temperatura >= TEMP_MIN_ANVISA) &&
                      (dados.temperatura <= TEMP_MAX_ANVISA);

  return dados;
}

// =============================================================================
// PUBLICAÇÃO MQTT
// =============================================================================

/*
 * Payload JSON publicado em: kura/clinica01/camarafria/telemetria
 *
 * {
 *   "id_dispositivo": 1,           // chave FK → DISPOSITIVO_IOT.ID_DISPOSITIVO
 *   "temperatura": 5.4,            // °C, 1 casa decimal
 *   "umidade": 62.3,               // %
 *   "ldr_adc": 280,                // valor bruto ADC 0-4095
 *   "porta_aberta": false,         // flag derivada do LDR
 *   "dentro_faixa": true,          // classificação prévia ANVISA
 *   "msg_id": 42,                  // sequencial para rastreamento/debug
 *   "uptime_s": 420                // segundos desde o boot do ESP32
 * }
 *
 * DECISÃO: incluímos id_dispositivo = 1 hardcoded pois este firmware
 * é específico para a câmara fria da clinica01. Em produção, o ID
 * viria de uma config no SPIFFS ou de um endpoint de provisioning.
 */
void publicarTelemetria(const LeituraSensor& dados) {
  // StaticJsonDocument usa stack (mais rápido e sem heap fragmentation)
  // 256 bytes é suficiente para este payload pequeno
  StaticJsonDocument<256> doc;

  doc["id_dispositivo"] = 1;   // FK → DISPOSITIVO_IOT.ID_DISPOSITIVO = 1
  doc["temperatura"]    = round(dados.temperatura * 10.0) / 10.0;  // 1 decimal
  doc["umidade"]        = round(dados.umidade     * 10.0) / 10.0;
  doc["ldr_adc"]        = dados.luzADC;
  doc["porta_aberta"]   = dados.portaAberta;
  doc["dentro_faixa"]   = dados.dentroFaixa;
  doc["msg_id"]         = ++contadorMsg;
  doc["uptime_s"]       = millis() / 1000;

  char buffer[256];
  size_t n = serializeJson(doc, buffer);

  // QoS 0 para telemetria de alta frequência: menor overhead.
  // O Node-RED processa em tempo real; dados perdidos isoladamente
  // são toleráveis. Alertas críticos usam QoS 1 (gerenciado no Node-RED).
  if (mqtt.publish(TOPIC_TELEMETRIA, buffer, false)) {
    Serial.printf("[MQTT] Publicado (%zu bytes): %s\n", n, buffer);
  } else {
    Serial.println("[MQTT] FALHA ao publicar. Próxima tentativa no próximo ciclo.");
  }
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(100); // estabilizar serial

  Serial.println("=====================================================");
  Serial.println(" KURA IoT — Câmara Fria de Vacinas v1.0");
  Serial.println(" Regra: ANVISA RDC 197/2017 | Faixa: 2°C a 8°C");
  Serial.println("=====================================================");

  dht.begin();
  pinMode(LDR_PIN, INPUT); // GPIO34 = input-only, sem pull interno

  setupWifi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  // Sem callback de subscribe — este firmware só publica (publisher-only)
  // Subscriptions ficam no Node-RED (separação de responsabilidades)
}

// =============================================================================
// LOOP PRINCIPAL — 100% non-blocking com millis()
// =============================================================================

void loop() {
  unsigned long agora = millis();

  // -- Bloco 1: Saúde do Wi-Fi (verificado a cada 30s) --
  if (agora - ultimoWifiCheck >= INTERVALO_WIFI_MS) {
    ultimoWifiCheck = agora;
    checkWifi();
  }

  // -- Bloco 2: Manutenção do MQTT (apenas se Wi-Fi estiver OK) --
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      // Tenta reconectar no máximo 1x a cada 5s (evita spam de tentativas)
      if (agora - ultimaReconexao >= INTERVALO_RECONEXAO_MS) {
        ultimaReconexao = agora;
        conectarMqtt();
      }
    }
    // CRÍTICO: mqtt.loop() DEVE rodar a cada iteração para:
    // - Processar keep-alive (PINGREQ/PINGRESP)
    // - Processar callbacks de mensagens recebidas (caso haja subscriptions)
    // - Manter o estado da conexão
    mqtt.loop();
  }

  // -- Bloco 3: Leitura e publicação a cada 10s --
  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;

    LeituraSensor dados = lerSensores();

    if (!dados.valido) {
      // DHT22 falhou: não publicar dado inválido no banco de dados.
      // Node-RED detectará a ausência e poderá gerar alerta SENSOR_OFFLINE
      // baseado no DT_ULTIMA_LEITURA em DISPOSITIVO_IOT.
      Serial.println("[LOOP] Leitura inválida. Pulando publicação.");
      return;
    }

    if (mqtt.connected()) {
      publicarTelemetria(dados);
    } else {
      // Sem MQTT: loga localmente para não perder o evento de porta aberta
      Serial.printf("[LOCAL] Temp=%.1f°C Umid=%.1f%% LDR=%d PortaAberta=%s\n",
        dados.temperatura,
        dados.umidade,
        dados.luzADC,
        dados.portaAberta ? "SIM" : "NAO"
      );
    }
  }
}
