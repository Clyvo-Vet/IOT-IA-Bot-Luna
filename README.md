# KURA IoT — Monitoramento de Câmara Fria de Vacinas

> **Subsistema IoT do KURA** — plataforma SaaS B2B para gestão de clínicas veterinárias, desenvolvida em parceria com a **Clyvo Vet** para o **Challenge FIAP 2026**.

[![FIAP Challenge 2026](https://img.shields.io/badge/FIAP-Challenge%202026-E63946?style=flat-square)](https://fiap.com.br)
[![Parceiro](https://img.shields.io/badge/Parceiro-Clyvo%20Vet-2DC653?style=flat-square)](https://clyvo.com.br)
[![ANVISA](https://img.shields.io/badge/Conformidade-ANVISA%20RDC%20197%2F2017-0077B6?style=flat-square)](#regra-de-negócio-anvisa)
[![Disciplina](https://img.shields.io/badge/Disciplina-Disruptive%20Architectures%3A%20IoT-7B2D8B?style=flat-square)](#)

---

## Índice

- [Contexto de Negócio](#contexto-de-negócio)
- [Problema que este módulo resolve](#problema-que-este-módulo-resolve)
- [Regra de Negócio ANVISA](#regra-de-negócio-anvisa)
- [Arquitetura](#arquitetura)
- [Stack Tecnológica](#stack-tecnológica)
- [Estrutura do Repositório](#estrutura-do-repositório)
- [Payload MQTT](#payload-mqtt)
- [Banco de Dados](#banco-de-dados)
- [Fluxo Node-RED](#fluxo-node-red)
- [Dashboard](#dashboard)
- [Simulação com Wokwi](#simulação-com-wokwi)
- [Como executar](#como-executar)
- [Evidências](#evidências)
- [Equipe](#equipe)

---

## Contexto de Negócio

O **KURA** é um SaaS B2B para clínicas veterinárias que endereça a fragmentação do cuidado animal. Em um mercado de **R$ 76,4 bilhões** (Instituto Pet Brasil, 2024), mais de 70% das clínicas ainda operam com prontuário em papel ou planilhas.

Este repositório contém o **subsistema IoT** do KURA: monitoramento em tempo real da câmara fria de vacinas. A perda de um lote de vacinas por falha de refrigeração gera prejuízo financeiro direto para a clínica e risco clínico para os pacientes — problema crítico e frequente que o KURA resolve com hardware de baixo custo, arquitetura orientada a eventos e persistência rastreável.

```
Câmara Fria
[ESP32 + DHT22 + LDR]
       │
       │  MQTT (QoS 1)
       ▼
   Node-RED
   ├── Persiste → LEITURA_TEMPERATURA (Oracle/MySQL)
   ├── Alerta  → ALERTA_TEMPERATURA   (Oracle/MySQL)
   ├── Cruza   → OpenWeather API (contexto climático)
   └── Exibe   → Dashboard node-red-dashboard
```

---

## Problema que este módulo resolve

| Situação sem KURA | Com KURA IoT |
|---|---|
| Clínica descobre temperatura fora da faixa ao abrir a geladeira — muitas horas depois | Alerta gerado em até 10 segundos após a primeira leitura anômala |
| Porta da câmara deixada aberta passa despercebida | LDR detecta luz e registra `ALERTA: Porta Aberta` em cada leitura afetada |
| Perda de lote descoberta apenas no vencimento das vacinas | Histórico time-series rastreável com timestamp de início e fim do desvio |
| Sem correlação com condições externas | Temperatura ambiente (OpenWeather) cruza com a interna — correlaciona estresse do compressor com calor externo |

---

## Regra de Negócio ANVISA

> **ANVISA RDC 197/2017** — vacinas biológicas devem ser armazenadas entre **2°C e 8°C** de forma contínua. A quebra da cadeia fria, ainda que temporária, implica na **inutilização obrigatória do lote** afetado.

O firmware e o nó Function do Node-RED aplicam essa regra de forma independente (defense in depth):

```
Temperatura lida  →  [2°C ≤ T ≤ 8°C] ?
                          ├── SIM → ST_DENTRO_FAIXA = 'S'  (normal)
                          └── NÃO → ST_DENTRO_FAIXA = 'N'  + ALERTA_TEMPERATURA
```

A severidade do alerta segue a escala documentada no schema:

| Faixa                        | Severidade   |
|------------------------------|--------------|
| 8–10°C ou 0–2°C              | `BAIXA`      |
| 10–12°C ou −2–0°C            | `MEDIA`      |
| >12°C ou <−2°C               | `ALTA`       |
| >20°C ou sensor offline >30min | `CRITICA`  |

---

## Arquitetura

```
┌─────────────────────────────────────────────────────────────────────┐
│                         EDGE (ESP32)                                │
│                                                                     │
│  DHT22 ──► temperatura (°C) + umidade (%)                          │
│  LDR   ──► luminosidade ADC (0–4095) → flag porta_aberta           │
│                                                                     │
│  Loop millis() — sem delay() — leitura a cada 10s                  │
│  LWT configurado: broker publica "offline" se ESP32 cair            │
└──────────────────────────┬──────────────────────────────────────────┘
                           │  MQTT QoS 1
                           │  tópico: kura/clinica01/camarafria/telemetria
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     MIDDLEWARE (Node-RED)                           │
│                                                                     │
│  [MQTT In] → [Guarda payload ESP32]                                 │
│            → [HTTP GET OpenWeather API]                             │
│            → [Funde ESP32 + Clima]                                  │
│            → [Function: Processa Telemetria] ──► 3 saídas:         │
│                ├── INSERT LEITURA_TEMPERATURA   → [MySQL node]      │
│                ├── INSERT ALERTA_TEMPERATURA    → [MySQL node] (*)  │
│                └── Dados formatados             → [Dashboard]       │
│                                                                     │
│  (*) apenas na transição normal → anômalo (evita spam de alertas)  │
│                                                                     │
│  [MQTT In status/LWT] → INSERT ALERTA SENSOR_OFFLINE               │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
    ┌──────────────────┐    ┌──────────────────────┐
    │  Oracle/MySQL    │    │  Dashboard            │
    │                  │    │  node-red-dashboard   │
    │  DISPOSITIVO_IOT │    │                       │
    │  LEITURA_TEMP    │    │  Gauge: temperatura   │
    │  ALERTA_TEMP     │    │  Gauge: umidade       │
    └──────────────────┘    │  Chart: histórico     │
                            │  Text: status/clima   │
                            └──────────────────────┘
```

### Decisões de arquitetura documentadas

**Por que o ESP32 não acessa o banco diretamente?**
Dispositivos de borda não têm onde guardar credenciais com segurança. MQTT é stateless, leve e tolerante a falha de rede — o ESP32 pode ficar offline por horas e o banco nunca fica exposto. Toda lógica de negócio (regra ANVISA, severidade do alerta, correlação climática) fica no Node-RED, onde pode ser atualizada sem reflashear firmware.

**Por que QoS 1 para telemetria e não QoS 2?**
QoS 2 garante entrega exatamente 1 vez, mas custa 4 trocas de pacotes por mensagem. Com leituras a cada 10s, uma leitura duplicada ocasional é tolerável e preferível à latência adicional. O controle de deduplicação (alerta apenas na transição de estado) está no nó Function, não no protocolo.

**Por que `flow.set()` para controle de alerta e não uma query de verificação ao banco?**
Consultar o banco a cada 10s para verificar se já existe alerta ativo criaria uma carga desnecessária e um ponto de falha. O contexto de fluxo do Node-RED é memória volátil — em caso de restart, o pior cenário é um alerta duplicado, que é benigno comparado a alertas perdidos.

---

## Stack Tecnológica

| Camada        | Tecnologia                                  | Versão         |
|---------------|---------------------------------------------|----------------|
| Microcontrolador | ESP32 DevKit v1                          | —              |
| Sensor temp/umid | DHT22                                   | —              |
| Sensor luz    | LDR + divisor de tensão 10kΩ               | —              |
| Firmware      | Arduino Framework (C++)                     | ESP32 Core 2.x |
| Protocolo     | MQTT                                        | v3.1.1         |
| Broker        | HiveMQ Cloud (público) / Mosquitto (local)  | —              |
| Orquestração  | Node-RED                                    | 3.x            |
| Banco de dados | Oracle 19c (produção) / MySQL 8.x (dev)   | —              |
| API Externa   | OpenWeatherMap                              | v2.5           |
| Dashboard     | node-red-dashboard                          | 3.x            |
| Simulação     | Wokwi                                       | —              |

### Bibliotecas Arduino

| Biblioteca            | Autor             | Uso                        |
|-----------------------|-------------------|----------------------------|
| `PubSubClient`        | Nick O'Leary      | Cliente MQTT               |
| `ArduinoJson`         | Benoît Blanchon   | Serialização JSON          |
| `DHT sensor library`  | Adafruit          | Leitura do DHT22           |
| `Adafruit Unified Sensor` | Adafruit     | Dependência do DHT         |

### Paletas Node-RED

| Paleta                    | Função               |
|---------------------------|----------------------|
| `node-red-node-mysql`     | Persistência MySQL   |
| `node-red-dashboard`      | UI (Gauge, Chart, Text) |

---

## Estrutura do Repositório

```
kura-iot/
├── firmware/
│   └── kura_camarafria.ino         # Firmware ESP32 — DHT22 + LDR + MQTT
│
├── nodered/
│   ├── flow.json                   # Fluxo Node-RED (importável via clipboard)
│   └── function_processa_telemetria.js  # Código do nó Function (versionado separado)
│
├── wokwi/
│   └── diagram.json                # Circuito para simulação no Wokwi
│
└── README.md
```

> **Nota:** o código do nó Function está separado em `.js` para facilitar o versionamento no Git (diff legível). No Node-RED, cole o conteúdo do arquivo diretamente no campo do nó.

---

## Payload MQTT

**Tópico de publicação:** `kura/clinica01/camarafria/telemetria`  
**Tópico de status/LWT:** `kura/clinica01/camarafria/status`

```json
{
  "id_dispositivo": 1,
  "temperatura": 5.4,
  "umidade": 62.3,
  "ldr_adc": 280,
  "porta_aberta": false,
  "dentro_faixa": true,
  "msg_id": 42,
  "uptime_s": 420
}
```

| Campo           | Tipo    | Descrição                                                   |
|-----------------|---------|-------------------------------------------------------------|
| `id_dispositivo`| int     | FK → `DISPOSITIVO_IOT.ID_DISPOSITIVO` no banco              |
| `temperatura`   | float   | °C com 1 casa decimal                                       |
| `umidade`       | float   | % com 1 casa decimal                                        |
| `ldr_adc`       | int     | Valor bruto ADC 0–4095 (0 = escuro, 4095 = muito iluminado) |
| `porta_aberta`  | boolean | `true` se `ldr_adc > 1500` (threshold configurável)        |
| `dentro_faixa`  | boolean | `true` se `2°C ≤ temperatura ≤ 8°C` (ANVISA)               |
| `msg_id`        | int     | Contador sequencial para rastreamento de perda de pacotes   |
| `uptime_s`      | int     | Segundos desde o boot do ESP32 (detecta reinicializações)   |

---

## Banco de Dados

Este módulo utiliza três tabelas do schema `kura_schema_v3.sql`, pertencentes ao domínio IoT (seção 9 do schema):

### `DISPOSITIVO_IOT`

Catálogo de sensores. Cada câmara fria da clínica = 1 registro. Contém os limites ANVISA configuráveis por dispositivo (`NR_TEMP_MINIMA`, `NR_TEMP_MAXIMA`).

```sql
-- Inserir o dispositivo antes de iniciar o firmware
INSERT INTO DISPOSITIVO_IOT
  (ID_CLINICA, DS_IDENTIFICADOR, NM_DISPOSITIVO, DS_TIPO,
   DS_LOCALIZACAO, NR_TEMP_MINIMA, NR_TEMP_MAXIMA, NR_INTERVALO_LEITURA)
VALUES
  (1, 'kura_esp32_clinica01_camarafria', 'Câmara Fria Principal',
   'TERMOMETRO_VACINA', 'Sala de Vacinas', 2.0, 8.0, 10);
```

### `LEITURA_TEMPERATURA`

Time-series de todas as leituras. Cresce ~8.640 linhas/dia (1 leitura a cada 10s).

```sql
-- Coluna relevante gerada pelo Node-RED:
-- ST_DENTRO_FAIXA: 'S' se temperatura dentro de 2-8°C
-- DS_OBSERVACAO:   texto de alerta se porta aberta ou temperatura fora de faixa
```

**Exemplo de linha com anomalia:**
```
ID_LEITURA | NR_TEMPERATURA | NR_UMIDADE | ST_DENTRO_FAIXA | DS_OBSERVACAO
---------- | -------------- | ---------- | --------------- | -----------------------------------------------
1042       | 11.8           | 58.1       | N               | TEMP_FORA_FAIXA: ACIMA_MAXIMO | Desvio=3.8°C
```

### `ALERTA_TEMPERATURA`

Um alerta por evento (não por leitura). Inserido apenas na transição de estado normal → anômalo.

```sql
-- Estrutura relevante:
-- DS_TIPO_ALERTA:    'TEMP_ALTA' | 'TEMP_BAIXA' | 'SENSOR_OFFLINE' | 'VARIACAO_BRUSCA'
-- DS_SEVERIDADE:     'BAIXA' | 'MEDIA' | 'ALTA' | 'CRITICA'
-- ST_RESOLVIDO:      'N' quando aberto, 'S' após equipe agir (atualizado via frontend)
-- DT_FIM:            preenchido quando temperatura volta à faixa (NULL = ativo)
```

---

## Fluxo Node-RED

### Estrutura dos nós

```
[MQTT In: telemetria] 
    → [fn: Guarda payload ESP32]
    → [HTTP GET: OpenWeather API]
    → [fn: Funde ESP32 + Clima]
    → [fn: Processa Telemetria + Clima]  ← 3 saídas
         │
         ├── [MySQL: INSERT LEITURA_TEMPERATURA]  → [Debug]
         ├── [MySQL: INSERT ALERTA_TEMPERATURA]   → [Debug]  (condicional)
         └── [ui_gauge temp] [ui_gauge umid] [ui_chart histórico] [ui_text status] [ui_text clima]

[MQTT In: status/LWT]
    → [fn: Verifica Offline]
    → [MySQL: INSERT ALERTA SENSOR_OFFLINE]
```

### Integração OpenWeather — justificativa técnica

A temperatura ambiente externa não é dado decorativo. Refrigeradores industriais trabalham proporcionalmente à diferença térmica entre interno e externo. O campo `delta_temp` (temperatura interna − temperatura externa) calculado no nó Function permite identificar padrões como:

> "Toda vez que a temperatura de São Paulo ultrapassa 32°C, a câmara fria começa a subir para 9–10°C após 2 horas."

Esse dado alimenta análise preditiva de manutenção preventiva — feature de valor para o plano Enterprise do KURA.

**URL configurada:**
```
https://api.openweathermap.org/data/2.5/weather?q=Sao+Paulo,BR&appid=SUA_CHAVE&units=metric&lang=pt_br
```

> Frequência: a cada telemetria recebida (~10s). Em produção, recomenda-se desacoplar para um inject independente de 10min para não exceder o limite gratuito da API (60 req/min).

### Como importar o fluxo

1. Abra o Node-RED: `http://localhost:1880`
2. Menu (☰) → **Import** → **Clipboard**
3. Cole o conteúdo de `nodered/flow.json`
4. Clique **Import**
5. Configure as credenciais nos nós marcados (MySQL e OpenWeather)
6. **Deploy**

---

## Dashboard

Acesse em: `http://localhost:1880/ui`

| Nó                | Função                                                            |
|-------------------|-------------------------------------------------------------------|
| `ui_gauge` (temp) | Gauge 0–30°C com segmentos verde (2–8°C), amarelo e vermelho     |
| `ui_gauge` (umid) | Gauge 0–100% para umidade relativa                                |
| `ui_chart`        | Gráfico de linha histórico — temperatura vs. tempo               |
| `ui_text` (status)| Texto dinâmico: `✅ NORMAL` ou `⚠️ ALERTA`                       |
| `ui_text` (clima) | Temperatura e descrição OpenWeather em tempo real                 |

---

## Simulação com Wokwi

O arquivo `wokwi/diagram.json` simula o circuito completo no [Wokwi](https://wokwi.com).

### Como usar

1. Acesse [wokwi.com](https://wokwi.com) → **New Project** → **ESP32**
2. Substitua o `diagram.json` gerado pelo arquivo deste repositório
3. Cole o código de `firmware/kura_camarafria.ino` no editor
4. Instale as bibliotecas via `libraries.txt` (Library Manager do Wokwi)
5. **Start Simulation**

### Cenários de teste

| Cenário                    | Como simular no Wokwi                     | Comportamento esperado                         |
|----------------------------|-------------------------------------------|------------------------------------------------|
| Temperatura normal         | `"temperature": "5.5"` no DHT22           | `ST_DENTRO_FAIXA = 'S'` — sem alerta           |
| Temperatura alta (MEDIA)   | `"temperature": "11.0"`                   | Alerta `TEMP_ALTA` / `MEDIA` inserido          |
| Temperatura crítica        | `"temperature": "22.0"`                   | Alerta `TEMP_ALTA` / `CRITICA` inserido        |
| Porta aberta               | `"lux": "500"` no sensor LDR              | `DS_OBSERVACAO = 'ALERTA: Porta Aberta ...'`   |
| Sensor offline             | Pausar simulação                          | Broker publica LWT → alerta `SENSOR_OFFLINE`   |

---

## Como executar

### Pré-requisitos

- [Arduino IDE 2.x](https://www.arduino.cc/en/software) com suporte ao ESP32 (board manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
- [Node-RED](https://nodered.org/docs/getting-started/) (Node.js 18+): `npm install -g --unsafe-perm node-red`
- MySQL 8.x ou Oracle 19c com o schema `kura_schema_v3.sql` aplicado
- Conta gratuita em [openweathermap.org](https://openweathermap.org/api) para obter a API key

### 1. Banco de dados

```bash
# MySQL (ambiente de desenvolvimento)
mysql -u root -p < kura_schema_v3.sql

# Inserir o dispositivo IoT (obrigatório antes de iniciar o firmware)
mysql -u root -p kura_db -e "
INSERT INTO DISPOSITIVO_IOT
  (ID_CLINICA, DS_IDENTIFICADOR, NM_DISPOSITIVO, DS_TIPO,
   DS_LOCALIZACAO, NR_TEMP_MINIMA, NR_TEMP_MAXIMA, NR_INTERVALO_LEITURA)
VALUES
  (1, 'kura_esp32_clinica01_camarafria', 'Câmara Fria Principal',
   'TERMOMETRO_VACINA', 'Sala de Vacinas', 2.0, 8.0, 10);"
```

### 2. Firmware

```bash
# Na Arduino IDE:
# 1. Abra firmware/kura_camarafria.ino
# 2. Edite WIFI_SSID, WIFI_PASSWORD e MQTT_BROKER
# 3. Instale as bibliotecas listadas em "Stack Tecnológica"
# 4. Selecione: Tools → Board → "ESP32 Dev Module"
# 5. Upload
# 6. Monitor serial (115200 baud) para verificar logs
```

### 3. Node-RED

```bash
# Iniciar Node-RED
node-red

# Instalar paletas necessárias (via Manage Palette ou linha de comando)
cd ~/.node-red
npm install node-red-node-mysql node-red-dashboard
```

Após importar o `flow.json`:
- Configure o nó **MySQL**: host, porta, banco, usuário e senha
- Configure o nó **HTTP Request**: substitua `SUA_CHAVE_OPENWEATHER` pela sua API key
- **Deploy**

Dashboard: `http://localhost:1880/ui`

---

## Evidências

> Para a apresentação do Challenge, as evidências abaixo devem ser gravadas no vídeo:

- [ ] Serial Monitor do ESP32 exibindo publicações MQTT a cada 10s
- [ ] Debug panel do Node-RED mostrando INSERTs sendo executados
- [ ] Tabela `LEITURA_TEMPERATURA` com dados persistidos (query no MySQL Workbench)
- [ ] Alerta inserido em `ALERTA_TEMPERATURA` ao simular temperatura fora de faixa (Wokwi: `"temperature": "12.0"`)
- [ ] Alerta de porta aberta ao simular LDR iluminado (Wokwi: `"lux": "500"`)
- [ ] Dashboard com Gauge e Chart atualizando em tempo real
- [ ] Dashboard exibindo clima externo via OpenWeather

---

## Equipe

| Nome | RM | Responsabilidade |
|------|----|-----------------|
| Felipe Ferrete | — | Tech Lead · Firmware ESP32 · Node-RED · Arquitetura IoT |
| [Integrante 2] | — | [Responsabilidade] |
| [Integrante 3] | — | [Responsabilidade] |
| [Integrante 4] | — | [Responsabilidade] |
| [Integrante 5] | — | [Responsabilidade] |

**Turma:** 2TDS — Fevereiro · **Curso:** Análise e Desenvolvimento de Sistemas  
**Período:** 1º Semestre 2026 · **Entrega Sprint 1 e 2:** 24/05/2025

---

## Relação com o Sistema KURA

Este repositório é o módulo IoT de um sistema maior. Os outros repositórios do projeto são:

| Repositório | Stack | Domínio |
|---|---|---|
| `kura-backend-clinica` | .NET 10 / ASP.NET Core | Prontuário, vacinas, eventos clínicos |
| `kura-backend-tutor` | Java Spring Boot 3.3 | Agendamentos, consentimentos, identidade |
| `kura-luna` | Python + YOLOv8n | IA, visão computacional, WhatsApp bot |
| `kura-mobile` | React Native + Expo | Portal do Tutor (app mobile) |
| **`kura-iot`** ← este | C++ (Arduino) + Node-RED | Monitoramento de câmara fria |

---

*KURA — parceria FIAP Challenge 2026 × Clyvo Vet*
