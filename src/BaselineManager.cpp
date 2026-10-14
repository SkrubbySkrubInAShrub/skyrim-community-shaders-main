#include "BaselineManager.h"

#include "Feature.h"
#include "FeatureIssues.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "Util.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <sstream>

using namespace SKSE;

namespace
{
	constexpr size_t MAX_BASELINE_FILE_SIZE = 1024 * 1024;
	constexpr int kBaselineJsonIndent = 2;

	/** @brief Parses a Baseline file without the discovery-time filename and sanitization passes. */
	bool ReadBaselineDocument(const std::filesystem::path& path, json& document)
	{
		auto parsed = Util::FileHelpers::ReadJsonObject(path, "Baseline file", MAX_BASELINE_FILE_SIZE);
		if (!parsed)
			return false;
		document = std::move(*parsed);
		return true;
	}

	/** @brief Erases one setting leaf, then any groups it leaves empty. */
	void EraseSettingPath(json& document, json::json_pointer pointer)
	{
		while (!pointer.empty() && document.contains(pointer)) {
			auto& parent = document.at(pointer.parent_pointer());
			parent.erase(pointer.back());
			if (!parent.empty())
				return;
			pointer = pointer.parent_pointer();
		}
	}
}

size_t BaselineManager::DiscoverFiles()
{
	files.clear();
	featureFileMap.clear();
	++revision;

	// Each enabled pack in layering order: later files win merge conflicts.
	for (const auto& packId : UnifiedPresetCatalog::GetSingleton().GetBaselinePackIds()) {
		for (const auto& packPath : Util::PathHelpers::GetUnifiedPackPaths(packId))
			DiscoverDirectory(packPath / UnifiedPresetCatalog::kBaselineSubdir, packId);
	}

	discovered = true;
	logger::info("Discovered {} Baseline files", files.size());
	return files.size();
}

std::string BaselineManager::ParseBaselineFeatureName(const std::filesystem::path& filePath)
{
	const auto filename = filePath.filename().string();
	if (filePath.extension() != ".json" || filename.empty() || filename[0] == '.' || filename[0] == '~') {
		return {};
	}

	auto name = filePath.stem().string();
	if (const auto underscore = name.find_last_of('_'); underscore != std::string::npos) {
		name.erase(0, underscore + 1);
	}
	return name;
}

