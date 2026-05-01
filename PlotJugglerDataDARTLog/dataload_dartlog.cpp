#include "dataload_dartlog.h"

#include <QFile>
#include <QMessageBox>
#include <QDateTime>
#include <QInputDialog>
#include <QProgressDialog>
#include <QFileInfo>

#include <vector>
#include <chrono>
#include <zlib.h>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#define REDUCE_PLOT 0
#define ADD_EDGES_TO_PLOT 0
#define DOWNSAMPLE_HZ 100

class PlotDataAccessor : public PlotData {
public:
    std::deque<Point>* directAccessPoints() {
        return &_points;
    }
};

namespace {

enum SignalDataType : uint8_t {
    TYPE_UINT8 = 1,
    TYPE_UINT16 = 2,
    TYPE_UINT32 = 3,
    TYPE_INT8 = 4,
    TYPE_INT16 = 5,
    TYPE_INT32 = 6,
    TYPE_FLOAT = 7,
    TYPE_DOUBLE = 8,
    TYPE_UINT64 = 9,
    TYPE_INT64 = 10,
    TYPE_STRING = 11,
    TYPE_ENUM = 12,
    TYPE_BYTE_ARRAY_32BIT = 13,
    TYPE_BYTE_ARRAY_64BIT = 14,
    TYPE_FLOAT_ARRAY = 15,
    TYPE_DOUBLE_ARRAY = 16
};

enum TagAttributeType : uint8_t {
    ATTR_UNIT = 1,
    ATTR_VERBOSE_SIGNAL = 2,
    ATTR_GENERATE_CONVERSION_SIGNAL = 3,
    ATTR_GENERATE_NEGATE_SIGNAL = 4,
    ATTR_GENERATE_ABS_SIGNAL = 5
};

enum class GeneratedSignalOperation : uint8_t {
    Conversion,
    Negate,
    Abs
};

struct NumericSeriesState {
    PlotDataAccessor* plot = nullptr;
    std::deque<PlotData::Point>* plotData = nullptr;
    double lastValue = DBL_MAX;
    double lastAddedTime = -1;
};

struct StringSeriesState {
    StringSeries* plot = nullptr;
    std::string lastValue;
    bool hasLastValue = false;
    double lastAddedTime = -1;
};

struct EnumValueState {
    double value = 0;
    std::string label;
    NumericSeriesState series;
};

struct ArrayElementSeriesState {
    NumericSeriesState series;
};

struct GeneratedSignalSeriesState {
    GeneratedSignalOperation operation = GeneratedSignalOperation::Conversion;
    std::string unit;
    double scale = 1.0;
    double offset = 0.0;
    NumericSeriesState series;
};

struct TagData {
    uint8_t type = 0;
    uint8_t enumValueType = 0;
    bool isVerbose = false;
    bool isIgnored = false;
    std::string baseName;
    std::string unit;
    std::string tagName;
    NumericSeriesState numericSeries;
    StringSeriesState stringSeries;
    std::vector<EnumValueState> enumValues;
    std::vector<ArrayElementSeriesState> arraySeries;
    std::vector<GeneratedSignalSeriesState> generatedSignals;
};

double fast_abs(double x) {
    if (x < 0)
		return -x;
	return x;
}

bool isNumericType(uint8_t type) {
    return type >= TYPE_UINT8 && type <= TYPE_INT64;
}

bool isSupportedType(uint8_t type, bool isAtLeastDARTLOG3) {
    if (isNumericType(type)) {
        return true;
    }

    if (!isAtLeastDARTLOG3) {
        return false;
    }

    return type == TYPE_STRING || type == TYPE_ENUM ||
            type == TYPE_BYTE_ARRAY_32BIT || type == TYPE_BYTE_ARRAY_64BIT ||
            type == TYPE_FLOAT_ARRAY || type == TYPE_DOUBLE_ARRAY;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string normalizeTagName(std::string name) {
    std::replace(name.begin(), name.end(), '_', '/');
    return name;
}

std::string normalizeUnitName(std::string unit) {
    std::replace(unit.begin(), unit.end(), '/', '_');
    return unit;
}

std::string appendUnitToTagName(const std::string& signalName, const std::string& unit) {
    if (unit.empty()) {
        return signalName;
    }
    return signalName + "_" + unit;
}

std::string buildGeneratedSeriesName(const TagData& tag,
                                     const GeneratedSignalSeriesState& generatedSignal) {
    switch (generatedSignal.operation) {
        case GeneratedSignalOperation::Conversion: {
            std::string convertedName = appendUnitToTagName(tag.baseName, generatedSignal.unit);
            if (convertedName == tag.tagName) {
                convertedName += "_Converted";
            }
            return convertedName;
        }
        case GeneratedSignalOperation::Negate:
            return tag.tagName + "_Negated";
        case GeneratedSignalOperation::Abs:
            return tag.tagName + "_Abs";
    }

    return tag.tagName + "_Generated";
}

std::string buildEnumSeriesName(const std::string& signalName,
                                const std::string& enumLabel,
                                std::unordered_set<std::string>& usedNames) {
    std::string suffix = enumLabel.empty() ? "value" : enumLabel;
    std::string seriesName = signalName + "/" + suffix;

    if (usedNames.insert(seriesName).second) {
        return seriesName;
    }

    for (size_t duplicateIndex = 1;; ++duplicateIndex) {
        std::string candidate = seriesName + "/" + std::to_string(duplicateIndex);
        if (usedNames.insert(candidate).second) {
            return candidate;
        }
    }
}

std::string buildArraySeriesName(const std::string& signalName, size_t index) {
    return signalName + "/" + std::to_string(index);
}

}  // namespace

DataLoadDARTLog::DataLoadDARTLog() {
    _extensions.push_back("dat");
    _extensions.push_back("gz");
}

const std::vector<const char*>& DataLoadDARTLog::compatibleFileExtensions() const {
    return _extensions;
}


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

