#ifndef ASSETS_H
#define ASSETS_H

#include <string>
#include <functional>
#include <memory>

#include <cJSON.h>
#include <esp_partition.h>
#include <model_path.h>
#include <map>
#include <string>

#include <spi_flash_mmap.h>

/**
 * @file assets.h
 * @brief assets Flash 分区下载、校验、映射和资源查找服务。
 */

/**
 * @brief 单个资源在 assets.bin 数据区中的位置。
 */
struct Asset {
    size_t size;
    size_t offset;
};

/**
 * @brief 全局资源分区管理器。
 *
 * assets.bin 包含索引 JSON、WakeNet 模型、字体和表情图片。初始化时 mmap 分区，
 * 校验索引及数据后将资源直接以指针暴露，避免复制大文件到 RAM。
 */
class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    /**
     * @brief 下载完整 assets.bin 到资源分区。
     * @param url 文件地址。
     * @param progress_callback 百分比和字节/秒回调。
     */
    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    /**
     * @brief 校验并应用资源、加载 SR 模型和主题。
     * @param refresh_display_theme 是否立即刷新显示主题。
     */
    bool Apply(bool refresh_display_theme = true);
    /**
     * @brief 按索引名称取得 mmap 数据。
     * @param ptr 输出首地址。
     * @param size 输出字节数。
     */
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    /**
     * @brief 查找 assets 分区并让策略完成 mmap 和索引校验。
     */
    bool InitializePartition();
    /**
     * @brief 卸载 SR 模型并解除 Flash mmap。
     */
    void UnApplyPartition();
    /**
     * @brief 查找分区表中 label=assets 的数据分区。
     */
    static bool FindPartition(Assets* assets);
    /**
     * @brief 从索引加载 srmodels.bin；root 可复用已解析的索引 JSON。
     */
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr);
  
    /**
     * @brief 不同 UI/资源格式的策略接口。
     */
    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets, bool refresh_display_theme = true) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) = 0;
    };
    
    class MmapStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    private:
        /**
         * @brief 计算资源索引使用的校验和。
         * @param data 数据首地址。
         * @param length 字节数。
         */
        static uint32_t CalculateChecksum(const char* data, uint32_t length);
        std::map<std::string, Asset> assets_;
        esp_partition_mmap_handle_t mmap_handle_ = 0;
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };
    
    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
