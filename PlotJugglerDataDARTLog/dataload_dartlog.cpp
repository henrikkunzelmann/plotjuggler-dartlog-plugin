#include "dataload_dartlog.h"
#include <QFile>
#include <QMessageBox>
#include <QDateTime>
#include <QInputDialog>
#include <qprogressdialog.h>
#include <vector>
#include <QFileInfo>

#include "qcompressor.h"
#include <chrono>

#define DISABLE_PREFIX_QUESTION 1
#define REDUCE_PLOT 0
#define ADD_EDGES_TO_PLOT 

class PlotDataAccessor : public PlotData {
public:
    std::deque<Point>* directAccessPoints() {
        return &_points;
    }
};

// Struct to hold all tag-related data
struct TagData {
    uint16_t type;
    PlotDataAccessor* plot;
    std::deque<PlotData::Point>* plotData;
    double lastTime;
    double lastValue;
    bool isXY;
    bool isVerbose;
    std::string tagName;
};

DataLoadDARTLog::DataLoadDARTLog() {
    _extensions.push_back("dat");
    _extensions.push_back("gz");
}

const std::vector<const char*>& DataLoadDARTLog::compatibleFileExtensions() const {
    return _extensions;
}

TagData tagData[UINT16_MAX];


bool DataLoadDARTLog::readDataFromFile(FileLoadInfo* info, PlotDataMapRef& plot_data) {
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

    double decompress_duration_ms = 0;
    double reading_duration_ms = 0;

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

        auto decompress_start = std::chrono::high_resolution_clock::now();
        if (!QCompressor::gzipDecompress(data, inputData, &progress_dialog)) {
            QMessageBox::warning(nullptr, "Warning reading file", "Could not fully decompress file: data may be incomplete or fully missing");
        }
        auto decompress_end = std::chrono::high_resolution_clock::now();
        decompress_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(decompress_end - decompress_start).count();
    }
    else {
        // Directly read file
        inputFile = &file;
		inputFileSize = file.size();
    }

    progress_dialog.setLabelText("Loading data... please wait");
    progress_dialog.setValue(0);
    progress_dialog.setRange(0, 100);
    QApplication::processEvents();

    auto reading_start = std::chrono::high_resolution_clock::now();

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
    const uint64_t progressDialogTickMask = (1024 * 1024);

    uint32_t verboseSignalsIgnoredCount = 0;

    PlotData::Point point(0, 0);
    while (true) {
        // Update file progress dialog
        if ((counter & (progressDialogTickMask - 1)) == 0) {
            progress_dialog.setValue((int)std::round((double)getPos() / inputFileSize * 100));
            if (progress_dialog.wasCanceled())
                break;

            QApplication::processEvents();
        }
        counter++;

        // Check if at end
		if (atEnd(64)) // some margin
            break;

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
            read((char*)&tagType, sizeof(tagType));

            if (tagType < 1 || tagType > 10) {
                QMessageBox::warning(nullptr, "Error reading file", "Wrong tag type read");
                break;
            }

            tagData[tagIndex].type = tagType;

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
            for (size_t i = 0; i < maxTagID; i++) {
                if (!tagData[i].tagName.empty() && tagData[i].tagName._Starts_with(name)) {
                    name += "/Value";
                    break;
                }
            }

            // Add unit
            if (unit.length() > 0)
                name += "_" + unit;

            tagData[tagIndex].tagName = name;

            if (verbose && !loadVerboseData) {
                tagData[tagIndex].plot = nullptr;
                verboseSignalsIgnoredCount++;
            }
            else {
                auto it = plot_data.addNumeric(name);
                auto plot = (PlotDataAccessor*)&it->second;

                tagData[tagIndex].plot = plot;
				tagData[tagIndex].plotData = plot->directAccessPoints();
            }

            tagData[tagIndex].isVerbose = verbose;
        }
        else {
            if (id > maxTagID) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read: over max tag id");
                break;
            }

			const auto& tag = tagData[id];
            if (tag.type == 0) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read: unknown tag id");
                break;
            }

            // Read value
            uint8_t type = tag.type;

            double value = 0;
            switch (type) {
            case 1: {
                uint8_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 2: {
                uint16_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 3: {
                uint32_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 4: {
                int8_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 5: {
                int16_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 6: {
                int32_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 7: {
                float v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 8: {
                double v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 9: {
                uint64_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            case 10: {
                int64_t v;
                read((char*)&v, sizeof(v));
                value = (double)v;
                break;
            }
            }

            if (id == timeTagID)
            {
                point.x = time;
                time = value;
            }

            // Skip verbose values
            PlotDataAccessor* data = tag.plot;
            if (data == nullptr)
                continue;

#if REDUCE_PLOT
            double lastVal = tag.lastValue;
            double lastT = tag.lastTime;

            bool valueChanged = std::abs(lastVal - value) >= 0.00001;
            bool timeChanged = std::abs(time - lastT) >= 0.1;

            if (valueChanged || timeChanged || tag.isXY) {
#if ADD_EDGES_TO_PLOT
                // Add point just before last value to ensure edges are in plot
                if (lastT >= 0 && valueChanged && timeChanged) {
                    PlotData::Point point(time - 0.001, lastVal);
                    tag.plot->pushBack(point);
                }
#endif

                point.y = value;
                data->directAccessPoints().push_back(point);

                tag.lastTime = time;
                tag.lastValue = value;
            }
#else
            point.y = value;
            tag.plotData->push_back(point);
#endif
        }
    }

    auto reading_end = std::chrono::high_resolution_clock::now();
    reading_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(reading_end - reading_start).count();

    // Add for all tags last value at the current time (also trigger range update)
    for (size_t i = 0; i < maxTagID; i++) {
		const auto& tag = tagData[i];
        if (tag.plot != nullptr && tag.lastValue != DBL_MAX) {
            PlotData::Point point(time, tag.lastValue);
            tag.plot->pushBack(point);
        }
    }

    // Add logger informations
    PlotData::Point version(0, 12);
    plot_data.addNumeric("dartlog_version_data")->second.pushBack(dartLogVersion);
    plot_data.addNumeric("dartlog_version_plugin")->second.pushBack(version);

    PlotData::Point gzipPoint(0, isGZip ? 1 : 0);
    plot_data.addNumeric("dartlog_is_gzip")->second.pushBack(gzipPoint);

    if (!loadVerboseData) {
        PlotData::Point verbosePoint(0, verboseSignalsIgnoredCount);
        plot_data.addNumeric("VERBOSE_DATA_NOT_LOADED")->second.pushBack(verbosePoint);
        PlotData::Point verboseCountPoint(0, verboseSignalsIgnoredCount);
        plot_data.addNumeric("verbose_signal_count")->second.pushBack(verboseCountPoint);
    }

    if (isGZip) {
        PlotData::Point decompressPoint(0, decompress_duration_ms);
        plot_data.addNumeric("dartlog_decompression_time_ms")->second.pushBack(decompressPoint);
    }

    PlotData::Point readingPoint(0, reading_duration_ms);
    plot_data.addNumeric("dartlog_reading_time_ms")->second.pushBack(readingPoint);

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
        return inputFileSize;
    return inputData.size();
}

bool DataLoadDARTLog::atEnd() {
    return atEnd(0);
}

bool DataLoadDARTLog::atEnd(size_t len) {
    return getPos() + len >= getSize();
}

void DataLoadDARTLog::read(char* data, qint64 maxLen) {
    if (inputFile != nullptr)
        inputFile->read(data, maxLen);
    else {
        memcpy(data, inputData.data() + pos, maxLen);
        pos += maxLen;
    }
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
    read((char*)b, sizeof(b));

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