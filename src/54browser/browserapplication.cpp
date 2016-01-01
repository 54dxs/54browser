/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "browserapplication.h"

#include "bookmarks.h"
#include "browsermainwindow.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "history.h"
#include "networkaccessmanager.h"
#include "tabwidget.h"
#include "webview.h"

#include <QtCore/QBuffer>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QLibraryInfo>
#include <QtCore/QSettings>
#include <QtCore/QTextStream>
#include <QtCore/QTranslator>

#include <QtGui/QDesktopServices>
#include <QtGui/QFileOpenEvent>
#include <QtWidgets/QMessageBox>

#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslSocket>

#include <QWebSettings>

#include <QtCore/QDebug>

DownloadManager *BrowserApplication::s_downloadManager = 0;
HistoryManager *BrowserApplication::s_historyManager = 0;
NetworkAccessManager *BrowserApplication::s_networkAccessManager = 0;
BookmarksManager *BrowserApplication::s_bookmarksManager = 0;

/**
 * 显示帮助信息
 * @param parser 参数列表
 * @param errorMessage 错误时要显示的信息
 */
static void showHelp(QCommandLineParser &parser, const QString errorMessage = QString())
{
    QString text;
    QTextStream str(&text);
    str << "<html><head/><body>";
    if (!errorMessage.isEmpty())
        str << errorMessage;
    str << "<pre>" << parser.helpText() << "</pre></body></html>";
    QMessageBox box(errorMessage.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
        QGuiApplication::applicationDisplayName(), text, QMessageBox::Ok);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.exec();
}

BrowserApplication::BrowserApplication(int &argc, char **argv)
    : QApplication(argc, argv)
    , m_localServer(0)
    , m_initialUrl(QString())
    , m_correctlyInitialized(false)
{
    QCoreApplication::setOrganizationName(QLatin1String("54dxs"));//设置公司名
    QCoreApplication::setApplicationName(QLatin1String("54browser"));//设置软件名
    QCoreApplication::setApplicationVersion(QLatin1String("0.1"));//设置软件版本

    // QCommandLineParser是提供了一系列命令行参数的类
    QCommandLineParser commandLineParser;
    // 定义应用程序的附加参数
    // 参数名称和描述将出现在参数下的“帮助”部分。如果指定了语法，它将被添加到使用行中，否则将被追加。
    commandLineParser.addPositionalArgument(QStringLiteral("url"), QStringLiteral("在浏览器窗口中加载的网址"));

    //通常arguments().at(0)是应用程序的名称，arguments().at(1)是第一个参数
    if (!commandLineParser.parse(QCoreApplication::arguments())) {
        showHelp(commandLineParser, QStringLiteral("<p>无效参数</p>"));
        return;
    }

    QStringList args = commandLineParser.positionalArguments();
    if (args.count() > 1) {
        showHelp(commandLineParser, QStringLiteral("<p>参数过多</p>"));
        return;
    } else if (args.count() == 1) {
        m_initialUrl = args.at(0);
    }
    if (!m_initialUrl.isEmpty() && !QUrl::fromUserInput(m_initialUrl).isValid()) {
        showHelp(commandLineParser, QString("<p>%1 不是一个有效的url</p>").arg(m_initialUrl));
        return;
    }

    m_correctlyInitialized = true;

    //创建一个本地socket连接，如果在500ms连接超时，返回
    QString serverName = QCoreApplication::applicationName();
    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (socket.waitForConnected(500)) {
        QTextStream stream(&socket);
        stream << m_initialUrl;
        stream.flush();
        socket.waitForBytesWritten();
        return;
    }

    // 设置为true时：最后一个可见窗口被关闭时，应用程序被关闭
#if defined(Q_OS_OSX)
    QApplication::setQuitOnLastWindowClosed(false);
#else
    QApplication::setQuitOnLastWindowClosed(true);
#endif

    m_localServer = new QLocalServer(this);
    connect(m_localServer, SIGNAL(newConnection()), this, SLOT(newLocalSocketConnection()));
    //根据名称去监听连接
    if (!m_localServer->listen(serverName)) {
        //如果连接有错误，则重新建立连接
        if (m_localServer->serverError() == QAbstractSocket::AddressInUseError
            && QFile::exists(m_localServer->serverName())) {
            QFile::remove(m_localServer->serverName());
            m_localServer->listen(serverName);
        }
    }

    //检测SSL是否开启
#ifndef QT_NO_OPENSSL
    if (!QSslSocket::supportsSsl()) {
    QMessageBox::information(0, "54browser", "这个系统不支持OpenSSL。SSL的网站将不可用。");
    }
#endif

    //创建一个桌面服务
    QDesktopServices::setUrlHandler(QLatin1String("http"), this, "openUrl");
    QString localSysName = QLocale::system().name();//获得本地系统名称

    installTranslator(QLatin1String("qt_") + localSysName);

    QSettings settings;
    settings.beginGroup(QLatin1String("sessions"));
    m_lastSession = settings.value(QLatin1String("lastSession")).toByteArray();
    settings.endGroup();

#if defined(Q_OS_OSX)
    connect(this, SIGNAL(lastWindowClosed()), this, SLOT(lastWindowClosed()));
#endif

    //以一个给定的时间间隔调用槽函数
    QTimer::singleShot(0, this, SLOT(postLaunch()));
}

