/*
 * Arduino UNO Controlador - Servo + TDS + Envío a ESP32-CAM (Sin recepción)
 * 
 * Secuencia:
 *  1. Mover servo a 90° (introducir electrodo).
 *  2. Medir TDS cada 5 segundos hasta que RSD ≤ 5%.
 *  3. Mostrar métricas finales.
 *  4. Mover servo a 0° (sacar electrodo).
 *  5. Enviar tren de pulsos de frecuencia = valor TDS Hz durante 1 segundo al ESP32-CAM.
 *  6. Fin del programa (el sistema se detiene).
 */

#include <Servo.h>
#include <math.h>
// #include <avr/wdt.h>        // Para wdt_disable()
#include "GravityTDS.h"

// ================== CONFIGURACIÓN DE PINES ==================
#define TdsSensorPin A1      // Pin analógico del sensor TDS
#define ServoPin 9           // Pin de control PWM del servomotor
#define PWM_OUT_PIN 5        // Pin de salida del pulso PWM hacia ESP32-CAM

// ================== OBJETOS ==================
GravityTDS gravityTds;
Servo miServo;

// ================== VARIABLES SENSOR TDS ==================
float temperature = 25.0;    // Temperatura ambiente (ajustable)

// Buffer circular para cálculo de estabilidad
const int numReadings = 10;
float readings[numReadings];
int readIndex = 0;
float total = 0;
bool bufferFull = false;

// Control de tiempo para lecturas
unsigned long lastReadTime = 0;
const unsigned long readInterval = 5000;  // 5 segundos

// Variables de estabilidad
bool estabilizado = false;
float valorFinalTDS = 0;
unsigned long tiempoInicioMedicion = 0;

// ================== ESTADOS DEL SISTEMA ==================
enum EstadoSistema {
  MOVIENDO_A_90,
  MIDIENDO,
  ESTABILIZADO,
  ENVIANDO_DATOS,
  ESPERANDO,
  FINALIZADO
};
EstadoSistema estado = MOVIENDO_A_90;
unsigned long waitStartTime = 0;

// ================== SETUP ==================
void setup() {
  // Desactivar Watchdog Timer por seguridad
  // wdt_disable();
  
  Serial.begin(9600);
  
  // Configurar pin de salida PWM
  pinMode(PWM_OUT_PIN, OUTPUT);
  digitalWrite(PWM_OUT_PIN, LOW);
  
  // Configurar servomotor
  miServo.attach(ServoPin);
  
  // Configurar sensor TDS
  gravityTds.setPin(TdsSensorPin);
  gravityTds.setAref(5.0);
  gravityTds.setAdcRange(1024);
  gravityTds.begin();
  
  Serial.println(F("--- SISTEMA CONTROLADOR INICIADO (Modo solo envío) ---"));
  
  // Posición inicial del servo (fuera del líquido)
  Serial.println(F("0. Reiniciando posición del servo (0°)"));
  miServo.write(0);
  delay(500);
  
  // Mover a posición de medición
  Serial.println(F("1. Moviendo servo a 90° (introduciendo electrodo)..."));
  miServo.write(90);
  delay(1500); // Esperar a que el servo se estabilice y el electrodo se humedezca
  
  // Inicializar buffer con ceros
  for (int i = 0; i < numReadings; i++) {
    readings[i] = 0;
  }
  
  Serial.println(F("2. Iniciando mediciones de TDS cada 5 segundos..."));
  Serial.println(F("--------------------------------------------------"));
  
  tiempoInicioMedicion = millis();
  estado = MIDIENDO;
}

