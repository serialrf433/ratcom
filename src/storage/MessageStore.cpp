#include "MessageStore.h"
#include "config/Config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>

bool MessageStore::begin(FlashStore* flash, SDStore* sd) {
    _flash = flash;
    _sd = sd;
    _flash->ensureDir(PATH_MESSAGES);

    // Check NVS migration flag — skip expensive migrations on subsequent boots
    Preferences mprefs;
    mprefs.begin("ratcom", true);
    bool migrated = mprefs.getBool("fs_migrated", false);
    mprefs.end();

    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratcom");
        _sd->ensureDir("/ratcom/messages");
        if (!migrated) {
            migrateFlashToSD();
        }
    }

    // One-time migration: rename old-format filenames to new counter-based format
    if (!migrated) {
        migrateOldFilenames();
        Preferences mp;
        mp.begin("ratcom", false);
        mp.putBool("fs_migrated", true);
        mp.end();
        Serial.println("[MSGSTORE] Migration complete, flag set");
    }

    // Start async write queue
    _writeQueue.begin(_sd, _flash);

    // Initialize receive counter from NVS
    initReceiveCounter();

    // Wire up counter for periodic NVS persist by WriteQueue task
    _writeQueue.setCounterRef(&_nextReceiveCounter);

    refreshConversations();
    Serial.printf("[MSGSTORE] %d conversations found, receive counter=%lu\n",
                  (int)_conversations.size(), (unsigned long)_nextReceiveCounter);
    return true;
}

void MessageStore::initReceiveCounter() {
    Preferences prefs;
    prefs.begin("ratcom", true);
    _nextReceiveCounter = prefs.getUInt("msgctr", 0);
    prefs.end();

    if (_nextReceiveCounter > 0) {
        // Trust the persisted counter — skip expensive file scan
        Serial.printf("[MSGSTORE] receive counter=%lu (from NVS)\n",
                      (unsigned long)_nextReceiveCounter);
        return;
    }

    // NVS has no counter — scan files (first boot only)
    uint32_t maxPrefix = 0;

    auto scanDir = [&](File& dir) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = entry.name();
                unsigned long val = strtoul(name.c_str(), nullptr, 10);
                if (val > maxPrefix) maxPrefix = (uint32_t)val;
            }
            entry = dir.openNextFile();
        }
    };

    if (_sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File peerDir = dir.openNextFile();
            while (peerDir) {
                if (peerDir.isDirectory()) {
                    scanDir(peerDir);
                }
                peerDir = dir.openNextFile();
            }
        }
    }

    File dir = LittleFS.open(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File peerDir = dir.openNextFile();
        while (peerDir) {
            if (peerDir.isDirectory()) {
                scanDir(peerDir);
            }
            peerDir = dir.openNextFile();
        }
    }

    // Safety: detect overflow from unmigrated old-format files
    if (maxPrefix > 1000000000) {
        Serial.printf("[MSGSTORE] WARNING: counter overflow detected (%lu) — resetting\n",
                      (unsigned long)maxPrefix);
        maxPrefix = 0;
    }

    _nextReceiveCounter = maxPrefix + 1;

    // Persist initial counter (one-time boot cost, acceptable)
    Preferences p;
    p.begin("ratcom", false);
    p.putUInt("msgctr", _nextReceiveCounter);
    p.end();

    Serial.printf("[MSGSTORE] Initialized receive counter to %lu from existing files\n",
                  (unsigned long)_nextReceiveCounter);
}

void MessageStore::migrateFlashToSD() {
    if (!_sd || !_sd->isReady() || !_flash) return;

    File dir = LittleFS.open(PATH_MESSAGES);
    if (!dir || !dir.isDirectory()) return;

    int migrated = 0;
    File peerDir = dir.openNextFile();
    while (peerDir) {
        if (peerDir.isDirectory()) {
            std::string peerHex = peerDir.name();
            String sdDir = sdConversationDir(peerHex);
            _sd->ensureDir(sdDir.c_str());

            File entry = peerDir.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String sdPath = sdDir + "/" + entry.name();
                    if (!_sd->exists(sdPath.c_str())) {
                        size_t size = entry.size();
                        if (size > 0 && size < 4096) {
                            String json = entry.readString();
                            _sd->writeString(sdPath.c_str(), json);
                            migrated++;
                            yield();
                        }
                    }
                }
                entry = peerDir.openNextFile();
            }

            enforceFlashLimit(peerHex);
        }
        peerDir = dir.openNextFile();
    }

    if (migrated > 0) {
        Serial.printf("[MSGSTORE] Migrated %d messages from flash to SD\n", migrated);
    }
}

