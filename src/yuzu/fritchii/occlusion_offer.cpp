// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/occlusion_offer.h"

#include <atomic>
#include <cstdlib>
#include <string>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "common/logging.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "video_core/fritchii_occl_flag.h"
#include "yuzu/configuration/shared_widget.h"
#include "yuzu/fritchii/fritchii_ui.h"

namespace FritchiiOcclusionOffer {
namespace {

constexpr const char* Pending = "pending";

/// Whether Full can act on the picked card, from NoteCards (true until then).
bool g_full_can_act = true;

bool StartsWithOne(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

/// One follower at boot as VideoCommon::OneFollower::ReadSwitches decides it: wanted with
/// FRITCHII_ONEF_LEAN=1 (built in) or FRITCHII_ONEF=1; on AMD's proprietary driver only with
/// FRITCHII_ONEF_AMD=1 too. The occlusion flags act only where it is off.
bool OneFollowerOn(bool amd_proprietary) {
    const bool wanted = StartsWithOne("FRITCHII_ONEF_LEAN") || StartsWithOne("FRITCHII_ONEF");
    return wanted && (!amd_proprietary || StartsWithOne("FRITCHII_ONEF_AMD"));
}

/// A switch sets FRITCHII_OCCL_FLAG (it wins over the setting: a test runs).
bool SwitchSetsFlag() {
    const char* const value = std::getenv("FRITCHII_OCCL_FLAG");
    return value != nullptr && value[0] != '\0';
}

QLabel* Label(const QString& text, const char* object_name) {
    auto* label = new QLabel(text);
    label->setObjectName(QLatin1String(object_name));
    label->setWordWrap(true);
    return label;
}

/// Turn it on or Not now. Closing the window counts as Not now.
bool AskToTurnOn(QWidget* parent) {
    const QString title = QObject::tr("Faster mode for AMD");
    const QString lead = QObject::tr("Can give more fps on AMD cards.");
    const QString later = QObject::tr("Change it any time in Graphics > Advanced.");
    const QString on_text = QObject::tr("Turn it on");
    const QString off_text = QObject::tr("Not now");

    if (!FritchiiUi::Enabled()) {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Fritchii"), title,
                        QMessageBox::NoButton, parent);
        box.setInformativeText(lead + QStringLiteral("\n\n") + later);
        QPushButton* const turn_on = box.addButton(on_text, QMessageBox::AcceptRole);
        QPushButton* const not_now = box.addButton(off_text, QMessageBox::RejectRole);
        box.setDefaultButton(turn_on);
        box.setEscapeButton(not_now);
        box.exec();
        return box.clickedButton() == turn_on;
    }

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("FritchiiOcclPrompt"));
    dialog.setWindowTitle(QStringLiteral("Fritchii"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setMinimumWidth(520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(40, 34, 40, 30);
    layout->setSpacing(14);
    layout->addWidget(Label(title, "FritchiiDialogTitle"));
    layout->addWidget(Label(lead, "FritchiiLead"));
    layout->addWidget(Label(later, "FritchiiLead"));
    layout->addSpacing(14);
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    row->addStretch(1);
    auto* not_now = new QPushButton(off_text);
    not_now->setAutoDefault(false);
    auto* turn_on = new QPushButton(on_text);
    turn_on->setDefault(true); // the violet filled button (style.qss, QPushButton:default)
    QObject::connect(not_now, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(turn_on, &QPushButton::clicked, &dialog, &QDialog::accept);
    row->addWidget(not_now);
    row->addWidget(turn_on);
    layout->addLayout(row);
    return dialog.exec() == QDialog::Accepted;
}

} // Anonymous namespace

void NoteCards(const std::vector<VkDeviceInfo::Record>& records) {
    if (records.empty()) {
        return; // unknown: Full stays offered
    }
    const size_t index = VkDeviceInfo::PickedIndex(
        records, Settings::values.vulkan_device.GetValue(true),
        Settings::values.vulkan_device_name.GetValue(true));
    const bool amd = index < records.size() && records[index].amd_proprietary;
    g_full_can_act = !OneFollowerOn(amd);
}

bool FullCanAct() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan &&
           g_full_can_act;
}

void LimitRow(QWidget* row_widget) {
    auto* const row = qobject_cast<ConfigurationShared::Widget*>(row_widget);
    if (row == nullptr || row->combobox == nullptr || FullCanAct()) {
        return;
    }
    const int full = row->combobox->findData(static_cast<u32>(Settings::OcclusionSpeedup::Full));
    auto* const model = qobject_cast<QStandardItemModel*>(row->combobox->model());
    if (model == nullptr || full < 0) {
        return;
    }
    if (QStandardItem* const item = model->item(full)) {
        item->setEnabled(false);
    }
    row->setToolTip(row->toolTip() + QStringLiteral("\n") +
                    QObject::tr("Does nothing on this card: it already uses another fast path."));
}

bool NoteSession() {
    if (!VideoCommon::OcclFlag::g_full_could_help.exchange(false, std::memory_order_relaxed)) {
        return false;
    }
#ifndef _WIN32
    return false; // offered on AMD's Windows driver only
#else
    auto& state = UISettings::values.fritchii_occl_offer;
    if (!state.GetValue().empty() || SwitchSetsFlag() ||
        Settings::values.occlusion_speedup.GetValue() != Settings::OcclusionSpeedup::Standard) {
        return false;
    }
    state.SetValue(Pending);
    LOG_INFO(Frontend, "Occlusion speed-up: this game could use Full; offered once when calm");
    return true;
#endif
}

bool OfferOnce(QWidget* parent, bool after_game) {
    const QByteArray mode = qgetenv("FRITCHII_OCCL_OFFER");
    if (mode == "0") {
        return false;
    }
    auto& state = UISettings::values.fritchii_occl_offer;
    const bool forced = mode == "ask";
    if (!forced && state.GetValue() != Pending) {
        return false;
    }
    if (QApplication::activeModalWidget() != nullptr) {
        return false; // another window is up: the next calm moment asks
    }
    const auto& setting = Settings::values.occlusion_speedup;
    if (!forced && setting.GetValue() != Settings::OcclusionSpeedup::Standard) {
        // The player picked Off or Full in the settings since: nothing to ask.
        state.SetValue("skipped (setting changed)");
        LOG_INFO(Frontend, "Occlusion speed-up offer: not shown, the setting was changed");
        return true;
    }
    const bool on = AskToTurnOn(parent);
    if (on) {
        Settings::values.occlusion_speedup.SetValue(Settings::OcclusionSpeedup::Full);
    }
    state.SetValue(on ? "on" : "not now");
    LOG_INFO(Frontend, "Occlusion speed-up offer: {} by the player ({})",
             on ? "Full turned on" : "not now",
             forced ? "FRITCHII_OCCL_OFFER=ask" : after_game ? "after a game" : "at launch");
    return true;
}

} // namespace FritchiiOcclusionOffer
