/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "gui/customfolderdialog.h"

#include "gui/folder.h"
#include "gui/folderman.h"
#include "libsync/account.h"
#include "libsync/common/utility.h"
#include "libsync/graphapi/space.h"
#include "libsync/graphapi/spacesmanager.h"
#include "libsync/networkjobs.h"
#include "libsync/vfs/vfs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace OCC {

Q_LOGGING_CATEGORY(lcCustomFolderDialog, "gui.customfolderdialog", QtInfoMsg)

namespace {
    // remote path relative to the Space root, empty for the root
    constexpr int SubPathRole = Qt::UserRole;
    constexpr int LoadedRole = Qt::UserRole + 1;

    // whether one remote path is equal to, or contains, the other
    bool remotePathsOverlap(const QString &a, const QString &b)
    {
        if (a.isEmpty() || b.isEmpty() || a == b) {
            return true;
        }
        return a.startsWith(b + '/'_L1) || b.startsWith(a + '/'_L1);
    }
}

CustomFolderDialog::CustomFolderDialog(const AccountStatePtr &accountState, QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
{
    setWindowTitle(tr("Sync a folder to a custom location"));

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Choose a folder on the server and any folder on this computer to keep in sync. "
                                "Pick the top entry to sync the whole Space."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    _spaceCombo = new QComboBox(this);
    form->addRow(tr("Space:"), _spaceCombo);
    layout->addLayout(form);

    _remoteTree = new QTreeWidget(this);
    _remoteTree->setHeaderLabel(tr("Folder on the server"));
    _remoteTree->setSelectionMode(QAbstractItemView::SingleSelection);
    _remoteTree->setMinimumHeight(220);
    layout->addWidget(_remoteTree, 1);

    auto *localRow = new QHBoxLayout;
    _localPathEdit = new QLineEdit(this);
    _localPathEdit->setPlaceholderText(tr("Local folder, e.g. %1").arg(QDir::toNativeSeparators(QDir::homePath() + "/Documents"_L1)));
    auto *browseButton = new QPushButton(tr("Choose…"), this);
    localRow->addWidget(_localPathEdit, 1);
    localRow->addWidget(browseButton);
    auto *localForm = new QFormLayout;
    localForm->addRow(tr("Local folder:"), localRow);
    layout->addLayout(localForm);

    _vfsCheckBox = new QCheckBox(tr("Use virtual files (download file contents on demand)"), this);
    const bool vfsAvailable = VfsPluginManager::instance().bestAvailableVfsMode() != Vfs::Mode::Off;
    _vfsCheckBox->setChecked(vfsAvailable);
    _vfsCheckBox->setEnabled(vfsAvailable);
    layout->addWidget(_vfsCheckBox);

    _infoLabel = new QLabel(this);
    _infoLabel->setWordWrap(true);
    _infoLabel->hide();
    layout->addWidget(_infoLabel);

    _errorLabel = new QLabel(this);
    _errorLabel->setWordWrap(true);
    _errorLabel->setStyleSheet(u"color: #c62828;"_s);
    _errorLabel->hide();
    layout->addWidget(_errorLabel);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    _buttons->button(QDialogButtonBox::Ok)->setText(tr("Add sync"));
    layout->addWidget(_buttons);

    connect(_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, &CustomFolderDialog::chooseLocalFolder);
    connect(_localPathEdit, &QLineEdit::textChanged, this, &CustomFolderDialog::validate);
    connect(_vfsCheckBox, &QCheckBox::toggled, this, &CustomFolderDialog::validate);
    connect(_remoteTree, &QTreeWidget::itemSelectionChanged, this, &CustomFolderDialog::validate);
    connect(_remoteTree, &QTreeWidget::itemExpanded, this, &CustomFolderDialog::listRemoteFolder);
    connect(_spaceCombo, &QComboBox::currentIndexChanged, this, &CustomFolderDialog::reloadRemoteTree);

    populateSpaces();
    validate();
}

void CustomFolderDialog::populateSpaces()
{
    auto spaces = _accountState->account()->spacesManager()->spaces();
    std::sort(spaces.begin(), spaces.end(), [](const GraphApi::Space *a, const GraphApi::Space *b) {
        if (a->priority() != b->priority()) {
            return a->priority() > b->priority();
        }
        return a->displayName().compare(b->displayName(), Qt::CaseInsensitive) < 0;
    });
    for (const auto *space : std::as_const(spaces)) {
        if (space->disabled()) {
            continue;
        }
        _spaceCombo->addItem(space->displayName(), space->id());
    }
    reloadRemoteTree();
}

QUrl CustomFolderDialog::currentSpaceRootUrl() const
{
    if (auto *space = _accountState->account()->spacesManager()->space(_spaceCombo->currentData().toString())) {
        return QUrl(space->drive().getRoot().getWebDavUrl());
    }
    return {};
}

QString CustomFolderDialog::currentRemoteSubPath() const
{
    const auto selected = _remoteTree->selectedItems();
    if (selected.isEmpty()) {
        return {};
    }
    return selected.first()->data(0, SubPathRole).toString();
}

void CustomFolderDialog::reloadRemoteTree()
{
    ++_treeGeneration;
    _remoteTree->clear();
    if (_spaceCombo->currentIndex() < 0) {
        validate();
        return;
    }
    auto *root = new QTreeWidgetItem(_remoteTree, {_spaceCombo->currentText()});
    root->setData(0, SubPathRole, QString());
    root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    root->setSelected(true);
    root->setExpanded(true); // triggers listRemoteFolder
    validate();
}

void CustomFolderDialog::listRemoteFolder(QTreeWidgetItem *item)
{
    if (item->data(0, LoadedRole).toBool()) {
        return;
    }
    item->setData(0, LoadedRole, true);

    const QUrl spaceRoot = currentSpaceRootUrl();
    if (!spaceRoot.isValid()) {
        return;
    }
    const QString subPath = item->data(0, SubPathRole).toString();

    auto *job = new PropfindJob(_accountState->account(), spaceRoot, subPath, PropfindJob::Depth::One, this);
    job->setProperties({QByteArrayLiteral("resourcetype")});

    // the tree might get rebuilt (Space changed) before the reply arrives, item would then be dangling
    const int generation = _treeGeneration;

    connect(job, &PropfindJob::directoryListingSubfolders, this, [this, item, subPath, spaceRoot, generation](const QStringList &list) {
        if (generation != _treeGeneration) {
            return;
        }
        const QString rootPath = Utility::ensureTrailingSlash(Utility::concatUrlPath(spaceRoot, subPath).path());
        QStringList names;
        for (const QString &path : list) {
            if (!path.startsWith(rootPath)) {
                continue;
            }
            QString name = path.mid(rootPath.size());
            while (name.endsWith('/'_L1)) {
                name.chop(1);
            }
            if (name.isEmpty() || name.contains('/'_L1)) {
                continue;
            }
            names.append(name);
        }
        names.sort(Qt::CaseInsensitive);
        for (const QString &name : std::as_const(names)) {
            auto *child = new QTreeWidgetItem(item, {name});
            child->setData(0, SubPathRole, subPath.isEmpty() ? name : u"%1/%2"_s.arg(subPath, name));
            child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        if (names.isEmpty()) {
            item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
        }
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, item, job, generation] {
        if (generation != _treeGeneration) {
            return;
        }
        qCWarning(lcCustomFolderDialog) << u"Failed to list remote folder" << job->reply()->errorString();
        item->setData(0, LoadedRole, false);
        _errorLabel->setText(tr("Could not load the list of folders from the server: %1").arg(job->reply()->errorString()));
        _errorLabel->show();
    });
    job->start();
}

void CustomFolderDialog::chooseLocalFolder()
{
    QString start = _localPathEdit->text();
    if (start.isEmpty()) {
        start = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select the local folder"), start);
    if (!dir.isEmpty()) {
        _localPathEdit->setText(QDir::toNativeSeparators(dir));
    }
}

void CustomFolderDialog::validate()
{
    QString error;
    QString info;

    const QString localPath = QDir::cleanPath(QDir::fromNativeSeparators(_localPathEdit->text().trimmed()));
    const QString spaceId = _spaceCombo->currentData().toString();
    const QString subPath = currentRemoteSubPath();
    const auto accountUuid = _accountState->account()->uuid();

    if (spaceId.isEmpty()) {
        error = tr("No Space available.");
    } else if (_remoteTree->selectedItems().isEmpty()) {
        error = tr("Select a folder on the server.");
    } else if (_localPathEdit->text().trimmed().isEmpty()) {
        // no error message, just not complete yet
        error = QStringLiteral(" ");
    } else if (QDir::isRelativePath(localPath)) {
        error = tr("Please enter an absolute path for the local folder.");
    }

    if (error.isEmpty()) {
        // don't sync the same server folder (or a parent/child of it) into two places
        for (auto *folder : FolderMan::instance()->folders()) {
            if (folder->accountState() != _accountState || !folder->space() || folder->space()->id() != spaceId) {
                continue;
            }
            if (remotePathsOverlap(folder->remoteSubPath(), subPath)) {
                error = tr("The selected server folder overlaps with the folder »%1« that is already synced to »%2«.")
                            .arg(folder->displayName(), folder->shortGuiLocalPath());
                break;
            }
        }
    }

    if (error.isEmpty()) {
        error = FolderMan::instance()->checkPathValidityForNewFolder(localPath, FolderMan::NewFolderType::SpacesFolder, accountUuid);
    }

    if (error.isEmpty() && _vfsCheckBox->isChecked()) {
        if (auto result = VfsPluginManager::instance().prepare(localPath, accountUuid, VfsPluginManager::instance().bestAvailableVfsMode()); !result) {
            error = result.error();
        }
    }

    if (error.isEmpty()) {
        const QDir dir(localPath);
        if (dir.exists() && !dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            info = tr("The local folder is not empty. Its contents will be merged with the server folder: "
                      "files that only exist locally will be uploaded, files that differ on both sides will be kept as conflict copies.");
        } else if (!dir.exists()) {
            info = tr("The local folder will be created.");
        }
    }

    const bool ok = error.isEmpty();
    _buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
    _errorLabel->setVisible(!ok && !error.trimmed().isEmpty());
    _errorLabel->setText(error.trimmed());
    _infoLabel->setVisible(ok && !info.isEmpty());
    _infoLabel->setText(info);
}

CustomFolderDialog::Result CustomFolderDialog::selection() const
{
    Result r;
    r.spaceId = _spaceCombo->currentData().toString();
    r.spaceRootDavUrl = currentSpaceRootUrl();
    r.spaceDisplayName = _spaceCombo->currentText();
    r.remoteSubPath = currentRemoteSubPath();
    r.localPath = QDir::cleanPath(QDir::fromNativeSeparators(_localPathEdit->text().trimmed()));
    r.useVirtualFiles = _vfsCheckBox->isChecked() && _vfsCheckBox->isEnabled();
    return r;
}

}
