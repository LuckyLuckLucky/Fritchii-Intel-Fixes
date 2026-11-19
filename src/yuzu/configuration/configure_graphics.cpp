// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <QSignalBlocker>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSlider>
#include <QStringList>
#include <QStringLiteral>
#include <QtCore/qobjectdefs.h>
#include <qabstractbutton.h>
#include <qboxlayout.h>
#include <qcombobox.h>
#include <qcoreevent.h>
#include <qglobal.h>
#include <qgridlayout.h>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure_graphics.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/amd_fsr4_panel.h"
#include "video_core/ocarina_upscale.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"
#include "yuzu/configuration/shared_widget.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/max_resolution.h"

#ifdef HAS_RESHADE
#include "yuzu/configuration/configure_post_processing.h"
#endif

ConfigureGraphics::ConfigureGraphics(
    const Core::System& system_, std::vector<VkDeviceInfo::Record>& records_,
    const std::function<void()>& expose_compute_option_,
    const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)>&
        update_aspect_ratio_,
    std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : ConfigurationShared::Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphics>()},
      records{records_}, expose_compute_option{expose_compute_option_},
      update_aspect_ratio{update_aspect_ratio_}, system{system_},
      combobox_translations{builder.ComboboxTranslations()} {
    RetrieveVulkanDevices();
    // The card a game would run on: without a saved choice, the automatic pick (so OK saves it).
    vulkan_device = static_cast<u32>(VkDeviceInfo::PickedIndex(
        records, Settings::values.vulkan_device.GetValue(),
        Settings::values.vulkan_device_name.GetValue()));

    ui->setupUi(this);

    Setup(builder);

    for (const auto& device : vulkan_devices) {
        vulkan_device_combobox->addItem(device);
    }

    UpdateBackgroundColorButton(QColor::fromRgb(Settings::values.bg_red.GetValue(),
                                                Settings::values.bg_green.GetValue(),
                                                Settings::values.bg_blue.GetValue()));
    UpdateAPILayout();
    PopulateVSyncModeSelection(false); //< must happen after UpdateAPILayout

    // VSync setting needs to be determined after populating the VSync combobox
    const auto vsync_mode_setting = Settings::values.vsync_mode.GetValue();
    const auto vsync_mode = VSyncSettingToMode(vsync_mode_setting);
    int index{};
    for (const auto mode : vsync_mode_combobox_enum_map) {
        if (mode == vsync_mode) {
            break;
        }
        index++;
    }
    if (static_cast<unsigned long>(index) < vsync_mode_combobox_enum_map.size()) {
        vsync_mode_combobox->setCurrentIndex(index);
    }

    connect(api_combobox, qOverload<int>(&QComboBox::activated), this, [this] {
        UpdateAPILayout();
        PopulateVSyncModeSelection(false);
    });
    connect(vulkan_device_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int device) {
                UpdateDeviceSelection(device);
                PopulateVSyncModeSelection(false);
            });
    connect(ui->bg_button, &QPushButton::clicked, this, [this] {
        const QColor new_bg_color = QColorDialog::getColor(bg_color);
        if (!new_bg_color.isValid()) {
            return;
        }
        UpdateBackgroundColorButton(new_bg_color);
    });

    const auto& update_screenshot_info = [this, &builder]() {
        const auto& combobox_enumerations = builder.ComboboxTranslations().at(
            Settings::EnumMetadata<Settings::AspectRatio>::Index());
        const auto ratio_index = aspect_ratio_combobox->currentIndex();
        const auto ratio =
            static_cast<Settings::AspectRatio>(combobox_enumerations[ratio_index].first);

        const auto& combobox_enumerations_resolution = builder.ComboboxTranslations().at(
            Settings::EnumMetadata<Settings::ResolutionSetup>::Index());
        const auto res_index = resolution_combobox->currentIndex();
        const auto setup = static_cast<Settings::ResolutionSetup>(
            combobox_enumerations_resolution[res_index].first);

        update_aspect_ratio(ratio, setup);
    };

    connect(aspect_ratio_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            update_screenshot_info);
    connect(resolution_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            update_screenshot_info);

    api_combobox->setEnabled(!UISettings::values.has_broken_vulkan && api_combobox->isEnabled());
    ui->api_widget->setEnabled(
        (!UISettings::values.has_broken_vulkan || Settings::IsConfiguringGlobal()) &&
        ui->api_widget->isEnabled());

    if (Settings::IsConfiguringGlobal()) {
        ui->bg_widget->setEnabled(Settings::values.bg_red.UsingGlobal());
    }
}

void ConfigureGraphics::PopulateVSyncModeSelection(bool use_setting) {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        vsync_mode_combobox->setEnabled(false);
        return;
    }
    vsync_mode_combobox->setEnabled(true);

    const int current_index = //< current selected vsync mode from combobox
        vsync_mode_combobox->currentIndex();
    const auto current_mode = //< current selected vsync mode as a VkPresentModeKHR
        current_index == -1 || use_setting
            ? VSyncSettingToMode(Settings::values.vsync_mode.GetValue())
            : vsync_mode_combobox_enum_map[current_index];
    int index{};
    const int device{vulkan_device_combobox->currentIndex()}; //< current selected Vulkan device

    const auto& present_modes = //< relevant vector of present modes for the selected device or API
        backend == Settings::RendererBackend::Vulkan && device > -1 ? device_present_modes[device]
                                                                    : default_present_modes;

    vsync_mode_combobox->clear();
    vsync_mode_combobox_enum_map.clear();
    vsync_mode_combobox_enum_map.reserve(present_modes.size());
    for (const auto present_mode : present_modes) {
        const auto mode_name = TranslateVSyncMode(present_mode, backend);
        if (mode_name.isEmpty()) {
            continue;
        }

        vsync_mode_combobox->insertItem(index, mode_name);
        vsync_mode_combobox_enum_map.push_back(present_mode);
        if (present_mode == current_mode) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
        index++;
    }

    if (!Settings::IsConfiguringGlobal()) {
        vsync_restore_global_button->setVisible(!Settings::values.vsync_mode.UsingGlobal());

        const Settings::VSyncMode global_vsync_mode = Settings::values.vsync_mode.GetValue(true);
        vsync_restore_global_button->setEnabled(
            ((backend == Settings::RendererBackend::OpenGL_GLSL ||
              backend == Settings::RendererBackend::OpenGL_GLASM ||
              backend == Settings::RendererBackend::OpenGL_SPIRV) &&
             (global_vsync_mode == Settings::VSyncMode::Immediate ||
              global_vsync_mode == Settings::VSyncMode::Fifo)) ||
            backend == Settings::RendererBackend::Vulkan);
    }
}

void ConfigureGraphics::UpdateVsyncSetting() const {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        return;
    }

    const auto mode = vsync_mode_combobox_enum_map[vsync_mode_combobox->currentIndex()];
    const auto vsync_mode = PresentModeToSetting(mode);
    Settings::values.vsync_mode.SetValue(vsync_mode);
}

void ConfigureGraphics::UpdateDeviceSelection(int device) {
    if (device == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan) {
        vulkan_device = device;
    }
    UpdateAmdFsr4Visibility();
    UpdateDlssVisibility();
}

ConfigureGraphics::~ConfigureGraphics() = default;

void ConfigureGraphics::SetConfiguration() {}