    reading_duration_ms = 0;
    io_duration_ms = 0;
    decompress_duration_ms = 0;

    isGZip = info->filename.endsWith(".gz", Qt::CaseInsensitive);
    filePtr = &file;
    inputFileSize = file.size();
    pos = 0;
    finished = false;
    bufferSize = 0;
    bufferOffset = 0;
    posBuffer = 0;

    bool downsample = false;
    if (inputFileSize > 150LL * 1024 * 1024) {
        double sizeMB = inputFileSize / (1024.0 * 1024.0);
        QMessageBox::StandardButton reply = QMessageBox::question(nullptr, "Large File", QString("File size is %1 MB. Downsample to 100 Hz?").arg(std::round(sizeMB), 0, 'f', 0), QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) 
            downsample = true;
    }

    if (isGZip) {
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
            QMessageBox::warning(nullptr, "Error", "Failed to initialize gzip decompressor");
            return false;
        }
    }

    progress_dialog.setLabelText("Loading data... please wait");
    progress_dialog.setValue(0);
    progress_dialog.setRange(0, 100);
    QApplication::processEvents();

    auto reading_start = std::chrono::high_resolution_clock::now();

    std::vector<TagData> tagData;

    uint32_t timeTagID = 0;
    bool hasTimeTag = false;
    double time = 0;

    // Check if file is empty
    if (atEnd(64)) {
        QMessageBox::warning(nullptr, "Error reading file", "File is empty");
        return false;
	}

    // Read header
    std::string header = readString();
    if (header != "DARTLOG" && header != "DARTLOG2" && header != "DARTLOG3") {
        QMessageBox::warning(nullptr, "Error reading file", "Not a DARTLOG file: header missing.");
        return false;
    }

    PlotData::Point dartLogVersion(0, 1);
    if (header == "DARTLOG2")
        dartLogVersion = PlotData::Point(0, 2);
    else if (header == "DARTLOG3")
        dartLogVersion = PlotData::Point(0, 3);

    bool isAtLeastDARTLOG2 = dartLogVersion.y >= 2;
    bool isAtLeastDARTLOG3 = dartLogVersion.y >= 3;

    bool loadVerboseData = false;
    uint64_t counter = 0;
    uint32_t lastID = 0;
    const uint64_t progressDialogTickMask = (1024 * 1024);

    uint32_t verboseSignalsIgnoredCount = 0;
    uint32_t byteArraySignalsIgnoredCount = 0;
    const double downsampleInterval = 1.0 / DOWNSAMPLE_HZ;

    auto initNumericSeries = [&](NumericSeriesState& state, const std::string& name) {
        auto it = plot_data.addNumeric(name);
        auto plot = reinterpret_cast<PlotDataAccessor*>(&it->second);

        state.plot = plot;
        state.plotData = plot->directAccessPoints();
        state.lastValue = DBL_MAX;
        state.lastAddedTime = -1;
    };

    auto initStringSeries = [&](StringSeriesState& state, const std::string& name) {
        auto it = plot_data.addStringSeries(name);

        state.plot = &it->second;
        state.lastValue.clear();
        state.hasLastValue = false;
        state.lastAddedTime = -1;
    };

    auto pushNumericPoint = [&](NumericSeriesState& state, double timestamp, double value) {
        if (state.plotData == nullptr) {
            return;
        }

        PlotData::Point localPoint(timestamp, value);
        if (downsample) {
            if (state.lastAddedTime < 0 || fast_abs(timestamp - state.lastAddedTime) >= downsampleInterval) {
                state.plotData->push_back(localPoint);
                state.lastAddedTime = timestamp;
            }
        }
        else {
            state.plotData->push_back(localPoint);
        }

        state.lastValue = value;
    };

    auto pushStringPoint = [&](StringSeriesState& state, double timestamp, const std::string& value) {
        if (state.plot == nullptr) {
            return;
        }

        if (downsample && state.lastAddedTime >= 0 && fast_abs(timestamp - state.lastAddedTime) < downsampleInterval) {
            state.lastValue = value;
            state.hasLastValue = true;
            return;
        }

        state.plot->pushBack({ timestamp, StringRef(value) });
        state.lastAddedTime = timestamp;
        state.lastValue = value;
        state.hasLastValue = true;
    };

    auto finalizeNumericSeries = [&](const NumericSeriesState& state) {
        if (state.plot != nullptr && state.lastValue != DBL_MAX) {
            state.plot->pushBack(PlotData::Point(time, state.lastValue));
        }
    };

    auto finalizeStringSeries = [&](const StringSeriesState& state) {
        if (state.plot != nullptr && state.hasLastValue) {
            state.plot->pushBack({ time, StringRef(state.lastValue) });
        }
    };

    auto pushGeneratedSignalPoints = [&](TagData& tag, double timestamp, double sourceValue) {
        for (auto& generatedSignal : tag.generatedSignals) {
            double generatedValue = sourceValue;
            switch (generatedSignal.operation) {
                case GeneratedSignalOperation::Conversion:
                    generatedValue = sourceValue * generatedSignal.scale + generatedSignal.offset;
                    break;
                case GeneratedSignalOperation::Negate:
                    generatedValue = -sourceValue;
                    break;
                case GeneratedSignalOperation::Abs:
                    generatedValue = fast_abs(sourceValue);
                    break;
            }

            pushNumericPoint(generatedSignal.series, timestamp, generatedValue);
        }
    };

    auto ensureArraySeries = [&](TagData& tag, size_t elementCount) {
        while (tag.arraySeries.size() < elementCount) {
            ArrayElementSeriesState elementState;
            initNumericSeries(elementState.series,
                              buildArraySeriesName(tag.tagName, tag.arraySeries.size()));
            tag.arraySeries.push_back(std::move(elementState));
        }
    };

    auto readNumericValue = [&](uint8_t type, double& value) {
        switch (type) {
            case TYPE_UINT8: {
                value = static_cast<double>(readUint8());
                return true;
            }
            case TYPE_UINT16: {
                value = static_cast<double>(readUint16());
                return true;
            }
            case TYPE_UINT32: {
                value = static_cast<double>(readUint32());
                return true;
            }
            case TYPE_INT8: {
                int8_t v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = static_cast<double>(v);
                return true;
            }
            case TYPE_INT16: {
                int16_t v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = static_cast<double>(v);
                return true;
            }
            case TYPE_INT32: {
                int32_t v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = static_cast<double>(v);
                return true;
            }
            case TYPE_FLOAT: {
                float v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = static_cast<double>(v);
                return true;
            }
            case TYPE_DOUBLE: {
                double v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = v;
                return true;
            }
            case TYPE_UINT64: {
                value = static_cast<double>(readUint64());
                return true;
            }
            case TYPE_INT64: {
                int64_t v;
                read(reinterpret_cast<char*>(&v), sizeof(v));
                value = static_cast<double>(v);
                return true;
            }
            default:
                return false;
        }
    };

    PlotData::Point point(0, 0);
    while (true) {
        // Update file progress dialog
        if ((counter & (progressDialogTickMask - 1)) == 0) {
            progress_dialog.setValue((int)std::round((double)pos / inputFileSize * 100));
            if (progress_dialog.wasCanceled())
                break;

            QApplication::processEvents();
        }
        counter++;

        // Check if at end
		if (atEnd(256)) // some margin
            break;

        // Read next tag
        uint32_t id;
        if (isAtLeastDARTLOG2) {
            uint8_t idPart = readUint8();
            if (idPart == 255) {
                uint32_t extendedID = readUint16();
                if (isAtLeastDARTLOG3 && extendedID == std::numeric_limits<uint16_t>::max()) {
                    extendedID = readUint32();
                }
                id = extendedID;
            }
            else if (idPart == 254)
                id = lastID + 1;
            else
                id = idPart;
        }
        else
            id = readUint16();

        lastID = id;

        if (id == 0) {
            uint32_t tagIndex = readUint16();
            if (isAtLeastDARTLOG3 && tagIndex == std::numeric_limits<uint16_t>::max()) {
                tagIndex = readUint32();
            }

            uint8_t tagType = readUint8();

            if (!isSupportedType(tagType, isAtLeastDARTLOG3)) {
                QMessageBox::warning(nullptr, "Error reading file", "Wrong tag type read");
                break;
            }

            TagData newTag;
            newTag.type = tagType;

            std::string name = readString();

            if (name.length() == 0) {
                QMessageBox::warning(nullptr, "Error reading file", "Empty tag name read");
                break;
            }

            if (tagType == TYPE_ENUM) {
                newTag.enumValueType = readUint8();
                if (!isNumericType(newTag.enumValueType)) {
                    QMessageBox::warning(nullptr, "Error reading file", "Wrong enum value type read");
                    break;
                }

                uint32_t enumValueCount = readUint16();
                if (enumValueCount == std::numeric_limits<uint16_t>::max()) {
                    enumValueCount = readUint32();
                }

                newTag.enumValues.reserve(enumValueCount);
                bool enumDefinitionValid = true;
                for (uint32_t valueIndex = 0; valueIndex < enumValueCount; ++valueIndex) {
                    double enumValue = 0;
                    if (!readNumericValue(newTag.enumValueType, enumValue)) {
                        QMessageBox::warning(nullptr, "Error reading file", "Unsupported enum value type read");
                        enumDefinitionValid = false;
                        break;
                    }

                    EnumValueState enumState;
                    enumState.value = enumValue;
                    enumState.label = readString();
                    newTag.enumValues.push_back(std::move(enumState));
                }

                if (!enumDefinitionValid) {
                    break;
                }
            }

            std::string unit = "";
            bool verbose = false;
            if (isAtLeastDARTLOG2)
            {
                while (true)
                {
                    uint8_t attributeType = readUint8();

                    if (attributeType == 0)
                        break;

                    uint32_t attributeLength = isAtLeastDARTLOG3 ? readUint16() : readUint8();
                    std::vector<char> attributeData(attributeLength);
                    if (attributeLength > 0) {
                        read(attributeData.data(), attributeLength);
                    }

                    auto attributeString = [&attributeData]() {
                        auto stringEnd = std::find(attributeData.begin(), attributeData.end(), '\0');
                        return std::string(attributeData.begin(), stringEnd);
                    };

                    switch (attributeType)
                    {
                    case ATTR_UNIT: {
                        unit = normalizeUnitName(attributeString());
                        break;
                    }
                    case ATTR_VERBOSE_SIGNAL: {
                        verbose = !attributeData.empty() && static_cast<uint8_t>(attributeData[0]) > 0;
                        break;
                    }
                    case ATTR_GENERATE_CONVERSION_SIGNAL: {
                        auto nullIt = std::find(attributeData.begin(), attributeData.end(), '\0');
                        size_t unitLength = static_cast<size_t>(std::distance(attributeData.begin(), nullIt));
                        size_t numericOffset = unitLength + 1;

                        if (numericOffset + sizeof(float) + sizeof(float) <= attributeData.size()) {
                            float scale = 1.0f;
                            float offset = 0.0f;
                            memcpy(&scale, attributeData.data() + numericOffset, sizeof(float));
                            memcpy(&offset, attributeData.data() + numericOffset + sizeof(float), sizeof(float));

                            GeneratedSignalSeriesState generatedSignal;
                            generatedSignal.operation = GeneratedSignalOperation::Conversion;
                            generatedSignal.unit = normalizeUnitName(std::string(attributeData.data(), unitLength));
                            generatedSignal.scale = static_cast<double>(scale);
                            generatedSignal.offset = static_cast<double>(offset);
                            newTag.generatedSignals.push_back(std::move(generatedSignal));
                        }
                        break;
                    }
                    case ATTR_GENERATE_NEGATE_SIGNAL: {
                        GeneratedSignalSeriesState generatedSignal;
                        generatedSignal.operation = GeneratedSignalOperation::Negate;
                        generatedSignal.unit = unit;
                        newTag.generatedSignals.push_back(std::move(generatedSignal));
                        break;
                    }
                    case ATTR_GENERATE_ABS_SIGNAL: {
                        GeneratedSignalSeriesState generatedSignal;
                        generatedSignal.operation = GeneratedSignalOperation::Abs;
                        generatedSignal.unit = unit;
                        newTag.generatedSignals.push_back(std::move(generatedSignal));
                        break;
                    }

                    default:
                        break;
                    }
                }
            }

            name = normalizeTagName(name);

            if (name == "time") {
                timeTagID = tagIndex;
                hasTimeTag = true;
            }

            // Check if the name is the start of a different value
            for (const auto& existingTag : tagData) {
                if (!existingTag.tagName.empty() && startsWith(existingTag.tagName, name)) {
                    name += "/Value";
                    break;
                }
            }

            newTag.baseName = name;
            newTag.unit = unit;
            name = appendUnitToTagName(name, unit);

            newTag.tagName = name;
            newTag.isVerbose = verbose;

            if (tagType == TYPE_BYTE_ARRAY_32BIT || tagType == TYPE_BYTE_ARRAY_64BIT) {
                newTag.isIgnored = true;
                byteArraySignalsIgnoredCount++;
            }
            else if (verbose && !loadVerboseData) {
                newTag.isIgnored = true;
                verboseSignalsIgnoredCount++;
            }
            else if (tagType == TYPE_STRING) {
                initStringSeries(newTag.stringSeries, name);
            }
            else if (tagType == TYPE_ENUM) {
                std::unordered_set<std::string> usedEnumSeriesNames;

                initStringSeries(newTag.stringSeries, name);
                initNumericSeries(newTag.numericSeries, name + "_raw");

                for (auto& enumState : newTag.enumValues) {
                    initNumericSeries(enumState.series,
                                      buildEnumSeriesName(name, enumState.label, usedEnumSeriesNames));
                }
            }
            else {
                initNumericSeries(newTag.numericSeries, name);
            }

            if (!newTag.isIgnored && (isNumericType(tagType) || tagType == TYPE_ENUM)) {
                for (auto& generatedSignal : newTag.generatedSignals) {
                    initNumericSeries(generatedSignal.series,
                                      buildGeneratedSeriesName(newTag, generatedSignal));
                }
            }

            if (tagData.size() <= tagIndex) {
                tagData.resize(static_cast<size_t>(tagIndex) + 4096);
            }
            tagData[tagIndex] = std::move(newTag);
        }
        else {
            if (tagData.size() <= id || tagData[id].type == 0) {
                QMessageBox::warning(nullptr, "Error reading file", "Invalid ID read: unknown tag id");
                break;
            }

			auto& tag = tagData[id];

            if (tag.type == TYPE_BYTE_ARRAY_32BIT) {
                skip(readUint32());
                continue;
            }

            if (tag.type == TYPE_BYTE_ARRAY_64BIT) {
                uint64_t length = readUint64();
                if (length > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
                    QMessageBox::warning(nullptr, "Error reading file", "BYTE_ARRAY_64BIT entry too large");
                    break;
                }

                skip(static_cast<qint64>(length));
                continue;
            }

            if (tag.type == TYPE_STRING) {
                std::string value = readString();

                if (tag.isIgnored) {
                    continue;
                }

                pushStringPoint(tag.stringSeries, time, value);
                continue;
            }

            if (tag.type == TYPE_FLOAT_ARRAY || tag.type == TYPE_DOUBLE_ARRAY) {
                uint32_t elementCount = readUint32();
                ensureArraySeries(tag, elementCount);

                for (uint32_t elementIndex = 0; elementIndex < elementCount; ++elementIndex) {
                    double elementValue = 0;
                    if (tag.type == TYPE_FLOAT_ARRAY) {
                        float v;
                        read(reinterpret_cast<char*>(&v), sizeof(v));
                        elementValue = static_cast<double>(v);
                    }
                    else {
                        double v;
                        read(reinterpret_cast<char*>(&v), sizeof(v));
                        elementValue = v;
                    }

                    if (!tag.isIgnored) {
                        pushNumericPoint(tag.arraySeries[elementIndex].series, time, elementValue);
                    }
                }
                continue;
            }

            // Read value
            uint8_t type = (tag.type == TYPE_ENUM) ? tag.enumValueType : tag.type;

            double value = 0;
            if (!readNumericValue(type, value)) {
                QMessageBox::warning(nullptr, "Error reading file", "Unsupported signal value type");
                break;
            }

            if (hasTimeTag && id == timeTagID)
            {
                time = value;
                point.x = time;
            }

            // Skip verbose values
            if (tag.isIgnored)
                continue;

            if (tag.type == TYPE_ENUM) {
                std::string enumLabel = std::to_string(value);

                pushNumericPoint(tag.numericSeries, time, value);
                pushGeneratedSignalPoints(tag, time, value);
                for (auto& enumState : tag.enumValues) {
                    bool isActive = (enumState.value == value);
                    pushNumericPoint(enumState.series, time, isActive ? 1.0 : 0.0);
                    if (isActive) {
                        enumLabel = enumState.label;
                    }
                }

                pushStringPoint(tag.stringSeries, time, enumLabel);
                continue;
            }

#if REDUCE_PLOT
            double lastVal = tag.numericSeries.lastValue;
            double lastT = tag.numericSeries.lastAddedTime;

            bool valueChanged = fast_abs(lastVal - value) >= 0.00001;
            bool timeChanged = fast_abs(time - lastT) >= 0.1;

            if (valueChanged || timeChanged) {
#if ADD_EDGES_TO_PLOT
                // Add point just before last value to ensure edges are in plot
                if (lastT >= 0 && valueChanged && timeChanged) {
                    PlotData::Point point(time - 0.001, lastVal);
                    tag.numericSeries.plot->pushBack(point);
                }
#endif

                point.y = value;
                tag.numericSeries.plotData->push_back(point);

                tag.numericSeries.lastAddedTime = time;
                tag.numericSeries.lastValue = value;
            }
#else
            pushNumericPoint(tag.numericSeries, time, value);
            pushGeneratedSignalPoints(tag, time, value);
#endif
        }
    }

    auto reading_end = std::chrono::high_resolution_clock::now();
	reading_duration_ms = std::chrono::duration_cast<std::chrono::microseconds>(reading_end - reading_start).count() / 1000.0;

    // Add for all tags last value at the current time (also trigger range update)
        for (const auto& tag : tagData) {
        finalizeNumericSeries(tag.numericSeries);
        finalizeStringSeries(tag.stringSeries);
        for (const auto& enumState : tag.enumValues) {
            finalizeNumericSeries(enumState.series);
        }
        for (const auto& arrayState : tag.arraySeries) {
            finalizeNumericSeries(arrayState.series);
        }
        for (const auto& generatedSignal : tag.generatedSignals) {
            finalizeNumericSeries(generatedSignal.series);
        }
    }

    // Add logger informations
    PlotData::Point version(0, 20);
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

    if (byteArraySignalsIgnoredCount > 0) {
        PlotData::Point ignoredByteArrayPoint(0, byteArraySignalsIgnoredCount);
        plot_data.addNumeric("BYTE_ARRAY_DATA_NOT_LOADED")->second.pushBack(ignoredByteArrayPoint);

        PlotData::Point ignoredByteArrayCountPoint(0, byteArraySignalsIgnoredCount);
        plot_data.addNumeric("byte_array_signal_count")->second.pushBack(ignoredByteArrayCountPoint);
    }

    PlotData::Point decompressPoint(0, decompress_duration_ms);
    plot_data.addNumeric("dartlog_decompression_time_ms")->second.pushBack(decompressPoint);

    PlotData::Point ioPoint(0, io_duration_ms);
    plot_data.addNumeric("dartlog_io_time_ms")->second.pushBack(ioPoint);

    PlotData::Point readingPoint(0, reading_duration_ms);
    plot_data.addNumeric("dartlog_reading_time_ms")->second.pushBack(readingPoint);

    PlotData::Point downsamplePoint(0, downsample ? DOWNSAMPLE_HZ : 0.0);
    plot_data.addNumeric("dartlog_downsampled")->second.pushBack(downsamplePoint);

    close();
    progress_dialog.close();
    return true;
}

