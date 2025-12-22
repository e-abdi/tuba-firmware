
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>
#include "net_console.h"

/* Resolve MS5837 device by compatible, independent of node label. */
static const struct device *const i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
static uint8_t g_ms5837_addr = 0x76; /* Try 0x76 first, fallback to 0x77 */
static uint16_t g_prom[8];
static bool g_prom_ok = false;
static uint8_t g_model = 255; /* 0=30BA, 1=02BA, 255=unrecognised */

/* Console UART for nonblocking keypress checks */
static const struct device *const uart_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Check if user pressed 'q' (or 'Q') to quit */
static bool quit_requested(void)
{
    /* Prefer net console (WiFi) input: requires ENTER */
    char line[128];
    if (net_console_poll_line(line, sizeof(line), K_NO_WAIT)) {
        if ((line[0] == 'q' || line[0] == 'Q') && line[1] == '\0') return true;
    }
    if (!device_is_ready(uart_console)) return false;
    unsigned char c;
    int rc = uart_poll_in(uart_console, &c);
    return (rc == 0 && (c == 'q' || c == 'Q'));
}

/* CRC-4 calculation for MS5637/MS5837 PROM */
static uint8_t ms_crc4(uint16_t prom[8])
{
    uint16_t n_rem = 0;
    uint16_t n_prom[8];
    for (int i = 0; i < 8; i++) n_prom[i] = prom[i];
    n_prom[0] &= 0x0FFF; /* mask out top 4 bits (CRC nibble) */
    n_prom[7] = 0;       /* last word not used in CRC for MS5637/MS5837 */

    for (uint8_t i = 0; i < 16; i++) {
        if (i % 2 == 1) {
            n_rem ^= (uint16_t)(n_prom[i>>1] & 0x00FF);
        } else {
            n_rem ^= (uint16_t)(n_prom[i>>1] >> 8);
        }
        for (uint8_t n_bit = 8; n_bit > 0; n_bit--) {
            if (n_rem & 0x8000) {
                n_rem = (n_rem << 1) ^ 0x3000;
            } else {
                n_rem = (n_rem << 1);
            }
        }
    }
    n_rem = (n_rem >> 12) & 0x000F;
    return (uint8_t)(n_rem ^ 0x00);
}

