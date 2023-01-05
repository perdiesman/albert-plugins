// Copyright (c) 2022 Manuel Schneider

#include "albert/logging.h"
#include "albert/util/timeprinter.hpp"
#include "plugin.h"
#include "ui_configwidget.h"
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <utility>
using namespace std;
ALBERT_LOGGING

static const char* CFG_BOOKMARKS_PATH = "bookmarks_path";
static const char* CFG_INDEX_HOSTNAME = "indexHostname";
static const bool  DEF_INDEX_HOSTNAME = false;

static const QStringList icon_urls = {"xdg:www", "xdg:web-browser", "xdg:emblem-web", ":favicon"};
static const char *app_dirs[] = {
    "BraveSoftware",
    "Google/Chrome",  // Google Chrome Macos
    "brave-browser",
    "chromium",
    "google-chrome",
    "vivaldi"
};

struct BookmarkItem : albert::Item
{
    BookmarkItem(QString i, QString n, QString u) : id_(::move(i)), name_(::move(n)), url_(::move(u)) {}

    QString id_;
    QString name_;
    QString url_;

    QString id() const override { return id_; }
    QString text() const override { return name_; }
    QString subtext() const override { return url_; }
    QStringList iconUrls() const override { return icon_urls; }
    vector<albert::Action> actions() const override { return {
        {"open-url", "Open URL",              [this]() { albert::openUrl(url_); }},
        {"copy-url", "Copy URL to clipboard", [this]() { albert::setClipboardText(url_); }}
    }; }
};


static std::vector<std::shared_ptr<BookmarkItem>> parseBookmarks(const QStringList& paths, const bool &abort)
{
    albert::TimePrinter tp("Indexed bookmarks in %1 µs");

    function<void(const QJsonObject &json, std::vector<std::shared_ptr<BookmarkItem>> &items)> recursiveJsonTreeWalker =
            [&recursiveJsonTreeWalker](const QJsonObject &json, std::vector<std::shared_ptr<BookmarkItem>> &items){
                QJsonValue type = json["type"];
                if (type != QJsonValue::Undefined) {

                    if (type.toString() == "folder")
                        for (const QJsonValueRef child: json["children"].toArray())
                            recursiveJsonTreeWalker(child.toObject(), items);

                    else if (type.toString() == "url")
                        items.emplace_back(make_shared<BookmarkItem>(json["guid"].toString(),
                                                                     json["name"].toString(),
                                                                     json["url"].toString()));
                }
            };

    std::vector<std::shared_ptr<BookmarkItem>> results;
    for (auto &path : paths) {
        if (abort) return {};
        if (QFile f(path); f.open(QIODevice::ReadOnly)) {
            for (const auto &root: QJsonDocument::fromJson(f.readAll()).object().value("roots").toObject())
                if (root.isObject())
                    recursiveJsonTreeWalker(root.toObject(), results);
            f.close();
        }
        else
            WARN << "Could not open Bookmarks file:" << path;
    }
    return results;
}


Plugin::Plugin()
{
    indexer.parallel = [this](const bool &abort){
        return parseBookmarks(paths_, abort);
    };
    indexer.finish = [this](vector<shared_ptr<BookmarkItem>> && res){
        auto msg = QString("%1 bookmarks indexed.").arg(bookmarks_.size());
        INFO << msg;
        emit statusChanged(msg);
        bookmarks_=::move(res);
        updateIndex();
    };

    auto s = settings();
    index_hostname_ = s->value(CFG_INDEX_HOSTNAME, DEF_INDEX_HOSTNAME).toBool();

    // If not configured try to automatically set paths
    if (s->contains(CFG_BOOKMARKS_PATH)){
        for (auto loc : {QStandardPaths::GenericDataLocation, QStandardPaths::GenericConfigLocation})
            for (const auto &path : QStandardPaths::standardLocations(loc))
                for (const char *app_dir_name : app_dirs)
                    for (QDirIterator it(QDir(path).filePath(app_dir_name), {"Bookmarks"},
                                         QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
                        paths_ << it.next();
        s->setValue(CFG_BOOKMARKS_PATH, paths_);
    }
    else
        paths_ = s->value(CFG_BOOKMARKS_PATH).toStringList();

    indexer.run();

    connect(&file_system_watcher_, &QFileSystemWatcher::fileChanged, [this](){
        // Update watches. Chromium seems to mv the file (inode change).
        file_system_watcher_.removePaths(file_system_watcher_.files());
        file_system_watcher_.addPaths(paths_);
        auto msg = QString("%1 bookmarks indexed.").arg(bookmarks_.size());
        INFO << msg;
        emit statusChanged(msg);
        indexer.run();
    });
}

void Plugin::setPaths(const QStringList& paths) {
    paths_ = paths;
    paths_.sort();
    settings()->setValue(CFG_BOOKMARKS_PATH, paths_);
    updateIndex();
}

vector<albert::IndexItem> Plugin::indexItems() const
{
    vector<albert::IndexItem> index_items;
    for (const auto & bookmark : bookmarks_){
        index_items.emplace_back(static_pointer_cast<albert::Item>(bookmark), bookmark->name_);
        if (index_hostname_)
            index_items.emplace_back(static_pointer_cast<albert::Item>(bookmark), QUrl(bookmark->url_).host());
    }
    return index_items;
}

QWidget *Plugin::buildConfigWidget()
{
    auto *w = new QWidget();
    Ui::ConfigWidget ui;
    ui.setupUi(w);
    auto *string_list_model = new QStringListModel();
    connect(w, &QWidget::destroyed, string_list_model, &QObject::deleteLater);
    string_list_model->setStringList(paths_);

    ui.listView_paths->setModel(string_list_model);
    ui.label_status->setText(QString("%1 bookmarks indexed.").arg(bookmarks_.size()));

    ui.checkBox_index_hostname->setChecked(index_hostname_);
    connect(ui.checkBox_index_hostname, &QCheckBox::toggled, [this](bool checked){
        settings()->setValue(CFG_INDEX_HOSTNAME, checked);
        index_hostname_ = checked;
        indexer.run();
    });

    connect(this, &Plugin::statusChanged, ui.label_status, &QLabel::setText);

    connect(ui.toolButton_add, &QToolButton::clicked, this,
            [this,w,string_list_model]() {
        auto path = QFileDialog::getOpenFileName(w, tr("Select Bookmarks file"),
                                                 QDir::homePath(), "Bookmarks (Bookmarks)");
        if (!path.isNull() && !paths_.contains(path)) {
            paths_ << path;
            setPaths(paths_);
            string_list_model->setStringList(paths_);
        }
    });

    connect(ui.toolButton_remove, &QToolButton::clicked, this,
            [this,listView_paths=ui.listView_paths,string_list_model]() {
        paths_.removeAt(listView_paths->currentIndex().row());
        setPaths(paths_);
        string_list_model->setStringList(paths_);
    });

    return w;
}

