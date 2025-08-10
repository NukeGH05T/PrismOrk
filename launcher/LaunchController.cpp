// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "LaunchController.h"
#include "Application.h"
#include "launch/steps/PrintServers.h"
#include "minecraft/auth/AccountData.h"
#include "minecraft/auth/AccountList.h"

#include "ui/InstanceWindow.h"
#include "ui/MainWindow.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/MSALoginDialog.h"
#include "ui/dialogs/ProfileSelectDialog.h"
#include "ui/dialogs/ProfileSetupDialog.h"
#include "ui/dialogs/ProgressDialog.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>

#include "BuildConfig.h"
#include "JavaCommon.h"
#include "launch/steps/TextPrint.h"
#include "tasks/Task.h"

LaunchController::LaunchController() : Task() {}

void LaunchController::executeTask()
{
    if (!m_instance) {
        emitFailed(tr("No instance specified!"));
        return;
    }

    if (!JavaCommon::checkJVMArgs(m_instance->settings()->get("JvmArgs").toString(), m_parentWidget)) {
        emitFailed(tr("Invalid Java arguments specified. Please fix this first."));
        return;
    }

    login();
}

void LaunchController::decideAccount()
{
    if (m_accountToUse) {
        return;
    }

    // Find an account to use.
    auto accounts = APPLICATION->accounts();

    // Removed forced login requirement - allow no accounts gracefully
    if (accounts->count() <= 0) {
        // No accounts found, do not force user to create one.
        m_accountToUse = nullptr;
        return;
    }

    // Select the account to use. If the instance has a specific account set, that will be used. Otherwise, the default account will be used
    auto instanceAccountId = m_instance->settings()->get("InstanceAccountId").toString();
    auto instanceAccountIndex = accounts->findAccountByProfileId(instanceAccountId);
    if (instanceAccountIndex == -1 || instanceAccountId.isEmpty()) {
        m_accountToUse = accounts->defaultAccount();
    } else {
        m_accountToUse = accounts->at(instanceAccountIndex);
    }

    if (!m_accountToUse) {
        // If no default account is set, ask the user which one to use.
        ProfileSelectDialog selectDialog(tr("Which account would you like to use?"), ProfileSelectDialog::GlobalDefaultCheckbox,
                                         m_parentWidget);

        selectDialog.exec();

        // Launch the instance with the selected account.
        m_accountToUse = selectDialog.selectedAccount();

        // If the user said to use the account as default, do that.
        if (selectDialog.useAsGlobalDefault() && m_accountToUse) {
            accounts->setDefaultAccount(m_accountToUse);
        }
    }
}

bool LaunchController::askPlayDemo()
{
    // Removed the forced demo prompt - always return false to disable demo mode prompt
    return false;
}

QString LaunchController::askOfflineName(QString playerName, bool /*demo*/, bool& ok)
{
    // we ask the user for a player name
    QString message = tr("Choose your offline mode player name.");

    QString lastOfflinePlayerName = APPLICATION->settings()->get("LastOfflinePlayerName").toString();
    QString usedname = lastOfflinePlayerName.isEmpty() ? playerName : lastOfflinePlayerName;
    QString name = QInputDialog::getText(m_parentWidget, tr("Player name"), message, QLineEdit::Normal, usedname, &ok);
    if (!ok)
        return {};
    if (name.length()) {
        usedname = name;
        APPLICATION->settings()->set("LastOfflinePlayerName", usedname);
    }
    return usedname;
}

void LaunchController::login()
{
    decideAccount();

    if (!m_accountToUse) {
        // No account available, create offline session directly
        bool ok = false;
        auto name = askOfflineName("Player", false, ok);
        if (ok) {
            m_session = std::make_shared<AuthSession>();
            static const QRegularExpression s_removeChars("[{}-]");
            m_session->MakeOffline(name);
            launchInstance();
            return;
        } else {
            emitFailed(tr("No account selected for launch."));
            return;
        }
    }

    // Bypass ownership checks: allow offline accounts to proceed
    // Also, don't force demo mode
    m_session = std::make_shared<AuthSession>();
    m_session->wants_online = false; // disable online mode
    m_session->demo = false;

    // If offline account, create offline session directly
    if (m_accountToUse->accountType() == AccountType::Offline) {
        // Offline session with saved player name or prompt
        bool ok = false;
        QString playerName = m_accountToUse->profileName();
        if (playerName.isEmpty())
            playerName = "Player";

        auto name = askOfflineName(playerName, false, ok);
        if (!ok) {
            emitFailed(tr("Offline player name was not set."));
            return;
        }

        static const QRegularExpression s_removeChars("[{}-]");
        m_session->MakeOffline(name);
        launchInstance();
        return;
    }

    // For other account types (if any), try to refresh and proceed without forcing online
    if (m_accountToUse->accountState() == AccountState::Offline || m_accountToUse->accountState() == AccountState::Unchecked) {
        m_accountToUse->refresh();
    }

    // Prepare session for launch
    m_accountToUse->fillSession(m_session);

    launchInstance();
}

