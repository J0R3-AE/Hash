#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static QStringList findInstalledBrowsers()
{
    const QStringList candidates = {
        "firefox",
        "microsoft-edge",
        "microsoft-edge-stable",
        "msedge",
        "edge",
        "google-chrome",
        "chromium",
        "chromium-browser",
        "brave",
        "brave-browser",
        "safari",
        "chrome"
    };

    QStringList browsers;
    for (const QString& candidate : candidates) {
        const QString resolved = QStandardPaths::findExecutable(candidate);
        if (!resolved.isEmpty() && !browsers.contains(candidate))
            browsers << candidate;
    }

    if (browsers.isEmpty())
        browsers << "firefox";

    return browsers;
}

static QString resolveBrowserExecutable(const QString& browserName)
{
    const QString normalized = browserName.trimmed().toLower();
    const QStringList candidates = {
        normalized,
        normalized + "-stable",
        normalized == "firefox" ? "firefox-esr" : normalized,
        normalized == "edge" ? "microsoft-edge" : normalized,
        normalized == "edge" ? "microsoft-edge-stable" : normalized,
        normalized == "edge" ? "msedge" : normalized,
        normalized == "chrome" ? "google-chrome" : normalized,
        normalized == "chrome" ? "chromium" : normalized,
        normalized == "chrome" ? "chromium-browser" : normalized,
        normalized == "firefox" ? "firefox-bin" : normalized,
        normalized == "brave" ? "brave-browser" : normalized
    };

    for (const QString& candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        const QString resolved = QStandardPaths::findExecutable(candidate);
        if (!resolved.isEmpty())
            return resolved;
    }

    return {};
}

