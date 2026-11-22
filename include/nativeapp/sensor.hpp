// nativeapp/sensor.hpp — Sensores (acelerômetro, giroscópio) via NDK.

#pragma once

#include <android/looper.h>
#include <android/sensor.h>
#include <android/log.h>

#include <array>
#include <cstring>

namespace nativeapp {

struct Vec3f { float x = 0, y = 0, z = 0; };

class Sensors {
public:
    static Sensors& instance() {
        static Sensors singleton;
        return singleton;
    }

    bool init() {
        if (initialized_) return true;
        manager_ = ASensorManager_getInstance();
        if (!manager_) return false;

        accelerometer_ = ASensorManager_getDefaultSensor(manager_, ASENSOR_TYPE_ACCELEROMETER);
        gyroscope_ = ASensorManager_getDefaultSensor(manager_, ASENSOR_TYPE_GYROSCOPE);
        if (!accelerometer_ && !gyroscope_) return false;

        looper_ = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        if (!looper_) return false;

        if (accelerometer_) {
            accel_event_queue_ = ASensorManager_createEventQueue(
                manager_, looper_, kLooperId, nullptr, nullptr);
            ASensorEventQueue_enableSensor(accel_event_queue_, accelerometer_);
            ASensorEventQueue_setEventRate(
                accel_event_queue_, accelerometer_, 1000000 / 60); // 60 Hz
        }
        if (gyroscope_) {
            gyro_event_queue_ = ASensorManager_createEventQueue(
                manager_, looper_, kLooperId + 1, nullptr, nullptr);
            ASensorEventQueue_enableSensor(gyro_event_queue_, gyroscope_);
            ASensorEventQueue_setEventRate(
                gyro_event_queue_, gyroscope_, 1000000 / 60);
        }
        initialized_ = true;
        return true;
    }

    /// Chamado a cada frame (ou no on_tick do App).
    void update() {
        if (!initialized_) return;
        ALooper_pollOnce(0, nullptr, nullptr, nullptr);

        if (accel_event_queue_) {
            ASensorEvent event;
            while (ASensorEventQueue_getEvents(accel_event_queue_, &event, 1) > 0) {
                // ASENSOR_TYPE_ACCELEROMETER usa vector (x,y,z) com escala do sensor.
                const float scale = ASensor_getResolution(accelerometer_) > 0
                    ? 1.0f / ASensor_getResolution(accelerometer_) : 1.0f;
                (void)scale;
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

    Vec3f acceleration() const { return acceleration_; }
    Vec3f angular_velocity() const { return angular_velocity_; }
    bool has_accelerometer() const { return accelerometer_ != nullptr; }
    bool has_gyroscope() const { return gyroscope_ != nullptr; }

    void destroy() {
        if (!initialized_) return;
        if (accel_event_queue_) {
            ASensorManager_destroyEventQueue(manager_, accel_event_queue_);
            accel_event_queue_ = nullptr;
        }
        if (gyro_event_queue_) {
            ASensorManager_destroyEventQueue(manager_, gyro_event_queue_);
            gyro_event_queue_ = nullptr;
        }
        initialized_ = false;
    }

private:
    Sensors() = default;
    static constexpr int kLooperId = 42;

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
