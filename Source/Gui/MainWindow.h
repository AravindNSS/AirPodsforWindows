//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include <atomic>
#include <thread>

#include <QDialog>

#include "ui_MainWindow.h"

#include <QMediaPlayer>
#include <QPropertyAnimation>

#include "Utils.h"
#include "MainWindowPresentation.h"
#include "AnimationPlayback.h"
#include "../Core/AirPods.h"
#include "../Core/ListeningModeControl.h"
#include "../Core/Update.h"
#include "Base.h"
#include "Widget/Battery.h"
#include "Widget/AnimationView.h"

namespace Gui {

class CloseButton;
class BatteryInfo;
class ListeningModeSelector;

class MainWindow : public QDialog
{
    Q_OBJECT

public:
    explicit MainWindow(Core::AirPods::ListeningModeController &listeningModeController,
                        QWidget *parent = nullptr);
    ~MainWindow();

    void StartUpdateChecks();
    void Show();

    void UpdateState(const Core::AirPods::State &state);
    void Available();
    void Unavailable();
    void Disconnect();
    void Bind();
    void Unbind();
    void AskUserUpdate(const Core::Update::ReleaseInfo &releaseInfo);

Q_SIGNALS:
    void UpdateStateSafely(const Core::AirPods::State &state);
    void AvailableSafely();
    void UnavailableSafely();
    void DisconnectSafely();
    void BindSafely();
    void UnbindSafely();
    void ShowSafely();
    void HideSafely();
    bool VersionUpdateAvailableSafely(const Core::Update::ReleaseInfo &releaseInfo, bool silent);
    void SilentUpdateAvailable(const Core::Update::ReleaseInfo &releaseInfo);

private:
    constexpr static int _minimumWindowWidth{320};
    constexpr static int _maximumWindowWidth{440};
    constexpr static int _maximumWindowHeight{520};
    constexpr static QSize _screenMargin{24, 24};
    constexpr static qreal _windowCornerRadius = 32.0;
    constexpr static int _deviceLabelMaximumPointSize = 18;
    constexpr static int _deviceLabelMinimumPointSize = 12;

    Ui::MainWindow _ui;

    QPropertyAnimation _posAnimation{this, "pos"};
    Widget::AnimationView *_animationView;
    AnimationPlayback *_playback;
    QTimer *_autoHideTimer = new QTimer{this};
    CloseButton *_closeButton;
    ListeningModeSelector *_listeningModeSelector;
    Widget::Battery *_leftBattery = new Widget::Battery{this};
    Widget::Battery *_rightBattery = new Widget::Battery{this};
    Widget::Battery *_caseBattery = new Widget::Battery{this};

    Core::Update::AsyncChecker _updateChecker{[this](auto &&...args) {
        VersionUpdateAvailableSafely(std::forward<decltype(args)>(args)...);
    }};
    std::optional<Core::AirPods::Model> _cacheModel;
    ButtonAction _buttonAction{ButtonAction::NoButton};
    MainWindowViewModel _viewModel;
    Core::AirPods::ListeningModeController &_listeningModeController;
    bool _isVisible{false};
    bool _controlDeviceConnected{false};
    Core::AirPods::Model _controlModel{Core::AirPods::Model::Unknown};
    std::atomic<bool> _deviceQueryRunning{false};
    std::jthread _deviceQueryThread;

    void ChangeButtonAction(ButtonAction action);
    void SetAnimation(std::optional<Core::AirPods::Model> model);
    void PlayAnimation();
    void StopAnimation();
    void BindDevice();
    void ShowDeviceSelector(std::vector<Core::Bluetooth::Device> devices);
    void ControlAutoHideTimer(bool start);
    void VersionUpdateAvailable(const Core::Update::ReleaseInfo &releaseInfo, bool silent);
    void Repaint();
    void ApplyTheme();
    void FitDeviceLabelFont(const QString &text);
    void UpdateListeningModeState(const Core::AirPods::ListeningModeState &state);
    void RetranslateListeningMode();

    void OnAppStateChanged(Qt::ApplicationState state);
    void OnPosMoveFinished();
    void OnAnimationClicked();
    void OnButtonClicked();

    void DoHide();
    void BeginShow(bool fromHidden);
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    UTILS_QT_DISABLE_ESC_QUIT(QDialog);
    UTILS_QT_REGISTER_LANGUAGECHANGE(QDialog, [this] {
        _ui.retranslateUi(this);
        RetranslateListeningMode();
        Repaint();
    });
};
} // namespace Gui
