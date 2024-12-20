#include <TB6612FNG.h>
#include <Wire.h>
#include "FastIMU.h"
#include <PID_v1.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// TB6612FNG has maximum PWM switching frequency of 100kHz.
#define DEFAULT_LEDC_FREQ 20000
#define DEFAULT_LEDC_RANGE 8
#define CHAN_PWMA 0
#define CHAN_PWMB 1

#define DELAY_MOTOR 1
#define DELAY_ANGLE 1

#define MAX_PWM_VALUE 200

// Definiciones de pines para control de motores
const int PWMA = 13; // PWM para motor 1
const int AIN1 = 14; // ain1 motor 1
const int AIN2 = 12; // ain2 motor 1
const int PWMB = 33; // PWM para motor 2
const int BIN1 = 26; // ain1 motor 2
const int BIN2 = 25; // ain2 motor 2
const int STBY = 27; // pin de StandBy

const int LED_PIN = 2; //pin del led montado sobre la esp32

// inicializacion de los motores
Tb6612fng motors(STBY, AIN1, AIN2, PWMB, BIN1, BIN2, PWMA);

// Definicion de los parametros de la PID
double angle, angle_filtro1, angle_offset;
double last_angles[3] = {0.0, 0.0, 0.0}; // Inicializa con ceros
double input, output, output_filtrada;
double setpoint = 0; // Queremos mantener el robot en 0 grado

// Variable por imperfecciones en la posición del acelerómetro
double neg_correct = 1.07;

// Constantes para el filtro complementario
const double dt = 0.001 * DELAY_MOTOR;  // Intervalo de tiempo (10 ms)
const double alpha = 0.90; // Peso del giroscopio en el filtro complementario

// Parámetros del PID
double Kp = 18.0;   // Ganancia proporcional
double Ki = 100.0;   // Ganancia integral
double Kd = 0.5;  // Ganancia derivativa

//PID myPID();
PID myPID(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);

// Dirección I2C del MPU6500
#define MPU6500_ADDRESS 0x68
MPU6500 mpu;

// Handles para las tareas de FreeRTOS
TaskHandle_t taskPID = NULL;
TaskHandle_t taskSensor = NULL;

// Variables para comunicación entre tareas
volatile double anguloActual = 0;
SemaphoreHandle_t xMutex;

double filtro_complementario(double acc_angle, double gyro_rate, double prev_angle) {
    return alpha * (prev_angle + gyro_rate * dt) + (1 - alpha) * acc_angle;
}

unsigned long last_time = 0;

double actualizarAngulo() {
    unsigned long current_time = millis();
    double dt = (current_time - last_time) / 1000.0; // Tiempo en segundos
    last_time = current_time;

    double gyro_rate = 0.0;
    double accel_angle = 0.0;         // Ángulo derivado del acelerómetro
    double angle_complementary = 0.0;  // Ángulo estimado

    // Actualizar datos del sensor
    mpu.update();
    AccelData accelData;
    GyroData gyroData;
    mpu.getAccel(&accelData);
    mpu.getGyro(&gyroData);

    // Calcular ángulo a partir del acelerómetro
    // Se tiene en cuenta el offset calculado del acelerómetro
    accel_angle = (atan2(accelData.accelY, -accelData.accelZ) * RAD_TO_DEG) - angle_offset;

    // Corregir el ángulo negativo
    if (accel_angle < 0) accel_angle *= neg_correct;

    // Integrar el giro para obtener el ángulo del giroscopio
    gyro_rate = gyroData.gyroX;

    // Calcular el ángulo utilizando el filtro complementario
    static double prev_angle = 0; // Inicializar ángulo previo
    angle_complementary = filtro_complementario(accel_angle, gyro_rate, prev_angle);
    prev_angle = angle_complementary; // Actualizar ángulo previo
    
    // Debug
    //Serial.printf("Angulo (filtro): %.2f, Angulo (acelerometro): %.2f\n", angle_complementary, accel_angle);
    
    return angle_complementary;
}

