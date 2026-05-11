// ============================================================================
//  KURA IoT — Node-RED: Nó Function Principal
//  Nome sugerido: "Processa Telemetria + Clima"
// ============================================================================
//  Contexto: Este nó recebe msg.payload com os dados FUNDIDOS do ESP32 e
//  da API OpenWeather, e prepara as queries SQL para o banco de dados KURA.
//
//  Entrada esperada em msg.payload (montado pelo nó anterior — veja abaixo):
//  {
//    "esp32": { ...payload do MQTT... },
//    "clima": { ...payload da OpenWeather API... }
//  }
//
//  Saídas:
//    output 1 → INSERT em LEITURA_TEMPERATURA  (sempre)
//    output 2 → INSERT em ALERTA_TEMPERATURA   (apenas se fora de faixa OU porta aberta)
//    output 3 → dados formatados para o Dashboard (sempre)
// ============================================================================

// --- 1. VALIDAÇÃO DE ENTRADA ---
const esp32 = msg.payload.esp32;
const clima  = msg.payload.clima;

if (!esp32 || esp32.temperatura === undefined) {
    node.warn("[KURA] Payload do ESP32 inválido ou ausente. Abortando.");
    return null; // descarta a mensagem — não grava dado ruim no banco
}

// --- 2. EXTRAÇÃO DOS DADOS DO ESP32 ---
const idDispositivo = esp32.id_dispositivo || 1;  // FK → DISPOSITIVO_IOT
const temperatura   = parseFloat(esp32.temperatura);
const umidade       = parseFloat(esp32.umidade);
const ldrADC        = parseInt(esp32.ldr_adc);
const portaAberta   = Boolean(esp32.porta_aberta);

// --- 3. REGRA DE NEGÓCIO: ANVISA RDC 197/2017 ---
// Faixa segura: 2°C a 8°C.
// Usamos os limites do schema (DISPOSITIVO_IOT.NR_TEMP_MINIMA/MAXIMA = 2.0/8.0).
// O ESP32 já pré-calculou dentro_faixa, mas recalculamos aqui para garantir
// que a lógica de negócio seja confiável e não dependa do edge.
const TEMP_MIN = 2.0;
const TEMP_MAX = 8.0;

const dentroFaixa  = (temperatura >= TEMP_MIN && temperatura <= TEMP_MAX);
const stDentroFaixa = dentroFaixa ? 'S' : 'N';

// --- 4. OBSERVAÇÃO: Alerta de Porta Aberta via LDR ---
// O LDR é a câmara escura. Luz detectada = porta aberta = risco de quebra de cadeia fria.
// Inserimos como DS_OBSERVACAO para rastreabilidade no time-series.
let dsObservacao = null;

if (portaAberta) {
    // Formatamos como alert string para fácil filtragem pelo frontend
    dsObservacao = `ALERTA: Porta Aberta | LDR_ADC=${ldrADC}`;
}

if (!dentroFaixa) {
    // Enriquecemos a observação com o desvio de temperatura
    const desvio   = temperatura < TEMP_MIN
        ? (temperatura - TEMP_MIN).toFixed(1)  // negativo = abaixo do mínimo
        : (temperatura - TEMP_MAX).toFixed(1); // positivo = acima do máximo

    const tipoDesvio = temperatura < TEMP_MIN ? "ABAIXO_MINIMO" : "ACIMA_MAXIMO";
    const extra = `TEMP_FORA_FAIXA: ${tipoDesvio} | Desvio=${desvio}°C`;
    dsObservacao = dsObservacao ? `${dsObservacao} | ${extra}` : extra;
}

// Limitar a 200 chars (constraint da tabela LEITURA_TEMPERATURA.DS_OBSERVACAO)
if (dsObservacao && dsObservacao.length > 200) {
    dsObservacao = dsObservacao.substring(0, 197) + "...";
}

// --- 5. EXTRAÇÃO DO CLIMA EXTERNO (OpenWeather) ---
// Útil para correlacionar: temperatura externa alta → refrigerador trabalha mais
// → risco de falha. Dado de contexto para análise posterior pela equipe da clínica.
const climaInfo = {};
if (clima && clima.main) {
    climaInfo.temp_ext   = clima.main.temp;          // °C (units=metric na URL)
    climaInfo.umidade_ext = clima.main.humidity;     // %
    climaInfo.descricao  = clima.weather?.[0]?.description || 'N/A';
    climaInfo.cidade     = clima.name || 'São Paulo';
    // Correlação: diferença entre interno e externo
    climaInfo.delta_temp = (temperatura - climaInfo.temp_ext).toFixed(1);
}

// ============================================================================
// --- 6. PREPARAR QUERY: LEITURA_TEMPERATURA ---
// Não usamos NEXTVAL explicitamente — o MySQL/Oracle usa DEFAULT AUTO_INCREMENT
// ou o sequence. O nó MySQL do Node-RED executa a query parametrizada.
// ============================================================================
const queryLeitura = `
    INSERT INTO LEITURA_TEMPERATURA
        (ID_DISPOSITIVO, NR_TEMPERATURA, NR_UMIDADE, DT_LEITURA, ST_DENTRO_FAIXA, DS_OBSERVACAO)
    VALUES
        (?, ?, ?, NOW(), ?, ?)
`;

const paramsLeitura = [
    idDispositivo,          // FK → DISPOSITIVO_IOT.ID_DISPOSITIVO
    temperatura,            // NR_TEMPERATURA  (NUMBER 5,2)
    isNaN(umidade) ? null : umidade, // NR_UMIDADE (opcional — NULL se DHT22 não fornecer)
    stDentroFaixa,          // ST_DENTRO_FAIXA: 'S' ou 'N' (CK constraint)
    dsObservacao            // DS_OBSERVACAO: null se tudo normal, string de alerta se houver problema
];

