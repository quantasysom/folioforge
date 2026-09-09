#include "window.h"
#include <QApplication>
#include <QDir>
#include <QTimer>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QDebug>
#include <QFontDatabase>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("FolioForge"); QCoreApplication::setApplicationName("FolioForge");
    bool smoke = app.arguments().contains("--smoke-test");
    if (smoke) {
#ifdef _WIN32
        // The offscreen QPA plugin does not discover Windows system fonts.
        int font = QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + "/Fonts/segoeui.ttf");
        if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
#endif
    }
    Window window(smoke); window.show();
    if (smoke) {
        auto path = QDir::current().absoluteFilePath("smoke-input.pdf");
        if (!QFileInfo::exists(path)) { qCritical() << "Run engine-tests to generate the smoke-test fixture first."; return 1; }
        window.openPath(path);
        auto timer = new QTimer(&window); auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
        QObject::connect(timer, &QTimer::timeout, &window, [&app, &window, elapsed] {
            if (window.hadError() || elapsed->elapsed() > 30000) { qCritical() << "Desktop smoke failed"; app.exit(1); }
            else if (window.smokeReady()) {
                auto state = window.advanceSmokeTest();
                if (state < 0) { qCritical() << "Desktop workflow assertion failed"; app.exit(1); }
                else if (state > 0) { bool saved = window.grab().save("desktop-smoke.png"); app.exit(saved ? 0 : 1); }
            }
        }); timer->start(100);
    } else {
        for (const auto& arg : app.arguments().mid(1)) if (QFileInfo::exists(arg)) window.openPath(arg);
    }
    return app.exec();
}