// Tarea para leer el sensor
void taskLeerSensor(void *parameter) {
    while (1) {
        double angulo = actualizarAngulo();

        xSemaphoreTake(xMutex, portMAX_DELAY);
        // Actualiza la variable global
        anguloActual = angulo;
        xSemaphoreGive(xMutex);
        
        // Para debug (opcional)
        //Serial.printf("Ángulo: %.2f\n", angulo);
        
        vTaskDelay(pdMS_TO_TICKS(DELAY_ANGLE));
    }
}

// Tarea para el control PID
void taskControlPID(void *parameter) {
    while (1) {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        double angulo = anguloActual;
        xSemaphoreGive(xMutex);

        if (fabs(angulo) < 0.5){
          input = 0;
          myPID.Compute();
          motors.brake();
        } else if (fabs(angulo) > 45){
          motors.brake();
        } else {
          input = angulo;

          myPID.Compute();
          output_filtrada = (output /(double) MAX_PWM_VALUE);
          motors.drive(output_filtrada);
        }

        // Para debug (opcional)
        // Serial.printf("PID output: %.2f\n", output_filtrada);
        
        vTaskDelay(pdMS_TO_TICKS(DELAY_MOTOR)); // 10ms
    }
}

void calibracionOffset(){
    vTaskDelay(pdMS_TO_TICKS(500));
    mpu.update();
    AccelData accelData;
    mpu.getAccel(&accelData);
    // Calcular el punto 0
    angle_offset = atan2(accelData.accelY, -accelData.accelZ) * RAD_TO_DEG;
    vTaskDelay(pdMS_TO_TICKS(500));
}

void setup() {
    pinMode(LED_PIN, OUTPUT);

    // Prender el led para indicar que el sistema está iniciando
    digitalWrite(LED_PIN, HIGH);
    // Inicializar el puerto serie
    Serial.begin(115200);
    while (!Serial) {}
    // Inicializar el I2C
    Wire.begin();

    // Inicializar el MPU6500
    calData calibration;
    mpu.calibrateAccelGyro(&calibration);

    if (mpu.init(calibration, MPU6500_ADDRESS) != 0) {
        Serial.println("Error al inicializar el MPU6500");
        while (1);
    }
    
    // Configurar el rango del acelerómetro
    if (mpu.setAccelRange(2) != 0) {
        Serial.println("Error al configurar el rango del acelerómetro");
    }

    // Configurar el giroscopio (opcional, pero recomendado)
    if (mpu.setGyroRange(250) != 0) {
        Serial.println("Error al configurar el rango del giroscopio");
    }

    // Calibrar el offset del acelerómetro
    // Dejar el robot en posición vertical durante la calibración
    calibracionOffset();

    // Inicializar el PID
    myPID.SetMode(AUTOMATIC);
    myPID.SetOutputLimits(-MAX_PWM_VALUE, MAX_PWM_VALUE); // Limitar la salida
    myPID.SetSampleTime(10);
    
    // Inicializar motores
    motors.begin();

    // Crear mutex para proteger variables compartidas
    xMutex = xSemaphoreCreateMutex();
    
    // Crear tarea para leer sensor en Core 0
    xTaskCreatePinnedToCore(
        taskLeerSensor,    // Función de la tarea
        "SensorTask",      // Nombre
        4096,             // Stack size
        NULL,             // Parámetros
        2,                // Prioridad
        &taskSensor,      // Handle
        0                 // Core
    );
    
    // Crear tarea para control PID en Core 1
    xTaskCreatePinnedToCore(
        taskControlPID,
        "PIDTask",
        4096,
        NULL,
        1,
        &taskPID,
        1
    );

    Serial.println("Sistema iniciado correctamente");
    Serial.printf("ANGLE_OFFSET = %.2f\n", angle_offset);
    

    digitalWrite(LED_PIN, LOW);
}

void loop() {
    // El loop principal queda vacío ya que todo se maneja en las tareas
}