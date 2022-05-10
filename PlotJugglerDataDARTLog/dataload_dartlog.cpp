#include "dataload_dartlog.h"
#include <QFile>
#include <QMessageBox>
#include <QDateTime>
#include <QInputDialog>
#include <map>

DataLoadDARTLog::DataLoadDARTLog() {
    _extensions.push_back("dat");
}

const std::vector<const char *> &DataLoadDARTLog::compatibleFileExtensions() const {
    return _extensions;
}

bool DataLoadDARTLog::readDataFromFile(FileLoadInfo *info, PlotDataMapRef &plot_data) {
    QFile file(info->filename);
    if (!file.open(QFile::ReadOnly))
        return false;

    std::map<uint16_t, uint16_t> tags;
    std::map<uint16_t, PlotData *> plots;
    uint16_t maxTagID = 0;
    uint16_t timeTagID = 0;
    float time = 0;

    // Read header
    std::string header = readString(&file);
    if (header != "DARTLOG") {
        QMessageBox::warning(nullptr, "Error reading file", "Not a DARTLOG file: header missing.");
        return false;
    }

    while (!file.atEnd()) {
        uint16_t id = readUint16(&file);
        if (id == 0) {
            uint16_t tagIndex = readUint16(&file);

            uint8_t tagType;
            file.read((char *) &tagType, sizeof(tagType));

            if (tagType < 1 || tagType > 10) {
                QMessageBox::warning(nullptr, "Error reading file", "Wrong tag type read");
                return false;
            }

            tags[tagIndex] = tagType;
            if (tagIndex > maxTagID)
                maxTagID = tagIndex;

            std::string name = readString(&file);

            if (name.length() == 0) {
                QMessageBox::warning(nullptr, "Error reading file", "Empty tag name read");
                return false;
            }

            std::replace(name.begin(), name.end(), '_', '/');

            if (name == "time")
                timeTagID = tagIndex;

            auto it = plot_data.addNumeric(name);
            plots[tagIndex] = &it->second;
        } else {
            if (id > maxTagID) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read");
                return false;
            }

            // Read value
            uint8_t type = tags[id];

            double value = 0;
            switch (type) {
                case 1: {
                    uint8_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 2: {
                    uint16_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 3: {
                    uint32_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 4: {
                    int8_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 5: {
                    int16_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 6: {
                    int32_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 7: {
                    float v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 8: {
                    double v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 9: {
                    uint64_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
                case 10: {
                    int64_t v;
                    file.read((char *) &v, sizeof(v));
                    value = (double) v;
                    break;
                }
            }

            if (id == timeTagID)
                time = value;

            PlotData::Point point(time, value);
            plots[id]->pushBack(point);
        }
    }

    //QMessageBox::information(nullptr, "File successfully read",  QString("Found %1 signals").arg(maxTagID));

    file.close();
    return true;
}

uint16_t DataLoadDARTLog::readUint16(QFile *file) {
    uint8_t b[2];
    file->read((char *) b, sizeof(b));

    return b[0] * 256 + b[1];
}

std::string DataLoadDARTLog::readString(QFile *file) {
    std::string str = "";
    while (!file->atEnd()) {
        char c;
        file->read(&c, sizeof(c));
        if (c == 0)
            return str;
        str += c;
    }
    return str;
}