void MessageStore::migrateOldFilenames() {
    uint32_t counter = 1;
    int totalMigrated = 0;

    // Detect old-format filenames: no leading zeros, not 13 digits before '_'
    auto isOldFormat = [](const String& name) -> bool {
        if (!name.endsWith("_i.json") && !name.endsWith("_o.json")) return false;
        int up = name.indexOf('_');
        if (up <= 0) return false;
        // New format: exactly 13 zero-padded digits before '_'
        if (up == 13 && name.charAt(0) == '0') return false;
        return true;
    };

    // Process SD conversations
    if (_sd && _sd->isReady()) {
        std::vector<String> peerDirs;
        {
            File dir = _sd->openDir(SD_PATH_MESSAGES);
            if (dir && dir.isDirectory()) {
                File peerDir = dir.openNextFile();
                while (peerDir) {
                    if (peerDir.isDirectory()) {
                        peerDirs.push_back(String(SD_PATH_MESSAGES) + peerDir.name());
                    }
                    peerDir = dir.openNextFile();
                }
            }
        }

        for (auto& peerPath : peerDirs) {
            struct OldFile { String name; unsigned long prefix; };
            std::vector<OldFile> oldFiles;

            File d = _sd->openDir(peerPath.c_str());
            if (!d || !d.isDirectory()) continue;
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (isOldFormat(name)) {
                        unsigned long prefix = strtoul(name.c_str(), nullptr, 10);
                        oldFiles.push_back({name, prefix});
                    }
                }
                entry = d.openNextFile();
            }

            if (oldFiles.empty()) continue;

            // Sort by prefix to maintain chronological order
            std::sort(oldFiles.begin(), oldFiles.end(),
                [](const OldFile& a, const OldFile& b) { return a.prefix < b.prefix; });

            for (auto& f : oldFiles) {
                int up = f.name.indexOf('_');
                String suffix = f.name.substring(up);
                char newName[64];
                snprintf(newName, sizeof(newName), "%013lu%s", (unsigned long)counter, suffix.c_str());
                counter++;

                String oldPath = peerPath + "/" + f.name;
                String newPath = peerPath + "/" + newName;

                String json = _sd->readString(oldPath.c_str());
                if (json.length() > 0) {
                    _sd->writeString(newPath.c_str(), json);
                    _sd->remove(oldPath.c_str());
                    totalMigrated++;
                }
                yield();
            }
        }
    }

    // Process flash conversations
    {
        std::vector<String> peerDirs;
        {
            File dir = LittleFS.open(PATH_MESSAGES);
            if (dir && dir.isDirectory()) {
                File peerDir = dir.openNextFile();
                while (peerDir) {
                    if (peerDir.isDirectory()) {
                        peerDirs.push_back(String(PATH_MESSAGES) + peerDir.name());
                    }
                    peerDir = dir.openNextFile();
                }
            }
        }

        for (auto& peerPath : peerDirs) {
            struct OldFile { String name; unsigned long prefix; };
            std::vector<OldFile> oldFiles;

            File d = LittleFS.open(peerPath);
            if (!d || !d.isDirectory()) continue;
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (isOldFormat(name)) {
                        unsigned long prefix = strtoul(name.c_str(), nullptr, 10);
                        oldFiles.push_back({name, prefix});
                    }
                }
                entry = d.openNextFile();
            }

            if (oldFiles.empty()) continue;

            std::sort(oldFiles.begin(), oldFiles.end(),
                [](const OldFile& a, const OldFile& b) { return a.prefix < b.prefix; });

            for (auto& f : oldFiles) {
                int up = f.name.indexOf('_');
                String suffix = f.name.substring(up);
                char newName[64];
                snprintf(newName, sizeof(newName), "%013lu%s", (unsigned long)counter, suffix.c_str());
                counter++;

                String oldPath = peerPath + "/" + f.name;
                String newPath = peerPath + "/" + newName;

                LittleFS.rename(oldPath, newPath);
                totalMigrated++;
                yield();
            }
        }
    }

    if (totalMigrated > 0) {
        Serial.printf("[MSGSTORE] Migrated %d old filenames to new format\n", totalMigrated);
    }
}