size_t BaselineManager::DiscoverDirectory(const std::filesystem::path& directory, const std::string& packId)
{
	if (!std::filesystem::exists(directory)) {
		logger::info("Baseline directory does not exist: {}", directory.string());
		return 0;
	}

	logger::info("Discovering Baseline files in: {}", directory.string());

	size_t filesProcessed = 0;
	size_t filesLoaded = 0;
	size_t maxFilesToProcess = 1000;  // Prevent processing too many files

	try {
		for (const auto& entry : std::filesystem::directory_iterator(directory)) {
			// Safety check to prevent infinite loops or DoS
			if (filesProcessed >= maxFilesToProcess) {
				logger::info("Reached maximum file processing limit ({}), stopping discovery", maxFilesToProcess);
				break;
			}
			filesProcessed++;

			// Skip if not a regular file or not a JSON file
			if (!entry.is_regular_file() || entry.path().extension() != ".json") {
				continue;
			}

			// Check filename length to prevent overly long names
			std::string filename = entry.path().filename().string();
			if (filename.length() > MAX_STRING_LENGTH) {
				logger::info("Skipping Baseline file with overly long name: {}", filename.substr(0, 50) + "...");
				continue;
			}

			// Skip hidden files or files starting with special characters
			if (filename.empty() || filename[0] == '.' || filename[0] == '~') {
				logger::info("Skipping hidden/temporary file: {}", filename);
				continue;
			}

			std::error_code ec;
			auto fileSize = std::filesystem::file_size(entry.path(), ec);
			if (ec) {
				logger::info("Could not get size of Baseline file, skipping: {}", entry.path().string());
				continue;
			}

			// Skip empty files or overly large files
			if (fileSize == 0) {
				logger::info("Skipping empty Baseline file: {}", entry.path().string());
				continue;
			}

			if (fileSize > MAX_BASELINE_FILE_SIZE) {
				logger::info("Skipping overly large Baseline file ({}KB): {}", fileSize / 1024, entry.path().string());
				continue;
			}

			// Pack files are per-feature only; this is what keeps Advanced/General/Menu keys (e.g. compiler threads) unshippable.
			// Checked against the build rather than load state: discovery runs before features load at boot.
			const auto featureName = ParseBaselineFeatureName(entry.path());
			if (!Feature::IsFeatureKnown(featureName)) {
				logger::warn("Baseline file {} targets '{}', which is not a feature in this build; skipping", entry.path().string(), featureName);
				ReportFailure(packId, featureName, "Feature is not part of this Community Shaders build", entry.path());
				continue;
			}

			try {
				auto baselineFile = LoadFile(entry.path(), packId);
				if (baselineFile) {
					size_t index = files.size();
					files.push_back(std::move(*baselineFile));

					// Map files by feature for quick lookup
					const auto& file = files[index];
					featureFileMap[file.featureName].push_back(index);

					filesLoaded++;
					logger::info("Loaded Baseline file: {} for {}", file.packId, file.featureName);
				} else {
					ReportFailure(packId, featureName, "File could not be loaded or parsed", entry.path());
				}
			} catch (const std::exception& e) {
				logger::info("Error loading Baseline file {}: {}", entry.path().string(), e.what());
				ReportFailure(packId, featureName, e.what(), entry.path());
				continue;
			}
		}
	} catch (const std::filesystem::filesystem_error& e) {
		logger::info("Error accessing Baseline directory: {}", e.what());
	} catch (const std::exception& e) {
		logger::info("Unexpected error during Baseline discovery: {}", e.what());
	}

	logger::info("Loaded {} Baseline files from {} ({} processed)", filesLoaded, directory.string(), filesProcessed);
	return filesProcessed;
}

size_t BaselineManager::ApplyBaselines(const std::string& featureName, json& featureJson)
{
	if (!discovered) {
		return 0;
	}

	size_t appliedCount = 0;

	auto it = featureFileMap.find(featureName);
	if (it != featureFileMap.end()) {
		for (size_t index : it->second) {
			auto& file = files[index];
			if (!RejectsUnknownKeys(file, featureJson)) {
				try {
					MergeJson(featureJson, file.values);
					appliedCount++;
				} catch (const std::exception& e) {
					logger::info("Failed to apply Baseline file from {} to {}: {}", file.packId, featureName, e.what());
					ReportFailure(file.packId, featureName, "Failed to apply: " + std::string(e.what()), file.filePath);
				}
			}
		}
	}

	return appliedCount;
}

std::vector<const BaselineManager::BaselineFile*> BaselineManager::GetFeatureFiles(const std::string& featureName) const
{
	std::vector<const BaselineFile*> result;

	auto it = featureFileMap.find(featureName);
	if (it != featureFileMap.end()) {
		for (size_t index : it->second) {
			result.push_back(&files[index]);
		}
	}

	return result;
}

bool BaselineManager::HasFeatureFiles(const std::string& featureName) const
{
	if (!discovered) {
		return false;
	}

	auto it = featureFileMap.find(featureName);
	return it != featureFileMap.end() && !it->second.empty();
}

void BaselineManager::Refresh()
{
	discovered = false;
	DiscoverFiles();
}

