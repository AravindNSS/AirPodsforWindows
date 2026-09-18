//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// GPL-3.0-or-later
//

#include "AapProtocol.h"

#include <algorithm>

namespace Core::AirPods::Aap {
namespace {

constexpr std::array<uint8_t, 7> kNoiseControlPrefix{0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D};

uint8_t ToWireValue(ListeningMode mode)
{
    switch (mode) {
    case ListeningMode::NoiseCancellation:
        return 0x02;
    case ListeningMode::Transparency:
        return 0x03;
    case ListeningMode::Adaptive:
        return 0x04;
    }
    return 0;
}

std::optional<ListeningMode> FromWireValue(uint8_t value)
{
    switch (value) {
    case 0x02:
        return ListeningMode::NoiseCancellation;
    case 0x03:
        return ListeningMode::Transparency;
    case 0x04:
        return ListeningMode::Adaptive;
    default:
        return std::nullopt;
    }
}

} // namespace

HandshakePacket MakeHandshake()
{
    return {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

FeaturePacket MakeFeatureEnable()
{
    return {0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

NotificationRequestPacket MakeNotificationRequest()
{
    return {0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFE, 0xFF};
}

NoiseControlPacket MakeNoiseControlCommand(ListeningMode mode)
{
    return {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, ToWireValue(mode), 0x00, 0x00, 0x00};
}

std::optional<ListeningMode> ParseNoiseControlPacket(std::span<const uint8_t> packet)
{
    if (packet.size() != NoiseControlPacket{}.size() ||
        !std::equal(kNoiseControlPrefix.begin(), kNoiseControlPrefix.end(), packet.begin()) ||
        packet[8] != 0 || packet[9] != 0 || packet[10] != 0)
    {
        return std::nullopt;
    }
    return FromWireValue(packet[7]);
}

std::vector<ListeningMode> NoiseControlStreamDecoder::Push(std::span<const uint8_t> bytes)
{
    std::vector<ListeningMode> result;
    if (bytes.empty()) {
        return result;
    }

    // Keep the diagnostic decoder bounded even if a malformed transport delivers an
    // unexpectedly large read. Retaining the newest bytes also preserves a packet prefix that
    // may continue in the next read.
    if (bytes.size() >= kMaximumPacketSize) {
        _buffer.assign(bytes.end() - static_cast<std::ptrdiff_t>(kMaximumPacketSize), bytes.end());
    }
    else {
        const auto combinedSize = _buffer.size() + bytes.size();
        if (combinedSize > kMaximumPacketSize) {
            const auto overflow = combinedSize - kMaximumPacketSize;
            _buffer.erase(
                _buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(overflow));
        }
        _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
    }

    size_t cursor = 0;
    while (cursor + kNoiseControlPrefix.size() <= _buffer.size()) {
        const auto prefix = std::search(
            _buffer.begin() + static_cast<std::ptrdiff_t>(cursor), _buffer.end(),
            kNoiseControlPrefix.begin(), kNoiseControlPrefix.end());
        if (prefix == _buffer.end()) {
            break;
        }

        const auto offset = static_cast<size_t>(std::distance(_buffer.begin(), prefix));
        if (_buffer.size() - offset < NoiseControlPacket{}.size()) {
            cursor = offset;
            break;
        }

        const auto packet = std::span<const uint8_t>{_buffer}.subspan(offset, NoiseControlPacket{}.size());
        if (const auto mode = ParseNoiseControlPacket(packet); mode.has_value()) {
            result.push_back(*mode);
        }
        cursor = offset + NoiseControlPacket{}.size();
    }

    if (cursor > 0) {
        _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(cursor));
    }

    // When no full prefix exists, retain only enough trailing bytes to complete one later.
    if (_buffer.size() > NoiseControlPacket{}.size() - 1) {
        _buffer.erase(
            _buffer.begin(),
            _buffer.end() - static_cast<std::ptrdiff_t>(NoiseControlPacket{}.size() - 1));
    }
    return result;
}

void NoiseControlStreamDecoder::Reset()
{
    _buffer.clear();
}

size_t NoiseControlStreamDecoder::BufferedSize() const
{
    return _buffer.size();
}

} // namespace Core::AirPods::Aap
