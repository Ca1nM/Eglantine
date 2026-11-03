// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
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
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
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

std::optional<std::string> HttpGetUrl(const std::string& full_url, int timeout_sec = 8) {
    const auto scheme_end = full_url.find("://");
    if (scheme_end == std::string::npos) {
        return std::nullopt;
    }
    const auto path_start = full_url.find('/', scheme_end + 3);
    const std::string host =
        path_start == std::string::npos ? full_url : full_url.substr(0, path_start);
    const std::string path =
        path_start == std::string::npos ? std::string{"/"} : full_url.substr(path_start);
    try {
        httplib::Client client(host);
        client.set_connection_timeout(timeout_sec);
        client.set_read_timeout(timeout_sec);
        client.set_write_timeout(timeout_sec);
        client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
        httplib::Headers headers{
            {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                           "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"},
        };
        auto res = client.Get(path, headers);
        if (!res || res->status != 200 || res->body.empty()) {
            return std::nullopt;
        }
        return std::move(res->body);
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "HTTP GET failed for {}: {}", full_url, e.what());
        return std::nullopt;
    }
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
    connect(ui->display_name, &QLineEdit::editingFinished, this, [this] {
        if (is_nup_file) {
            SaveNupMetadata(true);
        }
    });

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
}

ConfigurePerGame::~ConfigurePerGame() {
    detect_watcher.waitForFinished();
}

