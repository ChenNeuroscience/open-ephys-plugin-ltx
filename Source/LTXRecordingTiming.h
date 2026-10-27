#ifndef LTX_RECORDING_TIMING_H
#define LTX_RECORDING_TIMING_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace LTX::Timing {

    constexpr double warningThresholdMs = 50.0;
    constexpr double maximumPaddingSeconds = 600.0;

    inline std::uint64_t paddingSamples(double seconds, double rate) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > maximumPaddingSeconds
            || !std::isfinite(rate) || rate <= 0)
            throw std::runtime_error("Invalid or excessive LTX startup padding");
        return static_cast<std::uint64_t>(std::llround(seconds * rate));
    }

    struct Snapshot {
        std::map<int, std::optional<std::int64_t>> starts;
        std::map<int, std::string> modes;
        std::set<int> inactive;
        std::optional<std::int64_t> epochMs;
        std::string error;

        double delaySeconds(int nodeId) const {
            if (!epochMs || !error.empty() || !starts.at(nodeId))
                throw std::runtime_error("LTX recording origins are not available");
            return double(*starts.at(nodeId) - *epochMs) / 1000.0;
        }
    };

    // All LTX instances in one loaded plugin share this coordinator. Membership
    // is fixed before choosing an epoch; callback arrival order is irrelevant.
    class Session {
    public:
        explicit Session(const std::set<int>& expected) {
            for (int id : expected) state.starts.emplace(id, std::nullopt);
        }

        void claim(int id, const std::string& mode) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!state.starts.count(id) || !claimed.insert(id).second)
                throw std::runtime_error("Duplicate or unexpected LTX recording participant");
            state.modes[id] = mode;
        }

        void publish(int id, std::int64_t milliseconds) {
            std::lock_guard<std::mutex> lock(mutex);
            if (milliseconds <= 0 || state.inactive.count(id) || state.starts.at(id)) {
                failLocked("Invalid or duplicate LTX software-time message");
                return;
            }
            state.starts.at(id) = milliseconds;
            resolveLocked();
        }

        void exclude(int id) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!state.starts.at(id)) state.inactive.insert(id);
            resolveLocked();
        }

        void fail(const std::string& message) {
            std::lock_guard<std::mutex> lock(mutex);
            failLocked(message);
        }

        Snapshot snapshot() const {
            std::lock_guard<std::mutex> lock(mutex);
            return state;
        }

        bool hasFailed() const { return failed.load(std::memory_order_relaxed); }

        bool waitForOrigins(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, timeout, [&] {
                return state.epochMs.has_value() || !state.error.empty();
            });
        }

        std::vector<std::pair<int, double>> takeWarnings() {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<std::pair<int, double>> warnings;
            if (announced || !state.epochMs || !state.error.empty()) return warnings;
            announced = true;
            for (const auto& entry : state.starts) {
                if (entry.second) {
                    double delayMs = double(*entry.second - *state.epochMs);
                    if (delayMs > warningThresholdMs) warnings.emplace_back(entry.first, delayMs);
                }
            }
            return warnings;
        }

    private:
        void failLocked(const std::string& message) {
            if (state.error.empty()) state.error = message;
            failed.store(true, std::memory_order_relaxed);
            changed.notify_all();
        }

        void resolveLocked() {
            if (state.epochMs || !state.error.empty()) return;
            std::optional<std::int64_t> earliest;
            std::optional<std::int64_t> latest;
            for (const auto& entry : state.starts) {
                if (!entry.second && !state.inactive.count(entry.first)) return;
                if (entry.second && (!earliest || *entry.second < *earliest)) earliest = entry.second;
                if (entry.second && (!latest || *entry.second > *latest)) latest = entry.second;
            }
            if (earliest && double(*latest - *earliest) / 1000.0 > maximumPaddingSeconds) {
                failLocked("LTX recording origins differ by more than ten minutes");
                return;
            }
            state.epochMs = earliest;
            changed.notify_all();
        }

        mutable std::mutex mutex;
        std::condition_variable changed;
        std::set<int> claimed;
        Snapshot state;
        bool announced = false;
        std::atomic<bool> failed{false};
    };

    inline std::shared_ptr<Session> join(const std::string& recordingKey,
                                       const std::set<int>& expected,
                                       int nodeId, const std::string& mode) {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<Session>> sessions;
        std::lock_guard<std::mutex> lock(mutex);
        // Retain only live sessions, including an earlier recording still closing.
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second.expired()) it = sessions.erase(it);
            else ++it;
        }
        std::string key = recordingKey;
        for (int id : expected) key += ":" + std::to_string(id);
        auto session = sessions[key].lock();
        if (!session) {
            session = std::make_shared<Session>(expected);
            sessions[key] = session;
        }
        session->claim(nodeId, mode);
        return session;
    }

}
#endif
