// Frame Counter - click-gap HUD for Geometry Dash (Geode, Android-compatible).
//
// How it works:
//  * GJBaseGameLayer::handleButton is the single funnel every input goes through:
//    real touches, imported macros and bot replays (xdBot etc.) all end up there.
//  * Gaps are measured on the game's own LEVEL CLOCK (m_gameState.m_levelTime), which
//    stays correct with GD 2.2081's native Click Between Steps (where the step counter
//    does not separate fast taps) and with TPS bypass.
//  * GJBaseGameLayer::processQueuedButtons is hooked to grab click timestamps for
//    sub-frame precision; processCommands is hooked to measure the physics step length.
//  * All gap math lives in logic.hpp (unit tested off-device).

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "logic.hpp"

using namespace geode::prelude;

class FCHud;

namespace {

fc::Tracker g_tr;
std::vector<PlayerButtonCommand> g_pending;

FCHud* g_hud = nullptr;

const char* kNames[fc::B_COUNT] = {"10-15", "7-9", "5-6", "4", "3", "2", "1", "<1 cbs", "16+"};

const ccColor3B kColors[fc::B_COUNT] = {
    {90, 255, 90},   // 10-15
    {170, 255, 80},  // 7-9
    {255, 230, 60},  // 5-6
    {255, 170, 40},  // 4
    {255, 120, 40},  // 3
    {255, 70, 50},   // 2
    {255, 40, 40},   // 1
    {255, 100, 255}, // cbs
    {170, 170, 170}  // 16+
};

std::string fmtFrames(double g) {
    if (std::abs(g - std::round(g)) < 0.005) return fmt::format("{}f", static_cast<int>(std::round(g)));
    return fmt::format("{:.2f}f", g);
}

bool isPlay(GJBaseGameLayer* l) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == l;
}

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    return s.empty() ? std::string("level") : s;
}

} // namespace


namespace {

struct SoundFx { const char* label; const char* file; };
const SoundFx kSfx[] = {
    {"Achievement", "achievement_01.ogg"},
    {"Buy", "buyItem01.ogg"},
    {"Click", "playSound_01.ogg"},
    {"Quit", "quitSound_01.ogg"},
    {"Reward", "reward01.ogg"},
    {"Key", "secretKey.ogg"},
    {"Crystal", "crystal01.ogg"},
};

std::chrono::steady_clock::time_point g_lastSound{};

// true if this input should trigger the sound under the user's chosen rule
bool soundMatches(std::string const& rule, int bucket, double gap) {
    if (rule == "Off" || bucket < 0) return false;
    if (rule == "Any input") return true;
    if (rule == "Sub-frame (CBS)") return bucket == fc::B_CBS;
    if (rule == "10-15") return bucket == fc::B_10_15;
    if (rule == "7-9") return bucket == fc::B_7_9;
    if (rule == "5-6") return bucket == fc::B_5_6;
    if (rule == "4") return bucket == fc::B_4;
    if (rule == "3") return bucket == fc::B_3;
    if (rule == "2") return bucket == fc::B_2;
    if (rule == "1") return bucket == fc::B_1;
    int n = gap < 0.97 ? 0 : static_cast<int>(std::floor(gap + 0.03));
    if (rule == "4 or tighter") return n <= 4;
    if (rule == "3 or tighter") return n <= 3;
    if (rule == "2 or tighter") return n <= 2;
    if (rule == "1 or tighter") return n <= 1;
    return false;
}

void maybePlaySound(int bucket, double gap) {
    auto mod = Mod::get();
    auto rule = mod->getSettingValue<std::string>("sound-rule");
    if (!soundMatches(rule, bucket, gap)) return;

    auto now = std::chrono::steady_clock::now();
    double cd = mod->getSettingValue<double>("sound-cooldown");
    if (std::chrono::duration<double>(now - g_lastSound).count() < cd) return;
    g_lastSound = now;

    float vol = static_cast<float>(mod->getSettingValue<double>("sound-volume"));
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;

    std::error_code ec;
    auto custom = mod->getSettingValue<std::filesystem::path>("sound-file");
    if (!custom.empty() && std::filesystem::is_regular_file(custom, ec)) {
        engine->playEffect(custom.string(), 1.f, 0.f, vol);
        return;
    }

    auto name = mod->getSettingValue<std::string>("sound-effect");
    const char* file = kSfx[0].file;
    for (auto const& fx : kSfx) if (name == fx.label) { file = fx.file; break; }
    engine->playEffect(file, 1.f, 0.f, vol);
}

// everything the HUD layout depends on, polled each frame so changes from the
// settings popup apply instantly
struct HudCfg {
    bool enabled, compact, color, session, tight, hold, tps;
    float scale, opacity;
    bool operator==(HudCfg const& o) const {
        return enabled == o.enabled && compact == o.compact && color == o.color && session == o.session &&
               tight == o.tight && hold == o.hold && tps == o.tps && scale == o.scale && opacity == o.opacity;
    }
};

HudCfg readCfg() {
    auto m = Mod::get();
    HudCfg c{};
    c.enabled = m->getSettingValue<bool>("enabled");
    c.compact = m->getSettingValue<bool>("compact-mode");
    c.color = m->getSettingValue<bool>("color-code");
    c.session = m->getSettingValue<bool>("show-session");
    c.tight = m->getSettingValue<bool>("show-tightest");
    c.hold = m->getSettingValue<bool>("show-hold");
    c.tps = m->getSettingValue<bool>("show-tps");
    c.scale = static_cast<float>(m->getSettingValue<double>("hud-scale"));
    c.opacity = static_cast<float>(m->getSettingValue<double>("hud-opacity"));
    return c;
}

} // namespace

