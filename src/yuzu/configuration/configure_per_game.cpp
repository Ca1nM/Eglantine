// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <QAbstractButton>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "common/fs/fs_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/settings_enums.h"
#include "common/settings_input.h"
#include "common/string_util.h"
#include "configuration/shared_widget.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/vk.h"
#include "ui_configure_per_game.h"
#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/configuration/configure_per_game_addons.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/util/util.h"

namespace {

constexpr int IconTargetSize = 256;
constexpr int MinCandidateScore = 300;
constexpr std::size_t MaxIconDownloads = 3;
constexpr std::size_t MaxSgdbChoices = 8;

std::vector<u8> EncodeSquareIcon(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    QImage img = source.convertToFormat(QImage::Format_RGBA8888);
    if (img.width() != IconTargetSize || img.height() != IconTargetSize) {
        img = img.scaled(IconTargetSize, IconTargetSize, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        if (img.width() > IconTargetSize || img.height() > IconTargetSize) {
            const int x = (img.width() - IconTargetSize) / 2;
            const int y = img.height() > img.width() ? (img.height() - IconTargetSize) / 3
                                                     : (img.height() - IconTargetSize) / 2;
            img = img.copy(x, y, IconTargetSize, IconTargetSize);
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return std::vector<u8>(encoded.begin(), encoded.end());
}

std::string NormalizeGameSlug(std::string title) {
    title = Common::ToLower(title);
    boost::replace_all(title, "é", "e");
    boost::replace_all(title, "è", "e");
    boost::replace_all(title, "ê", "e");
    boost::replace_all(title, "ō", "o");
    std::replace(title.begin(), title.end(), ' ', '-');
    std::replace(title.begin(), title.end(), '_', '-');
    std::erase_if(title, [](unsigned char c) { return !std::isalnum(c) && c != '-'; });
    title.erase(title.begin(),
                std::find_if(title.begin(), title.end(), [](char ch) { return ch != '-'; }));
    title.erase(std::find_if(title.rbegin(), title.rend(), [](char ch) { return ch != '-'; }).base(),
                title.end());
    title.erase(std::unique(title.begin(), title.end(),
                            [](char a, char b) { return a == '-' && b == '-'; }),
                title.end());
    return title;
}

std::string ResolveGameNameAlias(const std::string& raw_name, u64 title_id) {
    static const std::unordered_map<u64, std::string> by_title_id{
        {0x0400C3F00006E000ULL, "Mario Kart World"},
        {0x0100000000010000ULL, "Super Mario Odyssey"},
        {0x0100152000022000ULL, "Mario Kart 8 Deluxe"},
        {0x01007EF00011E000ULL, "The Legend of Zelda: Breath of the Wild"},
        {0x0100F2C0115B6000ULL, "The Legend of Zelda: Tears of the Kingdom"},
        {0x01006A800016E000ULL, "Super Smash Bros. Ultimate"},
        {0x01006F8002326000ULL, "Animal Crossing: New Horizons"},
    };
    static const std::unordered_map<std::string, std::string> by_alias{
        {"mkw", "Mario Kart World"},
        {"mariokartworld", "Mario Kart World"},
        {"mario-kart-world", "Mario Kart World"},
        {"dkb", "Donkey Kong Bananza"},
        {"donkeykongbananza", "Donkey Kong Bananza"},
        {"kar", "Kirby Air Riders"},
        {"kirbyairriders", "Kirby Air Riders"},
        {"mp4", "Metroid Prime 4: Beyond"},
        {"metroidprime4", "Metroid Prime 4: Beyond"},
        {"plza", "Pokemon Legends: Z-A"},
        {"pokemonlegendsza", "Pokemon Legends: Z-A"},
        {"mk8", "Mario Kart 8 Deluxe"},
        {"mk8d", "Mario Kart 8 Deluxe"},
        {"smo", "Super Mario Odyssey"},
        {"botw", "The Legend of Zelda: Breath of the Wild"},
        {"totk", "The Legend of Zelda: Tears of the Kingdom"},
        {"ssbu", "Super Smash Bros. Ultimate"},
        {"acnh", "Animal Crossing: New Horizons"},
        {"smbw", "Super Mario Bros. Wonder"},
        {"splatoon3", "Splatoon 3"},
        {"xc3", "Xenoblade Chronicles 3"},
        {"lm3", "Luigi's Mansion 3"},
        {"pikmin4", "Pikmin 4"},
    };
    const std::string slug = NormalizeGameSlug(raw_name);
    if (const auto it = by_alias.find(slug); it != by_alias.end()) {
        return it->second;
    }
    if (slug.empty()) {
        if (const auto it = by_title_id.find(title_id); it != by_title_id.end()) {
            return it->second;
        }
    }
    return raw_name;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

constexpr std::chrono::seconds DetectIconDeadline{10};
constexpr std::size_t MaxHttpBodySize = 4 * 1024 * 1024;
constexpr std::chrono::seconds PrimarySourceDeadline{5};
constexpr std::string_view IconSiteHost = "https://switchiconshowdown.com";
constexpr std::string_view SgdbApiBase = "https://www.steamgriddb.com/api/v2";

bool IsAllowedIconHost(std::string_view host) {
    return host == "switchiconshowdown.com" || host == "steamgriddb.com" ||
           host.ends_with(".steamgriddb.com");
}

enum class HttpStatus { Ok, NetworkError, Timeout, Cancelled, Unauthorized };

struct HttpResult {
    HttpStatus status{HttpStatus::NetworkError};
    std::string body;
    std::string detail;
};

// Every request shares one deadline and cancel flag, so a whole search can never run past it.
HttpResult HttpGet(const std::string& full_url, std::chrono::steady_clock::time_point deadline,
                   const std::atomic_bool& cancel, const std::string& bearer_token = {}) {
    using namespace std::chrono;
    HttpResult result;
    if (cancel) {
        result.status = HttpStatus::Cancelled;
        return result;
    }
    constexpr std::string_view scheme = "https://";
    if (!full_url.starts_with(scheme)) {
        result.detail = "unsupported URL";
        return result;
    }
    const auto path_start = full_url.find('/', scheme.size());
    const std::string origin =
        path_start == std::string::npos ? full_url : full_url.substr(0, path_start);
    const std::string path =
        path_start == std::string::npos ? std::string{"/"} : full_url.substr(path_start);
    if (!IsAllowedIconHost(std::string_view{origin}.substr(scheme.size()))) {
        result.detail = "unexpected host";
        return result;
    }
    const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
    if (remaining.count() <= 0) {
        result.status = HttpStatus::Timeout;
        return result;
    }
    try {
        httplib::Client client(origin);
        client.set_connection_timeout(remaining);
        client.set_read_timeout(remaining);
        client.set_write_timeout(remaining);
        client.set_max_timeout(remaining);
        client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
        httplib::Headers headers{{"User-Agent", "Eglantine"}};
        if (!bearer_token.empty()) {
            headers.emplace("Authorization", "Bearer " + bearer_token);
        }
        auto res = client.Get(path, headers);
        if (!res) {
            result.status = res.error() == httplib::Error::Canceled || steady_clock::now() >= deadline
                                ? HttpStatus::Timeout
                                : HttpStatus::NetworkError;
            result.detail = httplib::to_string(res.error());
            return result;
        }
        if (res->status != 200) {
            result.status = res->status == 401 || res->status == 403 ? HttpStatus::Unauthorized
                                                                      : HttpStatus::NetworkError;
            result.detail = fmt::format("HTTP {}", res->status);
            return result;
        }
        if (res->body.empty() || res->body.size() > MaxHttpBodySize) {
            result.detail = "unexpected response size";
            return result;
        }
        result.status = HttpStatus::Ok;
        result.body = std::move(res->body);
    } catch (const std::exception& e) {
        result.detail = e.what();
    } catch (...) {
        result.detail = "unknown error";
    }
    return result;
}

struct IconCandidate {
    int id{};
    std::string title;
};

std::string DecodeHtmlEntities(std::string text) {
    static constexpr std::pair<std::string_view, std::string_view> entities[]{
        {"&amp;", "&"},  {"&lt;", "<"},     {"&gt;", ">"},     {"&quot;", "\""},
        {"&#039;", "'"}, {"&#39;", "'"},    {"&apos;", "'"},   {"&eacute;", "\xC3\xA9"},
    };
    for (const auto& [from, to] : entities) {
        boost::replace_all(text, from, to);
    }
    return text;
}

// Rows look like: <a href='./detail.php?id=N'><img ...></a></td><td><center>Title</center>
std::vector<IconCandidate> ParseSearchResults(const std::string& html) {
    constexpr std::string_view id_marker = "detail.php?id=";
    constexpr std::string_view title_marker = "<td><center>";
    std::vector<IconCandidate> out;
    std::size_t pos = 0;
    while (out.size() < 16) {
        const auto id_pos = html.find(id_marker, pos);
        if (id_pos == std::string::npos) {
            break;
        }
        pos = id_pos + id_marker.size();
        const auto id_end = html.find_first_not_of("0123456789", pos);
        const auto digits = html.substr(pos, id_end == std::string::npos ? id_end : id_end - pos);
        if (digits.empty() || digits.size() > 9) {
            continue;
        }
        const auto title_pos = html.find(title_marker, pos);
        if (title_pos == std::string::npos) {
            break;
        }
        const auto title_begin = title_pos + title_marker.size();
        const auto title_end = html.find("</center>", title_begin);
        if (title_end == std::string::npos) {
            break;
        }
        auto title = DecodeHtmlEntities(html.substr(title_begin, title_end - title_begin));
        if (title.find('<') == std::string::npos && !title.empty()) {
            out.push_back({std::stoi(digits), std::move(title)});
        }
        pos = title_end;
    }
    return out;
}

std::string UrlEncodePathSegment(std::string value) {
    std::ranges::replace(value, '/', ' ');
    std::ranges::replace(value, '\\', ' ');
    auto encoded = UrlEncode(value);
    boost::replace_all(encoded, "+", "%20");
    return encoded;
}

// Icon image URLs from a SteamGridDB response, best size first.
std::vector<std::string> ParseSgdbIconUrls(const std::string& body) {
    std::vector<std::string> urls;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("data") ||
        !json["data"].is_array()) {
        return urls;
    }
    for (const auto& item : json["data"]) {
        if (urls.size() >= MaxSgdbChoices) {
            break;
        }
        if (item.is_object() && item.contains("url") && item["url"].is_string()) {
            urls.push_back(item["url"].get<std::string>());
        }
    }
    return urls;
}

int ScoreCandidateTitle(const std::string& candidate, const std::string& query) {
    const std::string c_slug = NormalizeGameSlug(candidate);
    const std::string q_slug = NormalizeGameSlug(query);
    if (c_slug.empty() || q_slug.empty()) {
        return -1000;
    }
    if (c_slug == q_slug) {
        return 1000;
    }
    int score = 0;
    if (c_slug.starts_with(q_slug + "-") || q_slug.starts_with(c_slug + "-")) {
        score += 750 - static_cast<int>(std::abs(static_cast<long>(c_slug.size()) -
                                                 static_cast<long>(q_slug.size())));
    } else if (c_slug.find(q_slug) != std::string::npos) {
        score += 600;
    }
    const auto split_words = [](const std::string& s) {
        std::set<std::string> words;
        std::size_t start = 0;
        while (start < s.size()) {
            const auto dash = s.find('-', start);
            const auto word = s.substr(start, dash - start);
            if (!word.empty()) {
                words.insert(word);
            }
            if (dash == std::string::npos) {
                break;
            }
            start = dash + 1;
        }
        return words;
    };
    const auto q_words = split_words(q_slug);
    const auto c_words = split_words(c_slug);
    int overlap = 0;
    for (const auto& w : q_words) {
        if (c_words.contains(w)) {
            ++overlap;
        }
    }
    score += overlap * 120 - static_cast<int>(c_words.size()) * 10;
    const std::string lower_cand = Common::ToLower(candidate);
    if (lower_cand.find("(series)") != std::string::npos ||
        lower_cand.find("(character)") != std::string::npos ||
        lower_cand.find("soundtrack") != std::string::npos) {
        score -= 400;
    }
    return score;
}

} // namespace

ConfigurePerGame::ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                                   std::vector<VkDeviceInfo::Record>& vk_device_records,
                                   Core::System& system_)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigurePerGame>()),
      file_path(std::filesystem::path(Common::FS::ToU8String(file_name))), title_id{title_id_},
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      tab_group{std::make_shared<std::vector<ConfigurationShared::Tab*>>()} {
    const auto config_file_name = title_id == 0 ? Common::FS::PathToUTF8String(file_path.filename())
                                                : fmt::format("{:016X}", title_id);
    game_config = std::make_unique<QtConfig>(config_file_name, Config::ConfigType::PerGameConfig);
    addons_tab = std::make_unique<ConfigurePerGameAddons>(system_, this);
    audio_tab = std::make_unique<ConfigureAudio>(system_, tab_group, *builder, this);
    cpu_tab = std::make_unique<ConfigureCpu>(system_, tab_group, *builder, this);
    graphics_advanced_tab =
        std::make_unique<ConfigureGraphicsAdvanced>(system_, tab_group, *builder, this);
    graphics_extensions_tab =
        std::make_unique<ConfigureGraphicsExtensions>(system_, tab_group, *builder, this);
    graphics_tab = std::make_unique<ConfigureGraphics>(
        system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
        [](Settings::AspectRatio, Settings::ResolutionSetup) {}, tab_group, *builder, this);
    input_tab = std::make_unique<ConfigureInputPerGame>(system_, game_config.get(), this);
    system_tab = std::make_unique<ConfigureSystem>(system_, tab_group, *builder, this);
    network_tab = std::make_unique<ConfigureNetwork>(system_, this);
    applets_tab = std::make_unique<ConfigureApplets>(system_, tab_group, *builder, this);

    ui->setupUi(this);

    ui->tabWidget->addTab(addons_tab.get(), tr("Add-Ons"));
    ui->tabWidget->addTab(system_tab.get(), tr("System"));
    ui->tabWidget->addTab(cpu_tab.get(), tr("CPU"));
    ui->tabWidget->addTab(graphics_tab.get(), tr("Graphics"));
    ui->tabWidget->addTab(graphics_advanced_tab.get(), tr("Adv. Graphics"));
    ui->tabWidget->addTab(graphics_extensions_tab.get(), tr("Ext. Graphics"));
    ui->tabWidget->addTab(audio_tab.get(), tr("Audio"));
    ui->tabWidget->addTab(input_tab.get(), tr("Input Profiles"));
    ui->tabWidget->addTab(network_tab.get(), tr("Network"));
    ui->tabWidget->addTab(applets_tab.get(), tr("Applets"));

    setFocusPolicy(Qt::ClickFocus);
    setWindowTitle(tr("Properties"));

    addons_tab->SetTitleId(title_id);

    scene = new QGraphicsScene;
    ui->icon_view->setScene(scene);
    ui->button_detect_icon->setIcon(
        QIcon::fromTheme(QStringLiteral("edit-find"), QIcon::fromTheme(QStringLiteral("search"))));
    ui->button_upload_icon->setIcon(QIcon::fromTheme(
        QStringLiteral("document-open"), QIcon::fromTheme(QStringLiteral("insert-image"))));

    connect(ui->button_upload_icon, &QPushButton::clicked, this,
            &ConfigurePerGame::OnUploadNupIcon);
    connect(ui->button_detect_icon, &QPushButton::clicked, this,
            &ConfigurePerGame::OnDetectNupIcon);
    connect(&detect_watcher, &QFutureWatcher<DetectIconResult>::finished, this,
            &ConfigurePerGame::OnDetectNupIconFinished);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
}

ConfigurePerGame::~ConfigurePerGame() {
    // Never block the UI thread: the detached worker stops on its own once it sees the flag.
    detect_cancel->store(true);
}

void ConfigurePerGame::ApplyConfiguration() {
    // Name and icon edits are written once, when the dialog is applied.
    SaveNupMetadata(true);

    for (const auto tab : *tab_group) {
        tab->ApplyConfiguration();
    }
    addons_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();

    if (Settings::IsDockedMode() && Settings::values.players.GetValue()[0].controller_type ==
                                        Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
        Settings::values.use_docked_mode.SetGlobal(true);
    }

    system.ApplySettings();
    Settings::LogSettings();

    game_config->SaveAllValues();
}
void ConfigurePerGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigurePerGame::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigurePerGame::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigurePerGame::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGame::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    addons_tab->LoadFromFile(file);

