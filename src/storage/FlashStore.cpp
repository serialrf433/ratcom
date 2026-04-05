#include "FlashStore.h"

// Global LittleFS mutex — prevents cross-task corruption
SemaphoreHandle_t FlashStore::_mutex = nullptr;

SemaphoreHandle_t FlashStore::mutex() {
    return _mutex;
}

// RAII lock guard for the mutex
struct FSLock {
    FSLock(SemaphoreHandle_t m) : _m(m) {
        if (_m) xSemaphoreTake(_m, portMAX_DELAY);
    }
    ~FSLock() {
        if (_m) xSemaphoreGive(_m);
    }
    SemaphoreHandle_t _m;
};

bool FlashStore::begin() {
    // Create mutex before any LittleFS access
    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
    }

    FSLock lock(_mutex);

    if (!LittleFS.begin(true)) {  // true = format if mount fails
        Serial.println("[FLASH] LittleFS mount failed!");
        return false;
    }
    _ready = true;

    // Ensure required directories
    LittleFS.mkdir("/identity");
    LittleFS.mkdir("/transport");
    LittleFS.mkdir("/config");
    LittleFS.mkdir("/contacts");
    LittleFS.mkdir("/messages");

    Serial.printf("[FLASH] LittleFS ready, total=%lu, used=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());

    // Clean orphaned .tmp and .bak files left from interrupted atomic writes
    cleanOrphanedFiles();

    return true;
}

void FlashStore::cleanOrphanedFiles() {
    // Scan top-level directories for orphaned .tmp/.bak files
    static const char* dirs[] = {"/config", "/contacts", "/identity", "/transport"};
    int cleaned = 0;

    for (const char* dirPath : dirs) {
        File dir = LittleFS.open(dirPath);
        if (!dir || !dir.isDirectory()) continue;
        File entry = dir.openNextFile();
        while (entry) {
            String name = entry.name();
            entry.close();
            if (name.endsWith(".tmp") || name.endsWith(".bak")) {
                String fullPath = String(dirPath) + "/" + name;
                LittleFS.remove(fullPath.c_str());
                cleaned++;
            }
            entry = dir.openNextFile();
        }
        dir.close();
    }

    // Also scan message subdirectories
    File msgRoot = LittleFS.open("/messages");
    if (msgRoot && msgRoot.isDirectory()) {
        File peerDir = msgRoot.openNextFile();
        while (peerDir) {
            if (peerDir.isDirectory()) {
                String peerPath = String("/messages/") + peerDir.name();
                peerDir.close();
                File d = LittleFS.open(peerPath);
                if (d && d.isDirectory()) {
                    File entry = d.openNextFile();
                    while (entry) {
                        String name = entry.name();
                        entry.close();
                        if (name.endsWith(".tmp") || name.endsWith(".bak")) {
                            String fullPath = peerPath + "/" + name;
                            LittleFS.remove(fullPath.c_str());
                            cleaned++;
                        }
                        entry = d.openNextFile();
                    }
                }
                d.close();
            } else {
                peerDir.close();
            }
            peerDir = msgRoot.openNextFile();
        }
    }
    msgRoot.close();

    if (cleaned > 0) {
        Serial.printf("[FLASH] Cleaned %d orphaned .tmp/.bak files\n", cleaned);
    }
}

void FlashStore::end() {
    FSLock lock(_mutex);
    LittleFS.end();
    _ready = false;
}

bool FlashStore::ensureDir(const char* path) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    if (LittleFS.exists(path)) return true;
    return LittleFS.mkdir(path);
}

bool FlashStore::exists(const char* path) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    return LittleFS.exists(path);
}

bool FlashStore::remove(const char* path) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    return LittleFS.remove(path);
}

bool FlashStore::removeDir(const char* path) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    return LittleFS.rmdir(path);
}

bool FlashStore::rename(const char* from, const char* to) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    return LittleFS.rename(from, to);
}

File FlashStore::openDir(const char* path) {
    if (!_ready) return File();
    FSLock lock(_mutex);
    return LittleFS.open(path);
}

File FlashStore::openFile(const char* path, const char* mode) {
    if (!_ready) return File();
    FSLock lock(_mutex);
    return LittleFS.open(path, mode);
}

bool FlashStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    if (!_ready) return false;
    // Refuse writes when heap is critically low — prevents OOM-induced LittleFS unmount
    if (ESP.getFreeHeap() < 4096) {
        Serial.printf("[FLASH] Write refused (heap=%lu) — OOM protection: %s\n",
                      (unsigned long)ESP.getFreeHeap(), path);
        return false;
    }
    FSLock lock(_mutex);

    // Step 1: Write to .tmp
    String tmpPath = String(path) + ".tmp";
    String bakPath = String(path) + ".bak";

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) return false;
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    // Step 2: Verify .tmp by reading back
    File verify = LittleFS.open(tmpPath.c_str(), "r");
    if (!verify || verify.size() != len) {
        if (verify) verify.close();
        LittleFS.remove(tmpPath.c_str());
        return false;
    }
    verify.close();

    // Step 3: Rename current to .bak (if exists)
    if (LittleFS.exists(path)) {
        LittleFS.remove(bakPath.c_str());
        LittleFS.rename(path, bakPath.c_str());
    }

    // Step 4: Rename .tmp to primary
    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        // Restore from backup on failure
        if (LittleFS.exists(bakPath.c_str())) {
            LittleFS.rename(bakPath.c_str(), path);
        }
        return false;
    }

    // Step 5: Remove .bak (no longer needed)
    LittleFS.remove(bakPath.c_str());

    return true;
}

bool FlashStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    if (!_ready) return false;
    FSLock lock(_mutex);

    // Try primary first
    File f = LittleFS.open(path, "r");
    if (!f) {
        // Try backup
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return false;
        Serial.printf("[FLASH] Restored from backup: %s\n", path);
    }

    bytesRead = f.readBytes((char*)buffer, maxLen);
    f.close();
    return bytesRead > 0;
}

bool FlashStore::writeString(const char* path, const String& data) {
    return writeAtomic(path, (const uint8_t*)data.c_str(), data.length());
}

bool FlashStore::writeDirect(const char* path, const uint8_t* data, size_t len) {
    if (!_ready) return false;
    if (ESP.getFreeHeap() < 4096) {
        Serial.printf("[FLASH] Write refused (heap=%lu) — OOM protection: %s\n",
                      (unsigned long)ESP.getFreeHeap(), path);
        return false;
    }
    FSLock lock(_mutex);
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    size_t written = f.write(data, len);
    f.flush();
    f.close();
    return written == len;
}

String FlashStore::readString(const char* path) {
    if (!_ready) return "";
    FSLock lock(_mutex);
    File f = LittleFS.open(path, "r");
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return "";
    }
    // Guard against corrupted/huge files exhausting heap
    if (f.size() > 8192) {
        Serial.printf("[FLASH] readString: file too large (%d bytes): %s\n", (int)f.size(), path);
        f.close();
        return "";
    }
    String result = f.readString();
    f.close();
    return result;
}

bool FlashStore::isReady() {
    return _ready;
}

bool FlashStore::format() {
    Serial.println("[FLASH] Formatting LittleFS...");
    {
        FSLock lock(_mutex);
        LittleFS.end();
        _ready = false;
        LittleFS.format();
    }
    // Re-mount with fresh filesystem (begin() takes its own lock)
    return begin();
}
