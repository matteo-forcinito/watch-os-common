/* QMI8658 raw six-axis mode. QST register map, no FIFO/interrupt setup.
 * Save configuration before touching it and restore on every exit path. */
#include "qmi8658.h"
#include <stddef.h>

static const uint8_t s_registers[] = {0x02, 0x03, 0x04, 0x06, 0x08};
static esp_err_t read_reg(qmi8658_t *s, uint8_t reg, uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(s->device, &reg, 1, data, size, 50);
}
static esp_err_t write_reg(qmi8658_t *s, uint8_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(s->device, bytes, sizeof(bytes), 50);
}
esp_err_t qmi8658_stop(qmi8658_t *s)
{
    if (s == NULL || s->device == NULL)
    {
        return ESP_OK;
    }
    esp_err_t result = ESP_OK;
    if (s->restore)
    {
        result = write_reg(s, 0x08, 0); /* Disable before changing rates. */
        for (size_t i = 0; i < sizeof(s_registers); i++)
        {
            esp_err_t err = write_reg(s, s_registers[i], s->saved[i]);
            if (result == ESP_OK)
            {
                result = err;
            }
        }
    }
    esp_err_t err = i2c_master_bus_rm_device(s->device);
    if (err == ESP_OK)
    {
        s->device = NULL;
        s->restore = false;
    }
    return result == ESP_OK ? err : result;
}
esp_err_t qmi8658_start(qmi8658_t *s, i2c_master_bus_handle_t bus, uint8_t address)
{
    if (s == NULL || bus == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep a failed removal reachable, and retry cleanup before reopening. */
    if (s->device != NULL)
    {
        esp_err_t cleanup = qmi8658_stop(s);
        if (cleanup != ESP_OK)
        {
            return cleanup;
        }
    }
    const i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address, .scl_speed_hz = 400000};
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &s->device);
    uint8_t id = 0;
    if (err == ESP_OK)
    {
        err = read_reg(s, 0, &id, 1);
        if (err == ESP_OK && id != 0x05)
        {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_registers); i++)
    {
        err = read_reg(s, s_registers[i], &s->saved[i], 1);
    }
    /* Do not take over a sensor already used by another board feature. */
    if (err == ESP_OK && (s->saved[4] & 0x0F) != 0)
    {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK)
    {
        s->restore = true;
        err = write_reg(s, 0x02, (s->saved[0] | 0x40) & ~0x01);
    }
    const uint8_t values[] = {0x18, 0x68, 0x00, 0x83};
    const uint8_t regs[] = {0x03, 0x04, 0x06, 0x08};
    /* +/-4 g, +/-1024 dps, 31.25 Hz; synchronized output locking. */
    for (size_t i = 0; err == ESP_OK && i < sizeof(regs); i++)
    {
        err = write_reg(s, regs[i], values[i]);
    }
    if (err != ESP_OK)
    {
        qmi8658_stop(s);
    }
    return err;
}
static int16_t signed_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
esp_err_t qmi8658_read(qmi8658_t *s, imu_sample_t *sample)
{
    if (s == NULL || s->device == NULL || sample == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t status = 0, data[14];
    esp_err_t err = read_reg(s, 0x2E, &status, 1);
    if (err != ESP_OK || (status & 0x03) != 0x03)
    {
        return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
    }
    /* STATUS0 locks the synchronized outputs; reading through GZ_H unlocks. */
    err = read_reg(s, 0x33, data, sizeof(data));
    if (err != ESP_OK)
    {
        return err;
    }
    sample->temperature_c = signed_le(data) / 256.0f;
    for (int axis = 0; axis < 3; axis++)
    {
        sample->acceleration_g[axis] = signed_le(data + 2 + axis * 2) / 8192.0f;
        sample->angular_dps[axis] = signed_le(data + 8 + axis * 2) / 32.0f;
    }
    return ESP_OK;
}