void ConfigureGraphics::Setup(const ConfigurationShared::Builder& builder) {
    QLayout* api_layout = ui->api_widget->layout();
    QWidget* api_grid_widget = new QWidget(this);
    QVBoxLayout* api_grid_layout = new QVBoxLayout(api_grid_widget);
    api_grid_layout->setContentsMargins(0, 0, 0, 0);
    api_layout->addWidget(api_grid_widget);

    QLayout& graphics_layout = *ui->graphics_widget->layout();

    std::map<u32, QWidget*> hold_graphics;
    std::map<u32, QWidget*> all_rows; ///< Every built row by setting id (not the groups)
    std::vector<QWidget*> hold_api;

    for (const auto setting : Settings::values.linkage.by_category[Settings::Category::Renderer]) {
#ifndef HAS_RESHADE
        if (setting->Id() == Settings::values.post_shader_enabled.Id()) {
            continue;
        }
#endif
#ifndef EGLANTINE_STREAMLINE
        // Only builds with NVIDIA Streamline (Windows) can use it.
        if (setting->Id() == Settings::values.nvidia_dlss.Id() ||
            setting->Id() == Settings::values.nvidia_dlss_quality.Id() ||
            setting->Id() == Settings::values.nvidia_dlss_preset.Id()) {
            continue;
        }
        if (setting->Id() == Settings::values.oot_dlss_jitter.Id()) {
            continue;
        }
#endif

        ConfigurationShared::Widget* widget = [&]() {
            if (setting->Id() == Settings::values.fsr_sharpening_slider.Id()) {
                // FSR needs a reversed slider and a 0.5 multiplier
                return builder.BuildWidget(
                    setting, apply_funcs, ConfigurationShared::RequestType::ReverseSlider, true,
                    0.5f, nullptr, tr("%", "FSR/SGSR sharpening percentage (e.g. 50%)"));
            } else {
                return builder.BuildWidget(setting, apply_funcs);
            }
        }();

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        if (nvidia_dlss_checkbox && (setting->Id() == Settings::values.nvidia_dlss_quality.Id() ||
                                     setting->Id() == Settings::values.nvidia_dlss_preset.Id())) {
            widget->setVisible(DlssOn());
            connect(nvidia_dlss_checkbox, &QCheckBox::toggled, widget,
                    [this, widget](bool) { widget->setVisible(DlssOn()); });
        }
        if (setting->Id() == Settings::values.nvidia_dlss_quality.Id()) {
            dlss_quality_combobox = widget->combobox;
            dlss_quality_widget = widget;
        }
        all_rows.emplace(setting->Id(), widget);
        if (setting->Id() == Settings::values.nvidia_dlss_quality.Id() ||
            setting->Id() == Settings::values.nvidia_dlss_preset.Id() ||
            setting->Id() == Settings::values.game_dlss_network.Id() ||
            setting->Id() == Settings::values.oot_dlss_jitter.Id()) {
            dlss_rows.push_back(widget);
        }

        if (setting->Id() == Settings::values.renderer_backend.Id()) {
            // Add the renderer combobox now so it's at the top
            api_grid_layout->addWidget(widget);
            api_combobox = widget->combobox;
            api_restore_global_button = widget->restore_button;
            if (!Settings::IsConfiguringGlobal()) {
                api_restore_global_button->connect(api_restore_global_button,
                                                   &QAbstractButton::clicked,
                                                   [this](bool) { UpdateAPILayout(); });
                // Detach API's restore button and place it where we want
                // Lets us put it on the side, and it will automatically scale if there's a
                // second combobox (vulkan_device)
                widget->layout()->removeWidget(api_restore_global_button);
                api_layout->addWidget(api_restore_global_button);
            }
        } else if (setting->Id() == Settings::values.vulkan_device.Id()) {
            // Keep track of vulkan_device's combobox so we can populate it
            hold_api.push_back(widget);
            vulkan_device_combobox = widget->combobox;
            vulkan_device_widget = widget;
        } else if (setting->Id() == Settings::values.vsync_mode.Id()) {
            // Keep track of vsync_mode's combobox so we can populate it
            vsync_mode_combobox = widget->combobox;

            // Since vsync is populated at runtime, we have to manually set up the button for
            // restoring the global setting.
            if (!Settings::IsConfiguringGlobal()) {
                QPushButton* restore_button =
                    ConfigurationShared::Widget::CreateRestoreGlobalButton(
                        Settings::values.vsync_mode.UsingGlobal(), widget);
                restore_button->setEnabled(true);
                widget->layout()->addWidget(restore_button);

                restore_button->connect(restore_button, &QAbstractButton::clicked,
                                        [restore_button, this](bool) {
                                            Settings::values.vsync_mode.SetGlobal(true);
                                            PopulateVSyncModeSelection(true);

                                            restore_button->setVisible(false);
                                        });

                std::function<void()> set_non_global = [restore_button, this]() {
                    Settings::values.vsync_mode.SetGlobal(false);
                    UpdateVsyncSetting();
                    restore_button->setVisible(true);
                };
                widget->combobox->connect(widget->combobox,
                                          QOverload<int>::of(&QComboBox::activated),
                                          [set_non_global]() { set_non_global(); });
                vsync_restore_global_button = restore_button;
            }
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.aspect_ratio.Id()) {
            // Keep track of the aspect ratio combobox to update other UI tabs that need it
            aspect_ratio_combobox = widget->combobox;
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.resolution_setup.Id()) {
            // Keep track of the resolution combobox to update other UI tabs that need it
            resolution_combobox = widget->combobox;
            resolution_widget = widget;
            if (FritchiiUi::Enabled()) {
                // The resolution, the upscaler and what goes with them, in one group (the
                // explanations live in the tooltips: no text under the rows).
                QWidget* group = new QWidget(this);
                QVBoxLayout* group_layout = new QVBoxLayout(group);
                resolution_group_layout = group_layout;
                group_layout->setContentsMargins(0, 0, 0, 0);
                group_layout->setSpacing(2);
                group_layout->addWidget(widget);
                hold_graphics.emplace(setting->Id(), group);
            } else {
                hold_graphics.emplace(setting->Id(), widget);
            }
#ifdef HAS_RESHADE
        } else if (setting->Id() == Settings::values.post_shader_enabled.Id()) {
            QPushButton* post_shader_button = new QPushButton(tr("Configure Effects..."), widget);
            post_shader_button->setVisible(widget->checkbox->isChecked());

            connect(post_shader_button, &QAbstractButton::clicked, this, [this]() {
                ConfigurePostProcessing dialog(this);
                dialog.exec();
            });
            connect(widget->checkbox, &QCheckBox::toggled, post_shader_button,
                    &QWidget::setVisible);

            QBoxLayout* row = qobject_cast<QBoxLayout*>(widget->layout());
            row->insertWidget(1, post_shader_button);

            hold_graphics.emplace(setting->Id(), widget);
#endif
#ifdef EGLANTINE_STREAMLINE
        } else if (setting->Id() == Settings::values.nvidia_dlss.Id()) {
            nvidia_dlss_checkbox = widget->checkbox;
            nvidia_dlss_widget = widget;
            // NVIDIA's files and a download button under the checkbox.
            QWidget* group = new QWidget(this);
            QVBoxLayout* group_layout = new QVBoxLayout(group);
            group_layout->setContentsMargins(0, 0, 0, 0);
            group_layout->setSpacing(2);
            group_layout->addWidget(widget);
            NvidiaDlssPanel* panel = new NvidiaDlssPanel(group);
            group_layout->addWidget(panel);
            panel->setVisible(widget->checkbox->isChecked());
            connect(widget->checkbox, &QCheckBox::toggled, panel, &QWidget::setVisible);
            nvidia_dlss_group = group;
            hold_graphics.emplace(setting->Id(), group);
#endif
        } else if (setting->Id() == Settings::values.oot_anti_aliasing.Id() &&
                   widget->combobox != nullptr) {
            // AMD FSR 4 is offered only for GPUs it runs on; AMD's files and a download button sit
            // under the choice while it's picked.
            QWidget* group = new QWidget(this);
            QVBoxLayout* group_layout = new QVBoxLayout(group);
            group_layout->setContentsMargins(0, 0, 0, 0);
            group_layout->setSpacing(2);
            group_layout->addWidget(widget);
            oot_aa_combobox = widget->combobox;
            oot_aa_widget = widget;
            amd_fsr4_panel = new AmdFsr4Panel(group);
            group_layout->addWidget(amd_fsr4_panel);
            connect(oot_aa_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this](int) { UpdateAmdFsr4Visibility(); });
            hold_graphics.emplace(setting->Id(), group);
        } else if (setting->Id() == Settings::values.scaling_filter.Id() &&
                   widget->combobox != nullptr && FritchiiUi::Enabled()) {
            // The filter's pros and cons and the one that suits this picture on this screen,
            // under the choice (owner 2026-11-18: settings as easy to understand as possible).
            QWidget* group = new QWidget(this);
            QVBoxLayout* group_layout = new QVBoxLayout(group);
            group_layout->setContentsMargins(0, 0, 0, 0);
            group_layout->setSpacing(2);
            group_layout->addWidget(widget);
            scaling_filter_combobox = widget->combobox;
            filter_note = new QLabel(group);
            filter_note->setWordWrap(true);
            group_layout->addWidget(filter_note);
            connect(scaling_filter_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int) { UpdateWindowFilterNote(); });
            hold_graphics.emplace(setting->Id(), group);
        } else {
            hold_graphics.emplace(setting->Id(), widget);
        }
    }

    SetupUpscalerChoice(hold_graphics);

    for (const auto& [id, widget] : hold_graphics) {
        graphics_layout.addWidget(widget);
    }

    for (auto widget : hold_api) {
        api_grid_layout->addWidget(widget);
    }

    SetupOcarinaUpscalerLabels();
    SetupOcarinaTextureRows(all_rows);
    SetupPictureMenu();
    SetupPlainNames(all_rows);

    // Background color is too specific to build into the new system, so we manage it here
    // (3 settings, all collected into a single widget with a QColor to manage on top)
    if (Settings::IsConfiguringGlobal()) {
        apply_funcs.push_back([this](bool powered_on) {
            Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
            Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
            Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
        });
    } else {
        QPushButton* bg_restore_button = ConfigurationShared::Widget::CreateRestoreGlobalButton(
            Settings::values.bg_red.UsingGlobal(), ui->bg_widget);
        ui->bg_widget->layout()->addWidget(bg_restore_button);

        bg_restore_button->connect(bg_restore_button, &QAbstractButton::clicked,
                                   [bg_restore_button, this](bool) {
                                       const int r = Settings::values.bg_red.GetValue(true);
                                       const int g = Settings::values.bg_green.GetValue(true);
                                       const int b = Settings::values.bg_blue.GetValue(true);
                                       UpdateBackgroundColorButton(QColor::fromRgb(r, g, b));

                                       bg_restore_button->setVisible(false);
                                       bg_restore_button->setEnabled(false);
                                   });

        ui->bg_button->connect(ui->bg_button, &QAbstractButton::clicked, [bg_restore_button](bool) {
            bg_restore_button->setVisible(true);
            bg_restore_button->setEnabled(true);
        });

        apply_funcs.push_back([bg_restore_button, this](bool powered_on) {
            const bool using_global = !bg_restore_button->isEnabled();
            Settings::values.bg_red.SetGlobal(using_global);
            Settings::values.bg_green.SetGlobal(using_global);
            Settings::values.bg_blue.SetGlobal(using_global);
            if (!using_global) {
                Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
                Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
                Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
            }
        });
    }
}

const QString ConfigureGraphics::TranslateVSyncMode(VkPresentModeKHR mode,
                                                    Settings::RendererBackend backend) const {
    // Fritchii: plain names (Mailbox is what the host frame pacer is made for).
    if (FritchiiUi::Enabled() && backend == Settings::RendererBackend::Vulkan) {
        switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return tr("Off");
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return tr("On (recommended)");
        case VK_PRESENT_MODE_FIFO_KHR:
            return tr("On, classic");
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return tr("On, tears when slow");
        default:
            return {};
        }
    }
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return (backend == Settings::RendererBackend::OpenGL_GLSL ||
                backend == Settings::RendererBackend::OpenGL_GLASM ||
                backend == Settings::RendererBackend::OpenGL_SPIRV)
                   ? tr("Off")
                   : QStringLiteral("Immediate (%1)").arg(tr("VSync Off"));
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return QStringLiteral("Mailbox (%1)").arg(tr("Recommended"));
    case VK_PRESENT_MODE_FIFO_KHR:
        return (backend == Settings::RendererBackend::OpenGL_GLSL ||
                backend == Settings::RendererBackend::OpenGL_GLASM ||
                backend == Settings::RendererBackend::OpenGL_SPIRV)
                   ? tr("On")
                   : QStringLiteral("FIFO (%1)").arg(tr("VSync On"));
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return QStringLiteral("FIFO Relaxed");
    default:
        return {};
    }
}

int ConfigureGraphics::FindIndex(u32 enumeration, int value) const {
    for (u32 i = 0; enumeration < combobox_translations.size() &&
                    i < combobox_translations.at(enumeration).size();
         i++)
        if (combobox_translations.at(enumeration)[i].first == u32(value))
            return i;
    return -1;
}

