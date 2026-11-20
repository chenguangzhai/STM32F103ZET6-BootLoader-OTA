# 第三方组件声明

本仓库的 `Boot_Loader/Drivers/` 与 `Application/Drivers/` 来自芯片厂商与 CMSIS 项目，
**保持其原始授权，不受根目录 [MIT License](LICENSE) 覆盖**。

| 目录 | 授权 | 授权文件 |
| --- | --- | --- |
| `*/Drivers/CMSIS/`（核心） | Apache-2.0（ARM） | `Drivers/CMSIS/LICENSE.txt` |
| `*/Drivers/CMSIS/Device/ST/STM32F1xx/` | BSD-3-Clause（ST） | 该目录内 `LICENSE.txt` |
| `*/Drivers/STM32F1xx_HAL_Driver/` | BSD-3-Clause（ST） | 该目录内 `LICENSE.txt` |

## 分发时需要注意

- **BSD-3-Clause**：保留版权声明、条件列表与免责声明；
  不得用 ST（STMicroelectronics）的名称为衍生产品背书或促销。
- **Apache-2.0**：保留版权与授权声明，附带 `NOTICE` 文件（如原组件提供），
  并在改动过的文件上标注改动。
- 重新分发上述目录时，请连同各自的 `LICENSE.txt` 一起提供。

这里只是划清授权边界，**不是替 ST / ARM 重新授权**。
