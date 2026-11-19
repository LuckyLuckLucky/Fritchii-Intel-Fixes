// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

class QWidget;

namespace VkDeviceInfo {
class Record;
}

// "OoT occlusion speed-up" (Settings::values.occlusion_speedup, video_core/fritchii_occl_flag.h):
// Standard by default; Full adds more and is offered once to players whose game ran it where it
// helps (AMD's Windows driver, one follower off). The offer comes after a game ends, or at the
// next plain launch when the app was closed with the game; never while a game runs.
// UISettings fritchii_occl_offer: empty, "pending" (a game could use Full), or the answer.
//
// FRITCHII_OCCL_OFFER: 0 never asks; "ask" asks at every plain launch on any PC (screenshots).
namespace FritchiiOcclusionOffer {

/// The main window's cards, read once at launch: whether Full can act on the picked one.
void NoteCards(const std::vector<VkDeviceInfo::Record>& records);

/// Whether Full can act on this PC: the picked card runs AMD's Windows driver with one follower
/// off (FRITCHII_ONEF_AMD not 1), or another card with one follower turned off by a switch.
[[nodiscard]] bool FullCanAct();

/// The setting's row (a ConfigurationShared::Widget): Full greyed out where it can't act, with
/// one short line in the tooltip.
void LimitRow(QWidget* row);

/// After a game (MainWindow::OnEmulationStopped): marks the offer pending when that game could
/// use Full. True when it changed the config (the caller saves it).
bool NoteSession();

/// Asks once when the offer is pending (or FRITCHII_OCCL_OFFER=ask), unless another window is
/// open (then it waits for the next calm moment). True when it recorded an answer.
bool OfferOnce(QWidget* parent, bool after_game);

} // namespace FritchiiOcclusionOffer