    ui->display_title_id->setText(
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper());

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(system, file);

    is_nup_file = loader != nullptr && loader->GetFileType() == Loader::FileType::NUP;
    ui->nup_icon_widget->setVisible(is_nup_file);
    ui->display_name->setReadOnly(!is_nup_file);
    ui->display_name->setClearButtonEnabled(is_nup_file);
    ui->display_name->setPlaceholderText(is_nup_file ? tr("Enter game name...") : QString{});
    ui->display_name->setToolTip(
        is_nup_file ? tr("Edit the game name stored in the NUP metadata") : QString{});

    if (!is_nup_file && control.first != nullptr) {
        ui->display_version->setText(QString::fromStdString(control.first->GetVersionString()));
        ui->display_name->setText(QString::fromStdString(control.first->GetApplicationName()));
        ui->display_developer->setText(QString::fromStdString(control.first->GetDeveloperName()));
    } else if (loader != nullptr) {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success) {
            ui->display_name->setText(QString::fromStdString(title));
        } else if (is_nup_file) {
            ui->display_name->setText(
                QFileInfo(QString::fromStdString(file->GetName())).completeBaseName());
        }

        FileSys::NACP nacp;
        if (loader->ReadControlData(nacp) == Loader::ResultStatus::Success) {
            ui->display_developer->setText(QString::fromStdString(nacp.GetDeveloperName()));
            const auto ver = nacp.GetVersionString();
            ui->display_version->setText(ver.empty() ? QStringLiteral("1.0.0")
                                                     : QString::fromStdString(ver));
        } else {
            ui->display_version->setText(QStringLiteral("1.0.0"));
        }
    }
    initial_nup_title = ui->display_name->text().trimmed().toStdString();
    nup_icon_modified = false;
    pending_nup_icon.clear();

    scene->clear();
    if (!is_nup_file && control.second != nullptr) {
        QPixmap map;
        const auto bytes = control.second->ReadAllBytes();
        if (map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()))) {
            UpdateIconPreview(map);
        }
    } else if (loader != nullptr) {
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            QPixmap map;
            if (map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()))) {
                UpdateIconPreview(map);
            }
        }
    }

    ui->display_filename->setText(QString::fromStdString(file->GetName()));

    if (loader != nullptr) {
        ui->display_format->setText(
            QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())));
    }

    const auto valueText = QtCommon::ReadableByteSize(file->GetSize());
    ui->display_size->setText(valueText);
}

