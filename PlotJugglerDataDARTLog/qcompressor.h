#ifndef QCOMPRESSOR_H
#define QCOMPRESSOR_H

#include <zlib.h>
#include <QByteArray>
#include <qprogressdialog.h>
#include <QApplication>

#define GZIP_WINDOWS_BIT 15 + 16
#define GZIP_CHUNK_SIZE 32 * 1024

class QCompressor
{
public:
    static bool gzipCompress(QByteArray input, QByteArray& output, int level = -1);
    static bool gzipDecompress(QByteArray input, QByteArray& output, QProgressDialog* dialog);
};

#endif // QCOMPRESSOR_H
