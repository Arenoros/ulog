#include "control/thread_role.hpp"

#include <utility>

namespace ulog::detail::control {
namespace {

thread_local UlogThreadRole current_role = UlogThreadRole::kExternal;

}  // namespace

UlogThreadRole GetUlogThreadRole() noexcept { return current_role; }

ScopedUlogThreadRole::ScopedUlogThreadRole(const UlogThreadRole role) noexcept
    : previous_(std::exchange(current_role, role)) {}

ScopedUlogThreadRole::~ScopedUlogThreadRole() { current_role = previous_; }

}  // namespace ulog::detail::control
