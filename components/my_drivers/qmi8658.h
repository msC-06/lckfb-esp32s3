#ifndef __QMI8658_h
#define __QMI8658_h
#include "bsp/bsp_iic.h"
#include "esp_err.h"
#include "esp_log.h"

#define  QMI8658_SENSOR_ADDR       0x6A 

enum qmi8658_reg
{
    QMI8658_WHO_AM_I,
    QMI8658_REVISION_ID,
    QMI8658_CTRL1,
    QMI8658_CTRL2,
    QMI8658_CTRL3,
    QMI8658_CTRL4,
    QMI8658_CTRL5,
    QMI8658_CTRL6,
    QMI8658_CTRL7,
    QMI8658_CTRL8,
    QMI8658_CTRL9,
    QMI8658_CATL1_L,
    QMI8658_CATL1_H,
    QMI8658_CATL2_L,
    QMI8658_CATL2_H,
    QMI8658_CATL3_L,
    QMI8658_CATL3_H,
    QMI8658_CATL4_L,
    QMI8658_CATL4_H,
    QMI8658_FIFO_WTM_TH,
    QMI8658_FIFO_CTRL,
    QMI8658_FIFO_SMPL_CNT,
    QMI8658_FIFO_STATUS,
    QMI8658_FIFO_DATA,
    QMI8658_STATUSINT = 45,
    QMI8658_STATUS0,
    QMI8658_STATUS1,
    QMI8658_TIMESTAMP_LOW,
    QMI8658_TIMESTAMP_MID,
    QMI8658_TIMESTAMP_HIGH,
    QMI8658_TEMP_L,
    QMI8658_TEMP_H,
    QMI8658_AX_L,
    QMI8658_AX_H,
    QMI8658_AY_L,
    QMI8658_AY_H,
    QMI8658_AZ_L,
    QMI8658_AZ_H,
    QMI8658_GX_L,
    QMI8658_GX_H,
    QMI8658_GY_L,
    QMI8658_GY_H,
    QMI8658_GZ_L,
    QMI8658_GZ_H,
    QMI8658_COD_STATUS = 70,
    QMI8658_dQW_L = 73,
    QMI8658_dQW_H,
    QMI8658_dQX_L,
    QMI8658_dQX_H,
    QMI8658_dQY_L,
    QMI8658_dQY_H,
    QMI8658_dQZ_L,
    QMI8658_dQZ_H,
    QMI8658_dVX_L,
    QMI8658_dVX_H,
    QMI8658_dVY_L,
    QMI8658_dVY_H,
    QMI8658_dVZ_L,
    QMI8658_dVZ_H,
    QMI8658_TAP_STATUS = 89,
    QMI8658_STEP_CNT_LOW,
    QMI8658_STEP_CNT_MIDL,
    QMI8658_STEP_CNT_HIGH,
    QMI8658_RESET = 96
};

typedef struct {
    float acc_x, acc_y, acc_z;
    float gyro_x, gyro_y, gyro_z;
    float AngleX, AngleY, AngleZ;  // 褰撳墠鍊捐锛圧oll, Pitch, Yaw锛?
    float last_roll;                // 涓婁竴娆oll瑙掑害锛堢敤浜庝簰琛ユ护娉級
    float last_pitch;               // 涓婁竴娆itch瑙掑害锛堢敤浜庝簰琛ユ护娉級
    float last_yaw;                 // 涓婁竴娆aw瑙掑害锛堢敤浜庨檧铻轰华绉垎锛?
} t_sQMI8658;

esp_err_t qmi8658_register_read(uint8_t reg_addr, uint8_t *data, size_t len);
esp_err_t qmi8658_register_write_byte(uint8_t reg_addr, uint8_t data);
esp_err_t qmi8658_init(void);
void qmi8658_close(void);
void qmi8658_Read_AccAndGry(t_sQMI8658 *p);
void qmi8658_fetch_angleFromAcc(t_sQMI8658 *p);
uint8_t qmi8658_fetch_motion(void);

#endif // __QMI8658_h