void ConfigureGraphics::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    UpdateVsyncSetting();

    Settings::RefreshNvidiaDlssRuntimeSettings();

    Settings::values.vulkan_device.SetGlobal(true);
    Settings::values.vulkan_device_name.SetGlobal(true);
    auto const index = Settings::EnumMetadata<Settings::RendererBackend>::Index();
    if (Settings::IsConfiguringGlobal() ||
        (!Settings::IsConfiguringGlobal() && api_restore_global_button->isEnabled())) {
        auto backend =
            index >= combobox_translations.size() ||
                    size_t(api_combobox->currentIndex()) >= combobox_translations.at(index).size()
                ? Settings::values.renderer_backend.GetValue()
                : Settings::RendererBackend(
                      combobox_translations.at(index)[api_combobox->currentIndex()].first);
        switch (backend) {
        case Settings::RendererBackend::Vulkan:
            Settings::values.vulkan_device.SetGlobal(Settings::IsConfiguringGlobal());
            Settings::values.vulkan_device.SetValue(vulkan_device_combobox->currentIndex());
            // The name too: a changed device order finds the same card again.
            Settings::values.vulkan_device_name.SetGlobal(Settings::IsConfiguringGlobal());
            if (const int picked = vulkan_device_combobox->currentIndex();
                picked >= 0 && size_t(picked) < records.size()) {
                Settings::values.vulkan_device_name.SetValue(records[picked].device_name);
            }
            break;
        case Settings::RendererBackend::OpenGL_GLSL:
        case Settings::RendererBackend::OpenGL_SPIRV:
        case Settings::RendererBackend::OpenGL_GLASM:
        case Settings::RendererBackend::Null:
            break;
        }
    }
}

void ConfigureGraphics::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphics::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphics::UpdateBackgroundColorButton(QColor color) {
    bg_color = color;

    QPixmap pixmap(ui->bg_button->size());
    pixmap.fill(bg_color);

    const QIcon color_icon(pixmap);
    ui->bg_button->setIcon(color_icon);
}

void ConfigureGraphics::UpdateAPILayout() {
    bool runtime_lock = !system.IsPoweredOn();
    bool need_global = !(Settings::IsConfiguringGlobal() || api_restore_global_button->isEnabled());
    vulkan_device = static_cast<u32>(VkDeviceInfo::PickedIndex(
        records, Settings::values.vulkan_device.GetValue(need_global),
        Settings::values.vulkan_device_name.GetValue(need_global)));
    vulkan_device_widget->setEnabled(!need_global && runtime_lock);

    const auto current_backend = GetCurrentGraphicsBackend();
    const bool is_vulkan = current_backend == Settings::RendererBackend::Vulkan;
    vulkan_device_widget->setVisible(is_vulkan);
    if (is_vulkan && int(vulkan_device) < vulkan_device_combobox->count()) {
        vulkan_device_combobox->setCurrentIndex(vulkan_device);
    }
    UpdateAmdFsr4Visibility();
    UpdateDlssVisibility();
}

namespace {
/// The "Upscaler" row for DLSS, which is the nvidia_dlss checkbox, not an OotAntiAliasing value.
constexpr u32 UPSCALER_DLSS = 0x100;
} // namespace

bool ConfigureGraphics::DlssDevice() const {
    const int device = vulkan_device_combobox ? vulkan_device_combobox->currentIndex() : -1;
    if (device < 0) {
        // Per game there is no device list: the global choice's card.
        const auto global = static_cast<size_t>(Settings::values.vulkan_device.GetValue(true));
        return global < records.size() && records[global].nvidia;
    }
    return GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan &&
           static_cast<size_t>(device) < records.size() && records[device].nvidia;
}

bool ConfigureGraphics::DlssOn() const {
    const bool checked = nvidia_dlss_checkbox != nullptr ? nvidia_dlss_checkbox->isChecked()
                                                         : Settings::values.nvidia_dlss.GetValue();
    return checked && DlssDevice();
}

void ConfigureGraphics::UpdateDlssVisibility() {
    if (nvidia_dlss_group == nullptr || nvidia_dlss_checkbox == nullptr) {
        return;
    }
    // DLSS runs only on NVIDIA GPUs: the option is hidden for any other GPU (AMD and Intel
    // players saw it and could turn it on for nothing). The game falls back to FSR 3.1 or its own
    // upscaler meanwhile.
    const bool nvidia = DlssDevice();
    // The upscaler list's DLSS entry too (2026-11-18: AMD players still saw "DLSS" there).
    if (upscaler_combobox != nullptr) {
        if (auto* const list = qobject_cast<QListView*>(upscaler_combobox->view())) {
            list->setRowHidden(upscaler_combobox->findData(UPSCALER_DLSS), !nvidia);
        }
    }
    // Not NVIDIA: every DLSS row goes, a saved "on" included (2026-11-16: an RX 7900 XT's game
    // settings still showed DLSS quality and model; DLSS can't run there, the value does nothing).
    nvidia_dlss_group->setVisible(nvidia);
    if (!nvidia) {
        for (QWidget* const row : dlss_rows) {
            row->setVisible(false);
        }
    }
    UpdateUpscalerChoice();
}

void ConfigureGraphics::UpdateAmdFsr4Visibility() {
    if (oot_aa_combobox == nullptr || amd_fsr4_panel == nullptr) {
        return;
    }
    // AMD's FSR 4 runtime is Windows-only and runs on the GPUs IsFsr4CapableGpu names.
#ifdef _WIN32
    const int device = vulkan_device_combobox ? vulkan_device_combobox->currentIndex() : -1;
    const bool capable = GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan &&
                         device >= 0 && static_cast<size_t>(device) < records.size() &&
                         records[device].fsr4_capable;
#else
    constexpr bool capable = false;
#endif
    const int fsr4_row =
        oot_aa_combobox->findData(static_cast<u32>(Settings::OotAntiAliasing::Fsr4));
    const bool fsr4_picked = fsr4_row >= 0 && oot_aa_combobox->currentIndex() == fsr4_row;
    // Hiding the row keeps the combobox's rows in step with the setting's values. A saved FSR 4
    // choice on another GPU stays visible (FSR 3.1 runs in its place).
    if (auto* view = qobject_cast<QListView*>(oot_aa_combobox->view()); view && fsr4_row >= 0) {
        view->setRowHidden(fsr4_row, !capable && !fsr4_picked);
    }
    amd_fsr4_panel->setVisible(capable && fsr4_picked);
    UpdateUpscalerChoice();
}

namespace {

/// The picture menu's "Resolution" for the game's dynamic size (Max Resolution off): up to
/// 1080p, and its 3/4 pass makes 1440p.
constexpr u32 DYNAMIC_INPUT = 0;

/// The height of the scene the game draws for a picture input (the dynamic one: at most 1080p).
constexpr u32 SceneHeight(u32 input) {
    return input == DYNAMIC_INPUT ? 1080U : input;
}

/// A fixed scene below 720p (540p and 360p, with FRITCHII_OOT_RENDER_STEPS).
constexpr bool LowScene(u32 input) {
    return input != DYNAMIC_INPUT && input < 720;
}

/// What oot_upscale_output's Auto (0) makes from that scene (VideoCommon::PlanOcarinaPassOutput):
/// 4K from Max Resolution's fixed scene (1080p from one below 720p, 4K is over 3x there), 1440p
/// from the dynamic one.
constexpr u32 AutoOutput(u32 input) {
    if (LowScene(input)) {
        return 1080;
    }
    return input == DYNAMIC_INPUT ? 1440U : 2160U;
}

/// The sizes DLSS is known to take from a scene: Native, its 1.5x (Quality), 2x (Performance) and
/// 3x (Ultra Performance) modes, and 4/3, the game's own 3/4 pass that DLSS has replaced since
/// v6.0. A pair no mode accepts turns DLSS off for the session, so other ratios (2.5x, 2.67x) are
/// not offered; nor is 8K, where a tester's 1440p -> 8K found no mode although it is exactly 3x.
/// A saved output outside this list still shows and runs as saved.
constexpr bool DlssTakesOutput(u32 scene, u32 output) {
    if (output > 2880) {
        return false;
    }
    return output == scene || 2 * output == 3 * scene || output == 2 * scene ||
           output == 3 * scene || 3 * output == 4 * scene;
}

/// The outputs DLSS and FSR make from OoT's scene, docked, as VideoCommon::PlanOcarinaPassOutput
/// runs them. They only enlarge, by up to 3x a side (FSR 3.1's range, DLSS Ultra Performance):
/// - A fixed scene (Max Resolution on: the game's 2560x1440, or oot_render_height's 720p, 1080p,
///   and with FRITCHII_OOT_RENDER_STEPS 360p, 540p, 810p, 900p, 1260p): the offered heights from
///   the scene's own (Native: anti-aliasing only) to 3x it. 720p -> 1080p is made at 1080p and
///   stretched into the game's 1440p target. Above 4K only where the card's memory allows it.
/// - With DLSS (`dlss`): only the sizes DlssTakesOutput knows it takes. FSR takes any ratio up to
///   3x.
/// - The dynamic scene: its 3/4 pass makes 1440p, or keeps the game's size (Native).
std::vector<u32> PictureOutputs(u32 input, u64 vram, bool dlss) {
    if (input == DYNAMIC_INPUT) {
        return {1080, 1440};
    }
    std::vector<u32> outputs;
    std::vector<u32> exact;
    for (const unsigned height : VideoCommon::OcarinaUpscaleOutputs) {
        if (height < input || height > 3 * input ||
            !VideoCommon::OcarinaUpscaleOutputFits(height, vram)) {
            continue;
        }
        outputs.push_back(height);
        if (DlssTakesOutput(input, height)) {
            exact.push_back(height);
        }
    }
    // A test height with no exact size (1260p) keeps the list, so the row isn't empty.
    return dlss && !exact.empty() ? exact : outputs;
}

/// The output a saved oot_upscale_output makes from that scene when the game runs it
/// (VideoCommon::ResolveOcarinaUpscaleOutput, then PlanOcarinaPassOutput). DLSS Native keeps
/// the scene's size whatever the output says.
u32 OutputFor(u32 input, u32 setting, bool dlss_native, u64 vram) {
    const u32 scene = SceneHeight(input);
    if (dlss_native) {
        return scene;
    }
    if (setting == 0 || !VideoCommon::OcarinaUpscaleOutputKnown(setting)) {
        return AutoOutput(input);
    }
    if (!VideoCommon::OcarinaUpscaleOutputFits(setting, vram)) {
        setting = 2160;
    }
    if (input == DYNAMIC_INPUT) {
        return setting <= 1080 ? 1080U : 1440U;
    }
    if (LowScene(input)) {
        const u32 low = VideoCommon::OcarinaLowSceneOutput(setting, input);
        return low != 0 ? low : scene;
    }
    if (setting <= scene) {
        return scene;
    }
    if (setting <= 1440) {
        return setting;
    }
    // More than 3x the scene: the largest offered height within 3x.
    u32 output = 1440;
    for (const unsigned height : VideoCommon::OcarinaUpscaleOutputs) {
        if (height > 1440 && height <= setting && height <= 3 * scene) {
            output = height;
        }
    }
    return output;
}

/// OoT's render height as the game would start with it (Settings::OcarinaRenderHeight, docked
/// with Max Resolution on): oot_render_height, or while it is 0 (not picked yet)
/// FRITCHII_DLSS_RENDER_SCALE's DLSS Performance (1080p) and Ultra Performance (720p).
u32 MenuRenderHeight(u32 setting, bool fsr, Settings::NvidiaDlssQuality quality) {
    if (setting != 0) {
        return setting < 1440 && Settings::OcarinaRenderHeightOffered(setting) ? setting : 1440U;
    }
    if (fsr || !Settings::OcarinaDlssRenderScaleSwitch()) {
        return 1440;
    }
    if (quality == Settings::NvidiaDlssQuality::Performance) {
        return 1080;
    }
    if (quality == Settings::NvidiaDlssQuality::UltraPerformance) {
        return 720;
    }
    return 1440;
}

/// "1440p", or "4K" for 2160 lines and "8K" for 4320.
QString HeightName(u32 height) {
    if (height == 2160) {
        return QStringLiteral("4K");
    }
    if (height == 4320) {
        return QStringLiteral("8K");
    }
    return QStringLiteral("%1p").arg(height);
}

/// A resolution scale as a number to compare (never 0).
u32 ScaleKey(Settings::ResolutionSetup setup) {
    Settings::ResolutionScalingInfo info{};
    Settings::TranslateResolutionInfo(setup, info);
    return (info.up_scale << 8) | info.down_shift;
}

} // namespace

