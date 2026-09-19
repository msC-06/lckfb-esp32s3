#include "qmi8658.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "qmi8658";

// 加速度计灵敏度 (LSB/g) - 取决于 CTRL2 中设置的量程
// 这里配置为 ±4g 量程: 8192 LSB/g
#define QMI8658_ACC_SENSITIVITY   8192.0f

// 陀螺仪灵敏度 (LSB/°/s) - 取决于 CTRL3 中设置的量程
// 这里配置为 ±512dps 量程: 64 LSB/°/s
#define QMI8658_GYRO_SENSITIVITY  64.0f

#define QMI8658_WHO_AM_I_EXPECTED 0x05  // QMI8658 的 WHO_AM_I 寄存器期望值

// 静态变量保存设备状态
static uint8_t s_device_addr = QMI8658_SENSOR_ADDR;
static uint8_t s_acc_range = 0;   // 0: ±2g, 1: ±4g, 2: ±8g, 3: ±16g
static uint8_t s_gyro_range = 0;  // 0: ±16dps, 1: ±32dps ... 7: ±2048dps

esp_err_t qmi8658_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return bsp_i2c_read_reg(s_device_addr, reg_addr, data, len);
}

esp_err_t qmi8658_register_write_byte(uint8_t reg_addr, uint8_t data)
{
    return bsp_i2c_write_reg(s_device_addr, reg_addr, &data, 1);
}