void ConfigurePerGame::UpdateIconPreview(const QPixmap& map) {
    scene->clear();
    if (map.isNull()) {
        return;
    }
    scene->addPixmap(map.scaled(ui->icon_view->width(), ui->icon_view->height(),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void ConfigurePerGame::OnUploadNupIcon() {
    if (!is_nup_file) {
        return;
    }
    const QString default_dir =
        file_path.has_parent_path()
            ? QString::fromStdString(Common::FS::PathToUTF8String(file_path.parent_path()))
            : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString selected = QtCommon::Frontend::GetOpenFileName(
        tr("Select Game Icon"), default_dir,
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    QImage img(selected);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Invalid Image"),
                             tr("The selected file could not be loaded as an image."));
        return;
    }
    pending_nup_icon = EncodeSquareIcon(img);
    if (pending_nup_icon.empty()) {
        return;
    }
    QPixmap map;
    if (map.loadFromData(pending_nup_icon.data(), static_cast<u32>(pending_nup_icon.size()))) {
        UpdateIconPreview(map);
    }
    nup_icon_modified = true;
}

void ConfigurePerGame::OnDetectNupIcon() {
    if (!is_nup_file || detect_watcher.isRunning()) {
        return;
    }
    QString query_qstr = ui->display_name->text().trimmed();
    if (query_qstr.isEmpty() && file != nullptr) {
        query_qstr = QFileInfo(QString::fromStdString(file->GetName())).completeBaseName();
    }
    const std::string raw_query = query_qstr.toStdString();
    if (raw_query.empty() && title_id == 0) {
        QMessageBox::information(this, tr("Detect Icon"),
                                 tr("Please enter a game name first so the icon can be detected."));
        return;
    }

    ui->button_detect_icon->setEnabled(false);
    ui->button_detect_icon->setText(tr("Detecting..."));

    detect_cancel->store(false);
    const u64 tid = title_id;
    const std::string api_key = UISettings::values.steamgriddb_api_key.GetValue();
    // The worker captures no pointer to this dialog, so it may safely outlive it.
    detect_watcher.setFuture(QtConcurrent::run([raw_query, tid, api_key,
                                                cancel = detect_cancel] {
        using namespace std::chrono;
        DetectIconResult out;
        try {
            const auto start = steady_clock::now();
            const auto deadline = start + DetectIconDeadline;
            const std::string search_name = ResolveGameNameAlias(raw_query, tid);
            if (search_name != raw_query) {
                out.resolved_title = search_name;
            }
            if (search_name.empty()) {
                out.status = DetectIconStatus::NotFound;
                return out;
            }

            const auto to_status = [](const HttpResult& res) {
                switch (res.status) {
                case HttpStatus::Timeout:
                    return DetectIconStatus::Timeout;
                case HttpStatus::Cancelled:
                    return DetectIconStatus::Cancelled;
                case HttpStatus::Unauthorized:
                    return DetectIconStatus::BadApiKey;
                default:
                    return DetectIconStatus::NetworkError;
                }
            };
            const auto decode_icon = [&out](const std::string& body) {
                QImage img;
                if (img.loadFromData(reinterpret_cast<const uchar*>(body.data()),
                                     static_cast<int>(body.size()))) {
                    out.icon_bytes = EncodeSquareIcon(img);
                }
                return !out.icon_bytes.empty();
            };

            // Primary: switchiconshowdown.com, limited so the fallback keeps some of the time.
            DetectIconStatus primary = DetectIconStatus::NotFound;
            std::string primary_error;
            const auto primary_deadline = std::min(deadline, start + PrimarySourceDeadline);
            do {
                std::vector<std::string> queries{search_name};
                if (const auto colon = search_name.find(':');
                    colon != std::string::npos && colon > 0) {
                    queries.push_back(search_name.substr(0, colon));
                }
                std::vector<IconCandidate> candidates;
                for (const auto& query : queries) {
                    const auto res =
                        HttpGet(std::string{IconSiteHost} + "/search.php?query=" + UrlEncode(query),
                                primary_deadline, *cancel);
                    if (res.status != HttpStatus::Ok) {
                        primary = to_status(res);
                        primary_error = res.detail;
                        break;
                    }
                    candidates = ParseSearchResults(res.body);
                    if (!candidates.empty()) {
                        break;
                    }
                }
                if (primary != DetectIconStatus::NotFound) {
                    break;
                }

                // The site can list one game several times; keep the closest title first.
                std::vector<std::pair<int, IconCandidate>> scored;
                for (auto& cand : candidates) {
                    const int score = ScoreCandidateTitle(cand.title, search_name);
                    if (score >= MinCandidateScore) {
                        scored.emplace_back(score, std::move(cand));
                    }
                }
                std::ranges::stable_sort(
                    scored, [](const auto& a, const auto& b) { return a.first > b.first; });
                for (std::size_t i = 0; i < scored.size() && i < MaxIconDownloads; ++i) {
                    const auto res = HttpGet(fmt::format("{}/images/100/{}.png", IconSiteHost,
                                                         scored[i].second.id),
                                             primary_deadline, *cancel);
                    if (res.status != HttpStatus::Ok) {
                        primary = to_status(res);
                        primary_error = res.detail;
                        break;
                    }
                    if (decode_icon(res.body)) {
                        out.status = DetectIconStatus::Found;
                        return out;
                    }
                }
            } while (false);

            if (primary == DetectIconStatus::Cancelled) {
                out.status = primary;
                return out;
            }

            // Fallback: SteamGridDB, which needs the user's API key.
            if (api_key.empty()) {
                out.status = primary == DetectIconStatus::NotFound ? DetectIconStatus::NeedsApiKey
                                                                   : primary;
                out.error = primary_error;
                return out;
            }
            const auto sgdb_get = [&](const std::string& path) {
                return HttpGet(std::string{SgdbApiBase} + path, deadline, *cancel, api_key);
            };
            const auto search = sgdb_get("/search/autocomplete/" + UrlEncodePathSegment(search_name));
            if (search.status != HttpStatus::Ok) {
                out.status = to_status(search);
                out.error = search.detail;
                return out;
            }
            const auto parsed = nlohmann::json::parse(search.body, nullptr, false);
            int game_id = 0;
            if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("data") &&
                parsed["data"].is_array() && !parsed["data"].empty() &&
                parsed["data"][0].is_object() && parsed["data"][0].contains("id") &&
                parsed["data"][0]["id"].is_number_integer()) {
                game_id = parsed["data"][0]["id"].get<int>();
            }
            if (game_id <= 0) {
                out.status = DetectIconStatus::NotFound;
                return out;
            }
            const auto icons =
                sgdb_get(fmt::format("/icons/game/{}?dimensions=256,512&mimes=image/png", game_id));
            if (icons.status != HttpStatus::Ok) {
                out.status = to_status(icons);
                out.error = icons.detail;
                return out;
            }
            // Collect every icon that arrives in time; the user picks one.
            for (const auto& url : ParseSgdbIconUrls(icons.body)) {
                const auto img = HttpGet(url, deadline, *cancel);
                if (img.status != HttpStatus::Ok) {
                    if (img.status == HttpStatus::Cancelled || img.status == HttpStatus::Timeout) {
                        if (out.choices.empty()) {
                            out.status = to_status(img);
                            out.error = img.detail;
                            return out;
                        }
                        break;
                    }
                    continue;
                }
                if (decode_icon(img.body)) {
                    out.choices.push_back(std::move(out.icon_bytes));
                    out.icon_bytes.clear();
                }
            }
            if (!out.choices.empty()) {
                out.status = DetectIconStatus::Choices;
                return out;
            }
            out.status = DetectIconStatus::NotFound;
        } catch (const std::exception& e) {
            out.status = DetectIconStatus::InternalError;
            out.error = e.what();
        } catch (...) {
            out.status = DetectIconStatus::InternalError;
            out.error = "unknown error";
        }
        return out;
    }));
}

int ConfigurePerGame::ChooseIcon(const std::vector<std::vector<u8>>& choices) {
    if (choices.size() == 1) {
        return 0;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Choose an Icon"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("SteamGridDB has several icons for this game. Pick one:"),
                                 &dialog));
    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(128, 128));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSpacing(8);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        QPixmap map;
        if (map.loadFromData(choices[i].data(), static_cast<u32>(choices[i].size()))) {
            auto* item = new QListWidgetItem(QIcon(map), QString::number(i + 1));
            item->setData(Qt::UserRole, static_cast<int>(i));
            list->addItem(item);
        }
    }
    if (list->count() == 0) {
        return -1;
    }
    list->setCurrentRow(0);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog.resize(520, 420);
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return -1;
    }
    return list->currentItem()->data(Qt::UserRole).toInt();
}