void ConfigureGraphics::SetupUpscalerChoice(std::map<u32, QWidget*>& hold_graphics) {
    if (!FritchiiUi::Enabled() || resolution_group_layout == nullptr) {
        return;
    }
    // The status line (UpdatePictureNote). Same size as the rows, not a small grey hint.
    const auto add_note = [this] {
        upscale_note = new QLabel(this);
        upscale_note->setWordWrap(true);
        upscale_note->setVisible(false);
        resolution_group_layout->addWidget(upscale_note);
    };
    // Per-game settings keep the separate rows: each has its own "use global" button.
    if (!Settings::IsConfiguringGlobal() || oot_aa_combobox == nullptr ||
        oot_aa_widget == nullptr) {
        add_note();
        return;
    }
    // A row like the setting rows: the name on the left, the choice on the right.
    const auto make_row = [this](const QString& name, QWidget* control) {
        QWidget* row = new QWidget(this);
        QHBoxLayout* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        QLabel* label = new QLabel(name, row);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        row_layout->addWidget(label, 1);
        row_layout->addWidget(control, 1);
        resolution_group_layout->addWidget(row);
        return row;
    };
    // Docked and without FRITCHII_DLSS_SCALED, OoT's upscale sizes are known: while an upscaler
    // is on, the picture menu (SetupPictureMenu) shows two choices, "Resolution" (the size the
    // game draws, in place of the scale row) and "Upscale to" (the size the upscaler makes). In
    // handheld or with the scaled test switch, the plain rows stay.
    const bool picture_menu = Settings::IsDockedMode() && !VideoCommon::OcarinaScaledUpscale();
    if (picture_menu) {
        // With no upscaler the add-on has no other place on this page (the first-start question
        // sends players here to change it). With one, "Resolution" below includes it.
        max_res_combobox = new QComboBox;
        max_res_combobox->addItem(tr("Off (up to 1080p)"));
        max_res_combobox->addItem(tr("On (1440p)"));
        max_res_row = make_row(tr("Max Resolution:"), max_res_combobox);
        max_res_row->setToolTip(tr("On: sharper, needs about an RTX 3070 / RX 6800 (8 GB).\n"
                                   "Off: like the Switch 2, more fps on weaker PCs."));
        max_res_row->setVisible(false);

        render_combobox = new QComboBox;
        render_row = make_row(tr("Resolution:"), render_combobox);
        render_row->setVisible(false);
    }

    upscaler_combobox = new QComboBox;
    QWidget* const row = make_row(tr("Upscaler:"), upscaler_combobox);
    row->setToolTip(tr("Makes a sharp picture from a smaller one, so the game runs faster.\n"
                       "DLSS: NVIDIA RTX cards. FSR 4: Radeon RX 6000 and newer. FSR 3.1: any "
                       "card.\nOoT only. Off keeps the game's own look."));
    using Aa = Settings::OotAntiAliasing;
    upscaler_combobox->addItem(tr("Off"), static_cast<u32>(Aa::Game));
    upscaler_combobox->addItem(tr("Off, no sharpening"), static_cast<u32>(Aa::NoSharpening));
    upscaler_combobox->addItem(tr("FSR 3.1"), static_cast<u32>(Aa::Fsr3));
    upscaler_combobox->addItem(tr("FSR 4"), static_cast<u32>(Aa::Fsr4));
    if (nvidia_dlss_checkbox != nullptr) {
        upscaler_combobox->addItem(tr("DLSS"), UPSCALER_DLSS);
    }
    // OoT's anti-aliasing and the DLSS checkbox apply when the game starts: locked while it runs.
    upscaler_combobox->setEnabled(oot_aa_widget->isEnabled() &&
                                  (nvidia_dlss_widget == nullptr || nvidia_dlss_widget->isEnabled()));

    if (picture_menu) {
        output_combobox = new QComboBox;
        output_row = make_row(tr("Upscale to:"), output_combobox);
        output_row->setVisible(false);
    }
    add_note();

    // The old rows' widgets keep the settings (and save them); only the choice above shows.
    oot_aa_widget->setVisible(false);
    if (nvidia_dlss_widget != nullptr) {
        nvidia_dlss_widget->setVisible(false);
    }
    // AMD's and NVIDIA's download panels and the upscaler's options move under the choice.
    for (const u32 id : {Settings::values.oot_anti_aliasing.Id(), Settings::values.nvidia_dlss.Id(),
                         Settings::values.nvidia_dlss_quality.Id(),
                         Settings::values.nvidia_dlss_preset.Id(),
                         Settings::values.oot_dlss_jitter.Id(),
                         Settings::values.oot_texture_sharpness.Id(),
                         Settings::values.oot_lod_bias_custom.Id()}) {
        if (const auto it = hold_graphics.find(id); it != hold_graphics.end()) {
            resolution_group_layout->addWidget(it->second);
            hold_graphics.erase(it);
        }
    }

    connect(upscaler_combobox, QOverload<int>::of(&QComboBox::activated), this,
            [this](int index) { ApplyUpscalerChoice(index); });
    connect(oot_aa_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { UpdateUpscalerChoice(); });
    if (nvidia_dlss_checkbox != nullptr) {
        connect(nvidia_dlss_checkbox, &QCheckBox::toggled, this,
                [this](bool) { UpdateUpscalerChoice(); });
    }
    UpdateUpscalerChoice();
}

void ConfigureGraphics::ApplyUpscalerChoice(int index) {
    if (upscaler_combobox == nullptr || index < 0) {
        return;
    }
    using Aa = Settings::OotAntiAliasing;
    // The picture menu keeps the sizes picked with the other upscaler.
    const bool was_upscaling = PictureUpscalerNow() != PictureUpscaler::None;
    const u32 output_before = was_upscaling ? PictureOutputNow() : 0;
    const u32 choice = upscaler_combobox->itemData(index).toUInt();
    const auto current = static_cast<Aa>(oot_aa_combobox->currentData().toUInt());
    const bool dlss = choice == UPSCALER_DLSS;
    // DLSS keeps Game or No sharpening under it (an FSR choice would win over DLSS).
    const Aa aa = dlss ? (VideoCommon::OcarinaFsrChosen(current) ? Aa::Game : current)
                       : static_cast<Aa>(choice);
    const int aa_row = oot_aa_combobox->findData(static_cast<u32>(aa));
    if (aa_row >= 0) {
        oot_aa_combobox->setCurrentIndex(aa_row);
    }
    if (nvidia_dlss_checkbox != nullptr) {
        nvidia_dlss_checkbox->setChecked(dlss);
    }
    if (output_row != nullptr) {
        if (PictureUpscalerNow() == PictureUpscaler::None) {
            // Off: the game's own upscale gets Max Resolution back as it was.
            if (was_upscaling) {
                max_resolution_upscaling = max_resolution_choice;
            }
            max_resolution_choice = max_resolution_when_off;
        } else if (was_upscaling) {
            // DLSS <-> FSR: the same output (DLSS Native <-> FSR at 1x included), or the nearest
            // one DLSS takes.
            ApplyPictureOutput(OfferedOutput(output_before));
        } else {
            // Back on: the "Resolution" picked before (a fixed height needs the add-on).
            max_resolution_choice = max_resolution_upscaling;
            // An output kept from FSR may not be one DLSS takes.
            if (const u32 now = PictureOutputNow();
                PictureUpscalerNow() == PictureUpscaler::Dlss && OfferedOutput(now) != now) {
                ApplyPictureOutput(OfferedOutput(now));
            }
        }
    }
    UpdateUpscalerChoice();
}

