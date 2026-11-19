// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <type_traits>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/link_stats.h"
#include "video_core/renderer_vulkan/vk_pcie_link.h"

#ifdef _WIN32
#include <windows.h>

#include <cfgmgr32.h>
#include <devpropdef.h>
#endif

namespace Vulkan {

#ifdef _WIN32
namespace {

// pciprop.h's DEVPKEY_PciDevice_* (the same GUID, ids 9 to 12) and devpkey.h's DEVPKEY_NAME, spelled
// out so that nothing has to define INITGUID here.
constexpr GUID PciGuid{0x3AB22E31, 0x8264, 0x4B4E, {0x9A, 0xF5, 0xA8, 0xD2, 0xD8, 0xE3, 0x3E, 0x62}};
constexpr GUID NameGuid{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
const DEVPROPKEY KeyCurrentSpeed{PciGuid, 9};
const DEVPROPKEY KeyCurrentWidth{PciGuid, 10};
const DEVPROPKEY KeyMaxSpeed{PciGuid, 11};
const DEVPROPKEY KeyMaxWidth{PciGuid, 12};
const DEVPROPKEY KeyName{NameGuid, 10};

constexpr u32 MaxHops = 16;
/// A Thunderbolt 3 or 4 port's PCI Express tunnel, whatever its virtual link reports.
constexpr u32 TunnelMbPerS = 2750;

[[nodiscard]] u32 LaneMbPerS(u32 speed_code) {
    static constexpr std::array<u32, 7> Rates{0, 250, 500, 985, 1969, 3938, 7563};
    return speed_code < Rates.size() ? Rates[speed_code] : 0;
}

/// cfgmgr32 taken at run time: no new import for a diagnostic.
struct ConfigManager {
    decltype(&CM_Get_Device_ID_List_SizeW) list_size{};
    decltype(&CM_Get_Device_ID_ListW) list{};
    decltype(&CM_Locate_DevNodeW) locate{};
    decltype(&CM_Get_Parent) parent{};
    decltype(&CM_Get_Device_IDW) device_id{};
    decltype(&CM_Get_DevNode_PropertyW) property{};

    [[nodiscard]] bool Load() {
        const HMODULE module = LoadLibraryW(L"cfgmgr32.dll");
        if (module == nullptr) {
            return false;
        }
        const auto get = [module](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(module, name)));
            return function != nullptr;
        };
        return get(list_size, "CM_Get_Device_ID_List_SizeW") &&
               get(list, "CM_Get_Device_ID_ListW") && get(locate, "CM_Locate_DevNodeW") &&
               get(parent, "CM_Get_Parent") && get(device_id, "CM_Get_Device_IDW") &&
               get(property, "CM_Get_DevNode_PropertyW");
    }

    [[nodiscard]] u32 Number(DEVINST node, const DEVPROPKEY& key) const {
        DEVPROPTYPE type{};
        UINT32 value{};
        ULONG size = sizeof(value);
        if (property(node, &key, &type, reinterpret_cast<PBYTE>(&value), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_UINT32) {
            return 0;
        }
        return value;
    }

    [[nodiscard]] std::wstring Name(DEVINST node) const {
        DEVPROPTYPE type{};
        std::array<wchar_t, 256> text{};
        ULONG size = static_cast<ULONG>(sizeof(wchar_t) * (text.size() - 1));
        if (property(node, &KeyName, &type, reinterpret_cast<PBYTE>(text.data()), &size, 0) !=
                CR_SUCCESS ||
            type != DEVPROP_TYPE_STRING) {
            return {};
        }
        return text.data();
    }

    [[nodiscard]] bool IsPci(DEVINST node) const {
        std::array<wchar_t, MAX_DEVICE_ID_LEN + 1> id{};
        return device_id(node, id.data(), MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
               std::wcsncmp(id.data(), L"PCI\\", 4) == 0;
    }
};

[[nodiscard]] std::string Narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t c : text) {
        result.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    }
    return result;
}

} // Anonymous namespace

