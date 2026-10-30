#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace LTX {

// RecordThread may split each channel at a circular-buffer boundary, or read
// different amounts per channel. Assemble rows by sample order, checking native
// timestamps so a genuinely missing channel sample cannot silently shift rows.
class PositionAssembler {
public:
    using Frame = std::array<float, 7>;
    static constexpr std::size_t maximumPendingRows = 65536;

    void append(int channel, const float* values, const double* timestamps, int count) {
        if (channel < 0 || channel >= int(received.size()) || count < 0)
            throw std::runtime_error("Invalid LTX position channel or sample count");
        if (received[channel] - emitted + std::uint64_t(count) > maximumPendingRows)
            throw std::runtime_error("LTX position channels did not catch up before the pending-row limit");
        for (int i = 0; i < count; ++i) {
            const auto offset = std::size_t(received[channel] - emitted);
            if (!std::isfinite(timestamps[i]))
                throw std::runtime_error("Invalid LTX position native timestamp");
            if (offset == rows.size()) rows.push_back(Row{{}, timestamps[i], 0});
            auto& row = rows.at(offset);
            if (std::abs(timestamps[i] - row.timestamp) > 1e-6) {
                std::ostringstream error;
                error << std::setprecision(17) << "LTX position native timestamps disagree at row " << received[channel]
                      << ", channel " << channel << ": expected " << row.timestamp
                      << ", received " << timestamps[i];
                throw std::runtime_error(error.str());
            }
            row.values[channel] = values[i];
            row.present |= 1u << channel;
            ++received[channel];
        }
    }

    bool pop(Frame& result) {
        if (rows.empty() || rows.front().present != 0x7f) return false;
        result = rows.front().values;
        rows.pop_front();
        ++emitted;
        return true;
    }

    void finish() const {
        if (rows.empty()) return;
        std::ostringstream error;
        error << "LTX position recording ended with " << rows.size() << " incomplete rows; channel sample counts:";
        for (auto count : received) error << ' ' << count;
        throw std::runtime_error(error.str());
    }

private:
    struct Row { Frame values; double timestamp; unsigned int present; };
    std::deque<Row> rows;
    std::array<std::uint64_t, 7> received{};
    std::uint64_t emitted = 0;
};
}
