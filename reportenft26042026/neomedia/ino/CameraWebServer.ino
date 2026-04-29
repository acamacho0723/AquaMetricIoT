/*
 *  ESP32-CAM - Enviar TDS a servidor remoto mediante PWM
 *  Mide el ancho de pulso en GPIO14 (proporcional a ppm) y envía al servidor.
 *  También muestra la lectura en una página web local mínima.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ================== CONFIGURACIÓN DE RED ==================
const char* ssid = "NIGHTHAWK";
const char* password = "1866425718";

// const char* ssid = "Redmi Note 13";
// const char* password = "NAYARNOSE6OLVIDA123";

// ================== SERVIDOR REMOTO ==================
const char* serverUrl = "https://hydroponics10101.pythonanywhere.com/recv";
const char* usr = "admin123123";
const char* pwd = "hydroponics10101v1";

// ================== CONFIGURACIÓN NTP ==================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -25200;   // UTC-7 (GMT-7)
const int   daylightOffset_sec = 0;

// ================== PINES ==================
#define FLASH_PIN   4   // LED del flash integrado
#define PWM_PIN     14  // Entrada del pulso PWM desde UNO (vía divisor)

// ================== SERVIDORES LOCALES ==================
WebServer server(80);
WebSocketsServer webSocket(81);

String lastTdsValue = "--";

// Control del flash en caso de error
unsigned long flashErrorOnTime = 0;
const unsigned long flashErrorDuration = 5000; // 5 segundos

// Variables para medición de frecuencia (tren de pulsos)
unsigned long lastPeriod = 0;
unsigned long lastValidFreqTime = 0;
const unsigned long minFreqInterval = 2000; // mínimo 2 segundos entre envíos

// ================== PÁGINA WEB MÍNIMA ==================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>TDS</title>
</head>
<body>
    <span id="tds">--</span>
    <script>
        var socket = new WebSocket('ws://' + location.hostname + ':81/');
        var span = document.getElementById('tds');
        socket.onmessage = function(e) {
            span.textContent = e.data;
        };
    </script>
</body>
</html>
)rawliteral";

// ================== MANEJADOR WEBSOCKET ==================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    // No se reciben mensajes del cliente
}

// ================== OBTENER HORA FORMATEADA ==================
String getFormattedTime() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

// ================== ENVIAR TDS AL SERVIDOR ==================
bool sendTdsToServer(int tdsValue) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR:WiFi no conectado");
        return false;
    }

    HTTPClient http;
    String fullUrl = String(serverUrl) + "?usr=" + String(usr) + "&pwd=" + String(pwd);
    http.begin(fullUrl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["timestamp"] = getFormattedTime();
    doc["tds"] = tdsValue;

    String jsonString;
    serializeJson(doc, jsonString);

    int httpCode = http.POST(jsonString);
    bool success = false;

    if (httpCode == 200) {
        success = true;
        // Opcional: leer respuesta
        // String response = http.getString();
    }
    http.end();

    if (!success) {
        Serial.println("ERROR:Servidor remoto");
    }
    return success;
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);  // Solo depuración local (no conectado al UNO)

    pinMode(FLASH_PIN, OUTPUT);
    digitalWrite(FLASH_PIN, LOW);
    pinMode(PWM_PIN, INPUT);

    // Conexión WiFi
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(FLASH_PIN, HIGH);
        delay(5000);
        Serial.println("Reiniciando ESP32 por falta de WiFi...");
        delay(1000);
        ESP.restart();
    }

    // Sincronizar hora NTP (sin imprimir ruido)
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    time_t now = time(nullptr);
    int timeout = 0;
    while (now < 100000 && timeout < 20) {
        delay(500);
        now = time(nullptr);
        timeout++;
    }

    // Servidor web y WebSocket
    server.on("/", []() {
        server.send_P(200, "text/html", index_html);
    });
    server.begin();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    Serial.println("Sistema listo (PWM en GPIO14)");
}

// ================== LOOP PRINCIPAL ==================
void loop() {
    server.handleClient();
    webSocket.loop();

    // --- Control del flash (apagar tras tiempo de error) ---
    if (flashErrorOnTime > 0 && millis() - flashErrorOnTime >= flashErrorDuration) {
        digitalWrite(FLASH_PIN, LOW);
        flashErrorOnTime = 0;
    }

    // --- Verificar WiFi periódicamente ---
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck >= 10000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            digitalWrite(FLASH_PIN, HIGH);
            flashErrorOnTime = millis();
        }
    }

    // --- Medición de frecuencia del tren de pulsos (frecuencia en Hz = ppm) ---
    // Esperar un flanco de subida y medir el período completo (HIGH+LOW)
    unsigned long highDuration = pulseIn(PWM_PIN, HIGH, 50000);  // timeout 50 ms
    if (highDuration > 0) {
        unsigned long lowDuration = pulseIn(PWM_PIN, LOW, 50000);
        if (lowDuration > 0) {
            unsigned long period = highDuration + lowDuration; // en microsegundos
            if (period > 0) {
                float freq = 1000000.0 / period; // frecuencia en Hz
                lastPeriod = period;
                lastValidFreqTime = millis();
                
                // Solo procesar si la frecuencia es razonable (evitar ruido)
                static unsigned long lastProcessed = 0;
                if (freq >= 10 && freq <= 2000) { // rango esperado 10-2000 ppm
                    if (millis() - lastProcessed >= minFreqInterval) {
                        lastProcessed = millis();
                        
                        int tds = (int)(freq + 0.5); // redondear al entero más cercano
                        
                        // Actualizar página web
                        String tdsStr = String(tds);
                        webSocket.broadcastTXT(tdsStr);
                        lastTdsValue = tdsStr;
                        
                        // Enviar al servidor
                        bool success = sendTdsToServer(tds);
                        if (success) {
                            digitalWrite(FLASH_PIN, LOW);
                        } else {
                            digitalWrite(FLASH_PIN, HIGH);
                            flashErrorOnTime = millis();
                        }
                    }
                }
            }
        }
    }

    delay(10);
}