void DataLoadDARTLog::close() {
    if (isGZip && !finished)
        inflateEnd(&strm);
    if (filePtr) 
        filePtr->close();
}

qint64 DataLoadDARTLog::getPos() {
    return pos;
}

qint64 DataLoadDARTLog::getSize() {
    return inputFileSize;
}

bool DataLoadDARTLog::atEnd() {
    return atEnd(0);
}

bool DataLoadDARTLog::atEnd(qint64 len) {
    while (posBuffer + len > bufferSize && !finished) {
        if (!loadMoreData()) {
            break;
        }
    }

    return posBuffer + len > bufferSize;
}

void DataLoadDARTLog::read(char* data, qint64 maxLen) {
    qint64 copied = 0;
    while (copied < maxLen) {
        if (posBuffer >= bufferSize && !loadMoreData()) {
            break;
        }

        qint64 available = bufferSize - posBuffer;
        if (available <= 0) {
            break;
        }

        qint64 chunk = std::min(maxLen - copied, available);
        memcpy(data + copied, buffer + posBuffer, chunk);
        posBuffer += chunk;
        pos += chunk;
        copied += chunk;
    }
}

void DataLoadDARTLog::skip(qint64 bytes) {
    qint64 skipped = 0;
    while (skipped < bytes) {
        if (posBuffer >= bufferSize && !loadMoreData()) {
            break;
        }

        qint64 available = bufferSize - posBuffer;
        if (available <= 0) {
            break;
        }

        qint64 chunk = std::min(bytes - skipped, available);
        posBuffer += chunk;
        pos += chunk;
        skipped += chunk;
    }
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

uint32_t DataLoadDARTLog::readUint32() {
    uint8_t b[4];
    read((char*)b, sizeof(b));

    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t DataLoadDARTLog::readUint64() {
    uint8_t b[8];
    read((char*)b, sizeof(b));

    return static_cast<uint64_t>(b[0]) |
           (static_cast<uint64_t>(b[1]) << 8) |
           (static_cast<uint64_t>(b[2]) << 16) |
           (static_cast<uint64_t>(b[3]) << 24) |
           (static_cast<uint64_t>(b[4]) << 32) |
           (static_cast<uint64_t>(b[5]) << 40) |
           (static_cast<uint64_t>(b[6]) << 48) |
           (static_cast<uint64_t>(b[7]) << 56);
}

std::string DataLoadDARTLog::readString() {
    std::string str = "";
    str.reserve(64);
    while (!atEnd()) {
        char c;
        read(&c, sizeof(c));
        if (c == 0)
            return str;
        str += c;
    }
    return str;
}

bool DataLoadDARTLog::loadMoreData() {
    if (finished)
        return false;

    // Move remaining data to front
    qint64 remaining = bufferSize - posBuffer;
    if (remaining > 0)
        memmove(buffer, buffer + posBuffer, remaining);
    posBuffer = 0;

    // Load new chunk into buffer + remaining
    qint64 loaded = loadNewChunk(buffer + remaining, CHUNK_SIZE);
    if (loaded == 0) {
        finished = true;
        bufferSize = remaining;
        return remaining > 0;
    }
    bufferSize = remaining + loaded;
    return true;
}

qint64 DataLoadDARTLog::loadNewChunk(char* chunkBuffer, qint64 maxSize) {
    if (isGZip) {
        strm.next_out = (Bytef*)chunkBuffer;
        strm.avail_out = maxSize;

        while (strm.avail_out > 0 && !finished) {
            if (strm.avail_in == 0) {
                auto io_start = std::chrono::high_resolution_clock::now();
                qint64 readSize = filePtr->read(compressedBuffer, CHUNK_SIZE);
                auto io_end = std::chrono::high_resolution_clock::now();
				io_duration_ms += std::chrono::duration_cast<std::chrono::microseconds>(io_end - io_start).count() / 1000.0;

                if (readSize == 0) {
                    // finish
                    int ret = inflate(&strm, Z_FINISH);
                    if (ret == Z_STREAM_END) {
                        finished = true;
                    } else {
                        finished = true;
                    }
                    break;
                }
                strm.next_in = (Bytef*)compressedBuffer;
                strm.avail_in = readSize;
            }
			auto decompress_start = std::chrono::high_resolution_clock::now();
            int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END) {
                inflateEnd(&strm);
                finished = true;
                break;
            } else if (ret != Z_OK) {
                inflateEnd(&strm);
                finished = true;
                break;
            }
			auto decompress_end = std::chrono::high_resolution_clock::now();
			decompress_duration_ms += std::chrono::duration_cast<std::chrono::microseconds>(decompress_end - decompress_start).count() / 1000.0;
        }
        size_t have = maxSize - strm.avail_out;
        return have;
    } 
    else {
        // Load directly from file
        auto io_start = std::chrono::high_resolution_clock::now();
        qint64 readSize = filePtr->read(chunkBuffer, maxSize);
        if (readSize == 0) {
            finished = true;
        }
        auto io_end = std::chrono::high_resolution_clock::now();
        io_duration_ms += std::chrono::duration_cast<std::chrono::microseconds>(io_end - io_start).count() / 1000.0;
        return readSize;
    }
}