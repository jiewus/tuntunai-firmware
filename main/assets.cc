/**
 * @file assets.cc
 * @brief 资源分区映射、校验、下载和主题应用实现。
 */
#include "assets.h"
#include "board.h"
#include "display.h"
#include "app/application.h"
#include <spi_flash_mmap.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <utility>


#define TAG "Assets"
#define PARTITION_LABEL "assets"

struct mmap_assets_table {
    char asset_name[32];          /*!< Name of the asset */
    uint32_t asset_size;          /*!< Size of the asset */
    uint32_t asset_offset;        /*!< Offset of the asset */
    uint16_t asset_width;         /*!< Width of the asset */
    uint16_t asset_height;        /*!< Height of the asset */
};

/**
 * @brief 构造 Assets 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
Assets::Assets() {
    strategy_ = std::make_unique<Assets::MmapStrategy>();
    // Initialize the partition
    InitializePartition();
}

/**
 * @brief 析构 Assets 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
Assets::~Assets() {
    UnApplyPartition();
}

/**
 * @brief 查找分区表中 label=assets 的数据分区。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::FindPartition(Assets* assets) {
    assets->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (assets->partition_ == nullptr) {
        ESP_LOGI(TAG, "No assets partition found");
        return false;
    }
    return true;
}

/**
 * @brief 校验并应用资源、加载 SR 模型和主题。
 * @param refresh_display_theme 是否立即刷新显示主题。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::Apply(bool refresh_display_theme) {
    return strategy_ ? strategy_->Apply(this, refresh_display_theme) : false;
}

/**
 * @brief 查找 assets 分区并让策略完成 mmap 和索引校验。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::InitializePartition() {
    return strategy_ ? strategy_->InitializePartition(this) : false;
}

/**
 * @brief 卸载 SR 模型并解除 Flash mmap。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Assets::UnApplyPartition() {
    if (strategy_) {
        strategy_->UnApplyPartition(this);
    }
}

/**
 * @brief 按索引名称取得 mmap 数据。
 * @param ptr 输出首地址。
 * @param size 输出字节数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    return strategy_ ? strategy_->GetAssetData(this, name, ptr, size) : false;
}

/**
 * @brief 从索引加载 srmodels.bin；root 可复用已解析的索引 JSON。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::LoadSrmodelsFromIndex(Assets* assets, cJSON* root) {
    void* ptr = nullptr;
    size_t size = 0;
    bool need_delete_root = false;

    // If root is not provided, parse index.json
    if (root == nullptr) {
        if (!assets->GetAssetData("index.json", ptr, size)) {
            ESP_LOGE(TAG, "The index.json file is not found");
            return false;
        }

        root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
        if (root == nullptr) {
            ESP_LOGE(TAG, "The index.json file is not valid");
            return false;
        }
        need_delete_root = true;
    }

    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
        std::string srmodels_file = srmodels->valuestring;
        if (assets->GetAssetData(srmodels_file, ptr, size)) {
            if (assets->models_list_ != nullptr) {
                esp_srmodel_deinit(assets->models_list_);
                assets->models_list_ = nullptr;
            }
            assets->models_list_ = srmodel_load(static_cast<uint8_t*>(ptr));
            if (assets->models_list_ != nullptr) {
                auto& app = Application::GetInstance();
                app.GetAudioService().SetModelsList(assets->models_list_);
                if (need_delete_root) {
                    cJSON_Delete(root);
                }
                return true;
            } else {
                ESP_LOGE(TAG, "Failed to load srmodels.bin");
            }
        } else {
            ESP_LOGE(TAG, "The srmodels file %s is not found", srmodels_file.c_str());
        }
    }

    if (need_delete_root) {
        cJSON_Delete(root);
    }
    return false;
}

/**
 * @brief 计算资源索引使用的校验和。
 * @param data 数据首地址。
 * @param length 字节数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
uint32_t Assets::MmapStrategy::CalculateChecksum(const char* data, uint32_t length) {
    uint32_t checksum = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < length; i++) {
        checksum += bytes[i];
    }
    return checksum & 0xFFFF;
}

/**
 * @brief 查找 assets 分区并让策略完成 mmap 和索引校验。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::MmapStrategy::InitializePartition(Assets* assets) {
    constexpr size_t kHeaderSize = 12;
    UnApplyPartition(assets);

    if (!Assets::FindPartition(assets)) {
        return false;
    }
    if (assets->partition_->size < kHeaderSize) {
        ESP_LOGE(TAG, "Assets partition is smaller than the header");
        return false;
    }

    int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    uint32_t storage_size = free_pages * 64 * 1024;
    ESP_LOGI(TAG, "The storage free size is %ld KB", storage_size / 1024);
    ESP_LOGI(TAG, "The partition size is %ld KB", assets->partition_->size / 1024);
    if (storage_size < assets->partition_->size) {
        ESP_LOGE(TAG, "The free size %ld KB is less than assets partition required %ld KB", storage_size / 1024, assets->partition_->size / 1024);
        return false;
    }

    esp_err_t err = esp_partition_mmap(assets->partition_, 0, assets->partition_->size, ESP_PARTITION_MMAP_DATA, (const void**)&mmap_root_, &mmap_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap assets partition: %s", esp_err_to_name(err));
        return false;
    }

    const auto fail_validation = [this, assets]() {
        UnApplyPartition(assets);
        return false;
    };

    uint32_t stored_files = 0;
    uint32_t stored_chksum = 0;
    uint32_t stored_len = 0;
    memcpy(&stored_files, mmap_root_, sizeof(stored_files));
    memcpy(&stored_chksum, mmap_root_ + 4, sizeof(stored_chksum));
    memcpy(&stored_len, mmap_root_ + 8, sizeof(stored_len));

    if (stored_len > assets->partition_->size - kHeaderSize) {
        ESP_LOGE(TAG, "The stored length exceeds the assets partition");
        return fail_validation();
    }
    if (stored_files > stored_len / sizeof(mmap_assets_table)) {
        ESP_LOGE(TAG, "The assets file table exceeds the stored data length");
        return fail_validation();
    }

    const size_t table_size = static_cast<size_t>(stored_files) * sizeof(mmap_assets_table);
    const size_t data_offset = kHeaderSize + table_size;
    const size_t data_size = stored_len - table_size;

    auto start_time = esp_timer_get_time();
    uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + kHeaderSize, stored_len);
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "The checksum calculation time is %d ms", int((end_time - start_time) / 1000));

    if (calculated_checksum != stored_chksum) {
        ESP_LOGE(TAG, "The calculated checksum (0x%lx) does not match the stored checksum (0x%lx)", calculated_checksum, stored_chksum);
        return fail_validation();
    }

    std::map<std::string, Asset> validated_assets;
    for (uint32_t i = 0; i < stored_files; i++) {
        mmap_assets_table item{};
        memcpy(&item, mmap_root_ + kHeaderSize + i * sizeof(mmap_assets_table), sizeof(item));
        const void* name_end = memchr(item.asset_name, '\0', sizeof(item.asset_name));
        if (name_end == nullptr || item.asset_name[0] == '\0') {
            ESP_LOGE(TAG, "Asset index %lu has an invalid name", static_cast<unsigned long>(i));
            return fail_validation();
        }

        const size_t relative_offset = static_cast<size_t>(item.asset_offset);
        const size_t asset_size = static_cast<size_t>(item.asset_size);
        if (relative_offset > data_size || data_size - relative_offset < 2
            || asset_size > data_size - relative_offset - 2) {
            ESP_LOGE(TAG, "Asset index %lu exceeds the stored data range",
                     static_cast<unsigned long>(i));
            return fail_validation();
        }

        const std::string name(item.asset_name);
        auto asset = Asset{
            .size = asset_size,
            .offset = data_offset + relative_offset
        };
        if (!validated_assets.emplace(name, asset).second) {
            ESP_LOGE(TAG, "Duplicate asset name: %s", name.c_str());
            return fail_validation();
        }
    }

    assets_ = std::move(validated_assets);
    checksum_valid_ = true;
    assets->partition_valid_ = true;
    return true;
}

/**
 * @brief 卸载 SR 模型并解除 Flash mmap。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Assets::MmapStrategy::UnApplyPartition(Assets* assets) {
    if (mmap_handle_ != 0) {
        esp_partition_munmap(mmap_handle_);
        mmap_handle_ = 0;
        mmap_root_ = nullptr;
    }
    checksum_valid_ = false;
    assets_.clear();
    assets->partition_valid_ = false;
}

/**
 * @brief 按索引名称取得 mmap 数据。
 * @param ptr 输出首地址。
 * @param size 输出字节数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::MmapStrategy::GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) {
    ptr = nullptr;
    size = 0;
    if (!assets->partition_valid_ || !checksum_valid_ || mmap_root_ == nullptr) {
        return false;
    }
    auto asset = assets_.find(name);
    if (asset == assets_.end()) {
        return false;
    }
    if (asset->second.offset > assets->partition_->size
        || assets->partition_->size - asset->second.offset < 2
        || asset->second.size > assets->partition_->size - asset->second.offset - 2) {
        ESP_LOGE(TAG, "The asset %s exceeds the mapped partition", name.c_str());
        return false;
    }
    auto data = (const char*)(mmap_root_ + asset->second.offset);
    if (data[0] != 'Z' || data[1] != 'Z') {
        ESP_LOGE(TAG, "The asset %s is not valid with magic %02x%02x", name.c_str(), data[0], data[1]);
        return false;
    }

    ptr = static_cast<void*>(const_cast<char*>(data + 2));
    size = asset->second.size;
    return true;
}

/**
 * @brief 校验并应用资源、加载 SR 模型和主题。
 * @param refresh_display_theme 是否立即刷新显示主题。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::MmapStrategy::Apply(Assets* assets, bool refresh_display_theme) {
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets->GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "The index.json file is not found");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "The index.json file is not valid");
        return false;
    }

    cJSON* version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version) && version->valuedouble > 1) {
        ESP_LOGE(TAG, "The assets version %d is not supported, please upgrade the firmware", version->valueint);
        cJSON_Delete(root);
        return false;
    }

    Assets::LoadSrmodelsFromIndex(assets, root);
    const bool display_applied = Board::GetInstance().GetDisplay()->ApplyAssets(
        *assets, root, refresh_display_theme);
    cJSON_Delete(root);
    return display_applied;
}

/**
 * @brief 下载完整 assets.bin 到资源分区。
 * @param url 文件地址。
 * @param progress_callback 百分比和字节/秒回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Assets::Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback) {
    ESP_LOGI(TAG, "Downloading new version of assets");

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get assets, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();

    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    if (content_length > partition_->size) {
        ESP_LOGE(TAG, "Assets file size (%u) is larger than partition size (%lu)",
                 content_length, partition_->size);
        return false;
    }

    constexpr size_t HEADER_SIZE = 12;

    if (content_length < HEADER_SIZE) {
        ESP_LOGE(TAG, "Content length (%u) is smaller than header size (%u)",
                 content_length, HEADER_SIZE);
        return false;
    }

    const size_t SECTOR_SIZE = esp_partition_get_main_flash_sector_size();
    using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;

    BufferPtr buffer(
        static_cast<char *>(heap_caps_malloc(SECTOR_SIZE, MALLOC_CAP_INTERNAL)),
        &heap_caps_free);

    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    // Unapply the partition
    Board::GetInstance().GetDisplay()->ReleaseAssetsForReload();
    UnApplyPartition();

    size_t sectors_to_erase = (content_length + SECTOR_SIZE - 1) / SECTOR_SIZE;
    size_t total_erase_size = sectors_to_erase * SECTOR_SIZE;
    ESP_LOGI(TAG,
             "Sector size: %u, content length: %u, "
             "sectors to erase: %u, total erase size: %u",
             SECTOR_SIZE, content_length,
             sectors_to_erase, total_erase_size);

    size_t total_written = 0;
    size_t recent_written = 0;
    size_t current_sector = 0;

    int64_t last_calc_time = esp_timer_get_time();

    uint8_t header_buf[HEADER_SIZE];
    size_t header_collected = 0;
    bool success = false;
    while (true) {
        int ret = http->Read(buffer.get(), SECTOR_SIZE);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            break;
        }

        if (ret == 0) {
            // End of data
            success = true;
            break;
        }

        size_t buf_pos = 0;

        // Collect header
        if (header_collected < HEADER_SIZE) {
            size_t need = HEADER_SIZE - header_collected;
            size_t take = std::min(static_cast<size_t>(ret), need);
            memcpy(header_buf + header_collected, buffer.get(), take);
            header_collected += take;
            buf_pos += take;
        }

        // Write payload
        if ((size_t)ret > buf_pos) {
            size_t write_len = (size_t)ret - buf_pos;
            size_t write_end_offset = HEADER_SIZE + total_written + write_len;
            size_t needed_sectors = (write_end_offset + SECTOR_SIZE - 1) / SECTOR_SIZE;
            // Erase sectors
            bool erase_failed = false;
            while (current_sector < needed_sectors) {
                size_t sector_start = current_sector * SECTOR_SIZE;
                size_t sector_end = sector_start + SECTOR_SIZE;
                if (sector_end > partition_->size) {
                    ESP_LOGE(TAG, "Sector end (%u) exceeds partition size (%lu)",
                             sector_end, partition_->size);
                    erase_failed = true;
                    break;
                }
                ESP_LOGD(TAG, "Erasing sector %u (offset: %u, size: %u)",
                         current_sector, sector_start, SECTOR_SIZE);
                esp_err_t err = esp_partition_erase_range(partition_, sector_start, SECTOR_SIZE);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to erase sector %u at offset %u: %s",
                             current_sector, sector_start, esp_err_to_name(err));
                    erase_failed = true;
                    break;
                }
                current_sector++;
            }

            if (erase_failed) {
                break;
            }

            esp_err_t err = esp_partition_write(partition_, HEADER_SIZE + total_written,
                                                buffer.get() + buf_pos, write_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write to assets partition at offset %u: %s",
                         (unsigned int)(HEADER_SIZE + total_written), esp_err_to_name(err));
                break;
            }

            total_written += write_len;
            recent_written += write_len;
        }

        // Calculate progress
        if (esp_timer_get_time() - last_calc_time >= 1000000 || (header_collected + total_written) == content_length) {
            size_t progress = (header_collected + total_written) * 100 / content_length;
            size_t speed = recent_written;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %u B/s, Sectors erased: %u",
                     progress,
                     (unsigned int)(header_collected + total_written),
                     (unsigned int)content_length,
                     (unsigned int)speed,
                     (unsigned int)current_sector);

            if (progress_callback) {
                progress_callback(progress, speed);
            }
            last_calc_time = esp_timer_get_time();
            recent_written = 0;
        }
    }

    // Check if the downloaded size matches the expected size
    if (success && (header_collected + total_written != content_length)) {
        ESP_LOGE(TAG, "Downloaded size (%u) does not match expected size (%u)",
                 (unsigned int)(header_collected + total_written), (unsigned int)content_length);
        success = false;
    }

    // Write header
    if (success) {
        esp_err_t err = esp_partition_write(partition_, 0, header_buf, HEADER_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write assets header to partition: %s", esp_err_to_name(err));
            success = false;
        }
    }

    if (!success) {
        ESP_LOGE(TAG, "Assets download failed");
        return false;
    }

    ESP_LOGI(TAG, "Header written, assets download completed, total written: %u bytes, total sectors erased: %u",
             (unsigned int)(header_collected + total_written), (unsigned int)current_sector);

    // Re-initialize the assets partition
    if (!InitializePartition()) {
        ESP_LOGE(TAG, "Failed to re-initialize assets partition");
        return false;
    }

    return true;
}