void ConfigurePerGame::ApplyConfiguration() {
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
    SaveNupMetadata(true);
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

    const auto nup_path = file_path;
    const u64 tid = title_id;
    detect_watcher.setFuture(QtConcurrent::run([this, raw_query, tid, nup_path] {
        DetectIconResult out;
        const std::string search_name = ResolveGameNameAlias(raw_query, tid);
        if (search_name != raw_query) {
            out.resolved_title = search_name;
        }
        const std::string slug = NormalizeGameSlug(search_name);

        const auto try_decode_bytes = [&](std::string_view bytes) -> bool {
            if (bytes.empty()) {
                return false;
            }
            QImage img;
            if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                  static_cast<int>(bytes.size()))) {
                return false;
            }
            out.icon_bytes = EncodeSquareIcon(img);
            return !out.icon_bytes.empty();
        };

        // 1. Check local image files next to the NUP archive
        if (!nup_path.empty() && nup_path.has_parent_path()) {
            const auto dir = nup_path.parent_path();
            const auto stem = Common::FS::PathToUTF8String(nup_path.stem());
            for (const auto& base : {slug, stem, std::string{"icon"}}) {
                if (base.empty()) {
                    continue;
                }
                for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
                    const auto candidate = dir / (base + ext);
                    std::error_code ec;
                    if (std::filesystem::is_regular_file(candidate, ec)) {
                        QImage img(QString::fromStdString(Common::FS::PathToUTF8String(candidate)));
                        if (!img.isNull()) {
                            out.icon_bytes = EncodeSquareIcon(img);
                            if (!out.icon_bytes.empty()) {
                                QMetaObject::invokeMethod(
                                    this, [this, out] {
                                        ui->button_detect_icon->setEnabled(true);
                                        ui->button_detect_icon->setText(tr("Detect Icon"));
                                        if (!out.resolved_title.empty()) {
                                            ui->display_name->setText(
                                                QString::fromStdString(out.resolved_title));
                                        }
                                        QPixmap map;
                                        if (map.loadFromData(
                                                out.icon_bytes.data(),
                                                static_cast<u32>(out.icon_bytes.size()))) {
                                            UpdateIconPreview(map);
                                        }
                                        pending_nup_icon = out.icon_bytes;
                                        nup_icon_modified = true;
                                        SaveNupMetadata(true);
                                    },
                                    Qt::QueuedConnection);
                                return out;
                            }
                        }
                    }
                }
            }
        }

        // 2. Check Eden official boxart repository
        if (!slug.empty()) {
            std::vector<std::string> candidate_slugs{slug};
            if (!slug.starts_with("the-legend-of-")) {
                candidate_slugs.push_back("the-legend-of-" + slug);
            }
            if (!slug.starts_with("super-")) {
                candidate_slugs.push_back("super-" + slug);
            }
            for (const auto& s : candidate_slugs) {
                const auto url = fmt::format(
                    "https://raw.githubusercontent.com/eden-emulator/boxart/refs/heads/master/img/{}.png",
                    s);
                if (const auto body = HttpGetUrl(url); body && try_decode_bytes(*body)) {
                    break;
                }
            }
        }

        // 3. Query Wikipedia pageimages API (handles Switch & Switch 2 games)
        if (out.icon_bytes.empty() && !search_name.empty()) {
            for (const auto& suffix : {" video game", " Nintendo", ""}) {
                const std::string api_url = fmt::format(
                    "https://en.wikipedia.org/w/api.php?action=query&format=json&generator=search"
                    "&gsrsearch={}&gsrlimit=8&prop=pageimages&piprop=thumbnail|original"
                    "&pithumbsize=512&pilicense=any",
                    UrlEncode(search_name + suffix));
                const auto json_body = HttpGetUrl(api_url);
                if (!json_body) {
                    continue;
                }
                const auto parsed = nlohmann::json::parse(*json_body, nullptr, false);
                if (parsed.is_discarded() || !parsed.contains("query") ||
                    !parsed["query"].contains("pages")) {
                    continue;
                }
                struct WikiCandidate {
                    int score{};
                    int index{};
                    std::string title;
                    std::string img_url;
                };
                std::vector<WikiCandidate> candidates;
                for (const auto& [_, page] : parsed["query"]["pages"].items()) {
                    std::string img_url;
                    if (page.contains("thumbnail") && page["thumbnail"].contains("source") &&
                        page["thumbnail"]["source"].is_string()) {
                        img_url = page["thumbnail"]["source"].get<std::string>();
                    } else if (page.contains("original") && page["original"].contains("source") &&
                               page["original"]["source"].is_string()) {
                        img_url = page["original"]["source"].get<std::string>();
                    }
                    if (img_url.empty()) {
                        continue;
                    }
                    const std::string page_title = page.value("title", std::string{});
                    const int page_index = page.value("index", 99);
                    candidates.push_back({ScoreCandidateTitle(page_title, search_name), page_index,
                                          page_title, std::move(img_url)});
                }
                std::ranges::sort(candidates, [](const WikiCandidate& a, const WikiCandidate& b) {
                    if (a.score != b.score) {
                        return a.score > b.score;
                    }
                    return a.index < b.index;
                });
                for (const auto& cand : candidates) {
                    if (const auto img_body = HttpGetUrl(cand.img_url);
                        img_body && try_decode_bytes(*img_body)) {
                        break;
                    }
                }
                if (!out.icon_bytes.empty()) {
                    break;
                }
            }
        }

        // 4. Fallback to Bing square game icon thumbnail search
        if (out.icon_bytes.empty() && !search_name.empty()) {
            const std::string bing_url = fmt::format(
                "https://www.bing.com/images/search?q={}&qft=+filterui:aspect-square",
                UrlEncode(search_name + " nintendo switch game icon cover"));
            if (const auto html = HttpGetUrl(bing_url)) {
                static const std::regex thumb_re(
                    R"(https://(?:tse[0-9]\.mm|th)\.bing\.com/th/id/OIP\.[A-Za-z0-9_\-]+)");
                for (std::sregex_iterator it(html->begin(), html->end(), thumb_re), end; it != end;
                     ++it) {
                    const std::string thumb_url = it->str() + "?w=256&h=256&c=7";
                    if (const auto img_body = HttpGetUrl(thumb_url);
                        img_body && try_decode_bytes(*img_body)) {
                        break;
                    }
                }
            }
        }

        if (out.icon_bytes.empty()) {
            out.error = fmt::format("Could not find a matching icon for \"{}\".", search_name);
        }

        QMetaObject::invokeMethod(
            this,
            [this, out] {
                ui->button_detect_icon->setEnabled(true);
                ui->button_detect_icon->setText(tr("Detect Icon"));
                if (out.icon_bytes.empty()) {
                    QMessageBox::information(this, tr("Detect Icon"),
                                             QString::fromStdString(out.error));
                    return;
                }
                if (!out.resolved_title.empty()) {
                    ui->display_name->setText(QString::fromStdString(out.resolved_title));
                }
                QPixmap map;
                if (map.loadFromData(out.icon_bytes.data(),
                                     static_cast<u32>(out.icon_bytes.size()))) {
                    UpdateIconPreview(map);
                }
                pending_nup_icon = out.icon_bytes;
                nup_icon_modified = true;
                SaveNupMetadata(true);
            },
            Qt::QueuedConnection);
        return out;
    }));
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
    std::string error;
    const auto res = FileSys::Nup::UpdateMetadata(file_path, update, &error);
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
