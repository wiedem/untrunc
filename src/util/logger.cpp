#include "logger.h"

#include <iostream>
#include <sstream>

class ConsoleLogger : public Logger {
	std::stringstream noise_buffer_;
	std::streambuf *orig_cout_ = nullptr, *orig_cerr_ = nullptr;
	bool active_ = false;
	int64_t total_omitted_ = 0;

	void cutNoise(bool force = false) {
		if (noise_buffer_.tellp() < 1 << 16 && !force) return;
		auto s = noise_buffer_.str();
		auto off = std::max(0LL, (long long)s.size() - (1 << 11));
		s = s.substr(off);
		total_omitted_ += off;
		noise_buffer_.str(s);
		noise_buffer_.seekp(0, std::ios::end);
	}

  public:
	void log(LogMode level, const std::string &msg) override {
		if (level == I)
			std::cout << "Info: ";
		else if (level == W || level == W2)
			std::cout << "Warning: ";
		else if (level <= E)
			std::cout << "Error: ";

		std::cout << msg;

		if (active_) cutNoise();

		if (level == ET) exit(1);
	}

	void enableNoiseSuppression() override {
		orig_cout_ = std::cout.rdbuf(noise_buffer_.rdbuf());
		orig_cerr_ = std::cerr.rdbuf(noise_buffer_.rdbuf());
		active_ = true;
	}

	void disableNoiseSuppression() override {
		if (!active_) return;
		std::cout.rdbuf(orig_cout_);
		std::cerr.rdbuf(orig_cerr_);
		active_ = false;

		cutNoise(true);
		auto s = noise_buffer_.str();
		auto off = s.find_first_of('\n');
		if (off != std::string::npos) s = s.substr(off);
		if (total_omitted_) {
			std::cout << "[[ " << total_omitted_ << " bytes omitted, next " << s.size() << " bytes were buffered ]]\n";
		}
		std::cout << s;
		noise_buffer_.str("");
		total_omitted_ = 0;
	}

	bool isNoiseSuppressed() const override { return active_; }
};

static ConsoleLogger console_logger;
Logger *g_logger = &console_logger;
