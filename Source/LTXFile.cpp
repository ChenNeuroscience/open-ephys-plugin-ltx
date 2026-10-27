
#include "LTXFile.h" 
#include "LTXRecordEnginePlugin.h" // only needed for LOG* methods
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <stdexcept>

namespace LTX {
	constexpr const char* data_start_token = "\r\ndata_start";
	constexpr const char* data_end_token = "\r\ndata_end";
	constexpr const char* placeholder_token = "              ";

	LTXFile::LTXFile(const std::string& basePath, const std::string& extension, std::chrono::system_clock::time_point start_tm_):
		start_tm(start_tm_) 
	{
		std::lock_guard<std::mutex> lock(mut);

		std::string fullpath = basePath + extension;

		LOGC("Opening file: ", fullpath);
		
		theFile = fopen(fullpath.c_str(), "wb");
		if (!theFile) throw std::runtime_error("Cannot open LTX file " + fullpath + ": " + std::strerror(errno));

		// headers: trial date and time
		std::time_t start_tm_t = std::chrono::system_clock::to_time_t(start_tm);
        std::tm start_tm_lt = *std::localtime(&start_tm_t); 
		
		char strftime_output[50];
		std::strftime(strftime_output, sizeof(strftime_output), "%A, %d %b %Y", &start_tm_lt);
		fprintf(theFile, "trial_date %s", strftime_output);
		std::strftime(strftime_output, sizeof(strftime_output), "%H:%M", &start_tm_lt);
		fprintf(theFile, "\r\ntrial_time %s", strftime_output);

		// headers: hard-coded values
		fprintf(theFile, "\r\ncreated_by open-ephys-plugin-ltx 1.1.0");

		// headers: placeholder for duration
		fprintf(theFile, "\r\nduration %s", placeholder_token);
		headerOffsetDuration = ftell(theFile) - strlen(placeholder_token);
	}
	
	LTXFile::~LTXFile() {
		std::lock_guard<std::mutex> lock(mut);

		if (status != FileWriteStatus::CLOSED) {
			LOGE("LTXFile destructor called before calling FinaliseFile().");
			fclose(theFile);
		}
	}

	template <typename T>
	void LTXFile::AddHeaderValue(const std::string& key, T value) {
		std::lock_guard<std::mutex> lock(mut);

		if (status != FileWriteStatus::HEADERS) {
			LOGE("AddHeaderValue with status not set to FileWriteStatus::HEADERS (status: ", status, ").");
			return;
		}
		
		// format the value as a string
		std::ostringstream oss;
		oss << value;

		fprintf(theFile, "\r\n%s %s", key.c_str(), oss.str().c_str());
	}
	
	template void LTXFile::AddHeaderValue<int>(const std::string&, int);
	template void LTXFile::AddHeaderValue<char const*>(const std::string&, char const*);
	template void LTXFile::AddHeaderValue<std::string>(const std::string&, std::string);
	template void LTXFile::AddHeaderValue<long long>(const std::string&, long long);
	template void LTXFile::AddHeaderValue<double>(const std::string&, double);

	void LTXFile::AddHeaderPlaceholder(const std::string& key) {
		std::lock_guard<std::mutex> lock(mut);

		if (status != FileWriteStatus::HEADERS) {
			LOGE("AddHeaderPlaceholder with status not set to FileWriteStatus::HEADERS (status: ", status, ").");
			return;
		}
		if (headerOffsetCustom != 0) {
			LOGE("AddHeaderPlaceholder more than once.");
			return;
		}

		fprintf(theFile, "\r\n%s %s", key.c_str(), placeholder_token);
		headerOffsetCustom = ftell(theFile) - strlen(placeholder_token);
	}
	
	template <typename T>
	void LTXFile::FinaliseHeaderPlaceholder(T value) {
		std::lock_guard<std::mutex> lock(mut);

		if (status == FileWriteStatus::HEADERS) {
			status = FileWriteStatus::EPILOGUE;
		} else if (status == FileWriteStatus::BINARY) {
			status = FileWriteStatus::EPILOGUE;
			fprintf(theFile, "%s", data_end_token);
		}

		if (status != FileWriteStatus::EPILOGUE) {
			LOGE("FinaliseHeaderPlaceholder called with bad status: ", status);
			return;
		} else if (headerOffsetCustom == 0) {
			LOGE("FinaliseHeaderPlaceholder either called twice, or without prior to AddHeaderPlaceholder.");
			return;
		}

		fseek(theFile, headerOffsetCustom, SEEK_SET);
		
		std::ostringstream oss;
		oss << value;
		
		// todo: maybe should check the string is less than the length of the placeholder?

		fprintf(theFile, "%s", oss.str().c_str());

		headerOffsetCustom = 0;
	}

	template void LTXFile::FinaliseHeaderPlaceholder<uint64_t>(uint64_t);


	void LTXFile::WriteBinaryData(void* buffer, size_t totalBytes) {
		std::lock_guard<std::mutex> lock(mut);

		if (status == FileWriteStatus::HEADERS) {
			status = FileWriteStatus::BINARY;
			fprintf(theFile, "%s", data_start_token);
		}

		if (status != FileWriteStatus::BINARY) {
			LOGE("WriteBinaryData called with status not set to HEADERS or BINARY (status: ", status, ")");
			return;
		}

		if (fwrite(buffer, 1, totalBytes, theFile) != totalBytes)
			throw std::runtime_error("Failed writing LTX binary data");
	}


	void LTXFile::FinaliseFile(std::chrono::system_clock::time_point end_tm) {
		std::lock_guard<std::mutex> lock(mut);

		if (status == FileWriteStatus::CLOSED) {
			LOGE("FinaliseFile called twice.");
			return;

		}

		if (status == FileWriteStatus::BINARY) {
			fprintf(theFile, "%s", data_end_token);
		}

		status = FileWriteStatus::CLOSED;

		// finalise the duration header
		fseek(theFile, headerOffsetDuration, SEEK_SET);
		std::chrono::seconds durationSeconds = std::chrono::duration_cast<std::chrono::seconds>(end_tm - start_tm);
		fprintf(theFile, "%lld", static_cast<long long>(durationSeconds.count()));

		fclose(theFile);
	}



}