class DownloaderWindow : public QMainWindow
{
public:
    DownloaderWindow()
    {
        auto* central = new QWidget(this);
        setCentralWidget(central);

        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        urlEdit = new QLineEdit(this);
        selectorEdit = new QLineEdit(this);
        selectorEdit->setText("mp4:1080");
        browserCombo = new QComboBox(this);
        browserCombo->addItems(findInstalledBrowsers());
        cookiesEdit = new QLineEdit(this);
        outputEdit = new QLineEdit(this);
        outputEdit->setText(QString::fromStdString(fs::current_path().string()));

        form->addRow("YouTube URL:", urlEdit);
        form->addRow("Selector:", selectorEdit);
        form->addRow("Browser:", browserCombo);
        form->addRow("Cookies file / SQLite DB:", cookiesEdit);
        form->addRow("Output folder:", outputEdit);

        auto* rowButtons = new QHBoxLayout;
        auto* browseCookiesButton = new QPushButton("Browse", this);
        auto* browseOutputButton = new QPushButton("Browse", this);
        auto* openLoginButton = new QPushButton("Open YouTube", this);
        auto* listButton = new QPushButton("List", this);
        startButton = new QPushButton("Download", this);
        startButton->setDefault(true);

        rowButtons->addWidget(browseCookiesButton);
        rowButtons->addWidget(browseOutputButton);
        rowButtons->addWidget(openLoginButton);
        rowButtons->addWidget(listButton);
        rowButtons->addWidget(startButton);

        layout->addLayout(form);
        layout->addLayout(rowButtons);

        logEdit = new QTextEdit(this);
        logEdit->setReadOnly(true);
        logEdit->setPlaceholderText("Download output will appear here.");
        logEdit->setMinimumHeight(180);
        layout->addWidget(logEdit, 1);

        connect(browseCookiesButton, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this, "Choose cookies.txt or sqlite database",
                QDir::homePath(), "Cookies file (*.txt *.sqlite *.sqlite3 *.db);;All files (*)");
            if (!path.isEmpty())
                cookiesEdit->setText(path);
        });

        connect(browseOutputButton, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getExistingDirectory(this, "Choose output folder", QDir::homePath());
            if (!path.isEmpty())
                outputEdit->setText(path);
        });

        connect(openLoginButton, &QPushButton::clicked, this, [this]() {
            const QString selectedBrowser = browserCombo->currentText().trimmed();
            const QString browserExecutable = resolveBrowserExecutable(selectedBrowser);
            if (!browserExecutable.isEmpty()) {
                const bool launched = QProcess::startDetached(browserExecutable, {"https://www.youtube.com"});
                if (launched) {
                    appendLog("Opened YouTube in: " + browserExecutable + "\nSign in there, then export a cookies.txt file.");
                    return;
                }
            }

            QDesktopServices::openUrl(QUrl("https://www.youtube.com"));
            appendLog("Opened YouTube in the system default browser. Sign in there, then export a cookies.txt file.");
        });

        const auto runBinaryCommand = [this](const QStringList& args) {
            QString binary = QDir::currentPath() + "/bin/yt-downloader";
            if (!QFileInfo(binary).exists())
                binary = QCoreApplication::applicationDirPath() + "/bin/yt-downloader";

            if (!QFileInfo(binary).exists()) {
                QMessageBox::critical(this, "Binary not found", "Could not find the yt-downloader binary. Build it with make first.");
                return;
            }

            process = new QProcess(this);
            process->setProgram(binary);
            process->setArguments(args);

            connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
                appendLog(QString::fromLocal8Bit(process->readAllStandardOutput()));
            });
            connect(process, &QProcess::readyReadStandardError, this, [this]() {
                appendLog(QString::fromLocal8Bit(process->readAllStandardError()));
            });
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this](int exitCode, QProcess::ExitStatus) {
                        appendLog(QString("\nProcess finished with exit code %1\n").arg(exitCode));
                        startButton->setEnabled(true);
                    });

            appendLog("Running: " + binary + " " + args.join(' '));
            startButton->setEnabled(false);
            process->start();
        };

        connect(listButton, &QPushButton::clicked, this, [this, runBinaryCommand]() {
            const QString url = urlEdit->text().trimmed();
            const QString browser = browserCombo->currentText();
            const QString cookies = cookiesEdit->text().trimmed();

            if (url.isEmpty()) {
                QMessageBox::warning(this, "Missing URL", "Please enter a YouTube video or playlist URL.");
                return;
            }
            if (url.contains("playlist", Qt::CaseInsensitive) && cookies.isEmpty()) {
                QMessageBox::warning(this, "Public playlist only",
                    "Without cookies, this tool can only list the anonymous public feed. Some private or restricted playlist entries will not appear.");
            }

            QStringList args;
            args << "list" << url << "--browser" << browser;
            if (!cookies.isEmpty())
                args << "--cookies" << cookies;
            runBinaryCommand(args);
        });

        connect(startButton, &QPushButton::clicked, this, [this, runBinaryCommand]() {
            const QString url = urlEdit->text().trimmed();
            const QString selector = selectorEdit->text().trimmed();
            const QString browser = browserCombo->currentText();
            const QString cookies = cookiesEdit->text().trimmed();
            const QString output = outputEdit->text().trimmed();

            if (url.isEmpty()) {
                QMessageBox::warning(this, "Missing URL", "Please enter a YouTube video or playlist URL.");
                return;
            }
            if (selector.isEmpty()) {
                QMessageBox::warning(this, "Missing selector", "Please enter a selector such as mp4:1080.");
                return;
            }
            if (cookies.isEmpty()) {
                QMessageBox::warning(this, "No cookies supplied",
                    "Continuing without cookies may fail for private, age-gated, or region-locked videos. "
                    "Public content may still work.");
            }

            QStringList args;
            args << "download" << url << selector << "--browser" << browser;
            if (!cookies.isEmpty())
                args << "--cookies" << cookies;
            args << "-o" << output;
            runBinaryCommand(args);
        });

        setWindowTitle("Hash YouTube Downloader");
        resize(760, 540);
    }

private:
    void appendLog(const QString& text)
    {
        logEdit->append(text);
        logEdit->moveCursor(QTextCursor::End);
    }

    QLineEdit* urlEdit = nullptr;
    QLineEdit* selectorEdit = nullptr;
    QComboBox* browserCombo = nullptr;
    QLineEdit* cookiesEdit = nullptr;
    QLineEdit* outputEdit = nullptr;
    QTextEdit* logEdit = nullptr;
    QPushButton* startButton = nullptr;
    QProcess* process = nullptr;
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    DownloaderWindow window;
    window.show();
    return app.exec();
}
