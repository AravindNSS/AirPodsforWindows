#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include <QtTest>
#include <QTemporaryFile>

#include <Config.h>
#include "Source/Core/AirPods.h"
#include "Source/Core/AapProtocol.h"
#include "Source/Core/ListeningModeControl.h"
#include "Source/Core/Settings.h"
#include "Source/Core/SettingsRepository.h"
#include "Source/Core/Update.h"
#include "Source/Gui/MainWindowPresentation.h"
#include "Source/Gui/TaskbarGeometry.h"

namespace {

using Core::AirPods::Model;
using Core::AirPods::Side;
using Core::AirPods::Details::Advertisement;
using Core::AirPods::Details::StateManager;
using ReceivedData = Core::Bluetooth::AdvertisementWatcher::ReceivedData;

class RecordingSettingsObserver final : public Core::Settings::ApplyObserver
{
public:
    void OnLanguageLocaleChanged(const QLocale &) override {}
    void OnAppearanceModeChanged(Core::Settings::AppearanceMode mode) override
    {
        appearanceMode = mode;
        ++appearanceModeChanges;
    }
    void OnAutoRunChanged(bool enable) override
    {
        autoRun = enable;
        ++autoRunChanges;
    }
    void OnLowAudioLatencyChanged(bool) override {}
    void OnAutomaticEarDetectionChanged(bool) override {}
    void OnRssiMinChanged(int16_t) override {}
    void OnDeviceAddressChanged(uint64_t) override {}
    void OnTrayIconBatteryChanged(Core::Settings::TrayIconBatteryBehavior) override {}
    void OnTrayQuickConnectEnabledChanged(bool enable) override
    {
        quickConnectEnabled = enable;
        ++quickConnectEnabledChanges;
    }
    void OnTrayQuickConnectDeviceChanged(const QString &deviceId) override
    {
        quickConnectDeviceId = deviceId;
        ++quickConnectDeviceChanges;
    }
    void OnTaskbarBatteryChanged(Core::Settings::TaskbarStatusBehavior) override {}

    bool autoRun{false};
    int autoRunChanges{0};
    Core::Settings::AppearanceMode appearanceMode{Core::Settings::AppearanceMode::System};
    int appearanceModeChanges{0};
    bool quickConnectEnabled{false};
    int quickConnectEnabledChanges{0};
    QString quickConnectDeviceId;
    int quickConnectDeviceChanges{0};
};

class FailingSyncRepository final : public Core::Settings::Repository
{
public:
    bool Contains(const QString &key) const override
    {
        return _values.Contains(key);
    }

    QVariant Read(const QString &key) const override
    {
        return _values.Read(key);
    }

    QStringList Keys() const override
    {
        return _values.Keys();
    }

    void Write(const QString &key, const QVariant &value) override
    {
        _values.Write(key, value);
    }

    void Remove(const QString &key) override
    {
        _values.Remove(key);
    }

    bool Sync() override
    {
        return false;
    }

private:
    Core::Settings::MemoryRepository _values;
};

class TestControlTransport final : public Core::AirPods::IAirPodsControlTransport
{
public:
    void Connect(Model model, uint64_t newSessionId) override
    {
        connectedModel = model;
        sessionId = newSessionId;
    }

    void Disconnect(uint64_t) override {}

    void SetListeningMode(Core::AirPods::ListeningMode mode, uint64_t commandSessionId) override
    {
        QCOMPARE(commandSessionId, sessionId);
        sentModes.push_back(mode);
    }

    void ReportReady(const Core::AirPods::ListeningCapabilities &capabilities)
    {
        emit Ready(sessionId, capabilities);
    }

    void ReportReadyFor(
        uint64_t targetSessionId, const Core::AirPods::ListeningCapabilities &capabilities)
    {
        emit Ready(targetSessionId, capabilities);
    }

    void Confirm(Core::AirPods::ListeningMode mode)
    {
        emit ModeConfirmed(sessionId, mode);
    }

    void FailFor(uint64_t targetSessionId, Core::AirPods::ListeningModeError error)
    {
        emit Failed(targetSessionId, error);
    }