// ============================================================================
// --- 7. LÓGICA DE ALERTA: ALERTA_TEMPERATURA ---
// Geramos alerta apenas quando há condição crítica. Evitamos 1 alerta por
// leitura (poluição visual). O Node-RED controla estado via context.flow.
// ============================================================================
let queryAlerta   = null;
let paramsAlerta  = null;

// Estado do alerta persistido no contexto do fluxo (memória do Node-RED)
// Permite evitar alertas duplicados: só insere quando há TRANSIÇÃO de estado.
const flowCtx    = flow.get("alertaAtivo") || { fora_faixa: false, porta_aberta: false };
const alertaAtivo = {
    fora_faixa:   !dentroFaixa,
    porta_aberta: portaAberta
};
flow.set("alertaAtivo", alertaAtivo);

// Insere alerta apenas na TRANSIÇÃO false → true (onset do problema)
const novoAlerteTemp  = alertaAtivo.fora_faixa   && !flowCtx.fora_faixa;
const novoAlertaPorta = alertaAtivo.porta_aberta && !flowCtx.porta_aberta;

if (novoAlerteTemp || novoAlertaPorta) {

    // Determinar tipo e severidade segundo comentário do schema:
    // BAIXA = 8-10°C ou 0-2°C | MEDIA = 10-12°C ou -2-0°C
    // ALTA  = >12°C ou <-2°C  | CRITICA = >20°C ou sensor offline >30min
    let tipoAlerta   = novoAlerteTemp ? (temperatura > TEMP_MAX ? 'TEMP_ALTA' : 'TEMP_BAIXA') : 'TEMP_ALTA';
    let severidade;

    if (novoAlertaPorta && !novoAlerteTemp) {
        // Porta aberta isolada: ainda não comprometeu a temperatura → MEDIA
        tipoAlerta = 'TEMP_ALTA'; // Porta aberta = pré-condição para elevação
        severidade = 'MEDIA';
    } else if (temperatura > 20 || temperatura < -2) {
        severidade = 'CRITICA';
    } else if (temperatura > 12 || temperatura < -2) {
        severidade = 'ALTA';
    } else if (temperatura > 10 || temperatura < 0) {
        severidade = 'MEDIA';
    } else {
        severidade = 'BAIXA';
    }

    // Mensagem humana para a equipe da clínica
    let mensagem = `[KURA IoT] Alerta na câmara fria. `;
    if (novoAlerteTemp) {
        mensagem += `Temperatura: ${temperatura}°C (Faixa ANVISA: ${TEMP_MIN}-${TEMP_MAX}°C). `;
    }
    if (novoAlertaPorta) {
        mensagem += `Porta da câmara detectada aberta (LDR_ADC=${ldrADC}). `;
    }
    if (climaInfo.temp_ext !== undefined) {
        mensagem += `Temp. externa: ${climaInfo.temp_ext}°C (${climaInfo.descricao}).`;
    }

    // Limitar a 500 chars (constraint ALERTA_TEMPERATURA.DS_MENSAGEM)
    mensagem = mensagem.substring(0, 500);

    queryAlerta = `
        INSERT INTO ALERTA_TEMPERATURA
            (ID_DISPOSITIVO, DS_TIPO_ALERTA, DS_SEVERIDADE, NR_TEMP_REGISTRADA, DT_INICIO, DS_MENSAGEM, ST_RESOLVIDO)
        VALUES
            (?, ?, ?, ?, NOW(), ?, 'N')
    `;

    paramsAlerta = [
        idDispositivo,   // FK → DISPOSITIVO_IOT
        tipoAlerta,      // CHECK: 'TEMP_ALTA','TEMP_BAIXA','SENSOR_OFFLINE','VARIACAO_BRUSCA'
        severidade,      // CHECK: 'BAIXA','MEDIA','ALTA','CRITICA'
        temperatura,     // NR_TEMP_REGISTRADA: temperatura no momento do alerta
        mensagem         // DS_MENSAGEM: até 500 chars
    ];
}

// ============================================================================
// --- 8. DADOS PARA O DASHBOARD (node-red-dashboard) ---
// ============================================================================
const dadosDashboard = {
    temperatura:     temperatura,
    umidade:         umidade,
    ldr_adc:         ldrADC,
    porta_aberta:    portaAberta,
    dentro_faixa:    dentroFaixa,
    status_display:  dentroFaixa && !portaAberta ? "✅ NORMAL" : "⚠️ ALERTA",
    cor_gauge:       dentroFaixa ? (portaAberta ? "orange" : "green") : "red",
    clima_externo:   climaInfo,
    timestamp:       new Date().toLocaleString('pt-BR', { timeZone: 'America/Sao_Paulo' })
};

// ============================================================================
// --- 9. SAÍDAS DO NÓ ---
// Configurar 3 saídas no painel do nó Function no Node-RED.
// ============================================================================

// Output 1: sempre → INSERT em LEITURA_TEMPERATURA
const msgLeitura = {
    topic:   queryLeitura,
    payload: paramsLeitura
};

// Output 2: apenas se há alerta novo → INSERT em ALERTA_TEMPERATURA
const msgAlerta = queryAlerta ? { topic: queryAlerta, payload: paramsAlerta } : null;

// Output 3: sempre → Dashboard (gauge, chart, texto)
const msgDash = { payload: dadosDashboard };

return [msgLeitura, msgAlerta, msgDash];
