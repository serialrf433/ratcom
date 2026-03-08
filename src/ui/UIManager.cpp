#include "UIManager.h"

void UIManager::begin() {
    _canvas.createSprite(Theme::SCREEN_W, Theme::SCREEN_H);
    _canvas.setColorDepth(16);
    _canvas.fillScreen(Theme::BG);
    _needsRender = true;
    _statusDirty = true;
    _contentDirty = true;
    _tabDirty = true;

    // Wire up dirty flag callbacks
    _statusBar.setDirtyFlag(&_statusDirty);
    _tabBar.setDirtyFlag(&_tabDirty);
}

void UIManager::setScreen(Screen* screen) {
    if (_currentScreen) {
        _currentScreen->onExit();
    }
    _currentScreen = screen;
    if (_currentScreen) {
        _currentScreen->onEnter();
    }
    markAllDirty();
}

void UIManager::render() {
    if (_bootMode) {
        // Boot mode: always render full screen
        _canvas.fillScreen(Theme::BG);
        if (_currentScreen) {
            _currentScreen->render(_canvas);
        }
        flush();
        return;
    }

    // Skip render if nothing changed
    if (!_statusDirty && !_contentDirty && !_tabDirty) return;

    // Full canvas redraw (M5Canvas doesn't support partial push)
    _canvas.fillScreen(Theme::BG);
    _statusBar.render(_canvas);

    if (_currentScreen) {
        _canvas.setClipRect(0, Theme::CONTENT_Y, Theme::CONTENT_W, Theme::CONTENT_H);
        _currentScreen->render(_canvas);
        _canvas.clearClipRect();
    }

    _tabBar.render(_canvas);

    if (_overlay) {
        _canvas.setClipRect(0, Theme::CONTENT_Y, Theme::CONTENT_W, Theme::CONTENT_H);
        _overlay->render(_canvas);
        _canvas.clearClipRect();
    }

    flush();
    _statusDirty = _contentDirty = _tabDirty = false;
}

void UIManager::flush() {
    _canvas.pushSprite(&M5.Display, 0, 0);
}

bool UIManager::handleKey(const KeyEvent& event) {
    markContentDirty();
    if (_currentScreen) {
        return _currentScreen->handleKey(event);
    }
    return false;
}