void ConfigurePerGame::OnDetectNupIconFinished() {
    ui->button_detect_icon->setEnabled(true);
    ui->button_detect_icon->setText(tr("Detect Icon"));

    DetectIconResult result;
    try {
        result = detect_watcher.result();
    } catch (const std::exception& e) {
        result.status = DetectIconStatus::InternalError;
        result.error = e.what();
    } catch (...) {
        result.status = DetectIconStatus::InternalError;
    }

    QString message;
    switch (result.status) {
    case DetectIconStatus::Found: {
        QPixmap map;
        if (!map.loadFromData(result.icon_bytes.data(),
                              static_cast<u32>(result.icon_bytes.size()))) {
            message = tr("The downloaded icon could not be displayed.");
            break;
        }
        if (!result.resolved_title.empty()) {
            ui->display_name->setText(QString::fromStdString(result.resolved_title));
        }
        UpdateIconPreview(map);
        pending_nup_icon = std::move(result.icon_bytes);
        nup_icon_modified = true;
        return;
    }
    case DetectIconStatus::Choices: {
        const int index = ChooseIcon(result.choices);
        if (index < 0) {
            return;
        }
        QPixmap map;
        if (!map.loadFromData(result.choices[index].data(),
                              static_cast<u32>(result.choices[index].size()))) {
            message = tr("The selected icon could not be displayed.");
            break;
        }
        if (!result.resolved_title.empty()) {
            ui->display_name->setText(QString::fromStdString(result.resolved_title));
        }
        UpdateIconPreview(map);
        pending_nup_icon = std::move(result.choices[index]);
        nup_icon_modified = true;
        return;
    }
    case DetectIconStatus::NotFound:
        message = tr("No matching icon was found on switchiconshowdown.com or SteamGridDB. Try a "
                     "different game name or upload an icon.");
        break;
    case DetectIconStatus::NeedsApiKey:
    case DetectIconStatus::BadApiKey: {
        const bool missing = result.status == DetectIconStatus::NeedsApiKey;
        QString text =
            missing ? tr("No matching icon was found on switchiconshowdown.com. A SteamGridDB API "
                         "key enables a fallback search.")
                    : tr("SteamGridDB rejected the saved API key.");
        if (UISettings::values.steamgriddb_api_key.GetValue().empty()) {
            text += QStringLiteral("<br><br>") +
                    tr("Create a free key on the <a href=\"%1\">SteamGridDB API preferences</a> "
                       "page.")
                        .arg(QStringLiteral("https://www.steamgriddb.com/profile/preferences/api"));
        }
        QDialog key_dialog(this);
        key_dialog.setWindowTitle(tr("SteamGridDB API Key"));
        auto* layout = new QVBoxLayout(&key_dialog);
        auto* label = new QLabel(text, &key_dialog);
        label->setTextFormat(Qt::RichText);
        label->setOpenExternalLinks(true);
        label->setWordWrap(true);
        auto* key_edit = new QLineEdit(
            QString::fromStdString(UISettings::values.steamgriddb_api_key.GetValue()),
            &key_dialog);
        key_edit->setEchoMode(QLineEdit::Password);
        key_edit->setPlaceholderText(tr("API key"));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             &key_dialog);
        connect(buttons, &QDialogButtonBox::accepted, &key_dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &key_dialog, &QDialog::reject);
        layout->addWidget(label);
        layout->addWidget(key_edit);
        layout->addWidget(buttons);
        key_dialog.setMinimumWidth(420);
        if (key_dialog.exec() == QDialog::Accepted && !key_edit->text().trimmed().isEmpty()) {
            UISettings::values.steamgriddb_api_key.SetValue(
                key_edit->text().trimmed().toStdString());
            OnDetectNupIcon();
        }
        return;
    }
    case DetectIconStatus::Timeout:
        message = tr("Icon detection took longer than %1 seconds and was stopped. Check your "
                     "connection and try again.")
                      .arg(DetectIconDeadline.count());
        break;
    case DetectIconStatus::NetworkError:
        message = tr("Could not reach switchiconshowdown.com (%1). Check your connection and try "
                     "again.")
                      .arg(QString::fromStdString(result.error));
        break;
    case DetectIconStatus::Cancelled:
        return;
    case DetectIconStatus::InternalError:
        message = tr("Icon detection failed unexpectedly: %1")
                      .arg(QString::fromStdString(result.error));
        break;
    }
    LOG_WARNING(Frontend, "Icon detection failed: {}", message.toStdString());
    QMessageBox::warning(this, tr("Detect Icon"), message);
}

