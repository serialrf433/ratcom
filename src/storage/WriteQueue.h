#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class SDStore;
class FlashStore;

enum class WriteBackend : uint8_t { SD_ONLY, FLASH_ONLY, BOTH };

struct WriteJob {
    char sdPath[128];
    char flashPath[128];
    String data;
    WriteBackend backend;
};

class WriteQueue {
public:
    bool begin(SDStore* sd, FlashStore* flash);

    // Enqueue a write job. For BOTH, provide sdPath and flashPath.
    // For SD_ONLY or FLASH_ONLY, the relevant path field is used.
    bool enqueue(const char* sdPath, const char* flashPath, const String& data, WriteBackend backend);

    // Convenience: single-path enqueue
    bool enqueue(const char* path, const String& data, WriteBackend backend = WriteBackend::SD_ONLY);

    int drainCount() const { return _pending; }
    bool isFull() const;

    // Set a counter value to be periodically persisted to NVS
    void setCounterRef(volatile uint32_t* counter) { _counterRef = counter; }

private:
    static void taskFunc(void* param);
    void processJob(const WriteJob& job);
    void periodicMaintenance();

    QueueHandle_t _queue = nullptr;
    TaskHandle_t _task = nullptr;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    volatile int _pending = 0;
    unsigned long _lastMaintenance = 0;
    volatile uint32_t* _counterRef = nullptr;
    uint32_t _lastPersistedCounter = 0;

    static constexpr int QUEUE_DEPTH = 16;
    static constexpr int TASK_STACK = 8192;
    static constexpr int TASK_PRIORITY = 1;
    static constexpr unsigned long MAINTENANCE_INTERVAL = 30000; // 30s
};
