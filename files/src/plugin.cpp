// Copyright (c) 2022 Manuel Schneider

#include "configwidget.h"
#include "plugin.h"
#include "fileitems.h"
ALBERT_LOGGING
using namespace std;

const char* CFG_PATHS = "paths";
const char* CFG_MIME_FILTERS = "mimeFilters";
const QStringList DEF_MIME_FILTERS = { "inode/directory", "application/*" };
const char* CFG_NAME_FILTERS = "nameFilters";
const QStringList DEF_NAME_FILTERS = { ".DS_Store" };
const char* CFG_INDEX_HIDDEN = "indexhidden";
const bool DEF_INDEX_HIDDEN = false;
const char* CFG_FOLLOW_SYMLINKS = "followSymlinks";
const bool DEF_FOLLOW_SYMLINKS = false;
const char* CFG_FS_WATCHES = "useFileSystemWatches";
const bool DEF_FS_WATCHES = false;
const char* CFG_MAX_DEPTH = "maxDepth";
const uint8_t DEF_MAX_DEPTH = 100;
const char* CFG_SCAN_INTERVAL = "scanInterval";
const uint DEF_SCAN_INTERVAL = 15;
const char* INDEX_FILE_NAME = "file_index.json";

Plugin::Plugin()
{
    connect(&fs_index_, &FsIndex::updatedFinished, this, [this](){ updateIndex(); });

    QJsonObject object;
    if (QFile file(cacheDir().filePath(INDEX_FILE_NAME)); file.open(QIODevice::ReadOnly))
        object = QJsonDocument(QJsonDocument::fromJson(file.readAll())).object();

    auto s = settings();
    auto paths = s->value(CFG_PATHS, QStringList()).toStringList();
    for (const auto &path : paths){
        FsIndexPath *p;
        if (auto it = object.find(path); it == object.end())
            p = new FsIndexPath(path);
        else
            p = new FsIndexPath(it.value().toObject());
        s->beginGroup(path);
        p->setFollowSymlinks(s->value(CFG_FOLLOW_SYMLINKS, DEF_FOLLOW_SYMLINKS).toBool());
        p->setIndexHidden(s->value(CFG_INDEX_HIDDEN, DEF_INDEX_HIDDEN).toBool());
        p->setNameFilters(s->value(CFG_NAME_FILTERS, DEF_NAME_FILTERS).toStringList());
        p->setMimeFilters(s->value(CFG_MIME_FILTERS, DEF_MIME_FILTERS).toStringList());
        p->setMaxDepth(s->value(CFG_MAX_DEPTH, DEF_MAX_DEPTH).toUInt());
        p->setScanInterval(s->value(CFG_SCAN_INTERVAL, DEF_SCAN_INTERVAL).toUInt());
        p->setWatchFilesystem(s->value(CFG_FS_WATCHES, DEF_FS_WATCHES).toBool());
        s->endGroup();
        if (!fs_index_.addPath(p))
            delete p;
    }
    fs_index_.update();

    update_item = albert::StandardItem::make(
        "scan_files",
        "Update index",
        "Update the file index",
        {":app_icon"},
        {{"scan_files", "Index", [this](){ fs_index_.update(); }}}
    );

    addAutoExtension(&homebrowser);
    addAutoExtension(&rootbrowser);
}

Plugin::~Plugin()
{
    auto s = settings();
    QStringList paths;
    QJsonObject object;
    for (auto &[path, fsp] : fs_index_.indexPaths()){
        paths << path;
        s->beginGroup(path);
        s->setValue(CFG_NAME_FILTERS, fsp->nameFilters());
        s->setValue(CFG_MIME_FILTERS, fsp->mimeFilters());
        s->setValue(CFG_INDEX_HIDDEN, fsp->indexHidden());
        s->setValue(CFG_FOLLOW_SYMLINKS, fsp->followSymlinks());
        s->setValue(CFG_MAX_DEPTH, fsp->maxDepth());
        s->setValue(CFG_FS_WATCHES, fsp->watchFileSystem());
        s->setValue(CFG_SCAN_INTERVAL, fsp->scanInterval());
        s->endGroup();
        object.insert(path, fsp->toJson());
    }
    s->setValue(CFG_PATHS, paths);

    if (QFile file(cacheDir().filePath(INDEX_FILE_NAME)); file.open(QIODevice::WriteOnly)) {
        DEBG << "Storing file index to" << file.fileName();
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        file.close();
    } else
        WARN << "Couldn't write to file:" << file.fileName();

    fs_index_.update();
}

vector<albert::IndexItem> Plugin::indexItems() const
{
    vector<shared_ptr<AbstractFileItem>> items;
    for (auto &[path, fsp] : fs_index_.indexPaths())
        fsp->items(items);
    vector<albert::IndexItem> ii;
    for (auto &file_item : items)
        ii.emplace_back(file_item, file_item->name());
    ii.emplace_back(update_item, update_item->text());
    return ii;
}

QWidget *Plugin::buildConfigWidget()
{
    return new ConfigWidget(this);
}

FsIndex &Plugin::fsIndex() { return fs_index_; }
