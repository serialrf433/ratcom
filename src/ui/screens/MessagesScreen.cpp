#include "MessagesScreen.h"
#include "ui/Theme.h"
#include "reticulum/AnnounceManager.h"

void MessagesScreen::onEnter() {
    refreshList();
}

void MessagesScreen::refreshList() {
    if (!_lxmf) return;

    _list.clear();
    _peerHexes.clear();

    const auto& convs = _lxmf->conversations();
    for (const auto& peerHex : convs) {
        // Show node name if known, otherwise formatted hex
        std::string label;
        if (_am) {
            const DiscoveredNode* node = _am->findNodeByHex(peerHex);
            if (node && !node->name.empty()) {
                label = node->name;
            }
        }
        if (label.empty()) {
            if (peerHex.size() >= 8) {
                label = peerHex.substr(0, 4) + ":" + peerHex.substr(4, 4);
            } else {
                label = peerHex;
            }
        }

        int unread = _lxmf->unreadCount(peerHex);
        if (unread > 0) {
            label += " [" + std::to_string(unread) + "]";
        }

        _list.addItem(label);
        _peerHexes.push_back(peerHex);
    }

    if (_list.itemCount() == 0) {
        _list.addItem("No conversations yet");
    }

    _lastRefresh = millis();
    _needsRefresh = false;
}

void MessagesScreen::render(M5Canvas& canvas) {
    // Event-driven refresh instead of timer-based
    if (_needsRefresh) {
        refreshList();
    }

    int y = Theme::CONTENT_Y;

    // Title
    canvas.setTextColor(Theme::SECONDARY);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.drawString("MESSAGES", 4, y + 2);

    // Draw separator
    canvas.drawFastHLine(0, y + 10, Theme::CONTENT_W, Theme::BORDER);

    // Draw conversation list
    _list.render(canvas, 0, y + 12, Theme::CONTENT_W, Theme::CONTENT_H - 14);
}

bool MessagesScreen::handleKey(const KeyEvent& event) {
    // ; = up, . = down on Cardputer Adv keyboard
    if (event.character == ';') {
        _list.scrollUp();
        return true;
    }
    if (event.character == '.') {
        _list.scrollDown();
        return true;
    }
    if (event.enter) {
        int idx = _list.getSelectedIndex();
        if (idx >= 0 && idx < (int)_peerHexes.size() && _openCb) {
            _openCb(_peerHexes[idx]);
        }
        return true;
    }
    return false;
}