void MessageStore::refreshConversations() {
    _conversations.clear();

    if (_sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    _conversations.push_back(entry.name());
                }
                entry = dir.openNextFile();
            }
        }
    }

    File dir = LittleFS.open(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                std::string name = entry.name();
                bool found = false;
                for (auto& c : _conversations) {
                    if (c == name) { found = true; break; }
                }
                if (!found) _conversations.push_back(name);
            }
            entry = dir.openNextFile();
        }
    }
}

void MessageStore::ensureConvDirs(const std::string& peerHex) {
    if (_ensuredDirs.count(peerHex)) return;

    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        _sd->ensureDir(sdDir.c_str());
    }
    if (_flash) {
        String flashDir = conversationDir(peerHex);
        _flash->ensureDir(flashDir.c_str());
    }
    _ensuredDirs.insert(peerHex);
}

bool MessageStore::saveMessage(const LXMFMessage& msg) {
    if (!_flash) return false;

    std::string peerHex = msg.incoming ?
        msg.sourceHash.toHex() : msg.destHash.toHex();

    // Assign receive counter (NO NVS write — batched by WriteQueue task)
    uint32_t counter = _nextReceiveCounter++;

    // Serialize to JSON (~1ms)
    JsonDocument doc;
    doc["src"] = msg.sourceHash.toHex();
    doc["dst"] = msg.destHash.toHex();
    doc["ts"] = msg.timestamp;
    doc["content"] = msg.content;
    doc["title"] = msg.title;
    doc["incoming"] = msg.incoming;
    doc["status"] = (int)msg.status;
    doc["rcv"] = counter;

    String json;
    serializeJson(doc, json);

    char filename[64];
    snprintf(filename, sizeof(filename), "%013lu_%c.json",
             (unsigned long)counter, msg.incoming ? 'i' : 'o');

    // Ensure conversation directories (cached — first time only)
    ensureConvDirs(peerHex);

    // Build paths
    String sdPath = sdConversationDir(peerHex) + "/" + filename;
    String flashPath = conversationDir(peerHex) + "/" + filename;

    // Enqueue BOTH writes to WriteQueue — fully non-blocking
    if (_sd && _sd->isReady()) {
        _writeQueue.enqueue(sdPath.c_str(), flashPath.c_str(), json, WriteBackend::BOTH);
    } else {
        // No SD — write flash only via queue
        _writeQueue.enqueue(nullptr, flashPath.c_str(), json, WriteBackend::FLASH_ONLY);
    }

    // Add to conversation list if new
    bool found = false;
    for (auto& c : _conversations) {
        if (c == peerHex) { found = true; break; }
    }
    if (!found) _conversations.push_back(peerHex);

    // Limit enforcement deferred to periodic (not per-message)
    unsigned long now = millis();
    if (now - _lastLimitEnforce >= LIMIT_ENFORCE_INTERVAL) {
        _lastLimitEnforce = now;
        enforceLimitsAll();
    }

    return true;
}

void MessageStore::enforceLimitsAll() {
    for (auto& conv : _conversations) {
        enforceFlashLimit(conv);
        enforceSDLimit(conv);
        yield();
    }
}