class FCHud : public CCLayer {
protected:
    CCNode* m_root = nullptr;
    CCScale9Sprite* m_bg = nullptr;
    CCLabelBMFont* m_rows[fc::B_COUNT] = {};
    CCLabelBMFont* m_tight = nullptr;
    CCLabelBMFont* m_hold = nullptr;
    CCLabelBMFont* m_tps = nullptr;
    CCLabelBMFont* m_toast = nullptr;
    bool m_dirty = true;
    HudCfg m_cfg{};
    bool m_dragging = false;
    CCPoint m_dragOffset = {0.f, 0.f};

    CCLabelBMFont* makeLabel() {
        auto l = CCLabelBMFont::create("", "bigFont.fnt");
        l->setAnchorPoint({0.f, 1.f});
        m_root->addChild(l, 1);
        return l;
    }

public:
    static FCHud* create() {
        auto ret = new FCHud();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    ~FCHud() override {
        if (g_hud == this) g_hud = nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        this->setID("frame-counter-hud"_spr);

        m_root = CCNode::create();
        this->addChild(m_root);

        m_bg = CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
        m_bg->setAnchorPoint({0.f, 1.f});
        m_bg->setColor({0, 0, 0});
        m_root->addChild(m_bg, 0);

        for (int i = 0; i < fc::B_COUNT; i++) m_rows[i] = makeLabel();
        m_tight = makeLabel();
        m_hold = makeLabel();
        m_tps = makeLabel();

        auto win = CCDirector::get()->getWinSize();
        m_toast = CCLabelBMFont::create("(with cbs)", "bigFont.fnt");
        m_toast->setScale(0.5f);
        m_toast->setColor({255, 120, 255});
        m_toast->setPosition({win.width / 2.f, win.height - 52.f});
        m_toast->setOpacity(0);
        this->addChild(m_toast, 5);

        this->setTouchEnabled(true);
        this->scheduleUpdate();
        this->rebuild();
        this->applyPosition();
        return true;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -700, true);
    }

    void refresh() { m_dirty = true; }

    void update(float) override {
        auto cfg = readCfg();
        if (!(cfg == m_cfg)) m_dirty = true;
        if (m_dirty) {
            m_dirty = false;
            this->rebuild();
        }
    }

    void showCbs() {
        if (!Mod::get()->getSettingValue<bool>("cbs-toast")) return;
        m_toast->stopAllActions();
        m_toast->setOpacity(255);
        m_toast->runAction(CCSequence::create(CCDelayTime::create(0.7f), CCFadeOut::create(0.3f), nullptr));
    }

    void applyPosition() {
        auto win = CCDirector::get()->getWinSize();
        float fx = Mod::get()->getSavedValue<float>("hud-x", 0.008f);
        float fy = Mod::get()->getSavedValue<float>("hud-y", 0.985f);
        m_root->setPosition(this->clamp({fx * win.width, fy * win.height}));
    }

    CCPoint clamp(CCPoint p) {
        auto win = CCDirector::get()->getWinSize();
        float s = m_root->getScale();
        auto sz = m_bg->getContentSize();
        float w = sz.width * s, h = sz.height * s;
        p.x = std::max(0.f, std::min(p.x, win.width - w));
        p.y = std::max(h, std::min(p.y, win.height));
        return p;
    }

