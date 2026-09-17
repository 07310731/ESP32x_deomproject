# cJSON（本地 vendoring）
# =====================
# ESP-IDF v5.2+ 不再把 cJSON 编进默认 components 树（v6.1 更是彻底拿掉），
# 改为通过 idf-component 注册表（espressif/cjson）按需拉取。本项目不希望走
# 网络拉依赖、也不希望 build 首次再额外多一个 managed_components/ 目录，所以
# 把 cJSON 1.7.18 的两个文件 vendor 成本地组件：
#
#   - cJSON.h  16 KB  MIT License（头里自带版权声明）
#   - cJSON.c  78 KB  MIT License
#
# 版本选择：1.7.18 是 DaveGamble/cJSON 在 v1.7.x 系列的最后一个 tag，也是当时
# ESP-IDF 旧版 bundled 用的最后一个版本，没有 CVE-2022-37434 那条（cJSON 已知
# 唯一的 CVE，影响 1.7.13 之前）。后续有安全补丁需要手动同步。
#
# 升级方法：从 https://github.com/DaveGamble/cJSON 拉新版同名两个文件直接覆盖
# 即可，API 在 v1.x 内保持兼容（本组件只用 Parse / Create* / Add*ToObject /
# PrintPreallocated / GetObjectItemCaseSensitive / IsString / Delete 这几个）。
idf_component_register(
    SRCS "cJSON.c"           # 全部实现都在这一个文件里
    INCLUDE_DIRS "."         # cJSON.h 跟 .c 在同一目录，include 直通
)