    Model connectedModel{Model::Unknown};
    uint64_t sessionId{0};
    std::vector<Core::AirPods::ListeningMode> sentModes;
};

std::vector<uint8_t> MakePacket(
    uint16_t modelId, Side side, uint8_t leftBattery, uint8_t rightBattery, uint8_t caseBattery,
    bool bothInCase = true, bool lidOpened = true)
{
    std::vector<uint8_t> packet(27, 0);
    packet[0] = static_cast<uint8_t>(Core::AppleCP::PacketType::ProximityPairing);
    packet[1] = 25;
    packet[3] = static_cast<uint8_t>(modelId & 0xff);
    packet[4] = static_cast<uint8_t>(modelId >> 8);

    constexpr uint8_t kCurrentInEar = 1 << 1;
    constexpr uint8_t kBothInCase = 1 << 2;
    constexpr uint8_t kAnotherInEar = 1 << 3;
    constexpr uint8_t kBroadcastFromLeft = 1 << 5;
    packet[5] = kCurrentInEar | kAnotherInEar;
    if (bothInCase) {
        packet[5] |= kBothInCase;
    }
    if (side == Side::Left) {
        packet[5] |= kBroadcastFromLeft;
        packet[6] = static_cast<uint8_t>((rightBattery << 4) | leftBattery);
    }
    else {
        packet[6] = static_cast<uint8_t>((leftBattery << 4) | rightBattery);
    }

    packet[7] = caseBattery;
    packet[8] = lidOpened ? 0 : 1 << 3;
    return packet;
}

ReceivedData MakeAdvertisementData(
    uint64_t address, int16_t rssi, Side side, uint8_t leftBattery = 8, uint8_t rightBattery = 7,
    uint8_t caseBattery = 5, uint16_t modelId = 0x2014)
{
    ReceivedData data;
    data.address = address;
    data.rssi = rssi;
    data.manufacturerDataMap.emplace(
        Core::AppleCP::VendorId, MakePacket(modelId, side, leftBattery, rightBattery, caseBattery));
    return data;
}

} // namespace

class AirPodsDomainTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void RejectsMalformedPackets();
    void RecognizesAirPodsMaxUsbC();
    void ResolvesAirPodsMaxUsbCDisplayName();
    void ParsesAdvertisementState();
    void FiltersDuplicateAndWeakAdvertisements();
    void MergesAdvertisementsFromBothSides();
    void RejectsAdvertisementsFromDifferentModels();
    void AcceptsKnownModelAfterUnknownAdvertisement();
    void PackagesCompatibleLowLatencySilence();
    void LoadsSettingsThroughRepository();
    void MigratesLegacySettingsRepository();
    void PreservesExistingSettingsDuringMigration();
    void RollsBackFailedSettingsMigration();
    void PropagatesQuickConnectSettings();
    void RejectsNullSettingsRepository();
    void ParsesUpdateVersions();
    void ParsesGitHubReleaseMetadata();
    void MatchesInstallerAssetsByArchitecture();
    void RejectsUpdateAssetForAnotherArchitecture();
    void RejectsReleaseMetadataFromAnotherRepository();
    void RejectsUpdateAssetsWithoutDigest();
    void VerifiesUpdateFileDigest();
    void PresentsMainWindowLifecycleStates();
    void PresentsMainWindowDeviceState();
    void PresentsMainWindowCaseBattery();
    void MapsMainWindowAnimationResources();
    void ConvertsTaskbarGeometryAcrossDpiBoundaries();
    void ModelsListeningModeCapabilities();
    void SerializesListeningModeRequests();
    void CorrelatesListeningModeConfirmations();
    void IgnoresStaleListeningModeSessions();
    void RestoresConfirmedModeAfterTimeout();
    void EncodesAndParsesAapNoiseControl();
    void DecodesSplitAndCombinedAapNotifications();
    void BoundsOversizedAapInput();
    void PresentsListeningModeLabelsAndErrors();
};

void AirPodsDomainTests::RejectsMalformedPackets()
{
    const std::vector<uint8_t> empty;
    QVERIFY(!Core::AppleCP::AirPods::IsValid(empty));

    auto packet = MakePacket(0x2014, Side::Left, 8, 7, 5);
    QVERIFY(Core::AppleCP::AirPods::IsValid(packet));

    packet[0] = static_cast<uint8_t>(Core::AppleCP::PacketType::AirDrop);
    QVERIFY(!Core::AppleCP::AirPods::IsValid(packet));

    packet = MakePacket(0x2014, Side::Left, 8, 7, 5);
    packet[1] = 24;
    QVERIFY(!Core::AppleCP::AirPods::IsValid(packet));
}

void AirPodsDomainTests::RecognizesAirPodsMaxUsbC()
{
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x201F), Model::AirPods_Max_USB_C);
    QCOMPARE(Helper::ToString(Model::AirPods_Max_USB_C), QString{"AirPods Max (USB-C)"});
}