std::unique_ptr<BaselineManager::BaselineFile> BaselineManager::LoadFile(const std::filesystem::path& filePath, const std::string& packId)
{
	try {
		// Check file size to prevent loading extremely large files
		std::error_code ec;
		auto fileSize = std::filesystem::file_size(filePath, ec);
		if (ec) {
			logger::info("Could not get file size for Baseline file: {}", filePath.string());
			return nullptr;
		}

		// Limit file size to 1MB to prevent abuse
		if (fileSize > MAX_BASELINE_FILE_SIZE) {
			logger::info("Baseline file too large ({}KB, max 1MB): {}", fileSize / 1024, filePath.string());
			return nullptr;
		}

		std::ifstream file(filePath);
		if (!file.is_open()) {
			logger::info("Could not open Baseline file: {}", filePath.string());
			return nullptr;
		}

		// Read entire file content for hash computation
		std::string fileContent;
		fileContent.reserve(static_cast<size_t>(fileSize));
		fileContent.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();

		// Validate file content is not empty
		if (fileContent.empty()) {
			logger::info("Baseline file is empty: {}", filePath.string());
			return nullptr;
		}

		// Parse JSON from content with error handling
		json fileJson;
		try {
			fileJson = json::parse(fileContent);
		} catch (const json::parse_error& e) {
			logger::info("JSON parse error in Baseline file {}: {} at byte {}",
				filePath.string(), e.what(), e.byte);
			return nullptr;
		} catch (const json::exception& e) {
			logger::info("JSON error in Baseline file {}: {}", filePath.string(), e.what());
			return nullptr;
		}

		// Validate the file's format and data types
		if (!ValidateFormat(fileJson, filePath.string())) {
			logger::info("Invalid Baseline file format: {}", filePath.string());
			return nullptr;
		}

		if (!ValidateJsonDataTypes(fileJson, "", filePath.string())) {
			logger::info("Invalid data types in Baseline file: {}", filePath.string());
			return nullptr;
		}

		auto baselineFile = std::make_unique<BaselineFile>();

		const auto featureName = ParseBaselineFeatureName(filePath);
		assert(Feature::IsFeatureKnown(featureName));

		baselineFile->packId = packId;
		baselineFile->featureName = featureName;
		baselineFile->filePath = filePath.string();

		// Extract and validate metadata if present
		if (fileJson.contains("_metadata") && fileJson["_metadata"].is_object()) {
			const auto& metadata = fileJson["_metadata"];

			try {
				if (metadata.contains("version") && metadata["version"].is_string()) {
					std::string version = metadata["version"];
					if (version.length() <= MAX_STRING_LENGTH) {
						baselineFile->version = version;
					} else {
						logger::info("Version string too long in Baseline file: {}", filePath.string());
					}
				}

				if (metadata.contains("description") && metadata["description"].is_string()) {
					std::string description = metadata["description"];
					if (description.length() <= MAX_STRING_LENGTH) {
						baselineFile->description = description;
					} else {
						logger::info("Description string too long in Baseline file: {}", filePath.string());
					}
				}

			} catch (const json::exception& e) {
				logger::info("Error parsing metadata in Baseline file {}: {}", filePath.string(), e.what());
				// Continue without metadata if it's malformed
			}
		}

		// Store the settings (excluding metadata) and sanitize it
		baselineFile->values = SanitizeJsonData(fileJson);
		if (baselineFile->values.contains("_metadata")) {
			baselineFile->values.erase("_metadata");
		}

		return baselineFile;

	} catch (const std::filesystem::filesystem_error& e) {
		logger::info("Filesystem error loading Baseline file {}: {}", filePath.string(), e.what());
		return nullptr;
	} catch (const std::exception& e) {
		logger::info("Unexpected error loading Baseline file {}: {}", filePath.string(), e.what());
		return nullptr;
	}
}