std::vector<LXMFMessage> MessageStore::loadConversation(const std::string& peerHex, int limit, int offset) const {
    std::vector<LXMFMessage> messages;

    std::vector<String> filenames;
    String sourceDir;
    bool useSD = false;

    if (_sd && _sd->isReady()) {
        sourceDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sourceDir.c_str());
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (!name.startsWith(".")) {
                        filenames.push_back(name);
                    }
                }
                entry = d.openNextFile();
            }
            useSD = true;
        }
    }

    if (!useSD && _flash) {
        sourceDir = conversationDir(peerHex);
        File d = LittleFS.open(sourceDir);
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (!name.startsWith(".")) {
                        filenames.push_back(name);
                    }
                }
                entry = d.openNextFile();
            }
        }
    }

    std::sort(filenames.begin(), filenames.end());

    int total = filenames.size();
    int startIdx = std::max(0, total - limit - offset);
    int endIdx = std::max(0, total - offset);
    if (startIdx >= endIdx) return messages;

    for (int i = startIdx; i < endIdx; i++) {
        String fullPath = sourceDir + "/" + filenames[i];
        String json;

        if (useSD) {
            json = _sd->readString(fullPath.c_str());
        } else {
            json = _flash->readString(fullPath.c_str());
        }

        if (json.length() == 0) continue;

        JsonDocument doc;
        if (deserializeJson(doc, json)) continue;

        LXMFMessage msg;
        std::string srcHex = doc["src"] | "";
        std::string dstHex = doc["dst"] | "";
        if (!srcHex.empty()) {
            msg.sourceHash = RNS::Bytes();
            msg.sourceHash.assignHex(srcHex.c_str());
        }
        if (!dstHex.empty()) {
            msg.destHash = RNS::Bytes();
            msg.destHash.assignHex(dstHex.c_str());
        }
        msg.timestamp = doc["ts"] | 0.0;
        msg.content = doc["content"] | "";
        msg.title = doc["title"] | "";
        msg.incoming = doc["incoming"] | false;
        msg.status = (LXMFStatus)(doc["status"] | 0);
        msg.receiveCounter = doc["rcv"] | (uint32_t)0;

        messages.push_back(msg);
    }

    // Sort chronologically; non-epoch timestamps (uptime-based) sort before epoch
    std::sort(messages.begin(), messages.end(),
        [](const LXMFMessage& a, const LXMFMessage& b) {
            bool aEpoch = a.timestamp > 1700000000;
            bool bEpoch = b.timestamp > 1700000000;
            if (aEpoch != bEpoch) return !aEpoch; // non-epoch sorts before epoch
            return a.timestamp < b.timestamp;
        });

    // Apply read status from .read_ctr file
    uint32_t lastRead = readLastReadCounter(peerHex);
    for (auto& m : messages) {
        if (!m.incoming) {
            m.read = true;
        } else {
            m.read = (m.receiveCounter <= lastRead);
        }
    }

    return messages;
}

int MessageStore::messageCount(const std::string& peerHex) const {
    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            int count = 0;
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (!name.startsWith(".")) count++;
                }
                entry = d.openNextFile();
            }
            return count;
        }
    }

    String dir = conversationDir(peerHex);
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return 0;

    int count = 0;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (!name.startsWith(".")) count++;
        }
        entry = d.openNextFile();
    }
    return count;
}

bool MessageStore::deleteConversation(const std::string& peerHex) {
    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                String path = sdDir + "/" + entry.name();
                entry.close();
                _sd->remove(path.c_str());
                entry = d.openNextFile();
            }
        }
        _sd->removeDir(sdDir.c_str());
    }

    String dir = conversationDir(peerHex);
    File d = LittleFS.open(dir);
    if (d && d.isDirectory()) {
        File entry = d.openNextFile();
        while (entry) {
            String path = String(dir) + "/" + entry.name();
            entry.close();
            LittleFS.remove(path);
            entry = d.openNextFile();
        }
    }
    LittleFS.rmdir(dir);

    _conversations.erase(
        std::remove(_conversations.begin(), _conversations.end(), peerHex),
        _conversations.end());

    _ensuredDirs.erase(peerHex);
    return true;
}