void ConfigureGraphics::UpdateUpscalerChoice() {
    if (upscaler_combobox == nullptr || oot_aa_combobox == nullptr) {
        return;
    }
    using Aa = Settings::OotAntiAliasing;
    const auto aa = static_cast<Aa>(oot_aa_combobox->currentData().toUInt());
    const bool dlss_on = DlssOn();
    // What runs: an FSR choice wins over DLSS (VideoCommon::SelectOcarinaUpscaler).
    const u32 shown =
        !VideoCommon::OcarinaFsrChosen(aa) && dlss_on ? UPSCALER_DLSS : static_cast<u32>(aa);
    const int shown_row = upscaler_combobox->findData(shown);
    if (shown_row >= 0 && upscaler_combobox->currentIndex() != shown_row) {
        upscaler_combobox->setCurrentIndex(shown_row);
    }
    // FSR 4 and DLSS only where they run, like the rows they stand for; a saved choice stays.
    const int device = vulkan_device_combobox ? vulkan_device_combobox->currentIndex() : -1;
    const bool vulkan = GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan &&
                        device >= 0 && static_cast<size_t>(device) < records.size();
#ifdef _WIN32
    const bool fsr4_capable = vulkan && records[device].fsr4_capable;
#else
    constexpr bool fsr4_capable = false;
#endif
    // Not offered on NVIDIA cards from before RTX either: most low-end GeForce cards can't run it.
    const bool dlss_card = vulkan && VkDeviceInfo::RunsDlss(records[device]);
    if (auto* view = qobject_cast<QListView*>(upscaler_combobox->view())) {
        const int fsr4_row = upscaler_combobox->findData(static_cast<u32>(Aa::Fsr4));
        if (fsr4_row >= 0) {
            view->setRowHidden(fsr4_row, !fsr4_capable && shown_row != fsr4_row);
        }
        const int dlss_row = upscaler_combobox->findData(UPSCALER_DLSS);
        if (dlss_row >= 0) {
            view->setRowHidden(dlss_row, !dlss_card && shown_row != dlss_row);
        }
    }
    UpdatePictureMenu();
}

void ConfigureGraphics::SetupOcarinaUpscalerLabels() {
    if (!FritchiiUi::Enabled()) {
        return;
    }
    if (nvidia_dlss_checkbox != nullptr) {
        connect(nvidia_dlss_checkbox, &QCheckBox::toggled, this,
                [this](bool) { UpdateOcarinaUpscalerLabels(); });
    }
    if (oot_aa_combobox != nullptr) {
        connect(oot_aa_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { UpdateOcarinaUpscalerLabels(); });
    }
    // What DLSS does in OoT, in the quality names (docked; the real sizes go to the status bar
    // and the log once DLSS runs). Which game pass DLSS replaces decides it:
    // - Max Resolution on: OoT renders a fixed 2560x1440 and DLSS replaces its 1440p pass.
    //   Native (DLAA) keeps 1440p, every other quality makes 4K. With FRITCHII_DLSS_RENDER_SCALE
    //   the game renders 1080p (Performance) or 720p (Ultra Performance) first.
    // - Max Resolution off: the game's dynamic resolution (1920x1080 at most in the owner's logs).
    //   DLSS replaces its 3/4 pass, which goes to 1440p: Native (DLAA) keeps the game's size,
    //   the others make 1440p. The size isn't known before the game runs.
    // In handheld it depends on the pass the game picks (with Max Resolution, its 1:1 1080p pass,
    // which only FSR replaces), and with FRITCHII_DLSS_SCALED the sizes follow the resolution
    // scale: the plain names stay.
    if (dlss_quality_combobox != nullptr && Settings::IsDockedMode() &&
        !VideoCommon::OcarinaScaledUpscale()) {
        const QString arrow = QStringLiteral(" %1 ").arg(QChar(0x2192));
        const bool max_resolution = Settings::OcarinaMaxResolutionOn();
        for (int row = 0; row < dlss_quality_combobox->count(); ++row) {
            const auto quality =
                static_cast<Settings::NvidiaDlssQuality>(dlss_quality_combobox->itemData(row).toUInt());
            const bool native = quality == Settings::NvidiaDlssQuality::Native;
            // The picture menu's "Resolution" (oot_render_height, global), or while it is not
            // picked FRITCHII_DLSS_RENDER_SCALE's qualities; and its "Upscale to", as saved.
            const u32 input = max_resolution
                                  ? MenuRenderHeight(Settings::values.oot_render_height.GetValue(),
                                                     false, quality)
                                  : DYNAMIC_INPUT;
            const u32 output = OutputFor(input, Settings::values.oot_upscale_output.GetValue(),
                                         false, PictureVram());
            const QString render = max_resolution ? HeightName(input) : tr("game's size");
            const QString sizes = native || output == SceneHeight(input)
                                      ? render
                                      : render + arrow + HeightName(output);
            dlss_quality_combobox->setItemText(
                row, QStringLiteral("%1: %2").arg(dlss_quality_combobox->itemText(row), sizes));
        }
    }
    UpdateOcarinaUpscalerLabels();
}

void ConfigureGraphics::UpdateOcarinaUpscalerLabels() {
    if (!FritchiiUi::Enabled() || resolution_widget == nullptr) {
        return;
    }
    // An FSR choice for OoT wins over DLSS (VideoCommon::SelectOcarinaUpscaler).
    bool fsr = false;
    if (oot_aa_combobox != nullptr && oot_aa_combobox->currentIndex() >= 0) {
        fsr = VideoCommon::OcarinaFsrChosen(
            static_cast<Settings::OotAntiAliasing>(oot_aa_combobox->currentData().toUInt()));
    }
    // Per game the DLSS checkbox isn't shown: its global value counts.
    const bool dlss = DlssOn();
    // While DLSS or FSR runs, OoT's scale goes to 1x for the session
    // (RasterizerVulkan::ApplyOcarinaResolution; FRITCHII_DLSS_SCALED keeps it). The picture menu
    // hides this row then; elsewhere (handheld, per game) the tooltip says it.
    QString tip = tr("Sharper picture. Higher costs fps and video memory.\nBelow 1080p can look "
                     "blocky.");
    if ((fsr || dlss) && !VideoCommon::OcarinaScaledUpscale()) {
        tip += tr("\nWith FSR or DLSS on, OoT ignores this: the upscaler picks the size.");
    }
    resolution_widget->setToolTip(tip);
    UpdatePictureNote();
}

void ConfigureGraphics::SetupOcarinaTextureRows(const std::map<u32, QWidget*>& rows) {
    const auto find = [&rows](u32 id) -> ConfigurationShared::Widget* {
        const auto it = rows.find(id);
        return it == rows.end() ? nullptr
                                : qobject_cast<ConfigurationShared::Widget*>(it->second);
    };
    // "DLSS jitter": only while DLSS would upscale OoT (its checkbox on and no FSR choice, which
    // wins over DLSS). Per game the DLSS checkbox isn't shown: its global value counts.
    if (ConfigurationShared::Widget* const jitter = find(Settings::values.oot_dlss_jitter.Id())) {
        const auto update = [this, jitter] {
            const bool dlss = DlssOn();
            bool fsr = false;
            if (oot_aa_combobox != nullptr && oot_aa_combobox->currentIndex() >= 0) {
                fsr = VideoCommon::OcarinaFsrChosen(static_cast<Settings::OotAntiAliasing>(
                    oot_aa_combobox->currentData().toUInt()));
            }
            jitter->setVisible(dlss && !fsr);
        };
        if (nvidia_dlss_checkbox != nullptr) {
            connect(nvidia_dlss_checkbox, &QCheckBox::toggled, jitter, update);
        }
        if (oot_aa_combobox != nullptr) {
            connect(oot_aa_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), jitter,
                    update);
        }
        update();
    }
    // The LOD bias value only with Custom, in steps of 0.05.
    ConfigurationShared::Widget* const custom = find(Settings::values.oot_lod_bias_custom.Id());
    if (custom != nullptr && custom->double_spinbox != nullptr) {
        custom->double_spinbox->setSingleStep(0.05);
    }
    ConfigurationShared::Widget* const sharpness =
        find(Settings::values.oot_texture_sharpness.Id());
    texture_sharpness_widget = sharpness;
    lod_custom_widget = custom;
    if (sharpness != nullptr) {
        texture_sharpness_combobox = sharpness->combobox;
    }
    if (texture_sharpness_combobox != nullptr) {
        connect(texture_sharpness_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { UpdateOcarinaTextureRows(); });
    }
    if (oot_aa_combobox != nullptr) {
        connect(oot_aa_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { UpdateOcarinaTextureRows(); });
    }
    if (nvidia_dlss_checkbox != nullptr) {
        connect(nvidia_dlss_checkbox, &QCheckBox::toggled, this,
                [this](bool) { UpdateOcarinaTextureRows(); });
    }
    UpdateOcarinaTextureRows();
}

void ConfigureGraphics::UpdateOcarinaTextureRows() {
    // The texture bias works only with FSR or DLSS (VideoCommon::OcarinaTextureLodBias): Fritchii
    // shows its rows only then, in every mode (handheld and per game too).
    const bool used = !FritchiiUi::Enabled() || OcarinaUpscalerPicked();
    if (texture_sharpness_widget != nullptr) {
        texture_sharpness_widget->setVisible(used);
    }
    if (lod_custom_widget != nullptr && texture_sharpness_combobox != nullptr) {
        lod_custom_widget->setVisible(used &&
                                      texture_sharpness_combobox->currentData().toUInt() ==
                                          static_cast<u32>(Settings::OotTextureSharpness::Custom));
    }
}

ConfigureGraphics::PictureUpscaler ConfigureGraphics::PictureUpscalerNow() const {
    if (output_row == nullptr || oot_aa_combobox == nullptr || oot_aa_combobox->currentIndex() < 0) {
        return PictureUpscaler::None;
    }
    // What runs: an FSR choice wins over DLSS (VideoCommon::SelectOcarinaUpscaler).
    if (VideoCommon::OcarinaFsrChosen(
            static_cast<Settings::OotAntiAliasing>(oot_aa_combobox->currentData().toUInt()))) {
        return PictureUpscaler::Fsr;
    }
    return DlssOn() ? PictureUpscaler::Dlss : PictureUpscaler::None;
}

bool ConfigureGraphics::OcarinaUpscalerPicked() const {
    if (oot_aa_combobox != nullptr && oot_aa_combobox->currentIndex() >= 0 &&
        VideoCommon::OcarinaFsrChosen(
            static_cast<Settings::OotAntiAliasing>(oot_aa_combobox->currentData().toUInt()))) {
        return true;
    }
    // A saved DLSS on a card from before RTX never runs: the game's own upscale stays.
    const VkDeviceInfo::Record* const record = PictureRecord();
    return DlssOn() && (record == nullptr || VkDeviceInfo::RunsDlss(*record));
}

