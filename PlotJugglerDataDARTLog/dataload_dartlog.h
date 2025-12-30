#pragma once

#include <QObject>
#include <QtPlugin>
#include "PlotJuggler/dataloader_base.h"

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
    QByteArray inputData;
    QFile* inputFile;
    size_t inputFileSize;

    qint64 pos;

    void close();
    qint64 getPos();
    qint64 getSize();
    bool atEnd();
    bool atEnd(size_t len);
    void read(char* data, qint64 maxLen);
    void skip(qint64 bytes);
    uint8_t readUint8();
    uint16_t readUint16();

    std::string readString();

private:
    std::vector<const char *> _extensions;

    std::string _default_time_axis;
};


