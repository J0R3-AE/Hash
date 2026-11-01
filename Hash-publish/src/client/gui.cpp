#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPair>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTextCursor>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#include <QtPlugin>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <filesystem>

namespace fs = std::filesystem;

// ============================================================
// Browser detection
// ============================================================

// These are the browser identifiers passed to the CLI.
static QStringList browserExecutableNames(const QString &browser)
{
    if (browser == "firefox")
    {
        return {
            "firefox.exe",
            "firefox",
            "firefox-esr",
            "firefox-bin"};
    }

    if (browser == "edge")
    {
        return {
            "msedge.exe",
            "msedge",
            "microsoft-edge",
            "microsoft-edge-stable"};
    }

    if (browser == "chrome")
    {
        return {
            "chrome.exe",
            "chrome",
            "google-chrome",
            "google-chrome-stable"};
    }

    if (browser == "brave")
    {
        return {
            "brave.exe",
            "brave",
            "brave-browser"};
    }

    if (browser == "chromium")
    {
        return {
            "chromium.exe",
            "chromium",
            "chromium-browser"};
    }

    return {};
}

// Return possible browser installation paths.
//
// On Windows, check Program Files and Local AppData.
// Under WSL, also check mounted Windows drives and user profiles.
static QStringList knownBrowserPaths(const QString &browser)
{
    QStringList roots;
    QStringList relativePaths;

#ifdef Q_OS_WIN

    roots << qEnvironmentVariable("ProgramFiles")
          << qEnvironmentVariable("ProgramFiles(x86)")
          << qEnvironmentVariable("LOCALAPPDATA");

#elif defined(Q_OS_LINUX)

    const bool isWSL =
        !qEnvironmentVariable("WSL_DISTRO_NAME").isEmpty() || QFileInfo::exists(
                                                                  "/proc/sys/fs/binfmt_misc/WSLInterop");

    if (isWSL)
    {
        roots << "/mnt/c/Program Files"
              << "/mnt/c/Program Files (x86)";

        QDir users("/mnt/c/Users");

        const QStringList profiles =
            users.entryList(
                QDir::Dirs | QDir::NoDotAndDotDot);

        for (const QString &profile : profiles)
        {
            roots << users.filePath(
                profile + "/AppData/Local");
        }
    }

#endif

    if (browser == "firefox")
    {
        relativePaths << "Mozilla Firefox/firefox.exe"
                      << "Programs/Mozilla Firefox/firefox.exe";
    }
    else if (browser == "edge")
    {
        relativePaths << "Microsoft/Edge/Application/msedge.exe";
    }
    else if (browser == "chrome")
    {
        relativePaths << "Google/Chrome/Application/chrome.exe";
    }
    else if (browser == "brave")
    {
        relativePaths
            << "BraveSoftware/Brave-Browser/Application/brave.exe";
    }
    else if (browser == "chromium")
    {
        relativePaths << "Chromium/Application/chromium.exe"
                      << "Chromium/Application/chrome.exe"
                      << "Programs/Chromium/Application/chrome.exe";
    }

    QStringList paths;

    for (const QString &root : roots)
    {
        if (root.isEmpty())
        {
            continue;
        }

        for (const QString &relativePath : relativePaths)
        {
            paths << QDir(root).filePath(relativePath);
        }
    }

    return paths;
}

// Resolve a browser identifier to its actual executable.
static QString resolveBrowserExecutable(const QString &browserName)
{
    const QString browser =
        browserName.trimmed().toLower();

    if (browser.isEmpty())
    {
        return {};
    }

    // First, search PATH.
    for (const QString &executable :
         browserExecutableNames(browser))
    {
        const QString resolved =
            QStandardPaths::findExecutable(executable);

        if (!resolved.isEmpty())
        {
            return resolved;
        }
    }

    // Then, search common installation directories.
    for (const QString &path : knownBrowserPaths(browser))
    {
        const QFileInfo info(path);

        if (info.exists() && info.isFile())
        {
            return info.absoluteFilePath();
        }
    }

    return {};
}

