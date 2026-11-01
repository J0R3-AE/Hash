
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
#include <QMetaObject>
#include <QPair>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextCursor>
#include <QTextEdit>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifdef YTDL_GUI_EMBEDDED_CLI
#include "ytdl/cli.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>
#endif

#ifdef Q_OS_WIN
#include <QtPlugin>

#if defined(QT_STATICPLUGIN)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif
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
// Embedded CLI output capture
// ============================================================

#ifdef YTDL_GUI_EMBEDDED_CLI

class GuiLogBuffer final : public std::streambuf
{
public:
    using Callback = std::function<void(const QString &)>;

    explicit GuiLogBuffer(Callback callback)
        : callback_(std::move(callback))
    {
    }

protected:
    int_type overflow(int_type ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
        {
            flushBuffer();
            return traits_type::not_eof(ch);
        }

        const char value = traits_type::to_char_type(ch);
        buffer_.push_back(value);

        if (value == '\n' || buffer_.size() >= 4096)
        {
            flushBuffer();
        }

        return ch;
    }

    std::streamsize xsputn(
        const char *data,
        std::streamsize size) override
    {
        for (std::streamsize i = 0; i < size; ++i)
        {
            overflow(traits_type::to_int_type(data[i]));
        }

        return size;
    }

    int sync() override
    {
        flushBuffer();
        return 0;
    }

private:
    void flushBuffer()
    {
        if (buffer_.empty())
        {
            return;
        }

        const QString text = QString::fromUtf8(
            buffer_.data(),
            static_cast<int>(buffer_.size()));

        buffer_.clear();
        callback_(text);
    }

    Callback callback_;
    std::string buffer_;
};

#endif

// ============================================================
// URL input normalization
// ============================================================

// Accept normal YouTube URLs and Markdown links pasted from
// chat or documentation, for example [title](https://youtu.be/...).
static QString normalizeUrlInput(QString input)
{
    input = input.trimmed();

    static const QRegularExpression markdownLink(
        R"(^\[[^\]]+\]\((https?://[^)]+)\)$)",
        QRegularExpression::CaseInsensitiveOption);

    const auto match = markdownLink.match(input);

    if (match.hasMatch())
    {
        return match.captured(1).trimmed();
    }

    return input;
}