const VkDeviceInfo::Record* ConfigureGraphics::PictureRecord() const {
    const int device = vulkan_device_combobox ? vulkan_device_combobox->currentIndex() : -1;
    const auto index = device >= 0
                           ? static_cast<size_t>(device)
                           : static_cast<size_t>(Settings::values.vulkan_device.GetValue(true));
    return index < records.size() ? &records[index] : nullptr;
}

QString ConfigureGraphics::PictureUpscalerName() const {
#ifdef _WIN32
    const VkDeviceInfo::Record* const record = PictureRecord();
    const bool fsr4_runs = record != nullptr && record->fsr4_capable;
#else
    constexpr bool fsr4_runs = false;
#endif
    if (PictureUpscalerNow() == PictureUpscaler::Fsr && !fsr4_runs) {
        return tr("FSR 3.1"); // A saved FSR 4 runs as FSR 3.1 here.
    }
    return upscaler_combobox != nullptr ? upscaler_combobox->currentText() : tr("The upscaler");
}

u32 ConfigureGraphics::PictureScaleNow() const {
    using Res = Settings::ResolutionSetup;
    // OoT's upscalers set the scale to 1x for the session (RasterizerVulkan::ApplyOcarinaResolution).
    if ((OcarinaUpscalerPicked() && !VideoCommon::OcarinaScaledUpscale()) ||
        resolution_combobox == nullptr || resolution_combobox->currentIndex() < 0) {
        return ScaleKey(Res::Res1X);
    }
    return ScaleKey(static_cast<Res>(resolution_combobox->currentData().toUInt()));
}

u64 ConfigureGraphics::PictureVram() const {
    // Outputs above 4K only where the game would take them (VideoCommon::OcarinaUpscaleOutputFits:
    // the card's largest device-local heap; the renderer checks again).
    const VkDeviceInfo::Record* const record = PictureRecord();
    return record != nullptr ? record->local_memory : 0;
}

u32 ConfigureGraphics::PictureInputNow() const {
    return max_resolution_choice ? render_height_choice : DYNAMIC_INPUT;
}

bool ConfigureGraphics::PictureDlssNative() const {
    return PictureUpscalerNow() == PictureUpscaler::Dlss && dlss_quality_combobox != nullptr &&
           dlss_quality_combobox->currentIndex() >= 0 &&
           static_cast<Settings::NvidiaDlssQuality>(dlss_quality_combobox->currentData().toUInt()) ==
               Settings::NvidiaDlssQuality::Native;
}

u32 ConfigureGraphics::PictureOutputNow() const {
    return OutputFor(PictureInputNow(), upscale_output_choice, PictureDlssNative(), PictureVram());
}

std::vector<u32> ConfigureGraphics::PictureInputsNow() const {
    // The render height, the add-on and the output apply when the game starts: while it runs,
    // only what it started with.
    if (system.IsPoweredOn()) {
        return {PictureInputNow()};
    }
    // Smallest first, like the resolution list without an upscaler; the dynamic size just
    // below the fixed 1080p.
    std::vector<u32> inputs;
    for (const u32 height : Settings::OcarinaRenderHeights) {
        if (height == 1080) {
            inputs.push_back(DYNAMIC_INPUT);
        }
        if (Settings::OcarinaRenderHeightOffered(height)) {
            inputs.push_back(height);
        }
    }
    return inputs;
}

std::vector<u32> ConfigureGraphics::PictureOutputsNow() const {
    const u32 input = PictureInputNow();
    const u32 scene = SceneHeight(input);
    const u64 vram = PictureVram();
    const bool dlss = PictureUpscalerNow() == PictureUpscaler::Dlss;
    std::vector<u32> outputs;
    if (system.IsPoweredOn()) {
        // The output the game started with, and DLSS Native (the DLSS quality applies live).
        const u32 saved = OutputFor(input, upscale_output_saved, false, vram);
        outputs.push_back(saved);
        const std::vector<u32> offered = PictureOutputs(input, vram, dlss);
        if (dlss && saved != scene && std::ranges::find(offered, scene) != offered.end()) {
            outputs.insert(outputs.begin(), scene);
        }
    } else {
        outputs = PictureOutputs(input, vram, dlss);
    }
    // A value set by hand that isn't offered still shows as it runs.
    if (const u32 now = PictureOutputNow(); std::ranges::find(outputs, now) == outputs.end()) {
        outputs.insert(std::ranges::upper_bound(outputs, now), now);
    }
    return outputs;
}

u32 ConfigureGraphics::OfferedOutput(u32 output) const {
    const u32 input = PictureInputNow();
    if (output == SceneHeight(input)) {
        return output; // Native
    }
    const std::vector<u32> outputs =
        PictureOutputs(input, PictureVram(), PictureUpscalerNow() == PictureUpscaler::Dlss);
    if (outputs.empty() || std::ranges::find(outputs, output) != outputs.end()) {
        return output;
    }
    const auto above = std::ranges::lower_bound(outputs, output);
    return above != outputs.end() ? *above : outputs.back();
}

void ConfigureGraphics::ApplyPictureInput(u32 input) {
    const u32 output_before = PictureOutputNow();
    const u32 scene_before = SceneHeight(PictureInputNow());
    // A fixed height needs Max Resolution's fixed scene (the render patch changes its scale).
    max_resolution_choice = input != DYNAMIC_INPUT;
    if (input != DYNAMIC_INPUT) {
        render_height_choice = input;
    }
    // The output stays where the new resolution still makes it, else the nearest it makes.
    const std::vector<u32> outputs =
        PictureOutputs(input, PictureVram(), PictureUpscalerNow() == PictureUpscaler::Dlss);
    u32 output = output_before;
    // A lower Resolution is picked to go faster: an output above the screen only costs fps and is
    // shrunk again, so it comes down to the screen (the largest size that fits it).
    if (const u32 screen = ScreenHeight();
        screen != 0 && !outputs.empty() && SceneHeight(input) < scene_before && output > screen) {
        const auto above = std::ranges::upper_bound(outputs, screen);
        output = above != outputs.begin() ? *std::prev(above) : outputs.front();
    }
    if (!outputs.empty() && std::ranges::find(outputs, output) == outputs.end()) {
        const auto above = std::ranges::lower_bound(outputs, output);
        output = above != outputs.end() ? *above : outputs.back();
    }
    ApplyPictureOutput(output);
}

void ConfigureGraphics::ApplyPictureOutput(u32 output) {
    const u32 input = PictureInputNow();
    const bool native = output == SceneHeight(input);
    if (PictureUpscalerNow() == PictureUpscaler::Dlss && dlss_quality_combobox != nullptr) {
        // Native (DLAA) for the scene's own size, else an upscale: the player's own quality stays
        // (it matters to other games) unless it is Native, or one of FRITCHII_DLSS_RENDER_SCALE's
        // (its render height is saved with oot_render_height now). Streamline picks its mode from
        // the real sizes either way.
        using Quality = Settings::NvidiaDlssQuality;
        const auto current = static_cast<Quality>(dlss_quality_combobox->currentData().toUInt());
        Quality quality = current;
        if (native) {
            quality = Quality::Native;
        } else if (current == Quality::Native ||
                   (Settings::OcarinaDlssRenderScaleSwitch() &&
                    (current == Quality::Performance || current == Quality::UltraPerformance))) {
            quality = Quality::Automatic;
        }
        const int row = dlss_quality_combobox->findData(static_cast<u32>(quality));
        if (row >= 0) {
            dlss_quality_combobox->setCurrentIndex(row);
        }
    }
    // While the game runs only the DLSS quality changes; the output applies at the next start.
    if (!system.IsPoweredOn()) {
        if (OutputFor(input, upscale_output_saved, false, PictureVram()) == output) {
            // Only a change is written: the saved value when it gives this output.
            upscale_output_choice = upscale_output_saved;
        } else {
            upscale_output_choice = output == AutoOutput(input) ? 0U : output;
        }
    }
    UpdatePictureMenu();
}

void ConfigureGraphics::SetupPictureMenu() {
    if (output_row == nullptr || render_row == nullptr) {
        return;
    }
    max_resolution_saved = FritchiiMaxResolution::Saved();
    max_resolution_choice = max_resolution_saved;
    // Not one of the offered heights: Auto, as the game takes it.
    upscale_output_saved = Settings::values.oot_upscale_output.GetValue();
    if (!VideoCommon::OcarinaUpscaleOutputKnown(upscale_output_saved)) {
        upscale_output_saved = 0;
    }
    upscale_output_choice = upscale_output_saved;
    // The render height as the game would start with it (a 0 setting with
    // FRITCHII_DLSS_RENDER_SCALE: the DLSS quality's).
    render_setting_saved = Settings::values.oot_render_height.GetValue();
    render_height_saved = MenuRenderHeight(render_setting_saved,
                                           PictureUpscalerNow() == PictureUpscaler::Fsr,
                                           Settings::values.nvidia_dlss_quality.GetValue());
    render_height_choice = render_height_saved;
    // Turning the upscaler off keeps the add-on as saved: a weak PC that keeps it off must not
    // end up at 1440p by trying an upscaler. The "Max Resolution" row then shows it.
    max_resolution_when_off = max_resolution_saved;
    max_resolution_upscaling = max_resolution_saved;

    connect(max_res_combobox, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        max_resolution_choice = index == 1;
        max_resolution_when_off = max_resolution_choice;
        max_resolution_upscaling = max_resolution_choice;
        UpdatePictureMenu();
    });
    connect(render_combobox, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index >= 0) {
            ApplyPictureInput(render_combobox->itemData(index).toUInt());
        }
    });
    connect(output_combobox, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index >= 0) {
            ApplyPictureOutput(output_combobox->itemData(index).toUInt());
        }
    });
    // Saved with the other add-ons by the global config (Config::SaveDisabledAddOnValues).
    apply_funcs.push_back([this](bool) { FritchiiMaxResolution::Save(max_resolution_choice); });
    apply_funcs.push_back([this](bool) {
        // Only a change is written: a value set by hand stays until a choice is made.
        if (upscale_output_choice != upscale_output_saved) {
            Settings::values.oot_upscale_output.SetValue(upscale_output_choice);
        }
        // A 0 setting with FRITCHII_DLSS_RENDER_SCALE follows the DLSS quality, which the menu
        // may have changed: the height the menu shows is saved.
        if (render_height_choice != render_height_saved ||
            (render_setting_saved == 0 && Settings::OcarinaDlssRenderScaleSwitch())) {
            Settings::values.oot_render_height.SetValue(render_height_choice);
        }
    });
    UpdatePictureMenu();
}

