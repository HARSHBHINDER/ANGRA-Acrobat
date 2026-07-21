#pragma once
// qpdf-backed protection/optimization. Compiled only when qpdf is available
// (ANGRA_HAVE_QPDF). Pure functions: path in, validated bytes out.

#include <QByteArray>
#include <QString>

namespace pdfprotect {

// AES-256 encryption. ownerPw falls back to userPw when empty.
// ponytail: fixed permission set (print allowed, modify denied); expose
// checkboxes when someone needs finer grain.
bool encrypt(const QString& srcPath, const QString& password, const QString& userPw,
             const QString& ownerPw, QByteArray* out, QString* err);
bool decrypt(const QString& srcPath, const QString& password, QByteArray* out, QString* err);
// Full rewrite dropping /Info and XMP metadata plus all prior revisions.
bool sanitize(const QString& srcPath, const QString& password, QByteArray* out, QString* err);
// Object streams + stream recompression + linearization.
bool optimize(const QString& srcPath, const QString& password, QByteArray* out, QString* err);
// Lenient parse + clean rewrite; warnings reports recovered issues.
bool repair(const QString& srcPath, const QString& password, QByteArray* out, QString* err,
            int* warnings);

} // namespace pdfprotect