void AirPodsDomainTests::ResolvesAirPodsMaxUsbCDisplayName()
{
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods Max", Model::AirPods_Max_USB_C),
        QString{"AirPods Max (USB-C)"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName(
            "Studio AirPods Max - Find My", Model::AirPods_Max_USB_C),
        QString{"Studio AirPods Max"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods Max", Model::AirPods_Max),
        QString{"AirPods Max"});
}

void AirPodsDomainTests::PackagesCompatibleLowLatencySilence()
{
    QFile qrc{QString{APD_SOURCE_DIR} + "/Source/Resource/Resource.qrc"};
    QVERIFY(qrc.open(QIODevice::ReadOnly | QIODevice::Text));

    const auto qrcContents = QString::fromUtf8(qrc.readAll());
    QVERIFY(qrcContents.contains("<file>Audio/Silence.mp3</file>"));
    QVERIFY(QFile::exists(QString{APD_SOURCE_DIR} + "/Source/Resource/Audio/Silence.mp3"));
}

void AirPodsDomainTests::ParsesAdvertisementState()
{
    const Advertisement advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)};
    const auto &state = advertisement.GetAdvState();

    QCOMPARE(state.model, Model::AirPods_Pro_2);
    QCOMPARE(state.side, Side::Left);
    QCOMPARE(state.pods.left.battery.Value(), 80U);
    QCOMPARE(state.pods.right.battery.Value(), 70U);
    QCOMPARE(state.caseBox.battery.Value(), 50U);
    QVERIFY(state.pods.left.isInEar);
    QVERIFY(state.pods.right.isInEar);
    QVERIFY(state.caseBox.isBothPodsInCase);
    QVERIFY(state.caseBox.isLidOpened);
}

void AirPodsDomainTests::FiltersDuplicateAndWeakAdvertisements()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    auto first =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)});
    QVERIFY(first.has_value());
    QVERIFY(!first->oldState.has_value());

    auto duplicate =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)});
    QVERIFY(!duplicate.has_value());

    auto weak = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x1234, -90, Side::Left, 7, 7, 5)});
    QVERIFY(!weak.has_value());
    QCOMPARE(manager.GetCurrentState()->pods.left.battery.Value(), 80U);
}

void AirPodsDomainTests::MergesAdvertisementsFromBothSides()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    QVERIFY(
        manager
            .OnAdvReceived(Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left, 8, 6, 4)})
            .has_value());

    auto update = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right, 9, 7, 5)});
    QVERIFY(update.has_value());
    QVERIFY(update->oldState.has_value());
    QCOMPARE(update->newState.pods.left.battery.Value(), 90U);
    QCOMPARE(update->newState.pods.right.battery.Value(), 70U);
    QCOMPARE(update->newState.caseBox.battery.Value(), 50U);
}

void AirPodsDomainTests::RejectsAdvertisementsFromDifferentModels()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    const auto first =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left)});
    QVERIFY(first.has_value());
    QCOMPARE(first->newState.model, Model::AirPods_Pro_2);

    const auto differentModel = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right, 8, 7, 5, 0x2013)});
    QVERIFY(!differentModel.has_value());
    QCOMPARE(manager.GetCurrentState()->model, Model::AirPods_Pro_2);
}

void AirPodsDomainTests::AcceptsKnownModelAfterUnknownAdvertisement()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    const auto unknown = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left, 8, 7, 5, 0xffff)});
    QVERIFY(unknown.has_value());
    QCOMPARE(unknown->newState.model, Model::Unknown);

    const auto known =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right)});
    QVERIFY(known.has_value());
    QCOMPARE(known->newState.model, Model::AirPods_Pro_2);
}

void AirPodsDomainTests::LoadsSettingsThroughRepository()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("auto_run", true);
    repository->Write("appearance_mode", QString{"Dark"});
    Core::Settings::SetRepository(std::move(repository));

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(Core::Settings::GetCurrent().auto_run);
    QCOMPARE(Core::Settings::GetCurrent().appearance_mode, Core::Settings::AppearanceMode::Dark);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QCOMPARE(observer.autoRunChanges, 1);
    QVERIFY(observer.autoRun);
    QCOMPARE(observer.appearanceModeChanges, 1);
    QCOMPARE(observer.appearanceMode, Core::Settings::AppearanceMode::Dark);

    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::MigratesLegacySettingsRepository()
{
    Core::Settings::MemoryRepository current;
    Core::Settings::MemoryRepository legacy;
    legacy.Write("auto_run", true);
    legacy.Write("device_address", QVariant::fromValue<qulonglong>(0x12345678));
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);

    QVERIFY(Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), true);
    QCOMPARE(current.Read("device_address").toULongLong(), qulonglong{0x12345678});
    QCOMPARE(current.Read("abi_version").toUInt(), Core::Settings::kFieldsAbiVersion);
}

