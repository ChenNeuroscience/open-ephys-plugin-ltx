

#ifndef LTXFILE_HEADER_H
#define LTXFILE_HEADER_H

#include <stdio.h>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <map>
#include <string>

namespace LTX {

    class LTXFile
    {
        /*
          Writing the file goes through various stages:
            You can call AddHeaderValue zero or more times, and at any point within
              that call AddHeaderPlaceholder zero or one times (i.e. only one header can be a palceholder).
            You can next call WriteBinaryData zero or more times.
            You can next call FinaliseHeaderPlaceholder zero or one times.
            You should always finish by calling FinaliseFile exactly once.
          To enforce this order is not violated a status is tacked within the class.

          All these methods are guarded by a mutex, which makes them thread-safe though
          not carefully optimised for multi-threaded use.
        */

    

    public:

        LTXFile(const std::string& basePath, const std::string& extension, std::chrono::system_clock::time_point start_tm);
        ~LTXFile();

        template <typename T>
        void AddHeaderValue(const std::string& key, T value);
        void AddHeaderPlaceholder(const std::string& key);

        // Fixed-width diagnostic headers can be updated after binary writes.
        void AddDeferredHeader(const std::string& key);
        void UpdateDeferredHeader(const std::string& key, const std::string& value);

        template <typename T>
        void FinaliseHeaderPlaceholder(T value);

        void WriteBinaryData(void* buffer, size_t totalBytes);

        void FinaliseFile(std::chrono::system_clock::time_point end_tm);

    private:
        enum FileWriteStatus {
            HEADERS,
            BINARY,
            EPILOGUE,
            CLOSED
        };

        long headerOffsetCustom = 0;
        long headerOffsetDuration = 0;
        std::map<std::string, long> deferredHeaders;
        FILE* theFile = nullptr;
        FileWriteStatus status = FileWriteStatus::HEADERS;
        std::chrono::system_clock::time_point start_tm;

        std::mutex mut;

    };
}



#endif // LTXFILE_HEADER_H
