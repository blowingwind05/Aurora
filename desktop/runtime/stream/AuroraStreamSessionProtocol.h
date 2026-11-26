// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Aurora {
namespace StreamSession {

constexpr int TransportLost = 76;
constexpr int RouteSwitched = 77;
constexpr char PlaybackReady[] = "aurora-playback/1,ready";
constexpr char PinnedControlAvailable[] = "aurora-playback/1,pinned-control";
constexpr char SwitchReady[] = "aurora-playback/1,switch-ready";
constexpr char SwitchDenied[] = "aurora-playback/1,switch-denied";
constexpr char SwitchRequest[] = "aurora-route-switch/1,stop\n";
constexpr char SwitchChannel[] = "AURORA_STREAM_ROUTE_SWITCH";
constexpr char DisplayChannel[] = "AURORA_STREAM_DISPLAY_CONTROL";
constexpr char DisplayPause[] = "aurora-display/1,pause\n";
constexpr char DisplayPaused[] = "aurora-display/1,paused";
constexpr char DisplayPauseDenied[] = "aurora-display/1,pause-denied";
constexpr char DisplayPresent[] = "aurora-display/1,present,";
constexpr char DisplayRelease[] = "aurora-display/1,release\n";

} // namespace StreamSession
} // namespace Aurora