void AirPodsDomainTests::PreservesExistingSettingsDuringMigration()
{
    Core::Settings::MemoryRepository current;
    Core::Settings::MemoryRepository legacy;
    current.Write("auto_run", false);
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);
    legacy.Write("auto_run", true);
    legacy.Write("language_locale", QStringLiteral("zh_TW"));

    QVERIFY(Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), false);
    QCOMPARE(current.Read("language_locale").toString(), QStringLiteral("zh_TW"));

    current.Write("auto_run", true);
    QVERIFY(!Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), true);
}

void AirPodsDomainTests::RollsBackFailedSettingsMigration()
{
    FailingSyncRepository current;
    Core::Settings::MemoryRepository legacy;
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);
    legacy.Write("auto_run", true);

    QVERIFY(!Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QVERIFY(current.Keys().isEmpty());
}

void AirPodsDomainTests::PropagatesQuickConnectSettings()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("tray_quick_connect_enabled", true);
    repository->Write("tray_quick_connect_device_id", "{A}");
    Core::Settings::SetRepository(std::move(repository));

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QVERIFY(observer.quickConnectEnabled);
    QCOMPARE(observer.quickConnectEnabledChanges, 1);
    QCOMPARE(observer.quickConnectDeviceId, QString{"{A}"});
    QCOMPARE(observer.quickConnectDeviceChanges, 1);

    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::RejectsNullSettingsRepository()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("auto_run", true);
    Core::Settings::SetRepository(std::move(repository));

    Core::Settings::SetRepository(nullptr);

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(Core::Settings::GetCurrent().auto_run);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::ParsesUpdateVersions()
{
    QCOMPARE(Core::Update::ToVersionNumber("v0.5.0"), QVersionNumber(0, 5, 0));
    QCOMPARE(Core::Update::ToVersionNumber("0.5.0"), QVersionNumber(0, 5, 0));
}

void AirPodsDomainTests::ParsesGitHubReleaseMetadata()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "## Change log\n- Add automatic updates\n\nInstallation notes",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win64.exe",
            "size": 123456,
            "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win64.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());

    QVERIFY(release.has_value());
    QCOMPARE(release->version, QVersionNumber(0, 5, 0));
    QCOMPARE(release->fileName, QString{"AirPodsDesktop-0.5.0-win64.exe"});
    QCOMPARE(release->fileSize, size_t{123456});
    QCOMPARE(
        release->sha256,
        QString{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"});
    QCOMPARE(release->changeLog, QString{"- Add automatic updates"});
    QVERIFY(release->CanAutoUpdate());
    QVERIFY(!release->isPreRelease);

    const auto generatedNotes = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "## What's Changed\n* Improve update handling\n\n**Full Changelog**: https://example.invalid/compare",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": []
    })json"}
                                    .arg(Config::UrlReleases);
    const auto generatedRelease =
        Core::Update::Details::ParseSingleReleaseResponse(generatedNotes.toStdString());

    QVERIFY(generatedRelease.has_value());
    QCOMPARE(generatedRelease->changeLog, QString{"* Improve update handling"});

    // Shaped after the real 0.4.2 release body: CRLF line endings and an emoji shortcode
    // between the hashes and the words.
    const auto decorated = QString{R"json({
        "tag_name": "0.4.2",
        "body": "Beta notice\r\n\r\n## :scroll: Change log\r\n1. Supported AirPods 4\r\n\r\nSorry",
        "html_url": "%1/tag/0.4.2",
        "prerelease": false,
        "assets": []
    })json"}
                               .arg(Config::UrlReleases);
    const auto decoratedRelease =
        Core::Update::Details::ParseSingleReleaseResponse(decorated.toStdString());

    QVERIFY(decoratedRelease.has_value());
    QCOMPARE(decoratedRelease->changeLog, QString{"1. Supported AirPods 4"});
}

void AirPodsDomainTests::RejectsUpdateAssetForAnotherArchitecture()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "Change log\nLegacy bridge",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win32-bridge.exe",
            "size": 123456,
            "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win32-bridge.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);

    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());
    QVERIFY(release.has_value());
    QVERIFY(release->fileName.isEmpty());
    QVERIFY(!release->CanAutoUpdate());
}