// Return display names and separate CLI identifiers.
static QList<QPair<QString, QString>> findInstalledBrowsers()
{
    const QList<QPair<QString, QString>> known = {
        qMakePair(QStringLiteral("Firefox"),
                  QStringLiteral("firefox")),

        qMakePair(QStringLiteral("Microsoft Edge"),
                  QStringLiteral("edge")),

        qMakePair(QStringLiteral("Google Chrome"),
                  QStringLiteral("chrome")),

        qMakePair(QStringLiteral("Brave"),
                  QStringLiteral("brave")),

        qMakePair(QStringLiteral("Chromium"),
                  QStringLiteral("chromium"))};

    QList<QPair<QString, QString>> installed;

    for (const auto &browser : known)
    {
        if (!resolveBrowserExecutable(browser.second).isEmpty())
        {
            installed.append(browser);
        }
    }

    return installed;
}

// ============================================================
// Main window
// ============================================================

class DownloaderWindow : public QMainWindow
{
public:
    DownloaderWindow()
    {
        auto *central = new QWidget(this);
        setCentralWidget(central);

        auto *layout = new QVBoxLayout(central);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        // ----------------------------------------------------
        // Form
        // ----------------------------------------------------

        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(
            QFormLayout::ExpandingFieldsGrow);

        urlEdit = new QLineEdit(this);

        selectorEdit = new QLineEdit(this);
        selectorEdit->setText("mp4:1080");

        browserCombo = new QComboBox(this);

        for (const auto &browser : findInstalledBrowsers())
        {
            browserCombo->addItem(
                browser.first,
                browser.second);
        }

        // If no supported browser was found, allow the user
        // to use the operating system's default browser.
        if (browserCombo->count() == 0)
        {
            browserCombo->addItem(
                "System default",
                QString());
        }

        cookiesEdit = new QLineEdit(this);

        outputEdit = new QLineEdit(this);
        outputEdit->setText(
            QString::fromStdString(
                fs::current_path().string()));

        form->addRow("YouTube URL:", urlEdit);
        form->addRow("Selector:", selectorEdit);
        form->addRow("Browser:", browserCombo);
        form->addRow("Cookies file / SQLite DB:", cookiesEdit);
        form->addRow("Output folder:", outputEdit);

        // ----------------------------------------------------
        // Buttons
        // ----------------------------------------------------

        auto *rowButtons = new QHBoxLayout;

        auto *browseCookiesButton =
            new QPushButton("Browse", this);

        auto *browseOutputButton =
            new QPushButton("Browse", this);

        auto *openLoginButton =
            new QPushButton("Open YouTube", this);

        auto *listButton =
            new QPushButton("List", this);

        startButton = new QPushButton("Download", this);
        startButton->setDefault(true);

        rowButtons->addWidget(browseCookiesButton);
        rowButtons->addWidget(browseOutputButton);
        rowButtons->addWidget(openLoginButton);
        rowButtons->addWidget(listButton);
        rowButtons->addWidget(startButton);

        layout->addLayout(form);
        layout->addLayout(rowButtons);

        // ----------------------------------------------------
        // Output log
        // ----------------------------------------------------

        logEdit = new QTextEdit(this);
        logEdit->setReadOnly(true);
        logEdit->setPlaceholderText(
            "Download output will appear here.");
        logEdit->setMinimumHeight(180);

        layout->addWidget(logEdit, 1);

        // ----------------------------------------------------
        // Browse for cookies
        // ----------------------------------------------------

        connect(
            browseCookiesButton,
            &QPushButton::clicked,
            this,
            [this]()
            {
                const QString path =
                    QFileDialog::getOpenFileName(
                        this,
                        "Choose cookies.txt or SQLite database",
                        QDir::homePath(),
                        "Cookies file (*.txt *.sqlite *.sqlite3 *.db);;"
                        "All files (*)");

                if (!path.isEmpty())
                {
                    cookiesEdit->setText(path);
                }
            });

        // ----------------------------------------------------
        // Browse for output directory
        // ----------------------------------------------------

        connect(
            browseOutputButton,
            &QPushButton::clicked,
            this,
            [this]()
            {
                const QString path =
                    QFileDialog::getExistingDirectory(
                        this,
                        "Choose output folder",
                        QDir::homePath());

                if (!path.isEmpty())
                {
                    outputEdit->setText(path);
                }
            });

        // ----------------------------------------------------
        // Open YouTube in the selected browser
        // ----------------------------------------------------

        connect(
            openLoginButton,
            &QPushButton::clicked,
            this,
            [this]()
            {
                const QString selectedBrowser =
                    browserCombo->currentData().toString();

                const QString browserExecutable =
                    resolveBrowserExecutable(selectedBrowser);

                if (!browserExecutable.isEmpty())
                {
                    const bool launched =
                        QProcess::startDetached(
                            browserExecutable,
                            QStringList{
                                "https://www.youtube.com"});

                    if (launched)
                    {
                        appendLog(
                            "Opened YouTube in: " + browserExecutable + "\nSign in there, then export "
                                                                        "a cookies.txt file.");

                        return;
                    }
                }

                const bool opened =
                    QDesktopServices::openUrl(
                        QUrl("https://www.youtube.com"));

                if (opened)
                {
                    appendLog(
                        "Opened YouTube in the system "
                        "default browser. Sign in there, "
                        "then export a cookies.txt file.");
                }
                else
                {
                    appendLog(
                        "Could not open YouTube.");
                }
            });

        // ----------------------------------------------------
        // Locate and launch the CLI
        // ----------------------------------------------------

        const auto runBinaryCommand = [this, listButton](const QStringList &args)
        {
            if (process && process->state() != QProcess::NotRunning)
            {
                appendLog("A command is already running. Please wait for it to finish.");
                return;
            }
            const QString cwd = QDir::currentPath();

            const QString appDir =
                QCoreApplication::applicationDirPath();

            QStringList candidates;

#ifdef Q_OS_WIN

            // Prefer native Windows executables.
            candidates
                << QDir(cwd).filePath(
                       "bin/windows/yt-downloader.exe")
                << QDir(cwd).filePath(
                       "bin/yt-downloader.exe")
                << QDir(appDir).filePath(
                       "yt-downloader.exe")
                << QDir(appDir).filePath(
                       "bin/yt-downloader.exe")
                << QDir(appDir).filePath(
                       "../../bin/windows/yt-downloader.exe");

#else

            // Prefer the Linux executable when running
            // the GUI under WSL or another Linux system.
            candidates
                << QDir(cwd).filePath(
                       "bin/yt-downloader")
                << QDir(appDir).filePath(
                       "yt-downloader")
                << QDir(appDir).filePath(
                       "../bin/yt-downloader")
                << QDir(appDir).filePath(
                       "../../bin/yt-downloader");

#endif

            QString binary;

            for (const QString &candidate : candidates)
            {
                const QFileInfo info(candidate);

                if (info.exists() && info.isFile())
                {
                    binary = info.absoluteFilePath();
                    break;
                }
            }

            if (binary.isEmpty())
            {
                QMessageBox::critical(
                    this,
                    "Binary not found",
                    "Could not find the yt-downloader "
                    "executable.\n\n"
                    "Build the CLI first and ensure it "
                    "is in the expected bin directory.");

                return;
            }

            process = new QProcess(this);

            QProcess *child = new QProcess(this);
            process = child;

#ifdef _WIN32
            child->setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments *args)
                {
                    args->flags |= CREATE_NO_WINDOW;
                });
#endif

