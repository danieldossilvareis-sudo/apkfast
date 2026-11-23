// nativeapp/sensor.hpp — Sensores (acelerômetro, giroscópio) via NDK.
//
// As filas são registradas no MESMO looper do App, com callback. O App::run()
// já faz o poll e despacha — este módulo NÃO consome eventos do looper.

#pragma once

#include <android/looper.h>
#include <android/sensor.h>
#include <android/log.h>

#include <array>

namespace nativeapp {

struct Vec3f { float x = 0, y = 0, z = 0; };

class Sensors {
public:
    static Sensors& instance() {
        static Sensors singleton;
        return singleton;
    }

    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;

    bool init() {
        if (initialized_) return true;
        manager_ = ASensorManager_getInstance();
        if (!manager_) return false;

        accelerometer_ = ASensorManager_getDefaultSensor(manager_, ASENSOR_TYPE_ACCELEROMETER);
        gyroscope_ = ASensorManager_getDefaultSensor(manager_, ASENSOR_TYPE_GYROSCOPE);
        if (!accelerometer_ && !gyroscope_) {
            manager_ = nullptr;
            return false;
        }

        // 0 = usa o looper da thread atual (o mesmo do App) com callbacks.
        looper_ = ALooper_prepare(0);
        if (!looper_) {
            accelerometer_ = gyroscope_ = nullptr;
            manager_ = nullptr;
            return false;
        }

        if (accelerometer_) {
            accel_event_queue_ = ASensorManager_createEventQueue(
                manager_, looper_, kAccelId, &Sensors::accel_callback, this);
            if (accel_event_queue_) {
                if (ASensorEventQueue_enableSensor(accel_event_queue_, accelerometer_) < 0 ||
                    ASensorEventQueue_setEventRate(
                        accel_event_queue_, accelerometer_, 1000000 / 60) < 0) {
                    __android_log_print(ANDROID_LOG_WARN, "nativeapp-sensor",
                                        "falha ao configurar acelerometro");
                }
            } else {
                accelerometer_ = nullptr;
            }
        }
        if (gyroscope_) {
            gyro_event_queue_ = ASensorManager_createEventQueue(
                manager_, looper_, kGyroId, &Sensors::gyro_callback, this);
            if (gyro_event_queue_) {
                if (ASensorEventQueue_enableSensor(gyro_event_queue_, gyroscope_) < 0 ||
                    ASensorEventQueue_setEventRate(
                        gyro_event_queue_, gyroscope_, 1000000 / 60) < 0) {
                    __android_log_print(ANDROID_LOG_WARN, "nativeapp-sensor",
                                        "falha ao configurar giroscopio");
                }
            } else {
                gyroscope_ = nullptr;
            }
        }
        if (!accelerometer_ && !gyroscope_) {
            destroy();
            return false;
        }
        initialized_ = true;
        return true;
    }

    /// Chamado automaticamente pelo callback do looper — não precisa chamar.
    void drain() {
        if (accel_event_queue_) {
            ASensorEvent event;
            while (ASensorEventQueue_getEvents(accel_event_queue_, &event, 1) > 0) {
                acceleration_ = {event.vector.x, event.vector.y, event.vector.z};
            }
        }
        if (gyro_event_queue_) {
            ASensorEvent event;
            while (ASensorEventQueue_getEvents(gyro_event_queue_, &event, 1) > 0) {
                angular_velocity_ = {event.vector.x, event.vector.y, event.vector.z};
            }
        }
    }

    /// Compatibilidade: não faz mais poll (evita roubar eventos do App).
    void update() { drain(); }

    Vec3f acceleration() const { return acceleration_; }
    Vec3f angular_velocity() const { return angular_velocity_; }
    bool has_accelerometer() const { return accelerometer_ != nullptr; }
    bool has_gyroscope() const { return gyroscope_ != nullptr; }

    void destroy() {
        if (manager_) {
            if (accel_event_queue_) {
                ASensorManager_destroyEventQueue(manager_, accel_event_queue_);
            }
            if (gyro_event_queue_) {
                ASensorManager_destroyEventQueue(manager_, gyro_event_queue_);
            }
        }
        accel_event_queue_ = nullptr;
        gyro_event_queue_ = nullptr;
        accelerometer_ = nullptr;
        gyroscope_ = nullptr;
        looper_ = nullptr;
        manager_ = nullptr;
        acceleration_ = {};
        angular_velocity_ = {};
        initialized_ = false;
    }

    ~Sensors() { destroy(); }

private:
    Sensors() = default;
    static constexpr int kAccelId = 42;
    static constexpr int kGyroId = 43;

    static int accel_callback(int, int, void* data) {
        static_cast<Sensors*>(data)->drain();
        return 1;
    }
    static int gyro_callback(int, int, void* data) {
        static_cast<Sensors*>(data)->drain();
        return 1;
    }

    ASensorManager* manager_ = nullptr;
    const ASensor* accelerometer_ = nullptr;
    const ASensor* gyroscope_ = nullptr;
    ASensorEventQueue* accel_event_queue_ = nullptr;
    ASensorEventQueue* gyro_event_queue_ = nullptr;
    ALooper* looper_ = nullptr;
    Vec3f acceleration_;
    Vec3f angular_velocity_;
    bool initialized_ = false;
};

}  // namespace nativeapp