esp_err_t qmi8658_init(void)
{
    esp_err_t ret;
    uint8_t id = 0;
    int retry = 3;

    // 1. 读取 WHO_AM_I 确认设备存在
    while (retry--) {
        ret = qmi8658_register_read(QMI8658_WHO_AM_I, &id, 1);
        if (ret == ESP_OK && id == QMI8658_WHO_AM_I_EXPECTED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (id != QMI8658_WHO_AM_I_EXPECTED) {
        ESP_LOGE(TAG, "QMI8658 not found, WHO_AM_I = 0x%02X (expected 0x%02X)", id, QMI8658_WHO_AM_I_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "QMI8658 detected, WHO_AM_I = 0x%02X", id);

    // 2. 软复位
    ret = qmi8658_register_write_byte(QMI8658_RESET, 0xB0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // 等待复位完成

    // 3. 配置 CTRL1: 地址自动递增 + 小端模式 (默认 0x40 已启用 AI)
    // 保持默认值即可，确保 bit6 (ADDR_AI) = 1
    ret = qmi8658_register_write_byte(QMI8658_CTRL1, 0x40);
    if (ret != ESP_OK) return ret;

    // 4. 配置 CTRL2: 加速度计设置
    // A_FS[6:4] = 001 (±4g), A_ODR[3:0] = 0011 (250Hz)
    s_acc_range = 1; // ±4g
    uint8_t ctrl2_val = (s_acc_range << 4) | 0x03;
    ret = qmi8658_register_write_byte(QMI8658_CTRL2, ctrl2_val);
    if (ret != ESP_OK) return ret;

    // 5. 配置 CTRL3: 陀螺仪设置
    // G_FS[6:4] = 100 (±512dps), G_ODR[3:0] = 0011 (250Hz)
    s_gyro_range = 4; // ±512dps
    uint8_t ctrl3_val = (s_gyro_range << 4) | 0x03;
    ret = qmi8658_register_write_byte(QMI8658_CTRL3, ctrl3_val);
    if (ret != ESP_OK) return ret;

    // 6. 配置 CTRL7: 使能加速度计和陀螺仪
    ret = qmi8658_register_write_byte(QMI8658_CTRL7, 0x03);
    if (ret != ESP_OK) return ret;

    // 7. 可选: 使能低通滤波器提高稳定性
    // A_LPF_EN=1, G_LPF_EN=1 (CTRL5)
    ret = qmi8658_register_write_byte(QMI8658_CTRL5, 0x11);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "QMI8658 initialized (ACC: ±4g, GYRO: ±512dps)");
    return ESP_OK;
}

void qmi8658_close(void)
{
    // 关闭传感器 (禁用加速度计和陀螺仪)
    qmi8658_register_write_byte(QMI8658_CTRL7, 0x00);
}

void qmi8658_Read_AccAndGry(t_sQMI8658 *p)
{
    if (p == NULL) return;

    uint8_t buf[12];
    // 从 AX_L (0x35) 开始连续读取 12 字节 (加速度 6 + 陀螺仪 6)
    esp_err_t ret = qmi8658_register_read(QMI8658_AX_L, buf, 12);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read sensor data failed: %s", esp_err_to_name(ret));
        return;
    }

    // 拼接有符号 16 位数据
    int16_t raw_acc_x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t raw_acc_y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t raw_acc_z = (int16_t)((buf[5] << 8) | buf[4]);
    int16_t raw_gyro_x = (int16_t)((buf[7] << 8) | buf[6]);
    int16_t raw_gyro_y = (int16_t)((buf[9] << 8) | buf[8]);
    int16_t raw_gyro_z = (int16_t)((buf[11] << 8) | buf[10]);

    // 转换为物理单位 (g 和 °/s)
    p->acc_x = (float)raw_acc_x / QMI8658_ACC_SENSITIVITY;
    p->acc_y = (float)raw_acc_y / QMI8658_ACC_SENSITIVITY;
    p->acc_z = (float)raw_acc_z / QMI8658_ACC_SENSITIVITY;

    p->gyro_x = (float)raw_gyro_x / QMI8658_GYRO_SENSITIVITY;
    p->gyro_y = (float)raw_gyro_y / QMI8658_GYRO_SENSITIVITY;
    p->gyro_z = (float)raw_gyro_z / QMI8658_GYRO_SENSITIVITY;
}

void qmi8658_fetch_angleFromAcc(t_sQMI8658 *p)
{
    if (p == NULL) return;

    // 使用加速度计计算 Roll 和 Pitch (Yaw 无法从加速度计获得)
    // Roll (绕 X 轴旋转)
    float roll = atan2f(p->acc_y, p->acc_z) * 180.0f / M_PI;

    // Pitch (绕 Y 轴旋转)
    float pitch = atan2f(-p->acc_x, sqrtf(p->acc_y * p->acc_y + p->acc_z * p->acc_z)) * 180.0f / M_PI;

    // 互补滤波: 融合加速度计角度和陀螺仪积分
    // alpha = 0.98 表示主要信任陀螺仪短期数据，加速度计修正长期漂移
    const float alpha = 0.98f;
    const float dt = 0.01f; // 假设采样周期 100Hz (10ms)

    // 积分陀螺仪得到角度变化
    float gyro_roll_rate = p->gyro_x;
    float gyro_pitch_rate = p->gyro_y;

    // 互补滤波计算
    p->AngleX = alpha * (p->last_roll + gyro_roll_rate * dt) + (1.0f - alpha) * roll;
    p->AngleY = alpha * (p->last_pitch + gyro_pitch_rate * dt) + (1.0f - alpha) * pitch;

    // 保存本次角度用于下次计算
    p->last_roll = p->AngleX;
    p->last_pitch = p->AngleY;

    // Yaw 角: 仅用陀螺仪积分 (无磁力计无法绝对定位)
    p->AngleZ = p->last_yaw + p->gyro_z * dt;
    p->last_yaw = p->AngleZ;
}

uint8_t qmi8658_fetch_motion(void)
{
    uint8_t status = 0;
    esp_err_t ret = qmi8658_register_read(QMI8658_STATUSINT, &status, 1);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read STATUSINT failed: %s", esp_err_to_name(ret));
        return 0;
    }

    // STATUSINT bit3: AnyMotion 检测到
    // STATUSINT bit4: NoMotion 检测到
    // 这里返回原始状态的低 4 位供上层判断
    return status & 0x0F;
}