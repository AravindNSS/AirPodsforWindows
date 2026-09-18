//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// GPL-3.0-or-later
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ListeningMode.h"

namespace Core::AirPods::Aap {

constexpr uint16_t kL2capPsm{0x1001};
constexpr size_t kMaximumPacketSize{512};

using HandshakePacket = std::array<uint8_t, 16>;
using FeaturePacket = std::array<uint8_t, 14>;
using NotificationRequestPacket = std::array<uint8_t, 10>;
using NoiseControlPacket = std::array<uint8_t, 11>;

HandshakePacket MakeHandshake();
FeaturePacket MakeFeatureEnable();
NotificationRequestPacket MakeNotificationRequest();
NoiseControlPacket MakeNoiseControlCommand(ListeningMode mode);
std::optional<ListeningMode> ParseNoiseControlPacket(std::span<const uint8_t> packet);

// AAP is a byte stream. This decoder recognizes complete noise-control notifications while
// retaining incomplete prefixes between reads and discarding unrelated notifications safely.
class NoiseControlStreamDecoder
{
public:
    std::vector<ListeningMode> Push(std::span<const uint8_t> bytes);
    void Reset();
    [[nodiscard]] size_t BufferedSize() const;

private:
    std::vector<uint8_t> _buffer;
};

} // namespace Core::AirPods::Aap
