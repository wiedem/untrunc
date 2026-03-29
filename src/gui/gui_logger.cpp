#include "gui_logger.h"

#include <iostream>
#include <stdexcept>

#include "../logger.h"

class GuiLogger : public Logger {
  public:
	void log(LogMode level, const std::string &msg) override {
		if (level == I)
			std::cout << "Info: ";
		else if (level == W || level == W2)
			std::cout << "Warning: ";
		else if (level <= E)
			std::cout << "Error: ";

		std::cout << msg;

		if (level == ET) throw std::runtime_error(msg);
	}
};

void installGuiLogger() {
	static GuiLogger gui_logger;
	g_logger = &gui_logger;
}