    void rebuild() {
        m_cfg = readCfg();
        bool compact = m_cfg.compact;
        bool colorCode = m_cfg.color;
        bool showSession = m_cfg.session;
        bool showTight = m_cfg.tight && !compact;
        bool showHold = m_cfg.hold && !compact;
        bool showTps = m_cfg.tps && !compact;
        float scale = m_cfg.scale;
        float opacity = m_cfg.opacity;
        GLubyte textOp = static_cast<GLubyte>(255.f * opacity);

        m_root->setScale(scale);
        m_root->setVisible(m_cfg.enabled);

        float fontScale = compact ? 0.30f : 0.36f;
        float pad = compact ? 4.f : 6.f;
        float gap = compact ? 1.f : 2.f;

        float y = -pad;
        float maxW = 0.f;

        auto place = [&](CCLabelBMFont* l, bool visible) {
            l->setVisible(visible);
            if (!visible) return;
            l->setScale(fontScale);
            l->setOpacity(textOp);
            l->setPosition({pad, y});
            auto cs = l->getContentSize();
            y -= cs.height * fontScale + gap;
            maxW = std::max(maxW, cs.width * fontScale);
        };

        for (int b = 0; b < fc::B_COUNT; b++) {
            bool extra = (b == fc::B_CBS || b == fc::B_OTHER);
            bool vis = true;
            if (extra && compact && g_tr.counts[b] == 0 && g_tr.session[b] + g_tr.counts[b] == 0) vis = false;
            std::string txt = fmt::format("{}: {}", kNames[b], g_tr.counts[b]);
            if (showSession) txt += fmt::format(" ({})", g_tr.session[b] + g_tr.counts[b]);
            m_rows[b]->setString(txt.c_str());
            m_rows[b]->setColor(colorCode ? kColors[b] : ccColor3B{255, 255, 255});
            place(m_rows[b], vis);
        }

        ccColor3B dim = {200, 200, 200};
        bool hasTight = g_tr.tightest != fc::INF;
        m_tight->setString(hasTight ? fmt::format("best gap: {}", fmtFrames(g_tr.tightest)).c_str() : "best gap: -");
        m_tight->setColor(dim);
        place(m_tight, showTight);

        bool hasHold = g_tr.shortestHold != fc::INF;
        m_hold->setString(hasHold ? fmt::format("min hold: {}", fmtFrames(g_tr.shortestHold)).c_str() : "min hold: -");
        m_hold->setColor(dim);
        place(m_hold, showHold);

        m_tps->setString(fmt::format("{:.0f} TPS", g_tr.tps()).c_str());
        m_tps->setColor(dim);
        place(m_tps, showTps);

        float totalH = -y + pad - gap;
        m_bg->setContentSize({maxW + pad * 2.f, totalH});
        m_bg->setPosition({0.f, 0.f});
        m_bg->setOpacity(static_cast<GLubyte>(255.f * opacity * 0.55f));

        this->applyPosition();
    }

    // dragging: only while the pause menu is open, so it never eats gameplay taps
    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!m_root || !m_root->isVisible()) return false;
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene || !scene->getChildByType<PauseLayer>(0)) return false;
        auto p = m_root->convertToNodeSpace(t->getLocation());
        auto sz = m_bg->getContentSize();
        CCRect r(0.f, -sz.height, sz.width, sz.height);
        if (!r.containsPoint(p)) return false;
        m_dragging = true;
        m_dragOffset = m_root->getPosition() - this->convertToNodeSpace(t->getLocation());
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (!m_dragging) return;
        m_root->setPosition(this->clamp(this->convertToNodeSpace(t->getLocation()) + m_dragOffset));
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { this->finishDrag(); }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { this->finishDrag(); }

    void finishDrag() {
        if (!m_dragging) return;
        m_dragging = false;
        auto win = CCDirector::get()->getWinSize();
        auto p = m_root->getPosition();
        Mod::get()->setSavedValue<float>("hud-x", p.x / win.width);
        Mod::get()->setSavedValue<float>("hud-y", p.y / win.height);
    }
};