void ConfigureGraphics::UpdatePictureMenu() {
    // The window filter's advice follows the picture (it reads the rows set below).
    const auto filter_note_update = qScopeGuard([this] { UpdateWindowFilterNote(); });
    const auto note_update = qScopeGuard([this] { UpdatePictureNote(); });
    if (output_row == nullptr || render_row == nullptr || resolution_widget == nullptr) {
        return;
    }
    const bool upscaling = PictureUpscalerNow() != PictureUpscaler::None;
    // The scale row while the game's own upscale runs. While an upscaler is on, OoT's scale is 1x
    // (RasterizerVulkan::ApplyOcarinaResolution): "Resolution" is then the size the game draws
    // and "Upscale to" the size the upscaler makes.
    resolution_widget->setVisible(!upscaling);
    render_row->setVisible(upscaling);
    output_row->setVisible(upscaling);
    if (max_res_row != nullptr) {
        max_res_row->setVisible(!upscaling);
        max_res_combobox->setCurrentIndex(max_resolution_choice ? 1 : 0);
        max_res_combobox->setEnabled(!system.IsPoweredOn()); // Applied when the game starts.
    }
    UpdateResolutionNames();
    // The DLSS quality follows the two choices (Native when they match): its row stays hidden
    // (the checkbox's toggle shows it: hide it again).
    if (dlss_quality_widget != nullptr) {
        dlss_quality_widget->setVisible(false);
    }
    UpdateOcarinaTextureRows();
    if (!upscaling) {
        return;
    }
    const bool running = system.IsPoweredOn();
    const u32 input = PictureInputNow();
    const u32 scene = SceneHeight(input);
    const u32 output = PictureOutputNow();

    const QSignalBlocker block_render(render_combobox);
    const QSignalBlocker block_output(output_combobox);
    render_combobox->clear();
    for (const u32 height : PictureInputsNow()) {
        render_combobox->addItem(height == DYNAMIC_INPUT ? tr("Up to 1080p (Max Resolution off)")
                                                         : HeightName(height),
                                 height);
    }
    render_combobox->setCurrentIndex(render_combobox->findData(input));
    render_combobox->setEnabled(render_combobox->count() > 1);
    render_row->setToolTip(running ? tr("Close the game to change it.")
                                   : tr("The size the game draws. Lower runs faster.\n1440p is "
                                        "the game's own size."));

    output_combobox->clear();
    const u32 screen = ScreenHeight();
    for (const u32 height : PictureOutputsNow()) {
        // The scene's own size: anti-aliasing only (DLSS Native, FSR at 1x).
        QString name =
            height == scene ? tr("%1 Native").arg(HeightName(height)) : HeightName(height);
        // Anything bigger is shrunk again to fit: the size that costs no more than it shows.
        if (height == screen) {
            name = tr("%1 (your screen)").arg(name);
        }
        output_combobox->addItem(name, height);
    }
    output_combobox->setCurrentIndex(output_combobox->findData(output));
    output_combobox->setEnabled(output_combobox->count() > 1);
    if (running) {
        output_row->setToolTip(tr("Close the game to pick other sizes."));
    } else if (output_combobox->count() > 1) {
        output_row->setToolTip(tr("The size you see. Bigger is sharper, smaller runs faster."));
    } else {
        output_row->setToolTip(tr("This Resolution makes only this size."));
    }
}

u32 ConfigureGraphics::ScreenHeight() const {
    const QScreen* const screen = window() != nullptr ? window()->screen() : nullptr;
    return screen != nullptr
               ? static_cast<u32>(qRound(screen->size().height() * screen->devicePixelRatio()))
               : 0;
}

bool ConfigureGraphics::MaxResolutionNow() const {
    return output_row != nullptr ? max_resolution_choice : FritchiiMaxResolution::Saved();
}

void ConfigureGraphics::UpdateResolutionNames() {
    if (resolution_combobox == nullptr) {
        return;
    }
    // The scales as the heights OoT draws at them: docked, then handheld. At 1x OoT draws 1440p
    // docked with the Max Resolution add-on (its dynamic resolution, at most 1080p, without it)
    // and 1080p handheld, not the Switch 1 heights (1080p/720p) these rows showed before
    // (2026-11-18: no "1440p" anywhere, and the 0.75x row for weak PCs read "810p").
    using Res = Settings::ResolutionSetup;
    const bool max_resolution = MaxResolutionNow();
    const u32 docked_base = max_resolution ? 1440 : 1080;
    for (int row = 0; row < resolution_combobox->count(); ++row) {
        const auto setup = static_cast<Res>(resolution_combobox->itemData(row).toUInt());
        Settings::ResolutionScalingInfo info{};
        Settings::TranslateResolutionInfo(setup, info);
        const u32 docked = docked_base * info.up_scale >> info.down_shift;
        const u32 handheld = 1080 * info.up_scale >> info.down_shift;
        const QString docked_text =
            max_resolution ? HeightName(docked) : tr("up to %1").arg(HeightName(docked));
        // Without Max Resolution both are 1080p at 1x: one number, not the same one twice.
        resolution_combobox->setItemText(
            row, docked == handheld ? docked_text
                                    : tr("%1 (Handheld %2)").arg(docked_text, HeightName(handheld)));
        // 360p to 8K (owner 2026-11-18); a scale outside it stays only while it is the current
        // one, so nobody's setting vanishes.
        if (auto* view = qobject_cast<QListView*>(resolution_combobox->view())) {
            view->setRowHidden(row, (docked < 360 || docked > 4320) &&
                                        row != resolution_combobox->currentIndex());
        }
    }
}

void ConfigureGraphics::UpdatePictureNote() {
    if (upscale_note == nullptr) {
        return;
    }
    const bool running = system.IsPoweredOn();
    const bool picture_menu = output_row != nullptr && render_row != nullptr;
    const PictureUpscaler upscaler = PictureUpscalerNow();
    const bool scale_changes =
        !running && picture_scale_saved != 0 && PictureScaleNow() != picture_scale_saved;
    QStringList lines;
    QString hint;
    if (picture_menu && upscaler != PictureUpscaler::None) {
        const VkDeviceInfo::Record* const record = PictureRecord();
        if (upscaler == PictureUpscaler::Dlss && record != nullptr && !VkDeviceInfo::RunsDlss(*record)) {
            // A saved choice from another card: the list no longer offers it here.
            lines << tr("This card can't run DLSS. Pick FSR 3.1.");
        } else {
            const u32 input = PictureInputNow();
            const u32 output = PictureOutputNow();
            const QString name = PictureUpscalerName();
            const QString drawn = input == DYNAMIC_INPUT ? tr("up to 1080p") : HeightName(input);
            lines << (output == SceneHeight(input)
                          ? tr("The game draws %1, %2 smooths the edges.").arg(drawn, name)
                          : tr("The game draws %1, %2 makes it %3.")
                                .arg(drawn, name, HeightName(output)));
            // How to go lower (owner 2026-11-18: "make it clear ... you need upscaler OFF"). The
            // lowest height on offer follows OcarinaRenderHeightOffered.
            const auto lowest = std::ranges::find_if(Settings::OcarinaRenderHeights,
                                                     Settings::OcarinaRenderHeightOffered);
            hint = tr("Below %1: set Upscaler to Off.").arg(HeightName(*lowest));
        }
    } else if (picture_menu) {
        // Below 1x with the game's own upscale, the passes OoT draws in compute stay at full size
        // (texture_cache.h, FillImageViews); oot_render_height lowers them too.
        if (resolution_combobox != nullptr && resolution_combobox->currentIndex() >= 0) {
            Settings::ResolutionScalingInfo info{};
            Settings::TranslateResolutionInfo(
                static_cast<Settings::ResolutionSetup>(resolution_combobox->currentData().toUInt()),
                info);
            if (info.up_scale < (1U << info.down_shift)) {
                hint = tr("Part of OoT stays at full size here. An Upscaler lowers all of it.");
            }
        }
    } else if (OcarinaUpscalerPicked() && !VideoCommon::OcarinaScaledUpscale()) {
        // Handheld and per-game settings have no picture menu.
        lines << tr("OoT ignores Resolution while an Upscaler is on.");
        hint = Settings::IsDockedMode() ? tr("Pick the sizes in the main Graphics settings.")
                                        : tr("Dock the console to pick the sizes.");
    }
    if (running) {
        // With DLSS, "Upscale to" still switches Native live.
        const bool live_row = picture_menu && upscaler != PictureUpscaler::None &&
                              output_combobox->isEnabled();
        lines << (live_row ? tr("Close the game to change the rest.")
                           : tr("Close the game to change these."));
    } else {
        if (scale_changes) {
            lines << tr("Shaders get prepared again at the next start.");
        }
        if (!hint.isEmpty() && lines.size() < 2) {
            lines << hint;
        }
    }
    upscale_note->setText(lines.join(QLatin1Char('\n')));
    upscale_note->setVisible(!lines.isEmpty());
}