BrowserApplication::~BrowserApplication()
{
    delete s_downloadManager;
    for (int i = 0; i < m_mainWindows.size(); ++i) {
        BrowserMainWindow *window = m_mainWindows.at(i);
        delete window;
    }
    delete s_networkAccessManager;
    delete s_bookmarksManager;
}

/**
 * 将最后被关闭的窗口添加至窗口管理器中
 */
#if defined(Q_OS_OSX)
void BrowserApplication::lastWindowClosed()
{
    //清除本地信息，创建一个新的浏览器窗口对象，并添加到窗口对象集合中
    clean();
    BrowserMainWindow *mw = new BrowserMainWindow;
    mw->slotHome();
    m_mainWindows.prepend(mw);
}
#endif

BrowserApplication *BrowserApplication::instance()
{
    return (static_cast<BrowserApplication *>(QCoreApplication::instance()));
}

/**
 * 退出浏览器
 */
#if defined(Q_OS_OSX)
#include <QtWidgets/QMessageBox>
void BrowserApplication::quitBrowser()
{
    clean();
    int tabCount = 0;//全部浏览器窗口的选项卡的总个数
    for (int i = 0; i < m_mainWindows.count(); ++i) {
        tabCount =+ m_mainWindows.at(i)->tabWidget()->count();
    }

    if (tabCount > 1) {
        int ret = QMessageBox::warning(mainWindow(), QString(),
                           tr("总共有 %1 个窗口和 %2 个选项卡被打开\n"
                              "你确定要全部退出吗？").arg(m_mainWindows.count()).arg(tabCount),
                           QMessageBox::Yes | QMessageBox::No,
                           QMessageBox::No);
        if (ret == QMessageBox::No)
            return;
    }

    exit(0);
}
#endif

/**
 * 请求桌面(直到窗口被显示，否则任何动作都将被延迟)
 */
void BrowserApplication::postLaunch()
{
    //QStandardPaths::DataLocation返回持久化应用程序数据可以存储的目录位置
    QString directory = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
    //如果目录为空，则设置一个默认的目录
    //QDir::homePath()返回用户主目录的绝对路径
    if (directory.isEmpty())
        directory = QDir::homePath() + QLatin1String("/.") + QCoreApplication::applicationName();
    QWebSettings::setIconDatabasePath(directory);//设置favicons的存储路径
//    QWebSettings::setLocalStoragePath(directory);//设置HTML5本地存储路径
    QWebSettings::setOfflineStoragePath(directory);//设置HTML5客户的数据库存储路径

    setWindowIcon(QIcon(QLatin1String(":browser.svg")));

    loadSettings();

    // newMainWindow() 需要在main()中被调用以唤醒
    if (m_mainWindows.count() > 0) {
        //如果m_initialUrl设置了值则使用之，否则开启主页
        if (!m_initialUrl.isEmpty())
            mainWindow()->loadPage(m_initialUrl);
        else
            mainWindow()->slotHome();
    }
    BrowserApplication::historyManager();
}

/**
 * 加载设置信息
 */
