#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <stdexcept>

namespace office {
inline bool isOfficePath(const QString& path) {
    for (const char* suffix : {".docx", ".doc", ".odt", ".rtf", ".txt", ".xlsx", ".xls", ".ods", ".pptx", ".ppt", ".odp"})
        if (path.endsWith(suffix, Qt::CaseInsensitive)) return true;
    return false;
}
// LibreOffice's command-line entry point: FOLIOFORGE_SOFFICE, then PATH, then the usual install locations.
inline QString findLibreOffice() {
    const auto configured = qEnvironmentVariable("FOLIOFORGE_SOFFICE");
    if (!configured.isEmpty()) return QFileInfo(configured).isExecutable() ? configured : QString();
    for (const char* name : {"soffice", "libreoffice"}) {
        auto found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) return found;
    }
    for (const char* path : {"/Applications/LibreOffice.app/Contents/MacOS/soffice", "/usr/lib/libreoffice/program/soffice", "/opt/libreoffice/program/soffice",
                             "C:/Program Files/LibreOffice/program/soffice.exe", "C:/Program Files (x86)/LibreOffice/program/soffice.exe"})
        if (QFileInfo(path).isExecutable()) return path;
    return {};
}
// Converts one document to PDF inside `workDir` (which the caller owns) and returns the PDF path.
inline QString convertToPdf(const QString& input, const QString& workDir) {
    const auto soffice = findLibreOffice();
    const auto name = QFileInfo(input).fileName();
    if (soffice.isEmpty())
        throw std::runtime_error("Converting " + name.toStdString() + " needs LibreOffice, which was not found. Install it from libreoffice.org (or set FOLIOFORGE_SOFFICE to the soffice program) and try again.");
    QDir().mkpath(workDir + "/out");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    // A private profile keeps the conversion independent of any LibreOffice window the user already has open.
    process.start(soffice, {"-env:UserInstallation=" + QUrl::fromLocalFile(workDir + "/profile").toString(), "--headless", "--norestore", "--convert-to", "pdf",
                            "--outdir", workDir + "/out", input});
    if (!process.waitForStarted(15000)) throw std::runtime_error("LibreOffice could not be started.");
    if (!process.waitForFinished(180000)) { process.kill(); process.waitForFinished(3000); throw std::runtime_error("Converting " + name.toStdString() + " timed out after 3 minutes."); }
    const auto output = workDir + "/out/" + QFileInfo(input).completeBaseName() + ".pdf";
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !QFileInfo::exists(output))
        throw std::runtime_error("LibreOffice could not convert " + name.toStdString() + ".");
    return output;
}
}
