#pragma once

#include <QObject>
#include <QtPlugin>
#include "PlotJuggler/dataloader_base.h"
#include <zlib.h>

using namespace PJ;

class DataLoadDARTLog : public DataLoader {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataLoader")
    Q_INTERFACES(PJ::DataLoader)

public:
    DataLoadDARTLog();

    virtual const std::vector<const char *> &compatibleFileExtensions() const override;

    bool readDataFromFile(PJ::FileLoadInfo *fileload_info,
                          PlotDataMapRef &destination) override;

    ~DataLoadDARTLog() override = default;

    virtual const char *name() const override {
        return "DARTLog Reader";
    }


protected:
    static const qint64 CHUNK_SIZE = 1024 * 1024;

    QFile* filePtr;
    qint64 inputFileSize;
    qint64 pos;

    z_stream strm;
    bool isGZip;
    bool finished;
    char buffer[CHUNK_SIZE * 2];
    qint64 bufferSize;
    qint64 bufferOffset;
    qint64 posBuffer;
    char compressedBuffer[CHUNK_SIZE];

    double reading_duration_ms = 0;
	double io_duration_ms = 0;
	double decompress_duration_ms = 0;

    void close();
    qint64 getPos();
    qint64 getSize();
    bool atEnd();
    bool atEnd(qint64 len);
    void read(char* data, qint64 maxLen);
    void skip(qint64 bytes);
    uint8_t readUint8();
    uint16_t readUint16();

    std::string readString();

    bool loadMoreData();
    qint64 loadNewChunk(char* chunkBuffer, qint64 maxSize);

private:
    std::vector<const char *> _extensions;

    std::string _default_time_axis;
};


