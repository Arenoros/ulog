#include <ulog/version.hpp>

int main() { return ulog::GetVersion() == ulog::kVersion ? 0 : 1; }