/* Reset I2C bus state after failed operation */
static void ms5837_bus_recover(void)
{
    /* Reconfigure bus to known state */
    (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    k_msleep(10);
}

/* Load PROM coefficients from device */
static int ms5837_load_prom(void)
{
    if (g_prom_ok) return 0; /* Already loaded */
    
    app_printk("[External Pressure] Loading PROM...\r\n");
    
    /* Set bus to 100 kHz */
    (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    k_msleep(5);
    
    /* Soft reset to clear any prior state */
    uint8_t reset_cmd = 0x1E;
    int rc = i2c_write(i2c_dev, &reset_cmd, 1, g_ms5837_addr);
    app_printk("[External Pressure] Soft reset: %d\r\n", rc);
    k_msleep(10); /* Wait for reset */
    
    /* Read PROM (7 words × 2 bytes each) */
    for (int i = 0; i < 7; i++) {
        uint8_t cmd = 0xA0 + (i * 2);
        uint8_t buf[2] = {0};
        
        int retries = 3;
        int rc_retry = -1;
        
        while (retries > 0 && rc_retry != 0) {
            /* Write address */
            rc_retry = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
            if (rc_retry == 0) {
                /* Read data */
                rc_retry = i2c_read(i2c_dev, buf, 2, g_ms5837_addr);
            }
            
            if (rc_retry != 0) {
                app_printk("[External Pressure]   PROM[%d] attempt failed (%d), retrying...\r\n", i, rc_retry);
                ms5837_bus_recover();
                k_msleep(5);
                retries--;
            }
        }
        
        if (rc_retry != 0) {
            app_printk("[External Pressure] PROM[%d] failed after retries, aborting\r\n", i);
            ms5837_bus_recover();
            return -EIO;
        }
        
        g_prom[i] = ((uint16_t)buf[0] << 8) | buf[1];
        app_printk("[External Pressure]   PROM[%d] = 0x%04X (%u) [raw: 0x%02X 0x%02X]\r\n", 
                   i, g_prom[i], g_prom[i], buf[0], buf[1]);
        k_msleep(1);
    }
    
    g_prom[7] = 0; /* CRC not read */
    /* CRC check (MS5637/MS5837 style: top nibble of PROM[0]) */
    uint8_t crc_read = (uint8_t)((g_prom[0] & 0xF000) >> 12);
    uint8_t crc_calc = ms_crc4(g_prom);
    if (crc_calc != crc_read) {
        app_printk("[External Pressure] PROM CRC mismatch: read=%u calc=%u\r\n", crc_read, crc_calc);
        return -EIO;
    }

    /* Detect model by sensitivity (C1) threshold */
    if (g_prom[1] > 37000) {
        g_model = 1; /* 02BA */
    } else if (g_prom[1] < 26000 || g_prom[1] > 49000) {
        g_model = 255; /* unrecognised */
    } else {
        g_model = 0; /* 30BA */
    }

    g_prom_ok = true;
    app_printk("[External Pressure] PROM: C1=%u C2=%u C3=%u C4=%u C5=%u C6=%u\r\n",
               g_prom[1], g_prom[2], g_prom[3], g_prom[4], g_prom[5], g_prom[6]);
    app_printk("[External Pressure] PROM loaded OK (model=%s)\r\n", g_model==1?"02BA":(g_model==0?"30BA":"unknown"));
    return 0;
}

/* Initialize sensor: just detect presence, defer PROM load */
static int ms5837_probe(void)
{
    if (i2c_dev == NULL || !device_is_ready(i2c_dev)) {
        app_printk("[External Pressure] i2c0 not ready\r\n");
        return -ENODEV;
    }
    
    /* Auto-detect address: try 0x76 first, then 0x77 */
    for (int addr_try = 0; addr_try < 2; addr_try++) {
        uint8_t try_addr = (addr_try == 0) ? 0x76 : 0x77;
        
        /* Set bus to 100 kHz */
        (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
        k_msleep(5);
        
        app_printk("[External Pressure] Probing address 0x%02x...\r\n", try_addr);
        
        /* Probe: try to read one PROM word (0xA0) with retries */
        int retries = 2;
        int rc = -1;
        while (retries > 0 && rc != 0) {
            uint8_t probe_cmd = 0xA0;
            uint8_t probe_buf[2] = {0};
            rc = i2c_write_read(i2c_dev, try_addr, &probe_cmd, 1, probe_buf, 2);
            if (rc != 0) {
                app_printk("[External Pressure]   Probe attempt failed (%d)\r\n", rc);
                ms5837_bus_recover();
                k_msleep(5);
                retries--;
            }
        }
        
        if (rc == 0) {
            g_ms5837_addr = try_addr;
            app_printk("[External Pressure] MS5837 detected at 0x%02x\r\n", try_addr);
            g_prom_ok = false; /* Force reload on first use */
            return 0; /* Success; PROM will load on first streaming call */
        }
    }
    
    app_printk("[External Pressure] MS5837 not found\r\n");
    return -ENODEV;
}

void ms5837_stream_interactive(void)
{
    if (ms5837_probe() != 0) return;
    
    /* Load PROM on first call */
    if (ms5837_load_prom() != 0) {
        app_printk("[External Pressure] PROM load failed, cleaning up bus and aborting\r\n");
        ms5837_bus_recover();
        k_msleep(10);
        return;
    }
    
    app_printk("[External Pressure] streaming — press 'q' to return; 'b' to recalibrate baseline\r\n");

    uint64_t next = k_uptime_get();
    int error_count = 0;
    /* Baseline calibration: average ~10 seconds at start */
    bool baseline_ready = false;
    double baseline_sum = 0.0;
    int baseline_samples = 0;
    
    int sample_count = 0;
    for (;;) {
        int rc;
        
        /* Ensure bus is ready and set to 100 kHz before each measurement */
        (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
        k_msleep(2);
        
        /* Start D1 conversion (pressure) OSR=8192: command 0x4A */
        uint8_t cmd = 0x4A;
        rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
        if (rc != 0) {
            app_printk("[External Pressure] D1 start failed (%d)\r\n", rc);
            error_count++;
            if (error_count > 3) break;
            k_msleep(100);
            continue;
        }
        
        k_msleep(20); /* OSR=8192 needs ~20ms */
        
        /* Read ADC result: 0x00 returns 3 bytes (separate write+read) */
        cmd = 0x00;
        uint8_t d1[3] = {0};
        rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
        if (rc == 0) rc = i2c_read(i2c_dev, d1, 3, g_ms5837_addr);
        if (rc != 0) {
            app_printk("[External Pressure] D1 read failed (%d)\r\n", rc);
            error_count++;
            if (error_count > 3) break;
            k_msleep(100);
            continue;
        }
        
        uint32_t D1 = ((uint32_t)d1[0]<<16)|((uint32_t)d1[1]<<8)|d1[2];
        
        k_msleep(2);
        
        /* Start D2 conversion (temperature) OSR=8192: command 0x5A */
        cmd = 0x5A;
        rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
        if (rc != 0) {
            app_printk("[External Pressure] D2 start failed (%d)\r\n", rc);
            error_count++;
            if (error_count > 3) break;
            k_msleep(100);
            continue;
        }
        
        k_msleep(20);
        
        cmd = 0x00;
        uint8_t d2[3] = {0};
        rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
        if (rc == 0) rc = i2c_read(i2c_dev, d2, 3, g_ms5837_addr);
        if (rc != 0) {
            app_printk("[External Pressure] D2 read failed (%d)\r\n", rc);
            error_count++;
            if (error_count > 3) break;
            k_msleep(100);
            continue;
        }
        
        uint32_t D2 = ((uint32_t)d2[0]<<16)|((uint32_t)d2[1]<<8)|d2[2];

        /* Basic sanity check on raw ADC values */
        if (D1 < 2000000 || D1 > 16777215 || D2 < 3000000 || D2 > 16777215) {
            app_printk("[External Pressure] anomaly: D1=%u D2=%u → resetting sensor\r\n", (unsigned)D1, (unsigned)D2);
            /* Soft reset and short recover */
            uint8_t rst = 0x1E;
            (void)i2c_write(i2c_dev, &rst, 1, g_ms5837_addr);
            k_msleep(10);
            ms5837_bus_recover();
            k_msleep(10);
            /* Force PROM reload next loop */
            g_prom_ok = false;
            if (ms5837_load_prom() != 0) {
                app_printk("[External Pressure] PROM reload failed after anomaly\r\n");
                error_count++;
                if (error_count > 3) break;
            }
            continue; /* Skip this sample */
        }
        
        /* Validate PROM */
        if (!g_prom_ok) {
            app_printk("[External Pressure] PROM not loaded\r\n");
            break;
        }

        /* BlueRobotics calculation with second-order compensation */
        int32_t dT = (int32_t)D2 - ((uint32_t)g_prom[5] * 256);
        int64_t SENS, OFF;
        int32_t SENSi = 0, OFFi = 0, Ti = 0;
        int64_t OFF2, SENS2;

        if (g_model == 1) {
            /* 02BA */
            SENS = (int64_t)g_prom[1] * 65536LL + ((int64_t)g_prom[3] * dT) / 128LL;
            OFF  = (int64_t)g_prom[2] * 131072LL + ((int64_t)g_prom[4] * dT) / 64LL;
        } else {
            /* 30BA or unknown */
            SENS = (int64_t)g_prom[1] * 32768LL + ((int64_t)g_prom[3] * dT) / 256LL;
            OFF  = (int64_t)g_prom[2] * 65536LL  + ((int64_t)g_prom[4] * dT) / 128LL;
        }

        int32_t TEMP = 2000 + (int32_t)((int64_t)dT * (int64_t)g_prom[6] / 8388608LL);

        if (g_model == 1) {
            if ((TEMP/100) < 20) {
                Ti   = (int32_t)((11LL * (int64_t)dT * (int64_t)dT) / 34359738368LL);
                OFFi = (int32_t)((31LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
                SENSi= (int32_t)((63LL * (TEMP - 2000) * (TEMP - 2000)) / 32);
            } else {
                Ti   = (int32_t)((3LL * (int64_t)dT * (int64_t)dT) / 8589934592LL);
                OFFi = (int32_t)((31LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
                SENSi= (int32_t)((63LL * (TEMP - 2000) * (TEMP - 2000)) / 32);
            }
        } else {
            if ((TEMP/100) < 20) {
                Ti   = (int32_t)((3LL * (int64_t)dT * (int64_t)dT) / 8589934592LL);
                OFFi = (int32_t)((3LL * (TEMP - 2000) * (TEMP - 2000)) / 2);
                SENSi= (int32_t)((5LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
                if ((TEMP/100) < -15) {
                    OFFi += (int32_t)(7LL * (TEMP + 1500) * (TEMP + 1500));
                    SENSi+= (int32_t)(4LL * (TEMP + 1500) * (TEMP + 1500));
                }
            } else {
                Ti   = (int32_t)((2LL * (int64_t)dT * (int64_t)dT) / 137438953472LL);
                OFFi = (int32_t)(((TEMP - 2000) * (TEMP - 2000)) / 16);
                SENSi= 0;
            }
        }

        OFF2  = OFF  - OFFi;
        SENS2 = SENS - SENSi;
        TEMP  = TEMP - Ti;

        int32_t P_int;
        if (g_model == 1) {
            P_int = (int32_t)((( (int64_t)D1 * SENS2 ) / 2097152LL - OFF2) / 32768LL);
        } else {
            P_int = (int32_t)((( (int64_t)D1 * SENS2 ) / 2097152LL - OFF2) / 8192LL);
        }

        double temp_out = TEMP / 100.0;
        double press_mbar = (g_model == 1) ? (P_int / 100.0) : (P_int / 10.0);
        double press_kpa = press_mbar * 0.1;

        static int sample_dbg = 0;
        if (sample_dbg < 5) {
            app_printk("[External Pressure] RAW D1=%u D2=%u TEMP=%.2fC P_int=%d model=%u\r\n",
                       (unsigned)D1, (unsigned)D2, temp_out, (int)P_int, (unsigned)g_model);
            sample_dbg++;
        }
        /* Additional sanity: extremely off values trigger a recover */
        if (temp_out < -10.0 || temp_out > 60.0 || press_kpa < 10.0) {
            app_printk("[External Pressure] out-of-range T/P → resetting (T=%.2f, P=%.2f)\r\n", temp_out, press_kpa);
            uint8_t rst2 = 0x1E;
            (void)i2c_write(i2c_dev, &rst2, 1, g_ms5837_addr);
            k_msleep(10);
            ms5837_bus_recover();
            k_msleep(10);
            g_prom_ok = false;
            if (ms5837_load_prom() != 0) {
                app_printk("[External Pressure] PROM reload failed after out-of-range\r\n");
                error_count++;
                if (error_count > 3) break;
            }
            continue;
        }

        /* Baseline calibration phase: average ~10 samples (1 Hz) */
        if (!baseline_ready) {
            baseline_sum += press_kpa;
            baseline_samples++;
            if (baseline_samples >= 10) {
                double baseline_kpa = baseline_sum / baseline_samples;
                app_printk("[External Pressure] Baseline calibrated: %.2f kPa (sea level)\r\n", baseline_kpa);
                /* Store in sum as baseline for later subtraction */
                baseline_sum = baseline_kpa;
                baseline_ready = true;
            } else {
                app_printk("T=%.2f C, P=%.2f kPa (calibrating %d/10)\r\n", temp_out, press_kpa, baseline_samples);
                error_count = 0;
                next += 1000;
                while (k_uptime_get() < next) {
                    if (quit_requested()) {
                        app_printk("[External Pressure] exit requested → back to menu\r\n");
                        return;
                    }
                    k_sleep(K_MSEC(20));
                }
                continue;
            }
        }

        /* Allow recalibration on 'b' key via net console */
        char line_in[16];
        if (net_console_poll_line(line_in, sizeof(line_in), K_NO_WAIT)) {
            if ((line_in[0] == 'b' || line_in[0] == 'B') && line_in[1] == '\0') {
                baseline_ready = false;
                baseline_sum = 0.0;
                baseline_samples = 0;
                app_printk("[External Pressure] Recalibrating baseline for 10 seconds...\r\n");
            }
        }

        /* Depth calculation: depth = (P - P0) / (rho * g) */
        double p0_kpa = baseline_sum; /* stored baseline */
        double rho = 1000.0;          /* water density kg/m^3 (fresh) */
        double g = 9.80665;           /* m/s^2 */
        double depth_m = ((press_kpa - p0_kpa) * 1000.0) / (rho * g);

        app_printk("T=%.2f C, P=%.2f kPa, Depth=%.2f m\r\n", temp_out, press_kpa, depth_m);
        error_count = 0;
        sample_count++;

        /* Periodic refresh to avoid drift on ESP32 bus */
        if ((sample_count % 50) == 0) {
            uint8_t rst3 = 0x1E;
            (void)i2c_write(i2c_dev, &rst3, 1, g_ms5837_addr);
            k_msleep(10);
            ms5837_bus_recover();
            k_msleep(5);
        }

        next += 1000;
        while (k_uptime_get() < next) {
            if (quit_requested()) {
                app_printk("[External Pressure] exit requested → back to menu\r\n");
                return;
            }
            k_sleep(K_MSEC(20));
        }
    }
    
    app_printk("[External Pressure] too many errors, exiting\r\n");
}

/* Non-interactive single-sample read of MS5837: returns temp (C) and pressure (kPa). */
int ms5837_read(double *temp_c, double *press_kpa)
{
    if (!g_prom_ok) {
        if (ms5837_probe() != 0) return -ENODEV;
        if (ms5837_load_prom() != 0) return -EIO;
    }
    
    (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    k_msleep(2);
    
    uint8_t cmd = 0x4A;
    int rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
    if (rc) return rc;
    k_msleep(20);
    
    cmd = 0x00;
    uint8_t d1[3] = {0};
    rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
    if (rc == 0) rc = i2c_read(i2c_dev, d1, 3, g_ms5837_addr);
    if (rc) return rc;
    uint32_t D1 = ((uint32_t)d1[0]<<16)|((uint32_t)d1[1]<<8)|d1[2];
    
    k_msleep(2);
    
    cmd = 0x5A;
    rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
    if (rc) return rc;
    k_msleep(20);
    
    cmd = 0x00;
    uint8_t d2[3] = {0};
    rc = i2c_write(i2c_dev, &cmd, 1, g_ms5837_addr);
    if (rc == 0) rc = i2c_read(i2c_dev, d2, 3, g_ms5837_addr);
    if (rc) return rc;
    uint32_t D2 = ((uint32_t)d2[0]<<16)|((uint32_t)d2[1]<<8)|d2[2];
    
    int32_t dT = (int32_t)D2 - ((uint32_t)g_prom[5] * 256);
    int64_t SENS, OFF;
    int32_t SENSi = 0, OFFi = 0, Ti = 0;
    int64_t OFF2, SENS2;

    if (g_model == 1) {
        SENS = (int64_t)g_prom[1] * 65536LL + ((int64_t)g_prom[3] * dT) / 128LL;
        OFF  = (int64_t)g_prom[2] * 131072LL + ((int64_t)g_prom[4] * dT) / 64LL;
    } else {
        SENS = (int64_t)g_prom[1] * 32768LL + ((int64_t)g_prom[3] * dT) / 256LL;
        OFF  = (int64_t)g_prom[2] * 65536LL  + ((int64_t)g_prom[4] * dT) / 128LL;
    }

    int32_t TEMP = 2000 + (int32_t)((int64_t)dT * (int64_t)g_prom[6] / 8388608LL);

    if (g_model == 1) {
        if ((TEMP/100) < 20) {
            Ti   = (int32_t)((11LL * (int64_t)dT * (int64_t)dT) / 34359738368LL);
            OFFi = (int32_t)((31LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
            SENSi= (int32_t)((63LL * (TEMP - 2000) * (TEMP - 2000)) / 32);
        } else {
            Ti   = (int32_t)((3LL * (int64_t)dT * (int64_t)dT) / 8589934592LL);
            OFFi = (int32_t)((31LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
            SENSi= (int32_t)((63LL * (TEMP - 2000) * (TEMP - 2000)) / 32);
        }
    } else {
        if ((TEMP/100) < 20) {
            Ti   = (int32_t)((3LL * (int64_t)dT * (int64_t)dT) / 8589934592LL);
            OFFi = (int32_t)((3LL * (TEMP - 2000) * (TEMP - 2000)) / 2);
            SENSi= (int32_t)((5LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
            if ((TEMP/100) < -15) {
                OFFi += (int32_t)(7LL * (TEMP + 1500) * (TEMP + 1500));
                SENSi+= (int32_t)(4LL * (TEMP + 1500) * (TEMP + 1500));
            }
        } else {
            Ti   = (int32_t)((2LL * (int64_t)dT * (int64_t)dT) / 137438953472LL);
            OFFi = (int32_t)(((TEMP - 2000) * (TEMP - 2000)) / 16);
            SENSi= 0;
        }
    }

    OFF2  = OFF  - OFFi;
    SENS2 = SENS - SENSi;
    TEMP  = TEMP - Ti;

    int32_t P_int;
    if (g_model == 1) {
        P_int = (int32_t)((( (int64_t)D1 * SENS2 ) / 2097152LL - OFF2) / 32768LL);
    } else {
        P_int = (int32_t)((( (int64_t)D1 * SENS2 ) / 2097152LL - OFF2) / 8192LL);
    }

    if (temp_c) *temp_c = TEMP / 100.0;
    if (press_kpa) {
        double press_mbar = (g_model == 1) ? (P_int / 100.0) : (P_int / 10.0);
        *press_kpa = press_mbar * 0.1; /* kPa */
    }
    return 0;
}


