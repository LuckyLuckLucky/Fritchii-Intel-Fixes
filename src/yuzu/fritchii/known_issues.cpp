// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/known_issues.h"

#include <string>

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/thread.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "yuzu/fritchii/cache_cleanup.h"
#include "yuzu/fritchii/fritchii_ui.h"

namespace FritchiiKnownIssues {

namespace {

struct Item {
    QString lead;
    QString text;
};

/// The card the game runs on is NVIDIA (DLSS presets only matter there).
bool NvidiaCard(const std::vector<VkDeviceInfo::Record>& records) {
    const auto index = static_cast<size_t>(Settings::values.vulkan_device.GetValue());
    return index < records.size() && records[index].nvidia;
}

/// Only what current builds show (owner 2026-11-18: no outdated items).
std::vector<Item> Items(const std::vector<VkDeviceInfo::Record>& records) {
    std::vector<Item> items;
    // v6.1 tied the GPU threads to the P-cores, which halved fps on some 12th to 14th gen Intel
    // CPUs; 6.2 lets Windows place them and turns off a leftover EDEN_HYBRID_CORES=0 (main.cpp).
    if (Common::IntelHybridCpu()) {
        items.push_back({QObject::tr("Fixed in 6.2: Intel 12th to 14th gen"),
                         QObject::tr("The fps drop on Intel CPUs with P-cores and E-cores is fixed. "
                                     "You don't need EDEN_HYBRID_CORES=0 any more: if you had "
                                     "added it, Fritchii turned it off for you.")});
    }
    items.insert(items.end(), {
        {QObject::tr("Killing enemies"),
         QObject::tr("Short hitches can still happen (fewer than before).")},
        {QObject::tr("Unlimited fps with VSync off"),
         QObject::tr("The game can crash right after a loading screen. Keep an fps cap, or "
                     "VSync on.")},
        {QObject::tr("Cutting lots of flowers"),
         QObject::tr("The game can slow down for about half a minute, then recovers.")},
        {QObject::tr("New areas"),
         QObject::tr("The first visit can show brief pop-in or a short stutter while shaders "
                     "build. It's smooth after.")},
    });
    if (NvidiaCard(records)) {
        items.push_back({QObject::tr("DLSS presets L and M"),
                         QObject::tr("They look great but cost a lot of fps. Automatic (K) is the "
                                     "tested default.")});
    }
    items.push_back({QObject::tr("Mods"),
                     QObject::tr("A mod that changes the same game code as Fritchii can turn off "
                                 "high refresh. Ctrl+U tells you which one.")});
    items.push_back({QObject::tr("Weaker PCs and Steam Deck"),
                     QObject::tr("Keep Max Resolution off and pick a lower Resolution in "
                                 "Graphics, like 810p.")});
    // A tester's 16 GB PC (test 10): free RAM fell to 0.5 GB and Windows paging froze the game
    // for up to 12 s (OoT's own memory is ~10 GB, Fritchii's ~2.7 GB more).
    items.push_back({QObject::tr("16 GB of RAM"),
                     QObject::tr("Close your browser and other programs while playing. When memory "
                                 "runs out, the game freezes for seconds.")});
    // Compute-written images go back to the game's size at every dispatch, so a lower scale only
    // shrinks the plain draws (2026-11-18). oot_render_height, with an upscaler, lowers them too.
    items.push_back({QObject::tr("Lower resolutions"),
                     QObject::tr("With the Upscaler off, going below 1080p gives less fps than "
                                 "you'd expect, because part of OoT's drawing stays at full size. "
                                 "With FSR or DLSS, Resolution 720p lowers all of it.")});
    return items;
}

/// The first showing can't be dismissed before the countdown ends (owner 2026-11-18: players
/// must actually read it): Esc and the window's X do nothing until "Got it" unlocks.
class ReadFirstDialog final : public QDialog {
public:
    using QDialog::QDialog;
    bool locked = false;

