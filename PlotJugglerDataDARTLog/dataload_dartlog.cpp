#include "dataload_dartlog.h"
#include <QFile>
#include <QMessageBox>
#include <QDateTime>
#include <QInputDialog>
#include <qprogressdialog.h>
#include <map>
#include <QFileInfo>

#include "qcompressor.h"

// Supported by plotjuggler nativly now
#define DISABLE_PREFIX_QUESTION 1
#define REDUCE_PLOT 0
#define ADD_EDGES_TO_PLOT 0

DataLoadDARTLog::DataLoadDARTLog() {
    _extensions.push_back("dat");
    _extensions.push_back("gz");
}

const std::vector<const char *> &DataLoadDARTLog::compatibleFileExtensions() const {
    return _extensions;
}

bool DataLoadDARTLog::readDataFromFile(FileLoadInfo *info, PlotDataMapRef &plot_data) {
    QFile file(info->filename);
    if (!file.open(QFile::ReadOnly))
        return false;

    // Load file info
    QFileInfo fileInfo(info->filename);

    // Show progress dialog
    QProgressDialog progress_dialog;
    progress_dialog.setWindowTitle("DARTLOG Plugin");
    progress_dialog.setLabelText("Loading... please wait");
    progress_dialog.setWindowModality(Qt::ApplicationModal);
    progress_dialog.setAutoClose(true);
    progress_dialog.setAutoReset(true);
    progress_dialog.show();
    progress_dialog.setValue(0);

    QApplication::processEvents();

    bool isGZip = info->filename.endsWith(".gz", Qt::CaseInsensitive);
    if (isGZip) {
        // Do not directly read file
        inputFile = nullptr;
        pos = 0;

        progress_dialog.setLabelText("Decompression... please wait");
        QByteArray data = file.readAll();
        if (data.size() == 0) {
            QMessageBox::warning(nullptr, "Error reading file", "Could not read file");
            return false;
        }
        file.close();

        if (!QCompressor::gzipDecompress(data, inputData, &progress_dialog)) {
            QMessageBox::warning(nullptr, "Warning reading file", "Could not fully decompress file: data may be incomplete or fully missing");
        }
    }
    else {
        // Directly read file
        inputFile = &file;
    }

    progress_dialog.setLabelText("Loading data... please wait");
    progress_dialog.setValue(0);
    progress_dialog.setRange(0, getSize());
    QApplication::processEvents();

    std::map<uint16_t, uint16_t> tags;
    std::map<uint16_t, PlotData *> plots;
    std::map<uint16_t, double> lastTime;
    std::map<uint16_t, double> lastValue;
    std::map<uint16_t, bool> isXY;
    std::map<uint16_t, bool> isVerbose;
    std::vector<uint16_t> tagIndices;
    std::vector<std::string> tagNames;
    uint16_t maxTagID = 0;
    uint16_t timeTagID = 0;
    float time = 0;

    // Read header
    std::string header = readString();
    if (header != "DARTLOG" && header != "DARTLOG2") {
        QMessageBox::warning(nullptr, "Error reading file", "Not a DARTLOG file: header missing.");
        return false;
    }

    PlotData::Point dartLogVersion(0, 1);
    if (header == "DARTLOG2")
        dartLogVersion = PlotData::Point(0, 2);

    bool isAtLeastDARTLOG2 = dartLogVersion.y >= 2;

#if DISABLE_PREFIX_QUESTION
    bool usePrefix = false;
#else
    bool usePrefix = QMessageBox::question(nullptr, "Load with prefix?", "Do you want to load the data with a prefix? If yes, you can load multiple data sets in the same PlotJuggler instance.", QMessageBox::Yes | QMessageBox::No) == QMessageBox::StandardButton::Yes;
#endif
    bool loadVerboseData = false;
    uint64_t counter = 0;
    uint16_t lastID = 0;

    while (!atEnd()) {
        // Update file progress dialog
        if (counter % (1024 * 32) == 0) {
            progress_dialog.setValue(getPos());
            if (progress_dialog.wasCanceled())
                break;

            QApplication::processEvents();
        }
        counter++;

        // Read next tag
        uint16_t id;
        if (isAtLeastDARTLOG2) {
            uint8_t idPart = readUint8();
            if (idPart == 255)
                id = readUint16();
            else if (idPart == 254)
                id = lastID + 1;
            else
                id = idPart;
        }
        else
            id = readUint16();

        lastID = id;

        if (id == 0) {
            uint16_t tagIndex = readUint16();

            uint8_t tagType;
            read((char *) &tagType, sizeof(tagType));

            if (tagType < 1 || tagType > 10) {
                QMessageBox::warning(nullptr, "Error reading file", "Wrong tag type read");
                break;
            }

            tags[tagIndex] = tagType;
            if (tagIndex > maxTagID)
                maxTagID = tagIndex;

            std::string name = readString();

            if (name.length() == 0) {
                QMessageBox::warning(nullptr, "Error reading file", "Empty tag name read");
                break;
            }


            std::string unit = "";
            bool verbose = false;
            if (isAtLeastDARTLOG2)
            {
                while (true)
                {
                    uint8_t attributeType;
                    read((char*)&attributeType, sizeof(attributeType));

                    if (attributeType == 0)
                        break;

                    uint8_t attributeLength;
                    read((char*)&attributeLength, sizeof(attributeLength));

                    switch (attributeType)
                    {
                        case 1: {   // unit
                            unit = readString();
                            std::replace(unit.begin(), unit.end(), '/', '_');
                            break;
                        }
                        case 2: { // verbose signal
                            verbose = readUint8() > 0;
                            break;
                        }
                          
                        default:
                            skip(attributeLength);
                            break;
                    }
                }
            }

            std::replace(name.begin(), name.end(), '_', '/');

            if (name == "time")
                timeTagID = tagIndex;

            if (usePrefix) 
                name = fileInfo.baseName().toStdString() + "/" + name;

            // Check if the name is the start of a different value
            for (size_t i = 0; i < tagNames.size(); i++) {
                if (tagNames[i]._Starts_with(name)) {
                    name += "/Value";
                    break;
                }
            }

            // Add unit
            if (unit.length() > 0)
                name += "_" + unit;

            tagIndices.push_back(tagIndex);
            tagNames.push_back(name);

            if (verbose && !loadVerboseData) {
                plots[tagIndex] = nullptr;
            }
            else {
                auto it = plot_data.addNumeric(name);
                plots[tagIndex] = &it->second;
            }

            isVerbose[tagIndex] = verbose;

            lastValue[tagIndex] = DBL_MAX;
            lastTime[tagIndex] = -1;
        } else {
            if (id > maxTagID) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read: over max tag id");
                break;
            }
            if (tags.find(id) == tags.end()) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read: unknown tag id");
                break;
            }

            // Read value
            uint8_t type = tags[id];

            double value = 0;
            switch (type) {
                case 1: {
                    uint8_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 2: {
                    uint16_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 3: {
                    uint32_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 4: {
                    int8_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 5: {
                    int16_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 6: {
                    int32_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 7: {
                    float v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 8: {
                    double v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 9: {
                    uint64_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 10: {
                    int64_t v;
                    read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
            }

            if (id == timeTagID)
                time = value;

            // Skip verbose values
            PJ::PlotData* data = plots[id];
            if (data == nullptr)
                continue;

#if REDUCE_PLOT
            double lastVal = lastValue[id];
            double lastT = lastTime[id];

            bool valueChanged = std::abs(lastVal - value) >= 0.00001;
            bool timeChanged = std::abs(time - lastT) >= 0.1;

            if (valueChanged || timeChanged || isXY[id]) {
#if ADD_EDGES_TO_PLOT
                // Add point just before last value to ensure edges are in plot
                if (lastT >= 0 && valueChanged && timeChanged) {
                    PlotData::Point point(time - 0.001, lastVal);
                    plots[id]->pushBack(point);
                }
#endif

                PlotData::Point point(time, value);
                plots[id]->pushBack(point);

                lastTime[id] = time;
                lastValue[id] = value;
            }
#else
            PlotData::Point point(time, value);
            data->pushBack(point);
#endif
        }
    }


    // Add for all tags last value at the current time
    for (size_t i = 0; i < tagIndices.size(); i++) {
        uint16_t tagIndex = tagIndices[i];
        if (plots[tagIndex] != nullptr && lastValue[tagIndex] != DBL_MAX) {
            PlotData::Point point(time, lastValue[tagIndex]);
            plots[tagIndex]->pushBack(point);
        }
    }

    // Add logger informations
    PlotData::Point version(0, 12);
    plot_data.addNumeric("dartlog_version_data")->second.pushBack(dartLogVersion);
    plot_data.addNumeric("dartlog_version_plugin")->second.pushBack(version);

    PlotData::Point gzipPoint(0, isGZip ? 1 : 0);
    plot_data.addNumeric("dartlog_is_gzip")->second.pushBack(gzipPoint);

    if (!loadVerboseData) {
        PlotData::Point gzipPoint(0, isGZip ? 1 : 0);
        plot_data.addNumeric("VERBOSE_DATA_NOT_LOADED")->second.pushBack(gzipPoint);
    }

    // QMessageBox::information(nullptr, "File successfully read",  QString("Found %1 signals").arg(maxTagID));

    close();
    progress_dialog.close();
    return true;
}

void DataLoadDARTLog::close() {
    if (inputFile != nullptr)
        inputFile->close();
}

qint64 DataLoadDARTLog::getPos() {
    if (inputFile != nullptr)
        return inputFile->pos();
    return pos;
}

qint64 DataLoadDARTLog::getSize() {
    if (inputFile != nullptr)
        return inputFile->size();
    return inputData.size();
}

bool DataLoadDARTLog::atEnd() {
    if (inputFile != nullptr)
        return inputFile->atEnd();
    return pos >= inputData.size();
}

qint64 DataLoadDARTLog::read(char* data, qint64 maxLen) {
    if (inputFile != nullptr)
        return inputFile->read(data, maxLen);

    for (size_t i = 0; i < maxLen; i++) {
        if (atEnd())
            break;
        data[i] = inputData.at(pos++);
    }
    return maxLen;
}

void DataLoadDARTLog::skip(qint64 bytes) {
    if (inputFile != nullptr)
        inputFile->skip(bytes);
    else
        pos += bytes;
}

uint8_t DataLoadDARTLog::readUint8() {
    uint8_t b;
    read((char*)&b, sizeof(b));
    return b;
}

uint16_t DataLoadDARTLog::readUint16() {
    uint8_t b[2];
    read((char *) b, sizeof(b));

    return b[0] + b[1] * 256;
}

std::string DataLoadDARTLog::readString() {
    std::string str = "";
    while (!atEnd()) {
        char c;
        read(&c, sizeof(c));
        if (c == 0)
            return str;
        str += c;
    }
    return str;
}