void AirPodsDomainTests::MatchesInstallerAssetsByArchitecture()
{
    using Core::Update::Details::IsCompatibleInstallerAsset;

    const auto installer = QStringLiteral("AirPodsDesktop-0.5.0-win64.exe");
    const auto portable = QStringLiteral("AirPodsDesktop-0.5.0-win64-portable.zip");
    const auto bridge = QStringLiteral("AirPodsDesktop-0.5.0-win32-bridge.exe");

    QVERIFY(IsCompatibleInstallerAsset(installer, QStringLiteral("win64")));
    QVERIFY(!IsCompatibleInstallerAsset(bridge, QStringLiteral("win64")));
    QVERIFY(!IsCompatibleInstallerAsset(portable, QStringLiteral("win64")));
    QVERIFY(IsCompatibleInstallerAsset(bridge, QStringLiteral("win32")));
    QVERIFY(!IsCompatibleInstallerAsset(installer, QStringLiteral("win32")));
}

void AirPodsDomainTests::RejectsReleaseMetadataFromAnotherRepository()
{
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(R"json({
        "tag_name": "v9.9.9",
        "body": "Change log\nUntrusted release",
        "html_url": "https://github.com/AnotherOwner/AirPodsDesktop/releases/tag/v9.9.9",
        "prerelease": false,
        "assets": []
    })json");

    QVERIFY(!release.has_value());
}

void AirPodsDomainTests::RejectsUpdateAssetsWithoutDigest()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "Change log\nUnsigned asset",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win64.exe",
            "size": 123456,
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win64.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());

    QVERIFY(release.has_value());
    QVERIFY(!release->CanAutoUpdate());
}

void AirPodsDomainTests::VerifiesUpdateFileDigest()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("test"), qint64{4});
    file.close();

    const QString expected = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    QVERIFY(Core::Update::Details::VerifyFileSha256(file.fileName(), expected));
    QVERIFY(!Core::Update::Details::VerifyFileSha256(
        file.fileName(), "0000000000000000000000000000000000000000000000000000000000000000"));
}

void AirPodsDomainTests::PresentsMainWindowLifecycleStates()
{
    Gui::MainWindowViewModel viewModel;

    auto presentation = viewModel.Present();
    QCOMPARE(presentation.title, QString{"Unavailable"});
    QCOMPARE(presentation.buttonAction, Gui::ButtonAction::NoButton);
    QVERIFY(!presentation.animationModel.has_value());

    viewModel.Available();
    QCOMPARE(viewModel.Present().title, QString{"Disconnected"});

    viewModel.Unbind();
    presentation = viewModel.Present();
    QCOMPARE(presentation.title, QString{"Waiting for Binding"});
    QCOMPARE(presentation.buttonAction, Gui::ButtonAction::Bind);

    viewModel.Disconnect();
    QCOMPARE(viewModel.Present(), presentation);

    viewModel.Bind();
    QCOMPARE(viewModel.Present().title, QString{"Disconnected"});
}

void AirPodsDomainTests::PresentsMainWindowDeviceState()
{
    Core::AirPods::State state;
    state.displayName = "Office AirPods";
    state.model = Core::AirPods::Model::AirPods_Pro_2;
    state.pods.left.battery = 80;
    state.pods.left.isCharging = true;
    state.pods.right.battery = 60;

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    const auto presentation = viewModel.Present();

    QCOMPARE(presentation.title, state.displayName);
    QCOMPARE(presentation.animationModel, std::optional{state.model});
    QVERIFY(presentation.leftBattery.visible);
    QVERIFY(presentation.leftBattery.charging);
    QCOMPARE(presentation.leftBattery.value, 80U);
    QVERIFY(presentation.rightBattery.visible);
    QCOMPARE(presentation.rightBattery.value, 60U);
    QVERIFY(!presentation.caseBattery.visible);
}

void AirPodsDomainTests::PresentsMainWindowCaseBattery()
{
    // Mirrors `PresentsMainWindowDeviceState`, which reports the pods but not the case.
    // Reporting only the case pins each slot to its own source.
    Core::AirPods::State state;
    state.displayName = "Office AirPods";
    state.model = Core::AirPods::Model::AirPods_4_ANC;
    state.caseBox.battery = 40;
    state.caseBox.isCharging = true;

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    auto presentation = viewModel.Present();

    QVERIFY(presentation.caseBattery.visible);
    QVERIFY(presentation.caseBattery.charging);
    QCOMPARE(presentation.caseBattery.value, 40U);
    QVERIFY(!presentation.leftBattery.visible);
    QVERIFY(!presentation.rightBattery.visible);

    state.caseBox.isCharging = false;
    viewModel.UpdateState(state);
    presentation = viewModel.Present();

    QVERIFY(presentation.caseBattery.visible);
    QVERIFY(!presentation.caseBattery.charging);
}