namespace {

// Appends this level's stats (session + current attempt) to <save dir>/stats/<level>.txt
void saveStats(GJGameLevel* level) {
    if (!level || !Mod::get()->getSettingValue<bool>("save-stats")) return;

    int counts[fc::B_COUNT];
    int total = g_tr.sessionTotal + g_tr.total;
    for (int i = 0; i < fc::B_COUNT; i++) counts[i] = g_tr.session[i] + g_tr.counts[i];
    if (total <= 0) return;

    std::string name = level->m_levelName;
    int id = level->m_levelID.value();
    double best = std::min(g_tr.sessionTightest, g_tr.tightest);
    double hold = std::min(g_tr.sessionShortestHold, g_tr.shortestHold);

    std::error_code ec;
    auto dir = Mod::get()->getSaveDir() / "stats";
    std::filesystem::create_directories(dir, ec);
    auto file = dir / (sanitize(name) + "-" + std::to_string(id) + ".txt");

    std::ofstream out(file, std::ios::app);
    if (!out) return;

    auto now = std::time(nullptr);
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    out << "=== " << name << " (" << id << ") - " << buf << " ===\n";
    for (int i = 0; i < fc::B_COUNT; i++) out << kNames[i] << ": " << counts[i] << "\n";
    out << "total inputs counted: " << total << "\n";
    if (best != fc::INF) out << "tightest gap: " << fmtFrames(best) << "\n";
    if (hold != fc::INF) out << "shortest hold: " << fmtFrames(hold) << "\n";
    out << "\n";
}

} // namespace

class $modify(FCGameLayer, GJBaseGameLayer) {
    // Measures how far the level clock moves per physics slice -> physics step length
    // (follows TPS bypass automatically).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g_hud || !isPlay(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        double l0 = m_gameState.m_levelTime;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_tr.noteSlice(m_gameState.m_levelTime - l0);
    }

    // Snapshot the queued commands so handleButton can look up their click timestamps.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (g_hud && isPlay(this)) {
            g_pending.assign(m_queuedButtons.begin(), m_queuedButtons.end());
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        g_pending.clear();
    }

    // Every input (touch, macro, bot) lands here.
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!g_hud || button != 1 || !isPlay(this)) return;
        if (m_player1 && m_player1->m_isDead) return;

        // click timestamp of this command (queued/live inputs and CBS macros have one;
        // bots that call handleButton directly just use the level clock)
        std::optional<double> ts;
        auto it = g_pending.end();
        for (auto p = g_pending.begin(); p != g_pending.end(); ++p) {
            if (static_cast<int>(p->m_button) != button || p->m_isPush != down) continue;
            if (p->m_isPlayer2 == !isPlayer1) { it = p; break; }
            if (it == g_pending.end()) it = p;  // same button+direction, player flag mismatch: keep as fallback
        }
        if (it != g_pending.end()) {
            ts = it->m_timestamp;
            g_pending.erase(it);
        }

        auto r = g_tr.onInput(m_gameState.m_levelTime, ts, down, isPlayer1, m_clickBetweenSteps);
        if (r.counted) {
            g_hud->refresh();
            if (r.bucket == fc::B_CBS) g_hud->showCbs();
            maybePlaySound(r.bucket, r.gap);
        }
    }
};

class $modify(FCPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_tr.resetAll();
        g_pending.clear();
        g_hud = nullptr;

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        if (mod->getSettingValue<bool>("reset-position")) {
            mod->setSavedValue<float>("hud-x", 0.008f);
            mod->setSavedValue<float>("hud-y", 0.985f);
            mod->setSettingValue<bool>("reset-position", false);
        }

        if (auto hud = FCHud::create()) {
            g_hud = hud;
            this->addChild(hud, 1000);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tr.resetAttempt();
        g_pending.clear();
        if (g_hud) g_hud->refresh();
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (!g_hud) return;

        saveStats(m_level);

        if (Mod::get()->getSettingValue<bool>("summary-on-complete") && !m_isPracticeMode && !m_isTestMode) {
            std::string txt;
            for (int i = 0; i < fc::B_COUNT; i++) {
                if (i >= fc::B_CBS && g_tr.counts[i] == 0) continue;
                txt += fmt::format("{}: {}\n", kNames[i], g_tr.counts[i]);
            }
            if (g_tr.tightest != fc::INF) txt += fmt::format("best gap: {}\n", fmtFrames(g_tr.tightest));
            if (g_tr.shortestHold != fc::INF) txt += fmt::format("min hold: {}\n", fmtFrames(g_tr.shortestHold));
            txt += fmt::format("inputs counted: {}", g_tr.total);
            FLAlertLayer::create("Frame Counter", txt, "OK")->show();
        }
    }

    void onQuit() {
        if (g_hud) saveStats(m_level);
        PlayLayer::onQuit();
    }
};

// Pause menu button ("FC") -> opens this mod's settings (size, opacity, sound, ...).
class $modify(FCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("FC");
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FCPauseLayer::onFCSettings));
        btn->setID("frame-counter-settings-btn"_spr);

        auto menu = CCMenu::create();
        menu->setID("frame-counter-menu"_spr);
        menu->setPosition({0.f, 0.f});
        btn->setPosition({win.width - 38.f, 34.f});
        menu->addChild(btn);
        this->addChild(menu, 10);
    }

    void onFCSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};
