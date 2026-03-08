#pragma once

#include <map>
#include "ui/Screen.h"
#include "ui/widgets/ScrollList.h"
#include "reticulum/LXMFManager.h"

class AnnounceManager;

class MessagesScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Messages"; }
    void onEnter() override;

    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }

    // Callback to open a conversation
    using OpenConversationCb = std::function<void(const std::string& peerHex)>;
    void setOpenCallback(OpenConversationCb cb) { _openCb = cb; }

    void notifyNewMessage() { _needsRefresh = true; }

private:
    void refreshList();

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    ScrollList _list;
    std::vector<std::string> _peerHexes;  // Parallel to list items
    unsigned long _lastRefresh = 0;
    OpenConversationCb _openCb;
    bool _needsRefresh = false;
};