bool BaselineManager::ValidateFormat(const json& fileJson, const std::string& filePath)
{
	try {
		// Basic validation - must be an object
		if (!fileJson.is_object()) {
			if (!filePath.empty()) {
				logger::info("Baseline file must contain a JSON object: {}", filePath);
			}
			return false;
		}

		// Check object size limits
		if (fileJson.size() > MAX_OBJECT_SIZE) {
			if (!filePath.empty()) {
				logger::info("Baseline file contains too many top-level properties (max {}): {}", MAX_OBJECT_SIZE, filePath);
			}
			return false;
		}

		// Must have at least one non-metadata field
		bool hasNonMetadata = false;
		for (const auto& [key, value] : fileJson.items()) {
			if (key.length() == 0 || key[0] != '_') {
				hasNonMetadata = true;
				break;
			}
		}

		if (!hasNonMetadata) {
			if (!filePath.empty()) {
				logger::info("Baseline file contains only metadata, no settings: {}", filePath);
			}
			return false;
		}

		// Validate metadata section if present
		if (fileJson.contains("_metadata")) {
			const auto& metadata = fileJson["_metadata"];
			if (!metadata.is_object()) {
				if (!filePath.empty()) {
					logger::info("_metadata must be an object in Baseline file: {}", filePath);
				}
				return false;
			}

			// Validate metadata fields
			for (const auto& [key, value] : metadata.items()) {
				if (key == "version" || key == "description") {
					if (!value.is_string()) {
						if (!filePath.empty()) {
							logger::info("Metadata field '{}' must be a string in Baseline file: {}", key, filePath);
						}
						return false;
					}
					if (value.get<std::string>().length() > MAX_STRING_LENGTH) {
						if (!filePath.empty()) {
							logger::info("Metadata field '{}' too long (max {} chars) in Baseline file: {}", key, MAX_STRING_LENGTH, filePath);
						}
						return false;
					}
				} else if (key == "enabled") {
					if (!value.is_boolean()) {
						if (!filePath.empty()) {
							logger::info("Metadata field 'enabled' must be a boolean in Baseline file: {}", filePath);
						}
						return false;
					}
				} else if (key == "modName") {
					// Allow modName in metadata but validate it
					if (!value.is_string()) {
						if (!filePath.empty()) {
							logger::info("Metadata field 'modName' must be a string in Baseline file: {}", filePath);
						}
						return false;
					}
				} else {
					// Unknown metadata field - warn but don't fail
					if (!filePath.empty()) {
						logger::info("Unknown metadata field '{}' in Baseline file: {}", key, filePath);
					}
				}
			}
		}

		return true;

	} catch (const json::exception& e) {
		if (!filePath.empty()) {
			logger::info("JSON error during validation of Baseline file {}: {}", filePath, e.what());
		}
		return false;
	} catch (const std::exception& e) {
		if (!filePath.empty()) {
			logger::info("Error during validation of Baseline file {}: {}", filePath, e.what());
		}
		return false;
	}
}

bool BaselineManager::ValidateJsonDataTypes(const json& jsonData, const std::string& path, const std::string& filePath)
{
	try {
		// Check nesting depth
		if (std::count(path.begin(), path.end(), '.') > MAX_JSON_DEPTH) {
			if (!filePath.empty()) {
				logger::info("JSON nesting too deep at '{}' (max {}) in Baseline file: {}", path, MAX_JSON_DEPTH, filePath);
			}
			return false;
		}

		if (jsonData.is_object()) {
			if (jsonData.size() > MAX_OBJECT_SIZE) {
				if (!filePath.empty()) {
					logger::info("JSON object too large at '{}' (max {} properties) in Baseline file: {}", path, MAX_OBJECT_SIZE, filePath);
				}
				return false;
			}

			for (const auto& [key, value] : jsonData.items()) {
				// Validate key length
				if (key.length() > MAX_STRING_LENGTH) {
					if (!filePath.empty()) {
						logger::info("JSON property name too long at '{}' (max {} chars) in Baseline file: {}", path + "." + key, MAX_STRING_LENGTH, filePath);
					}
					return false;
				}

				// Recursively validate value
				std::string newPath = path.empty() ? key : path + "." + key;
				if (!ValidateJsonDataTypes(value, newPath, filePath)) {
					return false;
				}
			}
		} else if (jsonData.is_array()) {
			if (jsonData.size() > MAX_ARRAY_SIZE) {
				if (!filePath.empty()) {
					logger::info("JSON array too large at '{}' (max {} elements) in Baseline file: {}", path, MAX_ARRAY_SIZE, filePath);
				}
				return false;
			}

			for (size_t i = 0; i < jsonData.size(); ++i) {
				std::string newPath = path + "[" + std::to_string(i) + "]";
				if (!ValidateJsonDataTypes(jsonData[i], newPath, filePath)) {
					return false;
				}
			}
		} else if (jsonData.is_string()) {
			if (jsonData.get<std::string>().length() > MAX_STRING_LENGTH) {
				if (!filePath.empty()) {
					logger::info("JSON string too long at '{}' (max {} chars) in Baseline file: {}", path, MAX_STRING_LENGTH, filePath);
				}
				return false;
			}
		} else if (jsonData.is_number()) {
			double value = jsonData.get<double>();
			if (value > MAX_NUMERIC_VALUE || value < MIN_NUMERIC_VALUE) {
				if (!filePath.empty()) {
					logger::info("JSON numeric value out of range at '{}' ({} to {}) in Baseline file: {}", path, MIN_NUMERIC_VALUE, MAX_NUMERIC_VALUE, filePath);
				}
				return false;
			}

			// Check for NaN and infinity
			if (std::isnan(value) || std::isinf(value)) {
				if (!filePath.empty()) {
					logger::info("JSON numeric value is NaN or infinity at '{}' in Baseline file: {}", path, filePath);
				}
				return false;
			}
		}
		// Boolean and null values are always valid

		return true;

	} catch (const json::exception& e) {
		if (!filePath.empty()) {
			logger::info("JSON error during data type validation at '{}' in Baseline file {}: {}", path, filePath, e.what());
		}
		return false;
	} catch (const std::exception& e) {
		if (!filePath.empty()) {
			logger::info("Error during data type validation at '{}' in Baseline file {}: {}", path, filePath, e.what());
		}
		return false;
	}
}

