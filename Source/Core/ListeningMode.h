//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#pragma once

#include <cstdint>
#include <optional>

#include <QMetaType>

namespace Core::AirPods {

enum class ListeningMode : uint8_t {
    Transparency,
    Adaptive,
    NoiseCancellation,
};

enum class ControlAvailability : uint8_t {
    Unavailable,
    Connecting,
    Ready,
    Faulted,
};

enum class ListeningModeError : uint8_t {
    None,
    UnsupportedModel,
    TransportUnavailable,
    ConnectionFailed,
    CommandRejected,
    ConfirmationTimedOut,
    ProtocolError,
    Disconnected,
};

struct ListeningCapabilities {
    bool transparency{false};
    bool adaptive{false};
    bool noiseCancellation{false};
    bool transportCompatible{false};
    uint32_t transportVersion{0};

    [[nodiscard]] bool Supports(ListeningMode mode) const
    {
        switch (mode) {
        case ListeningMode::Transparency:
            return transparency;
        case ListeningMode::Adaptive:
            return adaptive;
        case ListeningMode::NoiseCancellation:
            return noiseCancellation;
        }
        return false;
    }

    [[nodiscard]] bool Any() const
    {
        return transparency || adaptive || noiseCancellation;
    }

    bool operator==(const ListeningCapabilities &) const = default;
};

struct ListeningModeState {
    ControlAvailability availability{ControlAvailability::Unavailable};
    ListeningCapabilities capabilities;
    std::optional<ListeningMode> confirmedMode;
    std::optional<ListeningMode> pendingMode;
    ListeningModeError error{ListeningModeError::None};

    bool operator==(const ListeningModeState &) const = default;
};

} // namespace Core::AirPods

Q_DECLARE_METATYPE(Core::AirPods::ListeningMode)
Q_DECLARE_METATYPE(Core::AirPods::ListeningModeError)
Q_DECLARE_METATYPE(Core::AirPods::ListeningCapabilities)
Q_DECLARE_METATYPE(Core::AirPods::ListeningModeState)