bool LaunchController::reauthenticateAccount(MinecraftAccountPtr /*account*/)
{
    // Disable reauthentication dialog since no online required
    emitFailed(tr("Account reauthentication is disabled in offline mode."));
    return false;
}

void LaunchController::launchInstance()
{
    Q_ASSERT_X(m_instance != NULL, "launchInstance", "instance is NULL");
    Q_ASSERT_X(m_session.get() != nullptr, "launchInstance", "session is NULL");

    if (!m_instance->reloadSettings()) {
        QMessageBox::critical(m_parentWidget, tr("Error!"), tr("Couldn't load the instance profile."));
        emitFailed(tr("Couldn't load the instance profile."));
        return;
    }

    m_launcher = m_instance->createLaunchTask(m_session, m_targetToJoin);
    if (!m_launcher) {
        emitFailed(tr("Couldn't instantiate a launcher."));
        return;
    }

    auto console = qobject_cast<InstanceWindow*>(m_parentWidget);
    auto showConsole = m_instance->settings()->get("ShowConsole").toBool();
    if (!console && showConsole) {
        APPLICATION->showInstanceWindow(m_instance);
    }
    connect(m_launcher.get(), &LaunchTask::readyForLaunch, this, &LaunchController::readyForLaunch);
    connect(m_launcher.get(), &LaunchTask::succeeded, this, &LaunchController::onSucceeded);
    connect(m_launcher.get(), &LaunchTask::failed, this, &LaunchController::onFailed);
    connect(m_launcher.get(), &LaunchTask::requestProgress, this, &LaunchController::onProgressRequested);

    // Mark as offline launch mode (no servers printed)
    QString online_mode = "offline";

    m_launcher->prependStep(
        makeShared<TextPrint>(m_launcher.get(), "Launched instance in " + online_mode + " mode\n", MessageLevel::Launcher));

    // Prepend Version
    {
        auto versionString = QString("%1 version: %2 (%3)")
                                 .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString(), BuildConfig.BUILD_PLATFORM);
        m_launcher->prependStep(makeShared<TextPrint>(m_launcher.get(), versionString + "\n\n", MessageLevel::Launcher));
    }
    m_launcher->start();
}

void LaunchController::readyForLaunch()
{
    if (!m_profiler) {
        m_launcher->proceed();
        return;
    }

    QString error;
    if (!m_profiler->check(&error)) {
        m_launcher->abort();
        emitFailed("Profiler startup failed!");
        QMessageBox::critical(m_parentWidget, tr("Error!"), tr("Profiler check for %1 failed: %2").arg(m_profiler->name(), error));
        return;
    }
    BaseProfiler* profilerInstance = m_profiler->createProfiler(m_launcher->instance(), this);

    connect(profilerInstance, &BaseProfiler::readyToLaunch, [this](const QString& message) {
        QMessageBox msg(m_parentWidget);
        msg.setText(tr("The game launch is delayed until you press the "
                       "button. This is the right time to setup the profiler, as the "
                       "profiler server is running now.\n\n%1")
                        .arg(message));
        msg.setWindowTitle(tr("Waiting."));
        msg.setIcon(QMessageBox::Information);
        msg.addButton(tr("&Launch"), QMessageBox::AcceptRole);
        msg.exec();
        m_launcher->proceed();
    });
    connect(profilerInstance, &BaseProfiler::abortLaunch, [this](const QString& message) {
        QMessageBox msg;
        msg.setText(tr("Couldn't start the profiler: %1").arg(message));
        msg.setWindowTitle(tr("Error"));
        msg.setIcon(QMessageBox::Critical);
        msg.addButton(QMessageBox::Ok);
        msg.setModal(true);
        msg.exec();
        m_launcher->abort();
        emitFailed("Profiler startup failed!");
    });
    profilerInstance->beginProfiling(m_launcher);
}

void LaunchController::onSucceeded()
{
    emitSucceeded();
}

void LaunchController::onFailed(QString reason)
{
    if (m_instance->settings()->get("ShowConsoleOnError").toBool()) {
        APPLICATION->showInstanceWindow(m_instance, "console");
    }
    emitFailed(reason);
}

void LaunchController::onProgressRequested(Task* task)
{
    ProgressDialog progDialog(m_parentWidget);
    progDialog.setSkipButton(true, tr("Abort"));
    m_launcher->proceed();
    progDialog.execWithTask(task);
}

bool LaunchController::abort()
{
    if (!m_launcher) {
        return true;
    }
    if (!m_launcher->canAbort()) {
        return false;
    }
    auto response = CustomMessageBox::selectable(m_parentWidget, tr("Kill Minecraft?"),
                                                 tr("This can cause the instance to get corrupted and should only be used if Minecraft "
                                                    "is frozen for some reason"),
                                                 QMessageBox::Question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                        ->exec();
    if (response == QMessageBox::Yes) {
        return m_launcher->abort();
    }
    return false;
}