json BaselineManager::SanitizeJsonData(const json& jsonData)
{
	try {
		json sanitized = jsonData;

		// Remove any potentially dangerous keys that start with system prefixes
		std::vector<std::string> dangerousPrefixes = { "__", "System", "Windows", "HKEY_", "C:\\" };

		std::function<void(json&)> sanitizeRecursive = [&](json& data) {
			if (data.is_object()) {
				auto it = data.begin();
				while (it != data.end()) {
					const std::string& key = it.key();

					// Check for dangerous key prefixes
					bool isDangerous = false;
					for (const auto& prefix : dangerousPrefixes) {
						if (key.length() >= prefix.length() &&
							key.substr(0, prefix.length()) == prefix) {
							isDangerous = true;
							break;
						}
					}

					if (isDangerous) {
						logger::info("Removing potentially dangerous key from file: {}", key);
						it = data.erase(it);
					} else {
						// Recursively sanitize the value
						sanitizeRecursive(it.value());
						++it;
					}
				}
			} else if (data.is_array()) {
				for (auto& element : data) {
					sanitizeRecursive(element);
				}
			} else if (data.is_string()) {
				std::string str = data.get<std::string>();

				// Remove any null characters or control characters except common ones
				str.erase(std::remove_if(str.begin(), str.end(), [](char c) {
					return (c >= 0 && c < 32 && c != '\t' && c != '\n' && c != '\r');
				}),
					str.end());

				// Truncate if too long
				if (str.length() > MAX_STRING_LENGTH) {
					str = str.substr(0, MAX_STRING_LENGTH);
					logger::info("Truncated overly long string in file data");
				}

				data = str;
			} else if (data.is_number()) {
				double value = data.get<double>();

				// Clamp to safe ranges
				if (value > MAX_NUMERIC_VALUE) {
					data = MAX_NUMERIC_VALUE;
					logger::info("Clamped numeric value to maximum allowed value");
				} else if (value < MIN_NUMERIC_VALUE) {
					data = MIN_NUMERIC_VALUE;
					logger::info("Clamped numeric value to minimum allowed value");
				} else if (std::isnan(value) || std::isinf(value)) {
					data = 0.0;
					logger::info("Replaced NaN/infinity numeric value with 0");
				}
			}
		};

		sanitizeRecursive(sanitized);
		return sanitized;

	} catch (const std::exception& e) {
		logger::info("Error during JSON sanitization: {}", e.what());
		return json::object();  // Return empty object on error
	}
}

