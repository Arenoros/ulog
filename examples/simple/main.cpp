#include <ulog/log.hpp>

int main() {
    LOG_INFO() << "hello from ulog";
    LOG_WARNING() << "value=" << 42;
    return 0;
}