    void reject() override {
        if (!locked) {
            QDialog::reject();
        }
    }
};

/// Seconds the first showing keeps "Got it" locked: about one per line.
constexpr int ReadSeconds = 8;

QLabel* Label(const QString& text, const char* object_name) {
    auto* label = new QLabel(text);
    label->setObjectName(QLatin1String(object_name));
    label->setWordWrap(true);
    return label;
}

} // Anonymous namespace

void ShowImpl(QWidget* parent, const std::vector<VkDeviceInfo::Record>& records, bool first) {
    const auto items = Items(records);
    const QString title = QObject::tr("Good to know: very important, please read");
    const QString footer = QObject::tr("Anything else: Help > Export Log, then send it to us.");

    if (!FritchiiUi::Enabled()) {
        QStringList lines;
        for (const Item& item : items) {
            lines.append(QStringLiteral("%1: %2").arg(item.lead, item.text));
        }
        QMessageBox box(QMessageBox::Information, title, lines.join(QStringLiteral("\n\n")),
                        QMessageBox::Ok, parent);
        box.setInformativeText(footer);
        if (first) {
            // The same lock as the Fritchii look: OK stays disabled for the countdown.
            QAbstractButton* const ok = box.button(QMessageBox::Ok);
            ok->setEnabled(false);
            QTimer::singleShot(ReadSeconds * 1000, &box, [ok] { ok->setEnabled(true); });
        }
        box.exec();
        return;
    }

    ReadFirstDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("FritchiiKnownIssues"));
    dialog.setWindowTitle(QObject::tr("Fritchii: very important, please read"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setMinimumWidth(640);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(40, 34, 40, 30);
    layout->setSpacing(14);
    layout->addWidget(Label(title, "FritchiiDialogTitle"));
    for (const Item& item : items) {
        auto* row = new QVBoxLayout;
        row->setSpacing(2);
        row->addWidget(Label(item.lead, "FritchiiKnownIssuesLead"));
        row->addWidget(Label(item.text, "FritchiiLead"));
        layout->addLayout(row);
    }
    layout->addSpacing(6);
    layout->addWidget(Label(footer, "FritchiiKnownIssuesFooter"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* ok = new QPushButton(QObject::tr("Got it"));
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(ok);
    layout->addLayout(buttons);
    QTimer countdown;
    int left = ReadSeconds;
    if (first) {
        dialog.locked = true;
        ok->setEnabled(false);
        ok->setText(QObject::tr("Got it (%1)").arg(left));
        countdown.setInterval(1000);
        QObject::connect(&countdown, &QTimer::timeout, &dialog, [&] {
            if (--left > 0) {
                ok->setText(QObject::tr("Got it (%1)").arg(left));
                return;
            }
            countdown.stop();
            dialog.locked = false;
            ok->setText(QObject::tr("Got it"));
            ok->setEnabled(true);
            ok->setFocus();
        });
        countdown.start();
    }
    dialog.exec();
}

void Show(QWidget* parent, const std::vector<VkDeviceInfo::Record>& records) {
    ShowImpl(parent, records, false);
}

bool ShowOnce(QWidget* parent, const std::vector<VkDeviceInfo::Record>& records) {
    // Once per release (6.2: the major version alone kept 6.1 players from seeing it again).
    std::string version{Common::g_build_version};
    if (const auto end = version.find(' '); end != std::string::npos) {
        version.resize(end);
    }
    if (version.empty() || FritchiiCacheCleanup::MajorVersion().empty()) {
        return false;
    }
    const QByteArray mode = qgetenv("FRITCHII_KNOWN_ISSUES");
    if (mode == "0") {
        return false;
    }
    auto& shown = UISettings::values.fritchii_known_issues_shown;
    if (shown.GetValue() == version && mode != "ask") {
        return false;
    }
    ShowImpl(parent, records, true);
    shown.SetValue(version);
    LOG_INFO(Frontend, "Known issues ({}): shown once for this version", version);
    return true;
}

} // namespace FritchiiKnownIssues