bool ConfigurePerGame::SaveNupMetadata(bool show_error) {
    if (!is_nup_file || file_path.empty()) {
        return false;
    }
    const std::string new_title = ui->display_name->text().trimmed().toStdString();
    const bool title_changed = new_title != initial_nup_title;
    if (!title_changed && !nup_icon_modified) {
        return true;
    }
    FileSys::Nup::MetadataUpdate update;
    if (title_changed) {
        update.title_name = new_title;
    }
    if (nup_icon_modified) {
        update.icon = pending_nup_icon;
    }
    // The archive is rewritten, which takes a while for large games. By the time OK is handled
    // the properties window may already be closed, so the popup falls back to its parent.
    QWidget* host = isVisible() ? static_cast<QWidget*>(this) : parentWidget();
    QProgressDialog progress(
        tr("Updating the NUP archive...\n\nDo not close the emulator until this has finished."),
        QString(), 0, 100, host);
    progress.setWindowTitle(tr("Saving NUP Metadata"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    progress.show();

    // The worker publishes its progress here; a timer on this thread moves the bar, so the bar
    // does not depend on style animations.
    std::atomic<int> percent{0};
    QTimer ticker;
    connect(&ticker, &QTimer::timeout, &progress,
            [&progress, &percent] { progress.setValue(percent.load()); });
    ticker.start(100);

    std::string error;
    QFutureWatcher<FileSys::Nup::PackResult> save_watcher;
    QEventLoop loop;
    connect(&save_watcher, &QFutureWatcher<FileSys::Nup::PackResult>::finished, &loop,
            &QEventLoop::quit);
    save_watcher.setFuture(QtConcurrent::run([path = file_path, update, &error, &percent] {
        try {
            return FileSys::Nup::UpdateMetadata(
                path, update, &error, [&percent](u64 done, u64 total) {
                    percent.store(total == 0 ? 0 : static_cast<int>(done * 100 / total));
                });
        } catch (const std::exception& e) {
            error = e.what();
            return FileSys::Nup::PackResult::IoError;
        }
    }));
    if (!save_watcher.isFinished()) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    ticker.stop();
    progress.close();
    const auto res = save_watcher.result();
    if (res != FileSys::Nup::PackResult::Success) {
        if (show_error) {
            QMessageBox::warning(
                this, tr("Update NUP Metadata"),
                tr("Failed to update NUP metadata:\n%1").arg(QString::fromStdString(error)));
        }
        return false;
    }
    initial_nup_title = new_title;
    nup_icon_modified = false;
    UISettings::values.is_game_list_reload_pending.exchange(true);
    return true;
}
