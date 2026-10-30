#ifndef LTXFILE_HEADER_H
#define LTXFILE_HEADER_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace LTX {

// Headers are followed by optional binary data. Any reserved header (including
// duration and sample counts) may be updated until FinaliseFile closes the file.
class LTXFile {
public:
    LTXFile(const std::string& basePath, const std::string& extension,
            std::chrono::system_clock::time_point start);
    ~LTXFile() = default;

    template <class T> void AddHeaderValue(const std::string& key, const T& value) {
        addHeaderText(key, format(value));
    }
    void ReserveHeader(const std::string& key, const std::string& initial = "unavailable");
    template <class T> void UpdateHeader(const std::string& key, const T& value) {
        std::lock_guard<std::mutex> lock(mutex);
        updateHeaderText(key, format(value));
    }
    void WriteBinaryData(const void* buffer, std::size_t bytes);
    void FinaliseFile(std::chrono::system_clock::time_point end);

private:
    enum class Status { Headers, Binary, Closed };
    static constexpr std::size_t reservedWidth = 32;
    using Offset = std::int64_t;

    template <class T> static std::string format(const T& value) {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(17) << value;
        return text.str();
    }
    void addHeaderText(const std::string& key, const std::string& value);
    void updateHeaderText(const std::string& key, const std::string& value);
    void writeBytes(const void* data, std::size_t bytes);
    void writeText(const std::string& text);
    Offset tell() const;
    void seek(Offset offset);
    [[noreturn]] void ioError(const std::string& operation) const;

    const std::string path;
    const std::chrono::system_clock::time_point start;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
    Status status = Status::Headers;
    bool firstHeader = true;
    std::map<std::string, Offset> reservedHeaders;
    std::mutex mutex;
};
}
#endif