void AirPodsDomainTests::MapsMainWindowAnimationResources()
{
    const auto pro = Gui::GetAnimationPresentation(Core::AirPods::Model::AirPods_Pro_2_USB_C);
    QCOMPARE(pro.resource, QString{"qrc:/Resource/Video/AirPods_Pro_2.avi"});
    QCOMPARE(pro.sourceSize, QSize(900, 450));

    const auto maxUsbC = Gui::GetAnimationPresentation(Core::AirPods::Model::AirPods_Max_USB_C);
    QCOMPARE(maxUsbC.resource, QString{"qrc:/Resource/Video/AirPods_Max.avi"});
    QCOMPARE(maxUsbC.sourceSize, QSize(600, 650));

    const auto fallback = Gui::GetAnimationPresentation(Core::AirPods::Model::Unknown);
    QCOMPARE(fallback.resource, QString{"qrc:/Resource/Video/AirPods_1.avi"});
    QCOMPARE(fallback.sourceSize, QSize(800, 400));
    QCOMPARE(
        Gui::GetModelImageResource(Core::AirPods::Model::AirPods_Pro_3),
        QString{":/Resource/Image/Animation/AirPods_Pro_3.png"});
    QVERIFY(Gui::GetModelImageResource(Core::AirPods::Model::Unknown).isEmpty());

    // The qrc is linked into the application, not into this binary, so read it from the source
    // tree: a mapping naming a resource the qrc does not carry would ship a blank animation.
    QFile qrc{QString{APD_SOURCE_DIR} + "/Source/Resource/Resource.qrc"};
    QVERIFY(qrc.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto qrcContents = QString::fromUtf8(qrc.readAll());

    for (uint32_t i = 0; i < static_cast<uint32_t>(Core::AirPods::Model::_Max); ++i) {
        const auto resource =
            Gui::GetAnimationPresentation(static_cast<Core::AirPods::Model>(i)).resource;
        const auto entry = QString{resource}.replace("qrc:/Resource/", "");
        QVERIFY2(
            qrcContents.contains("<file>" + entry + "</file>"),
            qPrintable(QString{"animation not listed in Resource.qrc: %1"}.arg(resource)));
        QVERIFY2(
            QFile::exists(QString{APD_SOURCE_DIR} + "/Source/Resource/" + entry),
            qPrintable(QString{"animation file missing: %1"}.arg(entry)));
    }
}

void AirPodsDomainTests::ConvertsTaskbarGeometryAcrossDpiBoundaries()
{
    // Reproduces issue #217: one 2560x1440 monitor at 125% display scaling. Win32 reports
    // physical pixels, while QWidget geometry is expressed in device-independent pixels.
    const auto layout =
        Gui::TaskbarGeometry::CalculateLayout(QSize{2560, 50}, QRect{160, 0, 2325, 50}, 120, true);

    QCOMPARE(layout.statusLogical, QRect(1988, 0, 60, 40));
    QCOMPARE(layout.taskButtonsNative, QRect(160, 0, 2325, 50));

    QCOMPARE(Gui::TaskbarGeometry::LogicalToNative(layout.statusLogical.right() + 1, 120), 2560);

    const auto restored =
        Gui::TaskbarGeometry::RestoreTaskButtons(QSize{2560, 50}, layout.taskButtonsNative, true);
    QCOMPARE(restored, QRect(160, 0, 2400, 50));

    const auto vertical =
        Gui::TaskbarGeometry::CalculateLayout(QSize{50, 1440}, QRect{0, 120, 50, 1270}, 120, false);
    QCOMPARE(vertical.statusLogical, QRect(0, 1112, 40, 40));
    QCOMPARE(vertical.taskButtonsNative, QRect(0, 120, 50, 1270));

    const auto detailed = Gui::TaskbarGeometry::CalculateLayout(
        QSize{2560, 50}, QRect{160, 0, 2325, 50}, 120, true, QSize{104, 40});
    QCOMPARE(detailed.statusLogical, QRect(1944, 0, 104, 40));
    QCOMPARE(detailed.taskButtonsNative, QRect(160, 0, 2270, 50));

    const auto detailedVertical = Gui::TaskbarGeometry::CalculateLayout(
        QSize{50, 1440}, QRect{0, 120, 50, 1270}, 120, false, QSize{40, 104});
    QCOMPARE(detailedVertical.statusLogical, QRect(0, 1048, 40, 104));
    QCOMPARE(detailedVertical.taskButtonsNative, QRect(0, 120, 50, 1190));
}

void AirPodsDomainTests::ModelsListeningModeCapabilities()
{
    using namespace Core::AirPods;

    ListeningCapabilities capabilities{
        .transparency = true,
        .adaptive = false,
        .noiseCancellation = true,
        .transportVersion = 1,
    };

    QVERIFY(capabilities.Any());
    QVERIFY(capabilities.Supports(ListeningMode::Transparency));
    QVERIFY(!capabilities.Supports(ListeningMode::Adaptive));
    QVERIFY(capabilities.Supports(ListeningMode::NoiseCancellation));
}

void AirPodsDomainTests::SerializesListeningModeRequests()
{
    using namespace Core::AirPods;

    auto transport = std::make_unique<TestControlTransport>();
    auto *transportObserver = transport.get();
    ListeningModeController controller{std::move(transport)};

    controller.SetDevice(Model::AirPods_Pro_3, true);
    QCOMPARE(controller.State().availability, ControlAvailability::Connecting);
    QCOMPARE(transportObserver->connectedModel, Model::AirPods_Pro_3);

    transportObserver->ReportReady({true, true, true, 1});
    QCOMPARE(controller.State().availability, ControlAvailability::Ready);

    controller.RequestMode(ListeningMode::Transparency);
    QCOMPARE(transportObserver->sentModes.size(), size_t{1});
    QCOMPARE(controller.State().pendingMode, std::optional{ListeningMode::Transparency});

    // A rapid second choice is retained but is not sent until the first command is confirmed.
    controller.RequestMode(ListeningMode::NoiseCancellation);
    QCOMPARE(transportObserver->sentModes.size(), size_t{1});
    QCOMPARE(controller.State().pendingMode, std::optional{ListeningMode::NoiseCancellation});

    transportObserver->Confirm(ListeningMode::Transparency);
    QCOMPARE(transportObserver->sentModes.size(), size_t{2});
    QCOMPARE(transportObserver->sentModes.back(), ListeningMode::NoiseCancellation);

    transportObserver->Confirm(ListeningMode::NoiseCancellation);
    QCOMPARE(controller.State().confirmedMode, std::optional{ListeningMode::NoiseCancellation});
    QVERIFY(!controller.State().pendingMode.has_value());
}

void AirPodsDomainTests::CorrelatesListeningModeConfirmations()
{
    using namespace Core::AirPods;

    auto transport = std::make_unique<TestControlTransport>();
    auto *transportObserver = transport.get();
    ListeningModeController controller{std::move(transport)};
    controller.SetDevice(Model::AirPods_Pro_3, true);
    transportObserver->ReportReady({true, true, true, 1});

    controller.RequestMode(ListeningMode::Transparency);
    controller.RequestMode(ListeningMode::NoiseCancellation);

    // A stem-originated notification updates the confirmed device state but does not complete the
    // transparency command that is still in flight.
    transportObserver->Confirm(ListeningMode::Adaptive);
    QCOMPARE(controller.State().confirmedMode, std::optional{ListeningMode::Adaptive});
    QCOMPARE(controller.State().pendingMode, std::optional{ListeningMode::NoiseCancellation});
    QCOMPARE(transportObserver->sentModes.size(), size_t{1});

    transportObserver->Confirm(ListeningMode::Transparency);
    QCOMPARE(transportObserver->sentModes.size(), size_t{2});
    QCOMPARE(transportObserver->sentModes.back(), ListeningMode::NoiseCancellation);
    QCOMPARE(controller.State().pendingMode, std::optional{ListeningMode::NoiseCancellation});
}

void AirPodsDomainTests::IgnoresStaleListeningModeSessions()
{
    using namespace Core::AirPods;

    auto transport = std::make_unique<TestControlTransport>();
    auto *transportObserver = transport.get();
    ListeningModeController controller{std::move(transport)};

    controller.SetDevice(Model::AirPods_Pro_3, true);
    const auto staleSession = transportObserver->sessionId;
    controller.SetDevice(Model::AirPods_Pro_3, false);
    transportObserver->ReportReadyFor(staleSession, {true, true, true, 1});
    QCOMPARE(controller.State().availability, ControlAvailability::Unavailable);

    controller.SetDevice(Model::AirPods_Pro_3, true);
    const auto currentSession = transportObserver->sessionId;
    QVERIFY(currentSession != staleSession);
    transportObserver->FailFor(staleSession, ListeningModeError::ConnectionFailed);
    QCOMPARE(controller.State().availability, ControlAvailability::Connecting);
    transportObserver->ReportReady({true, true, true, 1});
    QCOMPARE(controller.State().availability, ControlAvailability::Ready);
}

void AirPodsDomainTests::RestoresConfirmedModeAfterTimeout()
{
    using namespace Core::AirPods;

    auto transport = std::make_unique<TestControlTransport>();
    auto *transportObserver = transport.get();
    ListeningModeController controller{std::move(transport), nullptr, 10};
    controller.SetDevice(Model::AirPods_Pro_3, true);
    transportObserver->ReportReady({true, true, true, 1});
    transportObserver->Confirm(ListeningMode::Adaptive);

    controller.RequestMode(ListeningMode::Transparency);
    QTRY_COMPARE(controller.State().error, ListeningModeError::ConfirmationTimedOut);
    QCOMPARE(controller.State().confirmedMode, std::optional{ListeningMode::Adaptive});
    QVERIFY(!controller.State().pendingMode.has_value());
    QCOMPARE(controller.State().availability, ControlAvailability::Ready);
}

void AirPodsDomainTests::EncodesAndParsesAapNoiseControl()
{
    using namespace Core::AirPods;

    const auto transparency = Aap::MakeNoiseControlCommand(ListeningMode::Transparency);
    QCOMPARE(transparency[7], uint8_t{0x03});
    QCOMPARE(Aap::ParseNoiseControlPacket(transparency), std::optional{ListeningMode::Transparency});

    auto unknown = transparency;
    unknown[7] = 0x7f;
    QVERIFY(!Aap::ParseNoiseControlPacket(unknown).has_value());

    const std::array<uint8_t, 2> truncated{0x04, 0x00};
    QVERIFY(!Aap::ParseNoiseControlPacket(truncated).has_value());
}

void AirPodsDomainTests::DecodesSplitAndCombinedAapNotifications()
{
    using namespace Core::AirPods;

    const auto adaptive = Aap::MakeNoiseControlCommand(ListeningMode::Adaptive);
    const auto cancellation = Aap::MakeNoiseControlCommand(ListeningMode::NoiseCancellation);
    Aap::NoiseControlStreamDecoder decoder;

    QCOMPARE(decoder.Push(std::span{adaptive}.first(5)).size(), size_t{0});
    const auto first = decoder.Push(std::span{adaptive}.subspan(5));
    QVERIFY(first == std::vector{ListeningMode::Adaptive});

    std::vector<uint8_t> combined{0xaa, 0xbb};
    combined.insert(combined.end(), cancellation.begin(), cancellation.end());
    combined.insert(combined.end(), adaptive.begin(), adaptive.end());
    const auto decoded = decoder.Push(combined);
    QVERIFY(decoded ==
            (std::vector{ListeningMode::NoiseCancellation, ListeningMode::Adaptive}));
}

void AirPodsDomainTests::BoundsOversizedAapInput()
{
    using namespace Core::AirPods;

    Aap::NoiseControlStreamDecoder decoder;
    std::vector<uint8_t> oversized(Aap::kMaximumPacketSize * 8, 0xaa);
    const auto packet = Aap::MakeNoiseControlCommand(ListeningMode::Adaptive);
    oversized.insert(oversized.end(), packet.begin(), packet.end());

    const auto decoded = decoder.Push(oversized);
    QVERIFY(decoded == std::vector{ListeningMode::Adaptive});
    QVERIFY(decoder.BufferedSize() <= Aap::kMaximumPacketSize);
}

void AirPodsDomainTests::PresentsListeningModeLabelsAndErrors()
{
    using namespace Core::AirPods;

    QCOMPARE(Gui::ListeningModeLabel(ListeningMode::Transparency), QString{"Transparency"});
    QCOMPARE(Gui::ListeningModeLabel(ListeningMode::Adaptive), QString{"Adaptive"});
    QCOMPARE(
        Gui::ListeningModeLabel(ListeningMode::NoiseCancellation),
        QString{"Noise Cancellation"});
    QVERIFY(!Gui::ListeningModeErrorText(ListeningModeError::ConfirmationTimedOut).isEmpty());
    QVERIFY(Gui::ListeningModeErrorText(ListeningModeError::None).isEmpty());
}

QTEST_GUILESS_MAIN(AirPodsDomainTests)

#include "AirPodsDomainTests.moc"