void BaselineManager::MergeJson(json& target, const json& overlay)
{
	try {
		if (!overlay.is_object()) {
			logger::info("Attempting to merge non-object JSON data, skipping");
			return;
		}

		for (const auto& [key, value] : overlay.items()) {
			// Skip metadata fields during merge
			if (key.length() > 0 && key[0] == '_') {
				continue;
			}

			// Validate key length
			if (key.length() > MAX_STRING_LENGTH) {
				logger::info("Skipping merge of key '{}' - too long", key.substr(0, 50) + "...");
				continue;
			}

			try {
				if (target.contains(key) && target[key].is_object() && value.is_object()) {
					// Recursively merge objects
					MergeJson(target[key], value);
				} else {
					// Validate the value before assignment
					if (value.is_string()) {
						std::string strValue = value.get<std::string>();
						if (strValue.length() > MAX_STRING_LENGTH) {
							logger::info("Truncating string value for key '{}' - too long", key);
							target[key] = strValue.substr(0, MAX_STRING_LENGTH);
						} else {
							target[key] = value;
						}
					} else if (value.is_number()) {
						double numValue = value.get<double>();
						if (std::isnan(numValue) || std::isinf(numValue)) {
							logger::info("Skipping merge of key '{}' - invalid numeric value", key);
							continue;
						}
						if (numValue > MAX_NUMERIC_VALUE || numValue < MIN_NUMERIC_VALUE) {
							logger::info("Clamping numeric value for key '{}'", key);
							target[key] = std::clamp(numValue, MIN_NUMERIC_VALUE, MAX_NUMERIC_VALUE);
						} else {
							target[key] = value;
						}
					} else if (value.is_array()) {
						if (value.size() > MAX_ARRAY_SIZE) {
							logger::info("Truncating array for key '{}' - too large", key);
							json truncatedArray = json::array();
							for (size_t i = 0; i < std::min(value.size(), MAX_ARRAY_SIZE); ++i) {
								truncatedArray.push_back(value[i]);
							}
							target[key] = truncatedArray;
						} else {
							target[key] = value;
						}
					} else if (value.is_object()) {
						if (value.size() > MAX_OBJECT_SIZE) {
							logger::info("Skipping merge of key '{}' - object too large", key);
							continue;
						}
						target[key] = value;
					} else {
						// Boolean, null, or other safe types
						target[key] = value;
					}
				}
			} catch (const json::exception& e) {
				logger::info("JSON error merging key '{}': {}", key, e.what());
				continue;
			} catch (const std::exception& e) {
				logger::info("Error merging key '{}': {}", key, e.what());
				continue;
			}
		}
	} catch (const json::exception& e) {
		logger::info("JSON error during merge operation: {}", e.what());
	} catch (const std::exception& e) {
		logger::info("Error during merge operation: {}", e.what());
	}
}

bool BaselineManager::RejectsUnknownKeys(BaselineFile& file, const json& featureJson)
{
	// MergeJson would drop unknown keys without error; an empty target has no shape, so keep the last verdict.
	if (featureJson.is_object() && !featureJson.empty()) {
		const bool alreadyReported = !file.unknownKeys.empty();
		file.unknownKeys.clear();
		Util::Settings::CollectUnknownSettingKeys(file.values, featureJson, "", file.unknownKeys);
		if (!file.unknownKeys.empty() && !alreadyReported) {
			std::string keys;
			for (const auto& key : file.unknownKeys)
				keys += (keys.empty() ? "" : ", ") + key;
			logger::warn("Skipped Baseline file from {} to {}: unrecognized keys {}", file.packId, file.featureName, keys);
			ReportFailure(file.packId, file.featureName, "Unrecognized settings keys: " + keys, file.filePath);
		}
	}
	return !file.unknownKeys.empty();
}

void BaselineManager::ReportFailure(const std::string& packId, const std::string& featureName, const std::string& errorMessage,
	const std::filesystem::path& filePath)
{
	FeatureIssues::FeatureFileInfo fileInfo;
	fileInfo.featureName = featureName;

	std::error_code ec;
	if (!filePath.empty() && std::filesystem::exists(filePath, ec)) {
		fileInfo.hasINI = true;  // Using hasINI to indicate file exists (even though it's JSON)
		fileInfo.iniPath = filePath.string();

		try {
			auto timestamp = std::filesystem::last_write_time(filePath);
			fileInfo.latestTimestamp = timestamp;
			fileInfo.latestTimestampFile = filePath.string();

			// Format timestamp for display
			auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
				timestamp - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
			auto time_t = std::chrono::system_clock::to_time_t(sctp);
			std::stringstream ss;
			ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
			fileInfo.timestampDisplay = ss.str();
		} catch (const std::exception&) {
			fileInfo.timestampDisplay = "Unknown";
		}
	}

	const auto fullErrorMessage = "Baseline file for feature '" + featureName + "' from preset pack '" + packId + "' failed to load: " + errorMessage;

	FeatureIssues::AddFeatureIssue(
		packId + "_" + featureName,  // Combined name as shortName
		"unknown",                   // version
		fullErrorMessage,
		FeatureIssues::FeatureIssueInfo::IssueType::BASELINE_FAILED,
		fileInfo);
}

