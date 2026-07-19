#pragma once
#include <stdint.h>

namespace hk::drivers::ps2_keyboard {

void initialize();
void handle_irq();
bool pop_char(char& out);
uint64_t buffered_count();

}
