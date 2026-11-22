// nativeapp/permission_types.hpp — Tipos compartilhados de permissões.

#pragma once

#include <string>
#include <vector>

namespace nativeapp {

enum class PermissionState {
    Unknown,
    Granted,
    Denied,
    PermanentlyDenied
};

struct PermissionResult {
    bool all_granted = false;
    std::vector<std::string> granted;
    std::vector<std::string> denied;
    bool should_show_rationale = false;
    bool permanently_denied = false;

    bool empty() const { return granted.empty() && denied.empty(); }
};

}  // namespace nativeapp