// ================== LOOP PRINCIPAL ==================
void loop() {
  // Máquina de estados principal (sin recepción de datos)
  switch (estado) {
    case MOVIENDO_A_90:
      {
        // Repetir ciclo
        Serial.println(F("Iniciando nuevo ciclo de medición."));
        miServo.write(90);
        delay(1500);
        
        // Reinicializar buffer y variables de medición
        for (int i = 0; i < numReadings; i++) {
          readings[i] = 0;
        }
        readIndex = 0;
        total = 0;
        bufferFull = false;
        estabilizado = false;
        valorFinalTDS = 0;
        lastReadTime = 0;
        tiempoInicioMedicion = millis();
        
        Serial.println(F("2. Iniciando mediciones de TDS cada 5 segundos..."));
        Serial.println(F("--------------------------------------------------"));
        estado = MIDIENDO;
      }
      break;
      
    case MIDIENDO:
      if (estabilizado) {
        estado = ESTABILIZADO;
        break;
      }
      
      if (millis() - lastReadTime >= readInterval) {
        lastReadTime = millis();
        
        gravityTds.setTemperature(temperature);
        gravityTds.update();
        float tds = gravityTds.getTdsValue();
        
        // Actualizar buffer circular
        total -= readings[readIndex];
        readings[readIndex] = tds;
        total += readings[readIndex];
        readIndex++;
        
        if (readIndex >= numReadings) {
          readIndex = 0;
          bufferFull = true;
        }
        
        if (!bufferFull) {
          Serial.print(F("TDS: "));
          Serial.print(tds, 0);
          Serial.print(F(" ppm (llenando buffer "));
          Serial.print(readIndex);
          Serial.print('/');
          Serial.print(numReadings);
          Serial.println(')');
          return;
        }
        
        // Calcular media y RSD
        float mean = total / numReadings;
        float sumSq = 0;
        for (int i = 0; i < numReadings; i++) {
          sumSq += pow(readings[i] - mean, 2);
        }
        float stdDev = sqrt(sumSq / (numReadings - 1));
        float rsd = (stdDev / mean) * 100.0;
        
        Serial.print(F("TDS: "));
        Serial.print(tds, 0);
        Serial.print(F(" ppm | Media: "));
        Serial.print(mean, 0);
        Serial.print(F(" | RSD: "));
        Serial.print(rsd, 1);
        Serial.println('%');
        
        if (rsd <= 5.0 && mean > 0) {
          estabilizado = true;
          valorFinalTDS = mean;
        }
      }
      break;
      
    case ESTABILIZADO:
      {
        unsigned long tiempoTotal = (millis() - tiempoInicioMedicion) / 1000;
        Serial.println();
        Serial.println(F("¡ESTABILIDAD ALCANZADA!"));
        Serial.print(F("Tiempo total de medición: "));
        Serial.print(tiempoTotal);
        Serial.println(F(" segundos"));
        Serial.print(F("Valor final de TDS: "));
        Serial.print(valorFinalTDS, 0);
        Serial.println(F(" ppm"));
        Serial.println(F("----------------------------------------"));
        
        Serial.println(F("3. Moviendo servo a 0° (sacando electrodo)..."));
        miServo.write(0);
        delay(1000);
        
        estado = ENVIANDO_DATOS;
        // Sin break para ejecutar inmediatamente el siguiente estado
      }
      
    case ENVIANDO_DATOS:
      {
        // Generar tren de pulsos de frecuencia = valorFinalTDS Hz durante 1 segundo
        unsigned int freq = (unsigned int)valorFinalTDS;
        if (freq < 10) freq = 10;      // Evitar frecuencias demasiado bajas
        if (freq > 2000) freq = 2000;  // Límite práctico para tone()
        
        Serial.print(F("4. Enviando tren de pulsos a ESP32-CAM (frecuencia = "));
        Serial.print(freq);
        Serial.println(F(" Hz)"));
        
        tone(PWM_OUT_PIN, freq);
        delay(1000);                   // Dejar sonar 1 segundo
        noTone(PWM_OUT_PIN);
        
        Serial.println(F("5. Proceso completado. Esperando 60 segundos para siguiente ciclo..."));
        waitStartTime = millis();
        estado = ESPERANDO;
      }
      break;
      
    case ESPERANDO:
      if (millis() - waitStartTime >= 60000) {
        estado = MOVIENDO_A_90;
      }
      break;
  }
}