// ============================================================
// Browser detection
// ============================================================

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
        !qEnvironmentVariable("WSL_DISTRO_NAME").isEmpty() ||
        QFileInfo::exists("/proc/sys/fs/binfmt_misc/WSLInterop");

    if (isWSL)
    {
        roots << "/mnt/c/Program Files"
              << "/mnt/c/Program Files (x86)";

        QDir users("/mnt/c/Users");

        const QStringList profiles = users.entryList(
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
        relativePaths
            << "Mozilla Firefox/firefox.exe"
            << "Programs/Mozilla Firefox/firefox.exe";
    }
    else if (browser == "edge")
    {
        relativePaths
            << "Microsoft/Edge/Application/msedge.exe";
    }
    else if (browser == "chrome")
    {
        relativePaths
            << "Google/Chrome/Application/chrome.exe";
    }
    else if (browser == "brave")
    {
        relativePaths
            << "BraveSoftware/Brave-Browser/Application/brave.exe";
    }
    else if (browser == "chromium")
    {
        relativePaths
            << "Chromium/Application/chromium.exe"
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

static QString resolveBrowserExecutable(const QString &browserName)
{
    const QString browser = browserName.trimmed().toLower();

    if (browser.isEmpty())
    {
        return {};
    }

    for (const QString &executable : browserExecutableNames(browser))
    {
        const QString resolved =
            QStandardPaths::findExecutable(executable);

        if (!resolved.isEmpty())
        {
            return resolved;
        }
    }

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
        urlEdit->setPlaceholderText(
            "https://www.youtube.com/watch?v=...");

        selectorEdit = new QLineEdit(this);
        selectorEdit->setText("mp4:1080");
        selectorEdit->setPlaceholderText("18,22 or mp4:1080");

        browserCombo = new QComboBox(this);

        for (const auto &browser : findInstalledBrowsers())
        {
            browserCombo->addItem(browser.first, browser.second);
        }

        if (browserCombo->count() == 0)
        {
            browserCombo->addItem("System default", QString());
        }

        cookiesEdit = new QLineEdit(this);
        cookiesEdit->setPlaceholderText(
            "Optional cookies.txt or SQLite database");

        outputEdit = new QLineEdit(this);
        outputEdit->setText(QDir::currentPath());

        // Cookies field with its Browse button beside it.
        auto *cookiesRow = new QHBoxLayout;
        cookiesRow->setContentsMargins(0, 0, 0, 0);
        cookiesRow->setSpacing(6);

        auto *browseCookiesButton =
            new QPushButton("Browse...", this);

        cookiesRow->addWidget(cookiesEdit, 1);
        cookiesRow->addWidget(browseCookiesButton);

        // Output-directory field with its Browse button beside it.
        auto *outputRow = new QHBoxLayout;
        outputRow->setContentsMargins(0, 0, 0, 0);
        outputRow->setSpacing(6);

        auto *browseOutputButton =
            new QPushButton("Browse...", this);

        outputRow->addWidget(outputEdit, 1);
        outputRow->addWidget(browseOutputButton);

        form->addRow("YouTube URL:", urlEdit);
        form->addRow("Selector:", selectorEdit);
        form->addRow("Browser:", browserCombo);
        form->addRow("Cookies file / SQLite DB:", cookiesRow);
        form->addRow("Output folder:", outputRow);

        layout->addLayout(form);

        // ----------------------------------------------------
        // Action buttons
        // ----------------------------------------------------

        auto *rowButtons = new QHBoxLayout;

        auto *openLoginButton =
            new QPushButton("Open YouTube", this);

        auto *listButton =
            new QPushButton("List", this);

        startButton = new QPushButton("Download", this);
        startButton->setDefault(true);

        rowButtons->addWidget(openLoginButton);
        rowButtons->addStretch(1);
        rowButtons->addWidget(listButton);
        rowButtons->addWidget(startButton);

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
        // Browse for cookies file
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
                        "Cookie files (*.txt *.sqlite *.sqlite3 *.db);;"
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
                        outputEdit->text().trimmed().isEmpty()
                            ? QDir::homePath()
                            : outputEdit->text().trimmed());

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
                const QString browser =
                    browserCombo->currentData().toString();

                const QString executable =
                    resolveBrowserExecutable(browser);

                if (!executable.isEmpty())
                {
                    const bool launched =
                        QProcess::startDetached(
                            executable,
                            QStringList{
                                "https://www.youtube.com"});

                    if (launched)
                    {
                        appendLog(
                            "Opened YouTube in: " + executable +
                            "\nSign in there, then export a cookies.txt file.\n");
                        return;
                    }
                }

                const bool opened =
                    QDesktopServices::openUrl(
                        QUrl("https://www.youtube.com"));

                if (opened)
                {
                    appendLog(
                        "Opened YouTube in the system default browser. "
                        "Sign in there, then export a cookies.txt file.\n");
                }
                else
                {
                    appendLog("Could not open YouTube.\n");
                }
            });

        // ----------------------------------------------------
        // Shared command runner
        // ----------------------------------------------------

        const auto runBinaryCommand =
            [this, listButton](const QStringList &args)
        {
#ifdef YTDL_GUI_EMBEDDED_CLI
            // Primary mode: run the CLI code compiled into this GUI.
            if (worker != nullptr)
            {
                appendLog(
                    "A command is already running. Please wait.\n");
                return;
            }

            appendLog(
                "Running integrated downloader:\n  " +
                args.join("\n  ") + "\n\n");

            startButton->setEnabled(false);
            listButton->setEnabled(false);

            auto exitCode = std::make_shared<std::atomic<int>>(1);

            QThread *job = QThread::create(
                [this, args, exitCode]()
                {
                    const auto postLog =
                        [this](const QString &text)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, text]()
                            {
                                appendLog(text);
                            },
                            Qt::QueuedConnection);
                    };

                    GuiLogBuffer stdoutBuffer(postLog);
                    GuiLogBuffer stderrBuffer(postLog);

                    std::streambuf *previousOut =
                        std::cout.rdbuf(&stdoutBuffer);

                    std::streambuf *previousErr =
                        std::cerr.rdbuf(&stderrBuffer);

                    int result = 1;

                    try
                    {
                        std::vector<std::string> storage;
                        storage.reserve(
                            static_cast<std::size_t>(args.size()) + 1);

                        storage.emplace_back("yt-downloader");

                        for (const QString &arg : args)
                        {
                            storage.push_back(
                                arg.toUtf8().toStdString());
                        }

                        std::vector<char *> argv;
                        argv.reserve(storage.size() + 1);

                        for (std::string &arg : storage)
                        {
                            argv.push_back(arg.data());
                        }

                        argv.push_back(nullptr);

                        result = ytdl_cli_main(
                            static_cast<int>(storage.size()),
                            argv.data());
                    }
                    catch (const std::exception &error)
                    {
                        std::cerr
                            << "Downloader exception: "
                            << error.what() << '\n';
                        result = 1;
                    }
                    catch (...)
                    {
                        std::cerr
                            << "Downloader failed with an unknown exception.\n";
                        result = 1;
                    }

                    std::cout.flush();
                    std::cerr.flush();

                    // Restore global streams before the worker exits.
                    std::cout.rdbuf(previousOut);
                    std::cerr.rdbuf(previousErr);

                    exitCode->store(
                        result,
                        std::memory_order_release);
                });

            worker = job;

            connect(
                job,
                &QThread::finished,
                this,
                [this, job, exitCode, listButton]()
                {
                    const int result =
                        exitCode->load(std::memory_order_acquire);

                    appendLog(
                        QString(
                            "\nIntegrated command finished with exit code %1\n")
                            .arg(result));

                    if (worker == job)
                    {
                        worker = nullptr;
                    }

                    startButton->setEnabled(true);
                    listButton->setEnabled(true);

                    job->deleteLater();
                });

            job->start();