void BrowserApplication::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("websettings"));

    QWebSettings *defaultSettings = QWebSettings::globalSettings();
    //设置标准字体设置
    QString standardFontFamily = defaultSettings->fontFamily(QWebSettings::StandardFont);//字体
    int standardFontSize = defaultSettings->fontSize(QWebSettings::DefaultFontSize);//字号
    QFont standardFont = QFont(standardFontFamily, standardFontSize);
    standardFont = qvariant_cast<QFont>(settings.value(QLatin1String("standardFont"), standardFont));
    defaultSettings->setFontFamily(QWebSettings::StandardFont, standardFont.family());
    defaultSettings->setFontSize(QWebSettings::DefaultFontSize, standardFont.pointSize());

    //设置固定字体设置
    QString fixedFontFamily = defaultSettings->fontFamily(QWebSettings::FixedFont);
    int fixedFontSize = defaultSettings->fontSize(QWebSettings::DefaultFixedFontSize);
    QFont fixedFont = QFont(fixedFontFamily, fixedFontSize);
    fixedFont = qvariant_cast<QFont>(settings.value(QLatin1String("fixedFont"), fixedFont));
    defaultSettings->setFontFamily(QWebSettings::FixedFont, fixedFont.family());
    defaultSettings->setFontSize(QWebSettings::DefaultFixedFontSize, fixedFont.pointSize());

    //设置Javascript及插件支持
    defaultSettings->setAttribute(QWebSettings::JavascriptEnabled, settings.value(QLatin1String("enableJavascript"), true).toBool());//设置是否启动Javascript
    defaultSettings->setAttribute(QWebSettings::PluginsEnabled, settings.value(QLatin1String("enablePlugins"), true).toBool());//设置是否启动插件

    //设置样式表的加载地址(该Url可以是：1.本地磁盘的文件路径；2.远程url)
    QUrl url = settings.value(QLatin1String("userStyleSheet")).toUrl();
    defaultSettings->setUserStyleSheetUrl(url);

    //设置是否启用DNS加速
    defaultSettings->setAttribute(QWebSettings::DnsPrefetchEnabled, true);

    settings.endGroup();
}

/**
 * 获得浏览器窗口集合
 * @return
 */
QList<BrowserMainWindow*> BrowserApplication::mainWindows()
{
    clean();
    QList<BrowserMainWindow*> list;
    for (int i = 0; i < m_mainWindows.count(); ++i)
        list.append(m_mainWindows.at(i));
    return list;
}

/**
 * 清理本地窗口集合中的Null数据
 */
void BrowserApplication::clean()
{
    // 清除所有已删除的主窗口
    for (int i = m_mainWindows.count() - 1; i >= 0; --i)
        if (m_mainWindows.at(i).isNull())
            m_mainWindows.removeAt(i);
}

/**
 * 保存Session
 */
void BrowserApplication::saveSession()
{
    QWebSettings *globalSettings = QWebSettings::globalSettings();
    //如果开启了”隐私浏览模式“则返回
    if (globalSettings->testAttribute(QWebSettings::PrivateBrowsingEnabled))
        return;

    clean();

    QSettings settings;
    settings.beginGroup(QLatin1String("sessions"));

    QByteArray data;
    QBuffer buffer(&data);
    QDataStream stream(&buffer);
    buffer.open(QIODevice::ReadWrite);

    stream << m_mainWindows.count();
    for (int i = 0; i < m_mainWindows.count(); ++i)
        stream << m_mainWindows.at(i)->saveState();
    settings.setValue(QLatin1String("lastSession"), data);
    settings.endGroup();
}

/**
 * 判断Session是否可以恢复(即lastSession是否为空)
 * @return
 */
bool BrowserApplication::canRestoreSession() const
{
    return !m_lastSession.isEmpty();
}

/**
 * 恢复Session
 */
void BrowserApplication::restoreLastSession()
{
    QList<QByteArray> windows;
    QBuffer buffer(&m_lastSession);
    QDataStream stream(&buffer);
    buffer.open(QIODevice::ReadOnly);
    int windowCount;
    stream >> windowCount;
    for (int i = 0; i < windowCount; ++i) {
        QByteArray windowState;
        stream >> windowState;
        windows.append(windowState);
    }
    for (int i = 0; i < windows.count(); ++i) {
        BrowserMainWindow *newWindow = 0;
        //如果当前窗体有且只有一个，且只有一个选项卡，这恢复该窗体;否则创建一个新的窗口
        if (m_mainWindows.count() == 1
            && mainWindow()->tabWidget()->count() == 1
            && mainWindow()->currentTab()->url() == QUrl()) {
            newWindow = mainWindow();
        } else {
            newWindow = newMainWindow();
        }
        newWindow->restoreState(windows.at(i));
    }
}

/**
 * 判断是否是唯一的浏览器
 * @return
 */
bool BrowserApplication::isTheOnlyBrowser() const
{
    return (m_localServer != 0);
}

/**
 * 是否被正确的初始化
 * @return
 */
bool BrowserApplication::isCorrectlyInitialized() const
{
    return m_correctlyInitialized;
}

/**
 * 根据一个名称安装一个翻译器
 * @param name
 */