bool BaselineManager::IsApplicable(const BaselineFile& info) const
{
	const auto* feature = Feature::FindFeatureByShortName(info.featureName);
	return feature && feature->UsesMainSettings();
}

bool BaselineManager::IsValidDocument(const json& document, const std::filesystem::path& filePath)
{
	const auto path = filePath.string();
	return ValidateFormat(document, path) && ValidateJsonDataTypes(document, "", path);
}

std::filesystem::path BaselineManager::GetExportDestination(const std::string& packName, const std::string& featureName) const
{
	const auto safeName = Util::FileHelpers::SanitizeFileName(packName);
	if (safeName.empty()) {
		return {};
	}
	// Resolves through the Data (VFS) path so new files land in the MO2 overwrite folder.
	return Util::PathHelpers::GetUnifiedPackPath(safeName) / UnifiedPresetCatalog::kBaselineSubdir / std::format("{}.json", featureName);
}

std::optional<json> BaselineManager::ReadExportTarget(const std::filesystem::path& destination)
{
	std::error_code ec;
	if (!std::filesystem::exists(destination, ec)) {
		return json::object();
	}
	json document;
	if (!ReadBaselineDocument(destination, document)) {
		return std::nullopt;
	}
	return document;
}

bool BaselineManager::ExportSettings(const std::string& packName, const std::string& featureName,
	std::span<const std::string> settingPaths, const json& featureSettings, std::span<const std::string> removePaths)
{
	const auto destination = GetExportDestination(packName, featureName);
	auto* feature = Feature::FindFeatureByShortName(featureName);
	if (destination.empty() || (settingPaths.empty() && removePaths.empty()) || !feature || !feature->UsesMainSettings()) {
		return false;
	}

	const auto available = Util::Settings::GetExportSettings(featureName, featureSettings);
	for (const auto& paths : { settingPaths, removePaths }) {
		for (const auto& path : paths) {
			if (std::ranges::find(available, path, &Util::Settings::ExportSetting::path) == available.end()) {
				logger::error("Cannot export unknown setting '{}' of {}", path, featureName);
				return false;
			}
		}
	}

	const auto selected = Util::Settings::SelectSettingPaths(featureSettings, { settingPaths.begin(), settingPaths.end() });
	if (selected.empty() && !settingPaths.empty()) {
		return false;
	}

	// Merge into an existing file of the same name so repeated exports accumulate rather than truncate,
	// reading it raw so any _metadata the author wrote survives.
	auto document = ReadExportTarget(destination);
	if (!document) {
		logger::error("Refusing to overwrite unreadable Baseline file {}", destination.string());
		return false;
	}
	for (const auto& path : removePaths) {
		EraseSettingPath(*document, json::json_pointer(path));
	}
	document->update(selected, true);

	if (!IsValidDocument(*document, destination)) {
		return false;
	}

	const auto packRoot = destination.parent_path().parent_path();
	Util::FileHelpers::EnsureDirectoryExists(destination.parent_path());
	if (!UnifiedPresetCatalog::EnsureBaselineManifest(packRoot, packName)) {
		return false;
	}

	// Atomic so a failed export cannot leave a pack's Baseline file truncated.
	if (!Util::FileHelpers::WriteJsonAtomically(destination, *document, kBaselineJsonIndent, "Baseline file")) {
		return false;
	}

	logger::info("Exported {} setting(s) of {} to {}, removed {}", settingPaths.size(), featureName, destination.string(), removePaths.size());
	Refresh();
	return true;
}
