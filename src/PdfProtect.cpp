#include "PdfProtect.h"

#include <functional>

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFWriter.hh>

namespace pdfprotect {
namespace {

bool run(const QString& srcPath, const QString& password, QByteArray* out, QString* err,
         int* warnings, const std::function<void(QPDF&, QPDFWriter&)>& configure) {
    try {
        QPDF pdf;
        pdf.processFile(srcPath.toLocal8Bit().constData(),
                        password.isEmpty() ? nullptr : password.toUtf8().constData());
        QPDFWriter writer(pdf);
        writer.setOutputMemory();
        configure(pdf, writer);
        writer.write();
        if (warnings)
            *warnings = static_cast<int>(pdf.getWarnings().size());
        const std::shared_ptr<Buffer> buf = writer.getBufferSharedPointer();
        *out = QByteArray(reinterpret_cast<const char*>(buf->getBuffer()),
                          static_cast<qsizetype>(buf->getSize()));
        return true;
    } catch (const std::exception& e) {
        if (err)
            *err = QString::fromUtf8(e.what());
        return false;
    }
}

} // namespace

bool encrypt(const QString& srcPath, const QString& password, const QString& userPw,
             const QString& ownerPw, QByteArray* out, QString* err) {
    const QByteArray user = userPw.toUtf8();
    const QByteArray owner = (ownerPw.isEmpty() ? userPw : ownerPw).toUtf8();
    return run(srcPath, password, out, err, nullptr, [&](QPDF&, QPDFWriter& w) {
        w.setR6EncryptionParameters(user.constData(), owner.constData(),
                                    /*accessibility*/ true, /*extract*/ false,
                                    /*assemble*/ false, /*annotate_and_form*/ true,
                                    /*form_filling*/ true, /*modify_other*/ false,
                                    qpdf_r3p_full, /*encrypt_metadata*/ true);
    });
}

bool decrypt(const QString& srcPath, const QString& password, QByteArray* out, QString* err) {
    return run(srcPath, password, out, err, nullptr,
               [](QPDF&, QPDFWriter& w) { w.setPreserveEncryption(false); });
}

bool sanitize(const QString& srcPath, const QString& password, QByteArray* out, QString* err) {
    return run(srcPath, password, out, err, nullptr, [](QPDF& pdf, QPDFWriter& w) {
        w.setPreserveEncryption(false);
        pdf.getTrailer().removeKey("/Info");
        pdf.getRoot().removeKey("/Metadata");
    });
}

bool optimize(const QString& srcPath, const QString& password, QByteArray* out, QString* err) {
    return run(srcPath, password, out, err, nullptr, [](QPDF&, QPDFWriter& w) {
        w.setObjectStreamMode(qpdf_o_generate);
        w.setCompressStreams(true);
        w.setRecompressFlate(true);
        w.setLinearization(true);
    });
}

bool repair(const QString& srcPath, const QString& password, QByteArray* out, QString* err,
            int* warnings) {
    return run(srcPath, password, out, err, warnings, [](QPDF&, QPDFWriter&) {});
}

} // namespace pdfprotect