PcieLink ProbePcieLink(u32 vendor_id, u32 device_id) {
    PcieLink best;
    ConfigManager cm;
    if (!cm.Load()) {
        return best;
    }
    ULONG chars = 0;
    if (cm.list_size(&chars, L"PCI", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || chars == 0) {
        return best;
    }
    std::vector<wchar_t> ids(chars);
    if (cm.list(L"PCI", ids.data(), chars, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) {
        return best;
    }
    std::array<wchar_t, 32> prefix{};
    swprintf(prefix.data(), prefix.size(), L"PCI\\VEN_%04X&DEV_%04X", vendor_id & 0xFFFF,
             device_id & 0xFFFF);
    const size_t prefix_length = std::wcslen(prefix.data());
    for (const wchar_t* id = ids.data(); *id != L'\0'; id += std::wcslen(id) + 1) {
        if (_wcsnicmp(id, prefix.data(), prefix_length) != 0) {
            continue;
        }
        DEVINST node{};
        if (cm.locate(&node, const_cast<DEVINSTID_W>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue; // not present now
        }
        ++best.devices;
        PcieLink link;
        for (u32 hop = 0; hop < MaxHops && cm.IsPci(node); ++hop) {
            // Idle links drop their speed to save power: the top speed, at the width in use.
            const u32 max_speed = cm.Number(node, KeyMaxSpeed);
            const u32 speed = max_speed != 0 ? max_speed : cm.Number(node, KeyCurrentSpeed);
            const u32 max_width = cm.Number(node, KeyMaxWidth);
            u32 width = cm.Number(node, KeyCurrentWidth);
            width = width != 0 ? width : max_width;
            if (max_width != 0) {
                width = (std::min)(width, max_width);
            }
            const std::wstring name = cm.Name(node);
            const bool tunnel = name.find(L"Thunderbolt") != std::wstring::npos ||
                                name.find(L"USB4") != std::wstring::npos;
            link.tunnelled = link.tunnelled || tunnel;
            u32 rate = LaneMbPerS(speed) * width;
            if (rate != 0) {
                if (tunnel) {
                    rate = (std::min)(rate, TunnelMbPerS);
                }
                ++link.links_read;
                if (hop == 0) {
                    link.own_mb_per_s = rate;
                }
                if (!link.known || rate < link.mb_per_s) {
                    link.known = true;
                    link.mb_per_s = rate;
                    link.speed_code = speed;
                    link.lanes = width;
                    link.narrowest = Narrow(name);
                }
            }
            DEVINST parent{};
            if (cm.parent(&parent, node, 0) != CR_SUCCESS) {
                break;
            }
            node = parent;
        }
        // Two cards of one model: the slower one, since nothing here tells them apart.
        if (link.known && (!best.known || link.mb_per_s < best.mb_per_s)) {
            const u32 devices = best.devices;
            best = std::move(link);
            best.devices = devices;
        }
    }
    return best;
}
#else
PcieLink ProbePcieLink(u32, u32) {
    return {};
}
#endif

void LogPcieLink(u32 vendor_id, u32 device_id) {
    const char* const value = std::getenv("FRITCHII_PCIE_LINK_LOG");
    if ((value == nullptr || value[0] == '0') && !VideoCommon::LinkStats::On()) {
        return;
    }
    const PcieLink link = ProbePcieLink(vendor_id, device_id);
    if (!link.known) {
        LOG_INFO(Render_Vulkan, "PCIe link to the GPU: not known ({} matching devices)",
                 link.devices);
        return;
    }
    LOG_INFO(Render_Vulkan,
             "PCIe link to the GPU: narrowest reported {} MB/s (speed code {}, x{}) at \"{}\"; the "
             "GPU's own link {} MB/s; {} links read, {} matching devices{}. Ports that report "
             "nothing are not counted",
             link.mb_per_s, link.speed_code, link.lanes, link.narrowest, link.own_mb_per_s,
             link.links_read, link.devices,
             link.tunnelled ? "; a Thunderbolt or USB4 port is on the way" : "");
}

} // namespace Vulkan
