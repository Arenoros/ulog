// Smoke-test for the ulog Conan package. Links against the installed
// headers + shared_ptr<NullLogger>, emits one record, exits zero.

#include <memory>

#include <ulog/log.hpp>
#include <ulog/null_logger.hpp>

int main() {
    ulog::SetDefaultLogger(ulog::MakeNullLogger());
    LOG_INFO() << "hello from conan test_package";
    ulog::SetNullDefaultLogger();
    return 0;
}
