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

#pragma once

#include "gui/accountstate.h"

#include <QDialog>
#include <QUrl>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace OCC {

/**
 * Lets the user sync a folder of a Space (or a whole Space) to any local folder.
 */
class CustomFolderDialog : public QDialog
{
    Q_OBJECT
public:
    struct Result
    {
        QString spaceId;
        QUrl spaceRootDavUrl;
        QString spaceDisplayName;
        /// relative to the Space root, empty for the whole Space
        QString remoteSubPath;
        QString localPath;
        bool useVirtualFiles = false;
    };

    explicit CustomFolderDialog(const AccountStatePtr &accountState, QWidget *parent = nullptr);

    Result result() const;

private:
    void populateSpaces();
    void reloadRemoteTree();
    void listRemoteFolder(QTreeWidgetItem *item);
    void chooseLocalFolder();
    void validate();

    QUrl currentSpaceRootUrl() const;
    QString currentRemoteSubPath() const;

    AccountStatePtr _accountState;

    QComboBox *_spaceCombo;
    QTreeWidget *_remoteTree;
    QLineEdit *_localPathEdit;
    QCheckBox *_vfsCheckBox;
    QLabel *_infoLabel;
    QLabel *_errorLabel;
    QDialogButtonBox *_buttons;

    // incremented whenever the remote tree is rebuilt, so late replies for old items are ignored
    int _treeGeneration = 0;
};

}