            child->setProgram(binary);
            child->setArguments(args);

            connect(
                child,
                &QProcess::readyReadStandardOutput,
                this,
                [this, child]()
                {
                    appendLog(QString::fromUtf8(
                        child->readAllStandardOutput()));
                });

            connect(
                child,
                &QProcess::readyReadStandardError,
                this,
                [this, child]()
                {
                    appendLog(QString::fromUtf8(
                        child->readAllStandardError()));
                });

            connect(
                child,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this, child, listButton](
                    int exitCode,
                    QProcess::ExitStatus)
                {
                    appendLog(
                        QString("\nProcess finished with exit code %1\n")
                            .arg(exitCode));

                    if (process == child)
                    {
                        process = nullptr;
                    }

                    startButton->setEnabled(true);
                    listButton->setEnabled(true);
                    child->deleteLater();
                });

            connect(
                child,
                &QProcess::errorOccurred,
                this,
                [this, child, listButton](QProcess::ProcessError error)
                {
                    if (error == QProcess::FailedToStart)
                    {
                        appendLog(
                            "Failed to start the downloader: " + child->errorString());

                        if (process == child)
                        {
                            process = nullptr;
                        }

                        startButton->setEnabled(true);
                        listButton->setEnabled(true);
                        child->deleteLater();
                    }
                });

            appendLog("Running: " + binary + " " + args.join(' '));

            startButton->setEnabled(false);
            listButton->setEnabled(false);
            child->start();
        };

        // ----------------------------------------------------
        // List video / playlist formats
        // ----------------------------------------------------

        connect(
            listButton,
            &QPushButton::clicked,
            this,
            [this, runBinaryCommand]()
            {
                const QString url =
                    urlEdit->text().trimmed();

                const QString browser =
                    browserCombo->currentData().toString();

                const QString cookies =
                    cookiesEdit->text().trimmed();

                if (url.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing URL",
                        "Please enter a YouTube video "
                        "or playlist URL.");

                    return;
                }

                if (url.contains(
                        "playlist",
                        Qt::CaseInsensitive) &&
                    cookies.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Public playlist only",
                        "Without cookies, this tool can "
                        "only list the anonymous public feed. "
                        "Some private or restricted playlist "
                        "entries will not appear.");
                }

                QStringList args;
                args << "list" << url;

                if (!browser.isEmpty())
                {
                    args << "--browser" << browser;
                }

                if (!cookies.isEmpty())
                {
                    args << "--cookies" << cookies;
                }

                runBinaryCommand(args);
            });

        // ----------------------------------------------------
        // Download
        // ----------------------------------------------------

        connect(
            startButton,
            &QPushButton::clicked,
            this,
            [this, runBinaryCommand]()
            {
                const QString url =
                    urlEdit->text().trimmed();

                const QString selector =
                    selectorEdit->text().trimmed();

                const QString browser =
                    browserCombo->currentData().toString();

                const QString cookies =
                    cookiesEdit->text().trimmed();

                const QString output =
                    outputEdit->text().trimmed();

                if (url.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing URL",
                        "Please enter a YouTube video "
                        "or playlist URL.");

                    return;
                }

                if (selector.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing selector",
                        "Please enter a selector such as "
                        "mp4:1080.");

                    return;
                }

                if (output.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing output folder",
                        "Please choose an output folder.");

                    return;
                }

                if (cookies.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "No cookies supplied",
                        "Continuing without cookies may fail "
                        "for private, age-gated, or "
                        "region-locked videos. Public content "
                        "may still work.");
                }

                QStringList args;
                args << "download" << url << selector;

                if (!browser.isEmpty())
                {
                    args << "--browser" << browser;
                }

                if (!cookies.isEmpty())
                {
                    args << "--cookies" << cookies;
                }

                args << "-o" << output;

                runBinaryCommand(args);
            });

        setWindowTitle("Hash YouTube Downloader");
        resize(760, 540);
    }

private:
    void appendLog(const QString &text)
    {
        logEdit->append(text);
        logEdit->moveCursor(QTextCursor::End);
    }

    QLineEdit *urlEdit = nullptr;
    QLineEdit *selectorEdit = nullptr;
    QComboBox *browserCombo = nullptr;
    QLineEdit *cookiesEdit = nullptr;
    QLineEdit *outputEdit = nullptr;

    QTextEdit *logEdit = nullptr;
    QPushButton *startButton = nullptr;

    QProcess *process = nullptr;
};

// ============================================================
// Entry point
// ============================================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    DownloaderWindow window;
    window.show();

    return app.exec();
}
