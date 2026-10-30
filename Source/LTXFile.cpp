#include "LTXFile.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace LTX {

LTXFile::LTXFile(const std::string& basePath, const std::string& extension,
                 std::chrono::system_clock::time_point startTime)
    : path(basePath + extension), start(startTime)
{
    file.reset(std::fopen(path.c_str(), "wb"));
    if (!file) ioError("open");
    const std::time_t time = std::chrono::system_clock::to_time_t(start);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) throw std::runtime_error("Cannot format LTX trial date");
#else
    if (!localtime_r(&time, &local)) throw std::runtime_error("Cannot format LTX trial date");
#endif
    char text[80]{};
    std::strftime(text, sizeof(text), "%A, %d %b %Y", &local);
    AddHeaderValue("trial_date", text);
    std::strftime(text, sizeof(text), "%H:%M", &local);
    AddHeaderValue("trial_time", text);
    AddHeaderValue("created_by", "open-ephys-plugin-ltx 1.1.1");
    ReserveHeader("duration", "0");
}

void LTXFile::addHeaderText(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (status != Status::Headers || reservedHeaders.count(key))
        throw std::runtime_error("Cannot add LTX header: " + key);
    writeText((firstHeader ? "" : "\r\n") + key + " " + value);
    firstHeader = false;
}

void LTXFile::ReserveHeader(const std::string& key, const std::string& initial) {
    std::lock_guard<std::mutex> lock(mutex);
    if (status != Status::Headers || reservedHeaders.count(key) || initial.size() > reservedWidth)
        throw std::runtime_error("Cannot reserve LTX header: " + key);
    writeText((firstHeader ? "" : "\r\n") + key + " ");
    reservedHeaders.emplace(key, tell());
    writeText(initial + std::string(reservedWidth - initial.size(), ' '));
    firstHeader = false;
}

void LTXFile::updateHeaderText(const std::string& key, const std::string& value) {
    if (status == Status::Closed || !reservedHeaders.count(key) || value.size() > reservedWidth)
        throw std::runtime_error("Cannot update reserved LTX header: " + key);
    const auto previous = tell();
    seek(reservedHeaders.at(key));
    writeText(value + std::string(reservedWidth - value.size(), ' '));
    seek(previous);
}

void LTXFile::WriteBinaryData(const void* data, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    if (status == Status::Closed) throw std::runtime_error("Cannot write closed LTX file: " + path);
    if (bytes == 0) return;
    if (status == Status::Headers) {
        writeText("\r\ndata_start");
        status = Status::Binary;
    }
    writeBytes(data, bytes);
}

void LTXFile::FinaliseFile(std::chrono::system_clock::time_point end) {
    std::lock_guard<std::mutex> lock(mutex);
    if (status == Status::Closed) return;
    if (status == Status::Binary) writeText("\r\ndata_end");
    updateHeaderText("duration", format(std::chrono::duration_cast<std::chrono::seconds>(end - start).count()));
    if (std::fflush(file.get()) != 0) ioError("flush");
    auto* handle = file.release();
    status = Status::Closed;
    if (std::fclose(handle) != 0) ioError("close");
}

void LTXFile::writeBytes(const void* data, std::size_t bytes) {
    if (std::fwrite(data, 1, bytes, file.get()) != bytes) ioError("write");
}
void LTXFile::writeText(const std::string& text) { writeBytes(text.data(), text.size()); }

LTXFile::Offset LTXFile::tell() const {
#ifdef _WIN32
    const auto result = _ftelli64(file.get());
#else
    const auto result = ftello(file.get());
#endif
    if (result < 0) ioError("tell position in");
    return static_cast<Offset>(result);
}
void LTXFile::seek(Offset offset) {
#ifdef _WIN32
    const int result = _fseeki64(file.get(), offset, SEEK_SET);
#else
    const int result = fseeko(file.get(), static_cast<off_t>(offset), SEEK_SET);
#endif
    if (result != 0) ioError("seek in");
}
void LTXFile::ioError(const std::string& operation) const {
    throw std::runtime_error("Cannot " + operation + " LTX file " + path + ": " + std::strerror(errno));
}
}
