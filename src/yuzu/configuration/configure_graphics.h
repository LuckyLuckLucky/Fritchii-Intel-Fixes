// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <typeindex>
#include <vector>
#include <QColor>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>
#include <vulkan/vulkan_core.h>
#include "common/common_types.h"
#include "common/settings_enums.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/util/vk.h"
#include "yuzu/configuration/configuration_shared.h"

class QPushButton;
class QEvent;
class QObject;
class QComboBox;
class QCheckBox;
class QLabel;
class QVBoxLayout;
class AmdFsr4Panel;

namespace Settings {
enum class NvdecEmulation : u32;
enum class RendererBackend : u32;
} // namespace Settings

namespace Core {
class System;
}

namespace Ui {
class ConfigureGraphics;
}

namespace ConfigurationShared {
class Builder;
}

class ConfigureGraphics : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphics(
        const Core::System& system_, std::vector<VkDeviceInfo::Record>& records,
        const std::function<void()>& expose_compute_option,
        const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)>&
            update_aspect_ratio,
        std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureGraphics() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void Setup(const ConfigurationShared::Builder& builder);

    void PopulateVSyncModeSelection(bool use_setting);
    void UpdateVsyncSetting() const;
    void UpdateBackgroundColorButton(QColor color);
    void UpdateAPILayout();
    void UpdateDeviceSelection(int device);
    void UpdateAmdFsr4Visibility();
    void UpdateDlssVisibility();
    /// The selected GPU can run NVIDIA DLSS (Vulkan on an NVIDIA card).
    [[nodiscard]] bool DlssDevice() const;
    /// DLSS would upscale: its checkbox on (per game: the global value) on a DLSS device. A saved
    /// "on" from another PC or build does nothing on AMD or Intel, so it shows nothing there.
    [[nodiscard]] bool DlssOn() const;
    /// Fritchii builds: OoT's real sizes in the DLSS quality names, and the resolution's tooltip
    /// while DLSS or FSR sets OoT's resolution.
    void SetupOcarinaUpscalerLabels();
    void UpdateOcarinaUpscalerLabels();
    /// Fritchii builds: short names and plain tooltips for this page's rows, the resolution
    /// scales as heights ("1080p (720p handheld)"). `rows`: every built row by setting id.
    void SetupPlainNames(const std::map<u32, QWidget*>& rows);
    /// Fritchii builds, global settings, docked: while FSR or DLSS upscales OoT, "Resolution" is
    /// the size the game draws (oot_render_height: 720p, 1080p, the game's 1440p, or its dynamic
    /// size with Max Resolution off) and "Upscale to" the size the upscaler makes
    /// (oot_upscale_output), only as OoT's upscale path really runs. The choices set the Max
    /// Resolution add-on and the DLSS quality (Native when the two match) (ocarina_upscale.h,
    /// Settings::OcarinaRenderHeight, Settings::OcarinaMaxResolutionOn).
    void SetupPictureMenu();
    void UpdatePictureMenu();
    /// Fritchii builds: the resolution list's names, as heights for the Max Resolution choice.
    void UpdateResolutionNames();
    /// The height of the screen this window is on, in pixels (0 = unknown).
    [[nodiscard]] u32 ScreenHeight() const;
    /// OoT's Max Resolution add-on as this page has it now (without a picture menu: as saved).
    [[nodiscard]] bool MaxResolutionNow() const;
    /// Fritchii builds: the short status line under the picture rows: what the choices make, why
    /// a row is locked or ignored, and when a change prepares shaders again.
    void UpdatePictureNote();
    /// FSR, or DLSS on a card that runs it, is picked for OoT (per game: the global checkbox).
    [[nodiscard]] bool OcarinaUpscalerPicked() const;
    /// The picked card's record (per game: the global choice's), null when there is none.
    [[nodiscard]] const VkDeviceInfo::Record* PictureRecord() const;
    /// The name of the upscaler that will run ("FSR 3.1" for a saved FSR 4 this card can't run).
    [[nodiscard]] QString PictureUpscalerName() const;
    /// The resolution scale OoT will run at (1x with an upscaler), as a number to compare: the
    /// prepared shaders depend on it.
    [[nodiscard]] u32 PictureScaleNow() const;
    /// A picked "Resolution" (0: the dynamic size, Max Resolution off): the output stays where
    /// that resolution makes it, else the nearest.
    void ApplyPictureInput(u32 input);
    /// `output` if the current choices offer it, else the nearest size they do.
    [[nodiscard]] u32 OfferedOutput(u32 output) const;
    /// A picked "Upscale to" height (the scene's own: DLSS Native, FSR at 1x).
    void ApplyPictureOutput(u32 output);
    /// Fritchii builds: the window filter's pros and cons, and which one suits the picture size
    /// on this screen (bigger: Area, smaller: ScaleForce), under the "Window filter" row.
    void UpdateWindowFilterNote();
    /// The upscaler the picture menu shows choices for (None: off, or no picture menu).
    enum class PictureUpscaler : u8 { None, Fsr, Dlss };
    [[nodiscard]] PictureUpscaler PictureUpscalerNow() const;
    /// The picked card's video memory (0 = unknown), for the outputs above 4K.
    [[nodiscard]] u64 PictureVram() const;
    /// The picked "Resolution" (0: the dynamic size) and the output the choices make now.
    [[nodiscard]] u32 PictureInputNow() const;
    [[nodiscard]] bool PictureDlssNative() const;
    [[nodiscard]] u32 PictureOutputNow() const;
    /// The heights each list offers (while the game runs: what it started with, plus DLSS Native).
    [[nodiscard]] std::vector<u32> PictureInputsNow() const;
    [[nodiscard]] std::vector<u32> PictureOutputsNow() const;
    /// Fritchii builds, global settings: one "Upscaler" choice under the resolution (Game, No
    /// sharpening, FSR 3.1, FSR 4, DLSS) that sets OoT's anti-aliasing and the DLSS checkbox,
    /// whose own rows are hidden; AMD's and NVIDIA's download panels and the DLSS options sit
    /// under it.
    void SetupUpscalerChoice(std::map<u32, QWidget*>& hold_graphics);
    void UpdateUpscalerChoice();
    void ApplyUpscalerChoice(int index);
    /// OoT's "DLSS jitter" row only while DLSS upscales OoT, the LOD bias row only with Custom
    /// texture sharpness. `rows`: the built setting rows by setting id.
    void SetupOcarinaTextureRows(const std::map<u32, QWidget*>& rows);
    /// Fritchii builds: the texture sharpness rows only while FSR or DLSS is picked.
    void UpdateOcarinaTextureRows();

    void RetrieveVulkanDevices();

    /* Turns a Vulkan present mode into a textual string for a UI
     * (and eventually for a human to read) */
    const QString TranslateVSyncMode(VkPresentModeKHR mode,
                                     Settings::RendererBackend backend) const;

    Settings::RendererBackend GetCurrentGraphicsBackend() const;

    int FindIndex(u32 enumeration, int value) const;

    std::unique_ptr<Ui::ConfigureGraphics> ui;
    QColor bg_color;

    std::vector<std::function<void(bool)>> apply_funcs{};

    std::vector<VkDeviceInfo::Record>& records;
    std::vector<QString> vulkan_devices;
    std::vector<std::vector<VkPresentModeKHR>> device_present_modes;
    std::vector<VkPresentModeKHR>
        vsync_mode_combobox_enum_map{}; //< Keeps track of which present mode corresponds to which
                                        // selection in the combobox
    u32 vulkan_device{};
    const std::function<void()>& expose_compute_option;
    const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)> update_aspect_ratio;

    const Core::System& system;
    const ConfigurationShared::ComboboxTranslationMap& combobox_translations;

    QPushButton* api_restore_global_button;
    QComboBox* vulkan_device_combobox;
    QComboBox* api_combobox;
    QComboBox* vsync_mode_combobox;
    QPushButton* vsync_restore_global_button;
    QWidget* vulkan_device_widget;
    QWidget* api_widget;
    QComboBox* aspect_ratio_combobox;
    QComboBox* resolution_combobox;
    QComboBox* oot_aa_combobox{};          ///< The oot_anti_aliasing choice
    AmdFsr4Panel* amd_fsr4_panel{};       ///< Under it while AMD FSR 4 is picked
    QCheckBox* nvidia_dlss_checkbox{};     ///< The nvidia_dlss setting
    QWidget* nvidia_dlss_group{};          ///< The DLSS checkbox and NVIDIA's panel under it
    std::vector<QWidget*> dlss_rows;       ///< DLSS quality/model/jitter/game's own DLSS rows
    QComboBox* dlss_quality_combobox{};    ///< The nvidia_dlss_quality choice
    QWidget* resolution_widget{};          ///< The resolution_setup row
    QVBoxLayout* resolution_group_layout{}; ///< Fritchii: the resolution choice and what's under it
    QWidget* oot_aa_widget{};              ///< The oot_anti_aliasing row
    QWidget* nvidia_dlss_widget{};         ///< The nvidia_dlss checkbox row
    QComboBox* upscaler_combobox{};        ///< Fritchii: the "Upscaler" choice
    // Fritchii's picture menu (SetupPictureMenu).
    QWidget* dlss_quality_widget{};        ///< The nvidia_dlss_quality row (hidden there)
    QWidget* max_res_row{};                ///< "Max Resolution", while no upscaler is on
    QComboBox* max_res_combobox{};
    QWidget* render_row{};                 ///< "Resolution": the size the game draws
    QComboBox* render_combobox{};
    QWidget* output_row{};                 ///< "Upscale to": the size the upscaler makes
    QComboBox* output_combobox{};
    QLabel* upscale_note{};                ///< The status line (UpdatePictureNote)
    u32 picture_scale_saved{};             ///< PictureScaleNow when the page opened
    QComboBox* scaling_filter_combobox{};  ///< The "Window filter" choice
    QLabel* filter_note{};                 ///< Fritchii: under it, pros, cons and the advice
    QWidget* texture_sharpness_widget{};
    QComboBox* texture_sharpness_combobox{};
    QWidget* lod_custom_widget{};
    bool max_resolution_choice{true};      ///< OoT's Max Resolution add-on, saved on Apply
    bool max_resolution_saved{true};       ///< ...as it was when the page opened
    bool max_resolution_when_off{true};    ///< Its value when the upscaler goes back to off
    bool max_resolution_upscaling{true};   ///< ...and when an upscaler comes back on
    u32 upscale_output_choice{};           ///< oot_upscale_output, saved on Apply (0 = Auto)
    u32 upscale_output_saved{};            ///< ...as it was when the page opened
    u32 render_height_choice{1440};        ///< oot_render_height, saved on Apply
    u32 render_height_saved{1440};         ///< ...the height it gave when the page opened
    u32 render_setting_saved{};            ///< ...its value then (0 = not picked yet)
};
