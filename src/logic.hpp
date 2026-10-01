#pragma once
// Pure logic (no Geode/GD deps) so it can be unit tested off-device.
//
// Timing model
//  * L  = level time (seconds) at the moment the game dispatches the input. Every input,
//         live, macro or bot, is dispatched through GJBaseGameLayer::handleButton.
//  * ts = optional click timestamp of the queued command (native Click-Between-Steps).
//  * stepDt = seconds of level time per physics step, measured from the game itself, so
//         TPS bypass / physics bypass are followed automatically.
//  gap (frames) = (L - prevL) / stepDt, refined / replaced by the timestamp gap when the
//  level clock can't resolve it (several inputs dispatched within one display frame).
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace fc {

enum Bucket : int { B_10_15 = 0, B_7_9, B_5_6, B_4, B_3, B_2, B_1, B_CBS, B_OTHER, B_COUNT };

inline int bucketFor(double g) {
    if (!(g > 0.0)) return -1;
    if (g < 0.97) return B_CBS;  // sub-frame: only possible with click-between-steps
    int n = static_cast<int>(std::floor(g + 0.03));
    if (n <= 1) return B_1;
    if (n == 2) return B_2;
    if (n == 3) return B_3;
    if (n == 4) return B_4;
    if (n <= 6) return B_5_6;
    if (n <= 9) return B_7_9;
    if (n <= 15) return B_10_15;
    return B_OTHER;
}

constexpr double INF = std::numeric_limits<double>::infinity();

struct Stream {
    bool has = false;
    bool down = false;
    double L = 0.0;
    double ts = -1.0;  // raw timestamp, -1 = none
};

struct InputResult {
    bool counted = false;
    int bucket = -1;
    double gap = 0.0;
    bool isHold = false;
};

struct Tracker {
    int counts[B_COUNT] = {};
    int session[B_COUNT] = {};
    int total = 0, sessionTotal = 0;
    double tightest = INF, sessionTightest = INF;
    double shortestHold = INF, sessionShortestHold = INF;

    // --- physics step length estimation (level-time seconds per step) ---
    static constexpr int RING = 128;
    float ring[RING] = {};
    int ringN = 0, ringPos = 0, sinceAdopt = 0;
    double stepDt = 1.0 / 240.0;

    // --- timestamp domain: (ts delta) = tsPerLevel * (level-time delta) ---
    // Timestamps are seconds in practice, so assume that until a long gap says otherwise.
    double tsPerLevel = 1.0;
    bool tsLearned = false;

    Stream st[2];

    double tps() const { return 1.0 / stepDt; }

    void resetAttempt() {
        for (int i = 0; i < B_COUNT; i++) { session[i] += counts[i]; counts[i] = 0; }
        sessionTotal += total; total = 0;
        sessionTightest = std::min(sessionTightest, tightest); tightest = INF;
        sessionShortestHold = std::min(sessionShortestHold, shortestHold); shortestHold = INF;
        st[0] = Stream{}; st[1] = Stream{};
    }

    void resetAll() { *this = Tracker{}; }

    // Call with how far the level clock advanced across one physics slice.
    // Un-split steps advance by exactly one step, split ones by less, so the
    // (robust) maximum over a window is the true step length.
    void noteSlice(double dL) {
        if (!(dL > 1.0 / 4000.0 && dL < 1.0 / 20.0)) return;
        ring[ringPos] = static_cast<float>(dL);
        ringPos = (ringPos + 1) % RING;
        if (ringN < RING) ringN++;
        if (++sinceAdopt < 16 || ringN < 16) return;
        sinceAdopt = 0;
        float mx = 0.f;
        for (int i = 0; i < ringN; i++) mx = std::max(mx, ring[i]);
        int near = 0;
        for (int i = 0; i < ringN; i++) if (ring[i] > mx * 0.99f) near++;
        if (near < 3) return;  // lone outlier, don't trust
        double tps = 1.0 / static_cast<double>(mx);
        double r = std::round(tps);
        if (std::abs(tps - r) < 0.02 * r) tps = r;  // snap jitter to an integer TPS
        stepDt = 1.0 / tps;
    }

    static bool plausibleScale(double s) {
        for (double e : {1.0, 1e3, 1e6, 1e9}) {
            if (s >= 0.4 * e && s <= 2.5 * e) return true;
        }
        return false;
    }

    // L: level time now. ts: click timestamp if known. cbsOn: game's CBS setting.
    InputResult onInput(double L, std::optional<double> ts, bool down, bool p1, bool cbsOn) {
        InputResult res;
        Stream& a = st[p1 ? 0 : 1];
        Stream& o = st[p1 ? 1 : 0];
        double rawTs = ts ? *ts : -1.0;

        // backwards clock (new attempt / rewind): restart this stream
        if (a.has && L + 1e-9 < a.L) a = Stream{};

        // game mirrors one click to both players in dual mode -> count once
        bool mirrored = o.has && std::abs(o.L - L) < 1e-7 && o.down == down;

        // a second finger pressing while already held (or releasing while released):
        // the game ignores it, so it is not an input transition
        bool repeat = a.has && a.down == down && (L - a.L) < 1.0;

        if (a.has && !mirrored && !repeat) {
            double dL = L - a.L;
            double gL = dL / stepDt;
            double g = gL;

            bool tsOk = rawTs >= 0.0 && a.ts >= 0.0 && rawTs > a.ts;

            // learn the timestamp scale from long, unambiguous gaps
            if (tsOk && dL >= 8.0 * stepDt) {
                double s = (rawTs - a.ts) / dL;
                if (plausibleScale(s)) {
                    tsPerLevel = tsLearned ? tsPerLevel * 0.8 + s * 0.2 : s;
                    tsLearned = true;
                }
            }

            if (tsOk && tsPerLevel > 0.0) {
                double gT = ((rawTs - a.ts) / tsPerLevel) / stepDt;
                // gT wildly off = wrong unit guess, never trust that
                if (std::abs(gT - gL) <= 64.0) {
                    if (std::abs(gT - gL) > 1.25) g = gT;  // level clock can't resolve it: trust timestamps
                    else if (cbsOn && gT > 0.0) g = gT;    // CBS: sub-step precision from timestamps
                }
            }

            if (g <= 0.0) g = 0.5;  // several inputs, same instant: only CBS can do that

            int b = bucketFor(g);
            if (b >= 0) {
                counts[b]++; total++;
                tightest = std::min(tightest, g);
                res = {true, b, g, a.down && !down};
                if (res.isHold) shortestHold = std::min(shortestHold, g);
            }
        }

        if (!repeat || !a.has) {
            a.has = true; a.down = down; a.L = L; a.ts = rawTs;
        } else if ((L - a.L) >= 1.0) {
            a.down = down; a.L = L; a.ts = rawTs;
        }
        return res;
    }
};

} // namespace fc
