//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//

#include "ListeningModeControl.h"

#include <utility>

namespace Core::AirPods {

void UnavailableControlTransport::Connect(Model model, uint64_t sessionId)
{
    Q_UNUSED(model);
    emit Failed(sessionId, ListeningModeError::TransportUnavailable);
}

void UnavailableControlTransport::Disconnect(uint64_t sessionId)
{
    emit Closed(sessionId);
}

void UnavailableControlTransport::SetListeningMode(ListeningMode mode, uint64_t sessionId)
{
    Q_UNUSED(mode);
    emit Failed(sessionId, ListeningModeError::TransportUnavailable);
}

#if defined APD_DEBUG
void SimulatedControlTransport::Connect(Model model, uint64_t sessionId)
{
    QTimer::singleShot(75, this, [this, model, sessionId] {
        if (model != Model::AirPods_Pro_3) {
            emit Failed(sessionId, ListeningModeError::UnsupportedModel);
            return;
        }
        emit Ready(sessionId, {
            .transparency = true,
            .adaptive = true,
            .noiseCancellation = true,
            .transportVersion = 1,
        });
        emit ModeConfirmed(sessionId, ListeningMode::Adaptive);
    });
}

void SimulatedControlTransport::Disconnect(uint64_t sessionId)
{
    emit Closed(sessionId);
}

void SimulatedControlTransport::SetListeningMode(ListeningMode mode, uint64_t sessionId)
{
    QTimer::singleShot(
        180, this, [this, mode, sessionId] { emit ModeConfirmed(sessionId, mode); });
}
#endif

ListeningModeController::ListeningModeController(
    std::unique_ptr<IAirPodsControlTransport> transport, QObject *parent,
    int confirmationTimeoutMs)
    : QObject{parent}, _transport{std::move(transport)}
{
    Q_ASSERT(_transport != nullptr);
    Q_ASSERT(confirmationTimeoutMs > 0);
    qRegisterMetaType<ListeningCapabilities>();
    qRegisterMetaType<ListeningModeState>();
    qRegisterMetaType<ListeningMode>();
    qRegisterMetaType<ListeningModeError>();
    _transport->setParent(this);
    _confirmationTimer.setSingleShot(true);
    _confirmationTimer.setInterval(confirmationTimeoutMs);
    _retryTimer.setSingleShot(true);
    _retryTimer.setInterval(1000);

    connect(
        _transport.get(), &IAirPodsControlTransport::Ready, this,
        &ListeningModeController::OnReady);
    connect(
        _transport.get(), &IAirPodsControlTransport::ModeConfirmed, this,
        &ListeningModeController::OnConfirmed);
    connect(
        _transport.get(), &IAirPodsControlTransport::Failed, this,
        &ListeningModeController::OnFailed);
    connect(
        _transport.get(), &IAirPodsControlTransport::Closed, this,
        &ListeningModeController::OnClosed);
    connect(
        &_confirmationTimer, &QTimer::timeout, this,
        &ListeningModeController::OnConfirmationTimedOut);
    connect(&_retryTimer, &QTimer::timeout, this, [this] {
        if (_connected && _model == Model::AirPods_Pro_3 &&
            _state.availability == ControlAvailability::Connecting)
        {
            BeginConnect();
        }
    });
}

void ListeningModeController::SetDevice(Model model, bool connected)
{
    _connected = false;
    const auto oldSessionId = _sessionId++;
    _confirmationTimer.stop();
    _retryTimer.stop();
    _inFlightMode.reset();
    _queuedMode.reset();
    _transport->Disconnect(oldSessionId);
    _model = model;
    _state = {};
    _connected = connected;

    if (!connected) {
        Publish();
        return;
    }
    if (model != Model::AirPods_Pro_3) {
        _state.error = ListeningModeError::UnsupportedModel;
        Publish();
        return;
    }

    _state.availability = ControlAvailability::Connecting;
    Publish();
    BeginConnect();
}

void ListeningModeController::RequestMode(ListeningMode mode)
{
    if (_state.availability != ControlAvailability::Ready ||
        !_state.capabilities.Supports(mode))
    {
        return;
    }

    if (_state.pendingMode.has_value()) {
        _queuedMode = mode;
        _state.pendingMode = mode;
        Publish();
        return;
    }
    Send(mode);
}

const ListeningModeState &ListeningModeController::State() const
{
    return _state;
}

void ListeningModeController::Publish()
{
    emit StateChanged(_state);
}

void ListeningModeController::Send(ListeningMode mode)
{
    _inFlightMode = mode;
    _state.pendingMode = mode;
    _state.error = ListeningModeError::None;
    Publish();
    _confirmationTimer.start();
    _transport->SetListeningMode(mode, _sessionId);
}

void ListeningModeController::BeginConnect()
{
    ++_sessionId;
    _transport->Connect(_model, _sessionId);
}

void ListeningModeController::OnReady(
    uint64_t sessionId, const ListeningCapabilities &capabilities)
{
    if (sessionId != _sessionId || _state.availability != ControlAvailability::Connecting) {
        return;
    }
    if (_model != Model::AirPods_Pro_3 || !capabilities.Any()) {
        OnFailed(sessionId, ListeningModeError::ProtocolError);
        return;
    }
    _state.availability = ControlAvailability::Ready;
    _state.capabilities = capabilities;
    _state.error = ListeningModeError::None;
    Publish();
}

void ListeningModeController::OnConfirmed(uint64_t sessionId, ListeningMode mode)
{
    if (sessionId != _sessionId || _state.availability != ControlAvailability::Ready) {
        return;
    }

    _state.confirmedMode = mode;
    _state.error = ListeningModeError::None;

    // Stem-originated and out-of-order notifications are authoritative device state, but they do
    // not complete a different command that is still awaiting its matching confirmation.
    if (_inFlightMode.has_value() && *_inFlightMode != mode) {
        Publish();
        return;
    }

    _confirmationTimer.stop();
    _inFlightMode.reset();
    if (_queuedMode.has_value() && _queuedMode != mode) {
        const auto next = *_queuedMode;
        _queuedMode.reset();
        Send(next);
    }
    else {
        _queuedMode.reset();
        _state.pendingMode.reset();
        Publish();
    }
}

void ListeningModeController::OnFailed(uint64_t sessionId, ListeningModeError error)
{
    if (sessionId != _sessionId || _state.availability == ControlAvailability::Unavailable) {
        return;
    }
    _confirmationTimer.stop();
    _inFlightMode.reset();
    _queuedMode.reset();
    _state.pendingMode.reset();
    _state.error = error;

    if (_connected &&
        (error == ListeningModeError::ConnectionFailed ||
         error == ListeningModeError::Disconnected))
    {
        _state.availability = ControlAvailability::Connecting;
        Publish();
        ScheduleReconnect();
        return;
    }

    if (error == ListeningModeError::CommandRejected &&
        _state.availability == ControlAvailability::Ready)
    {
        // A rejected command does not make the control channel unusable. Keep the last confirmed
        // AirPods mode authoritative and let the user try again.
        Publish();
        return;
    }

    _state.availability = ControlAvailability::Faulted;
    Publish();
}

void ListeningModeController::OnClosed(uint64_t sessionId)
{
    if (sessionId != _sessionId || !_connected ||
        _state.availability == ControlAvailability::Unavailable)
    {
        return;
    }
    _confirmationTimer.stop();
    _inFlightMode.reset();
    _queuedMode.reset();
    _state = {
        .availability = ControlAvailability::Connecting,
        .error = ListeningModeError::Disconnected,
    };
    Publish();
    ScheduleReconnect();
}

void ListeningModeController::OnConfirmationTimedOut()
{
    _inFlightMode.reset();
    _queuedMode.reset();
    _state.pendingMode.reset();
    _state.error = ListeningModeError::ConfirmationTimedOut;
    Publish();
}

void ListeningModeController::ScheduleReconnect()
{
    if (!_retryTimer.isActive()) {
        _retryTimer.start();
    }
}

std::unique_ptr<IAirPodsControlTransport> CreateControlTransport(QObject *parent)
{
#if defined APD_DEBUG
    return std::make_unique<SimulatedControlTransport>(parent);
#else
    return std::make_unique<UnavailableControlTransport>(parent);
#endif
}

} // namespace Core::AirPods
