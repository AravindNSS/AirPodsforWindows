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

#include <memory>
#include <optional>

#include <QObject>
#include <QTimer>

#include "Base.h"
#include "ListeningMode.h"

namespace Core::AirPods {

// The transport boundary intentionally exposes semantic operations, not raw AAP bytes. The
// production implementation will talk to the privileged broker; tests and debug builds can use a
// simulator without changing the controller or UI.
class IAirPodsControlTransport : public QObject
{
    Q_OBJECT

public:
    explicit IAirPodsControlTransport(QObject *parent = nullptr) : QObject{parent} {}
    ~IAirPodsControlTransport() override = default;

    virtual void Connect(Model model, uint64_t sessionId) = 0;
    virtual void Disconnect(uint64_t sessionId) = 0;
    virtual void SetListeningMode(ListeningMode mode, uint64_t sessionId) = 0;

Q_SIGNALS:
    void Ready(uint64_t sessionId, const Core::AirPods::ListeningCapabilities &capabilities);
    void ModeConfirmed(uint64_t sessionId, Core::AirPods::ListeningMode mode);
    void Failed(uint64_t sessionId, Core::AirPods::ListeningModeError error);
    void Closed(uint64_t sessionId);
};

class UnavailableControlTransport final : public IAirPodsControlTransport
{
public:
    using IAirPodsControlTransport::IAirPodsControlTransport;

    void Connect(Model model, uint64_t sessionId) override;
    void Disconnect(uint64_t sessionId) override;
    void SetListeningMode(ListeningMode mode, uint64_t sessionId) override;
};

#if defined APD_DEBUG
class SimulatedControlTransport final : public IAirPodsControlTransport
{
public:
    using IAirPodsControlTransport::IAirPodsControlTransport;

    void Connect(Model model, uint64_t sessionId) override;
    void Disconnect(uint64_t sessionId) override;
    void SetListeningMode(ListeningMode mode, uint64_t sessionId) override;
};
#endif

class ListeningModeController final : public QObject
{
    Q_OBJECT

public:
    explicit ListeningModeController(
        std::unique_ptr<IAirPodsControlTransport> transport, QObject *parent = nullptr,
        int confirmationTimeoutMs = 3000);

    void SetDevice(Model model, bool connected);
    void RequestMode(ListeningMode mode);
    [[nodiscard]] const ListeningModeState &State() const;

Q_SIGNALS:
    void StateChanged(const Core::AirPods::ListeningModeState &state);

private:
    std::unique_ptr<IAirPodsControlTransport> _transport;
    ListeningModeState _state;
    Model _model{Model::Unknown};
    bool _connected{false};
    uint64_t _sessionId{0};
    std::optional<ListeningMode> _inFlightMode;
    std::optional<ListeningMode> _queuedMode;
    QTimer _confirmationTimer;
    QTimer _retryTimer;

    void Publish();
    void Send(ListeningMode mode);
    void BeginConnect();
    void OnReady(uint64_t sessionId, const ListeningCapabilities &capabilities);
    void OnConfirmed(uint64_t sessionId, ListeningMode mode);
    void OnFailed(uint64_t sessionId, ListeningModeError error);
    void OnClosed(uint64_t sessionId);
    void OnConfirmationTimedOut();
    void ScheduleReconnect();
};

std::unique_ptr<IAirPodsControlTransport> CreateControlTransport(QObject *parent = nullptr);

} // namespace Core::AirPods