#else
            // Fallback: this GUI was built without embedded CLI support.
            if (process &&
                process->state() != QProcess::NotRunning)
            {
                appendLog(
                    "A command is already running. Please wait.\n");
                return;
            }

            const QString cwd = QDir::currentPath();
            const QString appDir =
                QCoreApplication::applicationDirPath();

            QStringList candidates;

#ifdef Q_OS_WIN
            candidates
                << QDir(appDir).filePath("yt-downloader.exe")
                << QDir(appDir).filePath("bin/yt-downloader.exe")
                << QDir(cwd).filePath(
                       "build/windows/bin/yt-downloader.exe")
                << QDir(cwd).filePath(
                       "bin/windows/yt-downloader.exe");
#else
            candidates
                << QDir(appDir).filePath("yt-downloader")
                << QDir(appDir).filePath("bin/yt-downloader")
                << QDir(cwd).filePath(
                       "build/linux/bin/yt-downloader")
                << QDir(cwd).filePath("bin/yt-downloader");
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
                    "Downloader unavailable",
                    "This GUI was built without its embedded CLI, "
                    "and the standalone yt-downloader executable "
                    "could not be found.");
                return;
            }

            QProcess *child = new QProcess(this);
            process = child;

#ifdef _WIN32
            child->setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments *processArgs)
                {
                    processArgs->flags |= CREATE_NO_WINDOW;
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
                    appendLog(
                        QString::fromUtf8(
                            child->readAllStandardOutput()));
                });

            connect(
                child,
                &QProcess::readyReadStandardError,
                this,
                [this, child]()
                {
                    appendLog(
                        QString::fromUtf8(
                            child->readAllStandardError()));
                });

            connect(
                child,
                QOverload<int, QProcess::ExitStatus>::of(
                    &QProcess::finished),
                this,
                [this, child, listButton](
                    int code,
                    QProcess::ExitStatus)
                {
                    appendLog(
                        QString("\nProcess finished with exit code %1\n")
                            .arg(code));

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
                [this, child, listButton](
                    QProcess::ProcessError error)
                {
                    if (error == QProcess::FailedToStart)
                    {
                        appendLog(
                            "Failed to start fallback CLI: " +
                            child->errorString() + "\n");

                        if (process == child)
                        {
                            process = nullptr;
                        }

                        startButton->setEnabled(true);
                        listButton->setEnabled(true);

                        child->deleteLater();
                    }
                });

            appendLog(
                "Running fallback CLI: " + binary + "\n");

            startButton->setEnabled(false);
            listButton->setEnabled(false);

            child->start();

#endif
        };

        // ----------------------------------------------------
        // List formats
        // ----------------------------------------------------

        connect(
            listButton,
            &QPushButton::clicked,
            this,
            [this, runBinaryCommand]()
            {
                const QString url =
                    normalizeUrlInput(urlEdit->text());

                const QString browser =
                    browserCombo->currentData().toString();

                const QString cookies =
                    cookiesEdit->text().trimmed();

                if (url.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing URL",
                        "Please enter a YouTube video or playlist URL.");
                    return;
                }

                if (url.contains("playlist", Qt::CaseInsensitive) &&
                    cookies.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Public playlist only",
                        "Without cookies, this tool can only list "
                        "the anonymous public feed. Some private "
                        "or restricted playlist entries will not appear.");
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
                    normalizeUrlInput(urlEdit->text());

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
                        "Please enter a YouTube video or playlist URL.");
                    return;
                }

                if (selector.isEmpty())
                {
                    QMessageBox::warning(
                        this,
                        "Missing selector",
                        "Please enter a selector such as mp4:1080.");
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
                        "Continuing without cookies may fail for private, "
                        "age-gated, or region-locked videos. Public content "
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
        resize(800, 560);
    }

    ~DownloaderWindow() override
    {
#ifdef YTDL_GUI_EMBEDDED_CLI
        // Do not destroy a running worker while it still references
        // this window for queued log updates.
        if (worker && worker->isRunning())
        {
            worker->wait();
        }
#endif
    }

private:
    void appendLog(const QString &text)
    {
        QTextCursor cursor = logEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);

        if (!text.endsWith('\n'))
        {
            cursor.insertText("\n");
        }

        logEdit->setTextCursor(cursor);
        logEdit->ensureCursorVisible();
    }

    QLineEdit *urlEdit = nullptr;
    QLineEdit *selectorEdit = nullptr;
    QComboBox *browserCombo = nullptr;
    QLineEdit *cookiesEdit = nullptr;
    QLineEdit *outputEdit = nullptr;

    QTextEdit *logEdit = nullptr;
    QPushButton *startButton = nullptr;

    QProcess *process = nullptr;

#ifdef YTDL_GUI_EMBEDDED_CLI
    QThread *worker = nullptr;
#endif
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