namespace {
/// One line each: what the window filter does well, then what it does badly.
QString FilterProsCons(Settings::ScalingFilter filter) {
    using F = Settings::ScalingFilter;
    switch (filter) {
    case F::Area:
        return ConfigureGraphics::tr("Cleanest when the picture is bigger than your screen. A bit "
                                     "soft when stretching a smaller one.");
    case F::ScaleForce:
        return ConfigureGraphics::tr("Clean, sharp edges when stretching a smaller picture. "
                                     "Shimmers when shrinking a bigger one.");
    case F::Fsr:
        return ConfigureGraphics::tr("Sharpest when stretching a smaller picture, can look "
                                     "over-sharpened. Shimmers when shrinking.");
    case F::Fsr3:
        return ConfigureGraphics::tr("Like FSR 1 with softer sharpening. Shimmers when "
                                     "shrinking.");
    case F::Bicubic:
    case F::ZeroTangent:
        return ConfigureGraphics::tr("Balanced and sharp when stretching. Shimmers when "
                                     "shrinking.");
    case F::Mitchell:
        return ConfigureGraphics::tr("Smooth with few halos. A little soft.");
    case F::BSpline:
        return ConfigureGraphics::tr("Very smooth. Blurry.");
    case F::Lanczos:
        return ConfigureGraphics::tr("Very sharp. Halos around bright edges, shimmers when "
                                     "shrinking.");
    case F::Spline1:
        return ConfigureGraphics::tr("Sharp and light. Shimmers when shrinking.");
    case F::Gaussian:
        return ConfigureGraphics::tr("Very soft and never shimmers. Blurry.");
    case F::Bilinear:
        return ConfigureGraphics::tr("The cheapest. Soft, and shimmers when shrinking.");
    case F::NearestNeighbor:
        return ConfigureGraphics::tr("Crisp pixels at exactly 2x or 3x. Blocky otherwise.");
    case F::Mmpx:
        return ConfigureGraphics::tr("Made for pixel-art games, not OoT.");
    case F::Sgsr:
    case F::SgsrEdge:
        return ConfigureGraphics::tr("A phone upscaler for Snapdragon chips, not tuned for PCs.");
    case F::Auto:
        break;
    }
    return {};
}
} // Anonymous namespace

void ConfigureGraphics::UpdateWindowFilterNote() {
    if (filter_note == nullptr || scaling_filter_combobox == nullptr ||
        scaling_filter_combobox->currentIndex() < 0) {
        return;
    }
    using F = Settings::ScalingFilter;
    const auto& filters =
        combobox_translations.at(Settings::EnumMetadata<F>::Index());
    const auto row = static_cast<size_t>(scaling_filter_combobox->currentIndex());
    if (row >= filters.size()) {
        filter_note->clear();
        return;
    }
    const auto filter = static_cast<F>(filters[row].first);

    // The picture OoT shows: the upscaler's output, else the resolution's height.
    u32 picture = 0;
    if (PictureUpscalerNow() != PictureUpscaler::None) {
        picture = PictureOutputNow();
    } else if (resolution_combobox != nullptr && resolution_combobox->currentIndex() >= 0) {
        Settings::ResolutionScalingInfo info{};
        Settings::TranslateResolutionInfo(
            static_cast<Settings::ResolutionSetup>(
                resolution_combobox->currentData().toUInt()),
            info);
        const u32 base = Settings::IsDockedMode() && MaxResolutionNow() ? 1440U : 1080U;
        picture = base * info.up_scale >> info.down_shift;
    }
    const u32 screen_height = ScreenHeight();

    // Bigger or smaller by more than 5%, as the Auto filter decides (present.h).
    const bool known = picture != 0 && screen_height != 0;
    const bool bigger = known && picture * 20 > screen_height * 21;
    const bool smaller = known && picture * 21 < screen_height * 20;
    const F best = bigger ? F::Area : F::ScaleForce;
    QString advice;
    if (bigger) {
        advice = tr("Your picture (%1) is bigger than your screen (%2): Area looks best.")
                     .arg(HeightName(picture), HeightName(screen_height));
    } else if (smaller) {
        advice = tr("Your picture (%1) is smaller than your screen (%2): ScaleForce looks best, "
                    "FSR 1 is sharper.")
                     .arg(HeightName(picture), HeightName(screen_height));
    } else if (known) {
        advice = tr("Your picture matches your screen (%1): every filter looks the same.")
                     .arg(HeightName(screen_height));
    }

    QString text;
    if (filter == F::Auto) {
        text = known ? tr("Auto uses %1 for you now, and switches by itself.")
                           .arg(bigger ? tr("Area") : tr("ScaleForce"))
                     : tr("Area when the picture is bigger than your screen, ScaleForce when it "
                          "is smaller.");
    } else {
        text = FilterProsCons(filter);
    }
    if (!advice.isEmpty() && (filter == F::Auto || filter == best || !(bigger || smaller))) {
        text += QStringLiteral("\n") + advice;
    } else if (!advice.isEmpty()) {
        text += QStringLiteral("\n") + advice + QStringLiteral(" ") + tr("Or pick Auto.");
    }
    filter_note->setText(text);
}

void ConfigureGraphics::SetupPlainNames(const std::map<u32, QWidget*>& rows) {
    if (!FritchiiUi::Enabled()) {
        return;
    }
    // Short names and tooltips in plain words: what it does, what it's good for, what it costs.
    const auto set = [&rows](u32 id, const QString& name, const QString& tip) {
        const auto it = rows.find(id);
        if (it == rows.end()) {
            return;
        }
        auto* row = qobject_cast<ConfigurationShared::Widget*>(it->second);
        if (row == nullptr) {
            return;
        }
        row->setToolTip(tip);
        if (name.isEmpty()) {
            return;
        }
        if (row->checkbox != nullptr && row->combobox == nullptr && row->slider == nullptr) {
            row->checkbox->setText(name);
            return;
        }
        // The name label ends with ':' (a slider's value label shows "25%").
        for (QLabel* label : row->findChildren<QLabel*>()) {
            if (label->text().endsWith(QLatin1Char(':'))) {
                label->setText(name);
                break;
            }
        }
    };
    using Settings::values;
    set(values.renderer_backend.Id(), tr("Graphics API:"),
        tr("How the picture is drawn.\nVulkan is fastest. Try OpenGL only if Vulkan fails."));
    set(values.vulkan_device.Id(), tr("Graphics card:"), tr("The graphics card that runs games."));
    set(values.vsync_mode.Id(), tr("VSync:"),
        tr("Stops the picture from tearing.\nOff has the least lag but the picture can tear."));
    set(values.scaling_filter.Id(), tr("Window filter:"),
        tr("How the picture is fitted to your window or screen.\nAuto: Area when the picture is "
           "bigger than your screen, ScaleForce when it is smaller."));
    set(values.game_dlss_network.Id(), tr("Use the game's own DLSS"),
        tr("Switch 2 games with DLSS use the game's own upscaler instead of FSR 3.1.\nCan be slow "
           "on older cards."));
    set(values.fsr_sharpening_slider.Id(), tr("FSR sharpness:"),
        tr("How much FSR sharpens. 0% turns it off.\nToo high looks grainy. Also used by the FSR "
           "window filters."));
    set(values.nvidia_dlss.Id(), tr("NVIDIA DLSS"),
        tr("NVIDIA's upscaler, RTX cards only.\nNeeds NVIDIA's files: use the download button."));
    set(values.nvidia_dlss_preset.Id(), tr("DLSS model:"),
        tr("Which DLSS version to use. Automatic picks one for your card.\nK is the tested one. "
           "Others can look sharper or softer."));
    set(values.nvidia_dlss_quality.Id(), tr("DLSS quality:"),
        tr("Lower quality runs faster but looks softer.\nNative only smooths edges, no "
           "upscaling."));
    set(values.oot_dlss_jitter.Id(), tr("DLSS jitter"),
        tr("Sharper fine detail with DLSS.\nTurn off if the picture shakes."));
    set(values.aspect_ratio.Id(), tr("Aspect ratio:"),
        tr("The picture's shape. 16:9 fits most screens.\nOthers stretch the picture."));
    set(values.anti_aliasing.Id(), tr("Anti-aliasing:"),
        tr("Smooths jagged edges.\nSMAA looks best, FXAA costs less fps."));
    set(values.oot_anti_aliasing.Id(), tr("Upscaler:"),
        tr("Makes a sharp picture from a smaller one, so the game runs faster.\nDLSS: NVIDIA RTX "
           "cards. FSR 4: Radeon RX 6000 and newer. FSR 3.1: any card.\nOoT only. Game keeps the "
           "game's own look."));
    set(values.oot_texture_sharpness.Id(), tr("Texture sharpness:"),
        tr("Sharper textures with FSR or DLSS.\nSharper can make surfaces shimmer."));
    set(values.oot_lod_bias_custom.Id(), tr("Sharpness value:"),
        tr("Lower is sharper.\nToo low makes textures shimmer."));
    set(values.oot_movie_upscale.Id(), tr("Cutscene videos:"),
        tr("How the videos are scaled up.\nThe game's own way looks best but is slow without an "
           "RTX card."));
    set(values.post_shader_enabled.Id(), {},
        tr("Extra picture effects.\nEach effect costs some fps."));
    set(values.fullscreen_mode.Id(), tr("Fullscreen:"),
        tr("Borderless switches windows faster.\nExclusive can lower lag a little."));
    ui->label->setText(tr("Background color:"));
    ui->bg_widget->setToolTip(tr("The color around the picture when it doesn't fill the "
                                 "window."));

    UpdateResolutionNames();
    picture_scale_saved = PictureScaleNow();
    UpdateOcarinaUpscalerLabels();
    if (resolution_combobox != nullptr) {
        connect(resolution_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    UpdateWindowFilterNote();
                    UpdatePictureNote();
                });
    }
    UpdateWindowFilterNote();
}

void ConfigureGraphics::RetrieveVulkanDevices() {
    vulkan_devices.clear();
    vulkan_devices.reserve(records.size());
    device_present_modes.clear();
    device_present_modes.reserve(records.size());
    for (const auto& record : records) {
        vulkan_devices.push_back(QString::fromStdString(record.name));
        device_present_modes.push_back(record.vsync_support);

        if (record.has_broken_compute) {
            expose_compute_option();
        }
    }
}

Settings::RendererBackend ConfigureGraphics::GetCurrentGraphicsBackend() const {
    const auto selected_backend = [&]() {
        auto const index = Settings::EnumMetadata<Settings::RendererBackend>::Index();
        if (!Settings::IsConfiguringGlobal() && !api_restore_global_button->isEnabled())
            return Settings::values.renderer_backend.GetValue(true);
        return index >= combobox_translations.size() || size_t(api_combobox->currentIndex()) >=
                                                            combobox_translations.at(index).size()
                   ? Settings::values.renderer_backend.GetValue()
                   : Settings::RendererBackend(
                         combobox_translations.at(index).at(api_combobox->currentIndex()).first);
    }();

    if (selected_backend == Settings::RendererBackend::Vulkan &&
        UISettings::values.has_broken_vulkan)
        return Settings::RendererBackend::OpenGL_GLSL;
    return selected_backend;
}