void MessageStore::markConversationRead(const std::string& peerHex) {
    uint32_t counter = _nextReceiveCounter > 0 ? _nextReceiveCounter - 1 : 0;
    String counterStr = String(counter);

    // Ensure dirs are created (cached)
    ensureConvDirs(peerHex);

    String sdPath = sdConversationDir(peerHex) + "/.read_ctr";
    String flashPath = conversationDir(peerHex) + "/.read_ctr";

    // Enqueue both writes — fully non-blocking
    if (_sd && _sd->isReady()) {
        _writeQueue.enqueue(sdPath.c_str(), flashPath.c_str(), counterStr, WriteBackend::BOTH);
    } else {
        _writeQueue.enqueue(nullptr, flashPath.c_str(), counterStr, WriteBackend::FLASH_ONLY);
    }
}

uint32_t MessageStore::readLastReadCounter(const std::string& peerHex) const {
    if (_sd && _sd->isReady()) {
        String sdPath = sdConversationDir(peerHex) + "/.read_ctr";
        String val = _sd->readString(sdPath.c_str());
        if (val.length() > 0) return strtoul(val.c_str(), nullptr, 10);
    }

    if (_flash) {
        String flashPath = conversationDir(peerHex) + "/.read_ctr";
        String val = _flash->readString(flashPath.c_str());
        if (val.length() > 0) return strtoul(val.c_str(), nullptr, 10);
    }

    return 0;
}

int MessageStore::unreadCountForPeer(const std::string& peerHex) const {
    uint32_t lastRead = readLastReadCounter(peerHex);
    int count = 0;

    auto countInDir = [&](File& d) {
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = entry.name();
                if (name.endsWith("_i.json")) {
                    unsigned long counter = strtoul(name.c_str(), nullptr, 10);
                    if (counter > lastRead) count++;
                }
            }
            entry = d.openNextFile();
        }
    };

    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            countInDir(d);
            return count;
        }
    }

    if (_flash) {
        String dir = conversationDir(peerHex);
        File d = LittleFS.open(dir);
        if (d && d.isDirectory()) {
            countInDir(d);
        }
    }

    return count;
}

String MessageStore::conversationDir(const std::string& peerHex) const {
    return String(PATH_MESSAGES) + peerHex.substr(0, 16).c_str();
}

String MessageStore::sdConversationDir(const std::string& peerHex) const {
    return String(SD_PATH_MESSAGES) + peerHex.substr(0, 16).c_str();
}

void MessageStore::enforceFlashLimit(const std::string& peerHex) {
    String dir = conversationDir(peerHex);
    std::vector<String> files;

    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;

    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (!name.startsWith(".")) {
                files.push_back(String(dir) + "/" + name);
            }
        }
        entry = d.openNextFile();
    }

    int limit = (_sd && _sd->isReady()) ? FLASH_MSG_CACHE_LIMIT : RATPUTER_MAX_MESSAGES_PER_CONV;
    if ((int)files.size() <= limit) return;

    std::sort(files.begin(), files.end());

    int excess = files.size() - limit;
    for (int i = 0; i < excess; i++) {
        LittleFS.remove(files[i]);
    }
    Serial.printf("[MSGSTORE] Flash trimmed %d old messages for %s (limit=%d)\n",
                  excess, peerHex.substr(0, 8).c_str(), limit);
}

void MessageStore::enforceSDLimit(const std::string& peerHex) {
    if (!_sd || !_sd->isReady()) return;

    String dir = sdConversationDir(peerHex);
    std::vector<String> files;

    File d = _sd->openDir(dir.c_str());
    if (!d || !d.isDirectory()) return;

    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (!name.startsWith(".")) {
                files.push_back(dir + "/" + name);
            }
        }
        entry = d.openNextFile();
    }

    if ((int)files.size() <= RATPUTER_MAX_MESSAGES_PER_CONV) return;

    std::sort(files.begin(), files.end());

    int excess = files.size() - RATPUTER_MAX_MESSAGES_PER_CONV;
    for (int i = 0; i < excess; i++) {
        _sd->remove(files[i].c_str());
    }
    Serial.printf("[MSGSTORE] SD trimmed %d old messages for %s\n",
                  excess, peerHex.substr(0, 8).c_str());
}
