#pragma once
#include <string>

enum LogMode { ET, E, W, I, W2, V, VV };

class Logger {
  public:
	virtual void log(LogMode level, const std::string &msg) = 0;
	virtual void enableNoiseSuppression() {}
	virtual void disableNoiseSuppression() {}
	virtual bool isNoiseSuppressed() const { return false; }
	virtual ~Logger() = default;
};

extern Logger *g_logger;