void BrowserApplication::installTranslator(const QString &name)
{
    QTranslator *translator = new QTranslator(this);
    translator->load(name, QLibraryInfo::location(QLibraryInfo::TranslationsPath));
    QApplication::installTranslator(translator);
}

#if defined(Q_OS_OSX)
bool BrowserApplication::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::ApplicationActivate: {
        clean();
        if (!m_mainWindows.isEmpty()) {
            BrowserMainWindow *mw = mainWindow();
            if (mw && !mw->isMinimized()) {
                mainWindow()->show();
            }
            return true;
        }
    }
    case QEvent::FileOpen:
        if (!m_mainWindows.isEmpty()) {
            mainWindow()->loadPage(static_cast<QFileOpenEvent *>(event)->file());
            return true;
        }
    default:
        break;
    }
    return QApplication::event(event);
}
#endif

/**
 * 在浏览器窗口中打开一个网址
 * @param url
 */
void BrowserApplication::openUrl(const QUrl &url)
{
    mainWindow()->loadPage(url.toString());
}

/**
 * 创建一个浏览器窗口
 * @return
 */
BrowserMainWindow *BrowserApplication::newMainWindow()
{
    BrowserMainWindow *browser = new BrowserMainWindow();
    m_mainWindows.prepend(browser);
    browser->show();
    return browser;
}

/**
 * 返回一个浏览器窗口
 * @return
 */
BrowserMainWindow *BrowserApplication::mainWindow()
{
    //先清除本地窗口,再重新创建一个浏览器窗口
    clean();
    if (m_mainWindows.isEmpty())
        newMainWindow();
    return m_mainWindows[0];
}

/**
 * 创建一个本地服务的连接
 */
void BrowserApplication::newLocalSocketConnection()
{
    //作为一个连接qlocalsocket对象返回下一个等待连接。
    //该socket是作为server的一个孩子，即当QLocalServer对象被销毁，孩子自动会被删除，回收内存
    QLocalSocket *socket = m_localServer->nextPendingConnection();
    if (!socket)
        return;
    socket->waitForReadyRead(1000);
    QTextStream stream(socket);
    QString url;
    stream >> url;
    if (!url.isEmpty()) {
        QSettings settings;
        settings.beginGroup(QLatin1String("general"));
        int openLinksIn = settings.value(QLatin1String("openLinksIn"), 0).toInt();
        settings.endGroup();
        //如果为1，这新建一个浏览器窗口，否则已经有了浏览器窗口则创建一个选项卡
        if (openLinksIn == 1)
            newMainWindow();
        else
            mainWindow()->tabWidget()->newTab();
        openUrl(url);
    }
    delete socket;
    //将窗口置顶并设置为活动窗口
    mainWindow()->raise();
    mainWindow()->activateWindow();
}

/**
 * 获得一个CookieJar对象
 * @return
 */
CookieJar *BrowserApplication::cookieJar()
{
    return (CookieJar*)networkAccessManager()->cookieJar();
}

/**
 * 获得一个下载管理器
 * @return
 */
DownloadManager *BrowserApplication::downloadManager()
{
    if (!s_downloadManager) {
        s_downloadManager = new DownloadManager();
    }
    return s_downloadManager;
}

/**
 * 获得一个网络接入管理器
 * @return
 */
NetworkAccessManager *BrowserApplication::networkAccessManager()
{
    if (!s_networkAccessManager) {
        s_networkAccessManager = new NetworkAccessManager();
        s_networkAccessManager->setCookieJar(new CookieJar);
    }
    return s_networkAccessManager;
}

/**
 * 获得一个历史浏览管理器
 * @return
 */
HistoryManager *BrowserApplication::historyManager()
{
    if (!s_historyManager) {
        s_historyManager = new HistoryManager();
        QWebHistoryInterface::setDefaultInterface(s_historyManager);
    }
    return s_historyManager;
}

/**
 * 获得一个收藏夹管理器
 * @return
 */
BookmarksManager *BrowserApplication::bookmarksManager()
{
    if (!s_bookmarksManager) {
        s_bookmarksManager = new BookmarksManager;
    }
    return s_bookmarksManager;
}

/**
 * 获得一个网站的favicons
 * @param url
 * @return
 */
QIcon BrowserApplication::icon(const QUrl &url) const
{
    QIcon icon = QWebSettings::iconForUrl(url);
    if (!icon.isNull())
        return icon.pixmap(16, 16);
    if (m_defaultIcon.isNull())
        m_defaultIcon = QIcon(QLatin1String(":defaulticon.png"));
    return m_defaultIcon.pixmap(16, 16);
}

