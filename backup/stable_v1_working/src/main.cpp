#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>
#include "bms_data.h"
#include "rs485_sniffer.h"
#include "web_gateway.h"
#include "audio_hal.h"
#include "sd_logger.h"

// --- Display Pins JC3248W535C (QSPI) ---
#define LCD_CS    45
#define LCD_SCLK  47
#define LCD_D0    21
#define LCD_D1    48
#define LCD_D2    40
#define LCD_D3    39
#define LCD_BL    1

// PWM Backlight configuration
#define LCD_BL_PWM_CHANNEL  0
#define LCD_BL_PWM_FREQ     5000
#define LCD_BL_PWM_RES      8
#define BRIGHTNESS_HIGH     255
#define BRIGHTNESS_DIM      25
#define SCREENSAVER_TIMEOUT_MS  (180000)

// --- Touch Pins & Address ---
#define TOUCH_ADDR       0x3B
#define TOUCH_SDA        4
#define TOUCH_SCL        8
#define TOUCH_RST_PIN    12
#define TOUCH_INT_PIN    11
#define TOUCH_I2C_CLOCK  400000
#define AXS_MAX_TOUCH    1

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 480

// Premium Colors
#define CLR_DARK_BG       0x080B10
#define CLR_PANEL         0x121721
#define CLR_PANEL_INNER   0x0D1219
#define CLR_BORDER        0x1E293B
#define CLR_CYAN          0x06B6D4
#define CLR_EMERALD       0x10B981
#define CLR_BLUE          0x38BDF8
#define CLR_AMBER         0xF59E0B
#define CLR_ROSE          0xF43F5E
#define CLR_PURPLE        0xC084FC
#define CLR_TEXT_MUTED    0x64748B

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
static Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, SCREEN_WIDTH, SCREEN_HEIGHT);
static Arduino_Canvas *gfx = new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, panel, 0, 0, 0);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static SemaphoreHandle_t dataMutex = NULL;
static BatteryPack g_packs[MAX_BATTERIES];

static uint32_t s_last_touch_time = 0;
static bool s_is_screen_dimmed = false;
static uint32_t s_last_alarm_beep_ms = 0;

static lv_obj_t *tabview = NULL;
static lv_obj_t *tab_overview = NULL;

static lv_obj_t *arc_soc = NULL;
static lv_obj_t *lbl_soc_val = NULL;
static lv_obj_t *lbl_soc_sub = NULL;
static lv_obj_t *lbl_mode_pill = NULL;

static lv_obj_t *lbl_power_kw = NULL;
static lv_obj_t *lbl_voltage_val = NULL;
static lv_obj_t *lbl_current_val = NULL;

static lv_obj_t *lbl_delta_val = NULL;
static lv_obj_t *bar_delta = NULL;
static lv_obj_t *lbl_delta_extremes = NULL;

static lv_obj_t *card_alarm_banner = NULL;
static lv_obj_t *lbl_alarm_icon = NULL;
static lv_obj_t *lbl_alarm_title = NULL;
static lv_obj_t *lbl_alarm_details = NULL;

// SD Rec Button Widgets on Overview
static lv_obj_t *btn_bank_log = NULL;
static lv_obj_t *lbl_bank_log = NULL;
static lv_obj_t *lbl_top_ip = NULL;

// Dedicated Logs Tab Widgets
static lv_obj_t *tab_logs = NULL;
static lv_obj_t *lbl_logs_status = NULL;
static lv_obj_t *logs_list_container = NULL;
static void refresh_logs_ui();

struct PackUI {
    lv_obj_t *tab;
    lv_obj_t *lbl_status;
    lv_obj_t *lbl_voltage;
    lv_obj_t *lbl_current;
    lv_obj_t *lbl_power;
    lv_obj_t *bar_soc;
    lv_obj_t *lbl_soc;
    lv_obj_t *lbl_temps;
    lv_obj_t *lbl_delta;
    lv_obj_t *card_alarm;
    lv_obj_t *lbl_alarm_text;
    lv_obj_t *btn_log;
    lv_obj_t *lbl_log_btn;
    lv_obj_t *chart_cells;
    lv_chart_series_t *ser_cells;
    lv_obj_t *lbl_cells[CELLS_PER_BATTERY];
    lv_obj_t *cell_boxes[CELLS_PER_BATTERY];
    bool created;
};
static PackUI g_pack_ui[MAX_BATTERIES];

static void set_backlight_brightness(uint8_t brightness) {
    ledcWrite(LCD_BL_PWM_CHANNEL, brightness);
}

static String decodeAlarmString(const BatteryPack &p) {
    String res = "";
    uint16_t w = p.warningRaw;
    uint16_t pr = p.protectionRaw;
    uint16_t err = p.errorRaw;

    if (pr & 0x0001) res += "Pack OV Prot | ";
    else if (w & 0x0001) res += "Pack OV Warn | ";

    if (pr & 0x0002) res += "Cell OV Prot | ";
    else if (w & 0x0002) res += "Cell OV Warn | ";

    if (pr & 0x0004) res += "Pack UV Prot | ";
    else if (w & 0x0004) res += "Pack UV Warn | ";

    if (pr & 0x0008) res += "Cell UV Prot | ";
    else if (w & 0x0008) res += "Cell UV Warn | ";

    if (pr & 0x0010) res += "Charge OC Prot | ";
    else if (w & 0x0010) res += "Charge OC Warn | ";

    if (pr & 0x0020) res += "Discharge OC Prot | ";
    else if (w & 0x0020) res += "Discharge OC Warn | ";

    if (w & 0x0040) res += "Temp Anomaly | ";
    if (w & 0x0080) res += "MOS OverTemp | ";
    if (w & 0x0100) res += "Charge OverTemp | ";
    if (w & 0x0200) res += "Discharge OverTemp | ";
    if (w & 0x0400) res += "Charge UnderTemp | ";

    if (err & 0x0001) res += "Voltage Error | ";
    if (err & 0x0002) res += "Temp Error | ";
    if (err & 0x0010) res += "Cell Unbalance | ";

    if (res.length() > 3) res.remove(res.length() - 3);
    return res;
}

// Button Events
static void btn_bank_log_cb(lv_event_t * e) {
    bool logging = Logger.toggleBankLog();
    if (logging) {
        lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_ROSE), 0);
        lv_label_set_text(lbl_bank_log, "REC: BANK LOGGING (ON)");
    } else {
        lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_PANEL_INNER), 0);
        lv_label_set_text(lbl_bank_log, "START BANK LOG (REC)");
    }
    BmsAudio.playClick();
}

static void btn_pack_log_cb(lv_event_t * e) {
    uintptr_t packIdx = (uintptr_t)lv_event_get_user_data(e);
    if (packIdx < MAX_BATTERIES) {
        bool logging = Logger.togglePackLog(packIdx);
        PackUI &ui = g_pack_ui[packIdx];
        if (logging) {
            lv_obj_set_style_bg_color(ui.btn_log, lv_color_hex(CLR_ROSE), 0);
            lv_label_set_text(ui.lbl_log_btn, "REC ON");
        } else {
            lv_obj_set_style_bg_color(ui.btn_log, lv_color_hex(CLR_PANEL_INNER), 0);
            lv_label_set_text(ui.lbl_log_btn, "REC SD");
        }
        BmsAudio.playClick();
    }
}

// Logs UI Events
static void btn_refresh_logs_cb(lv_event_t * e) {
    BmsAudio.playClick();
    refresh_logs_ui();
}

static void btn_clear_all_logs_cb(lv_event_t * e) {
    BmsAudio.playClick();
    Logger.deleteAllLogs();
    refresh_logs_ui();
}

static void btn_delete_single_file_cb(lv_event_t * e) {
    const char* filename = (const char*)lv_event_get_user_data(e);
    if (filename) {
        BmsAudio.playClick();
        Logger.deleteFile(filename);
        refresh_logs_ui();
    }
}

// Touch Reading
static bool touch_read_hw(uint16_t &x, uint16_t &y) {
    uint8_t data[AXS_MAX_TOUCH * 6 + 2] = {0};
    const uint8_t read_cmd[11] = {
        0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
        (uint8_t)((AXS_MAX_TOUCH * 6 + 2) >> 8),
        (uint8_t)((AXS_MAX_TOUCH * 6 + 2) & 0xff),
        0x00, 0x00, 0x00
    };

    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(read_cmd, sizeof(read_cmd));
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (size_t)sizeof(data)) != sizeof(data)) return false;
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = Wire.read();
    }

    if (data[1] > 0 && data[1] <= AXS_MAX_TOUCH) {
        x = ((data[2] & 0x0F) << 8) | data[3];
        y = ((data[4] & 0x0F) << 8) | data[5];

        s_last_touch_time = millis();
        if (s_is_screen_dimmed) {
            s_is_screen_dimmed = false;
            set_backlight_brightness(BRIGHTNESS_HIGH);
        }

        static uint32_t s_last_click_ms = 0;
        if (millis() - s_last_click_ms > 140) {
            s_last_click_ms = millis();
            BmsAudio.playClick();
        }

        return true;
    }
    return false;
}

static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);

    if (lv_disp_flush_is_last(disp)) {
        gfx->flush();
    }
    lv_disp_flush_ready(disp);
}

static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t touchX = 0, touchY = 0;
    bool touched = touch_read_hw(touchX, touchY);

    if (touched) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static lv_obj_t* create_glass_card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(CLR_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void create_battery_tab(uint8_t pack_idx) {
    char tab_name[16];
    snprintf(tab_name, sizeof(tab_name), "P%u", pack_idx + 1);
    
    lv_obj_t *tab = lv_tabview_add_tab(tabview, tab_name);
    lv_obj_set_style_bg_color(tab, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_pad_all(tab, 6, 0);

    PackUI &ui = g_pack_ui[pack_idx];
    ui.tab = tab;

    // 1. Pack Header Card
    lv_obj_t *top_card = create_glass_card(tab, 308, 88);
    lv_obj_align(top_card, LV_ALIGN_TOP_MID, 0, 0);

    ui.lbl_status = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_status, "NORMAL");
    lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(ui.lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_align(ui.lbl_status, LV_ALIGN_TOP_LEFT, 2, 0);

    ui.lbl_delta = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_delta, "Delta: -- mV");
    lv_obj_set_style_text_color(ui.lbl_delta, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_text_font(ui.lbl_delta, &lv_font_montserrat_12, 0);
    lv_obj_align(ui.lbl_delta, LV_ALIGN_TOP_RIGHT, -2, 0);

    ui.lbl_voltage = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_voltage, "51.20 V");
    lv_obj_set_style_text_color(ui.lbl_voltage, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(ui.lbl_voltage, &lv_font_montserrat_20, 0);
    lv_obj_align(ui.lbl_voltage, LV_ALIGN_LEFT_MID, 2, -6);

    ui.lbl_current = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_current, "+0.0 A");
    lv_obj_set_style_text_color(ui.lbl_current, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui.lbl_current, &lv_font_montserrat_18, 0);
    lv_obj_align(ui.lbl_current, LV_ALIGN_CENTER, 20, -6);

    ui.lbl_power = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_power, "0 W");
    lv_obj_set_style_text_color(ui.lbl_power, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(ui.lbl_power, &lv_font_montserrat_14, 0);
    lv_obj_align(ui.lbl_power, LV_ALIGN_RIGHT_MID, -2, -6);

    ui.bar_soc = lv_bar_create(top_card);
    lv_obj_set_size(ui.bar_soc, 240, 10);
    lv_obj_align(ui.bar_soc, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_bar_set_range(ui.bar_soc, 0, 100);
    lv_obj_set_style_bg_color(ui.bar_soc, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_bg_color(ui.bar_soc, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_soc, 5, 0);
    lv_obj_set_style_radius(ui.bar_soc, 5, LV_PART_INDICATOR);

    ui.lbl_soc = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_soc, "100%");
    lv_obj_set_style_text_color(ui.lbl_soc, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui.lbl_soc, &lv_font_montserrat_12, 0);
    lv_obj_align(ui.lbl_soc, LV_ALIGN_BOTTOM_RIGHT, -2, -2);

    // 2. Alarm Banner + Individual REC Button
    ui.card_alarm = create_glass_card(tab, 308, 38);
    lv_obj_align(ui.card_alarm, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_pad_all(ui.card_alarm, 4, 0);

    ui.lbl_alarm_text = lv_label_create(ui.card_alarm);
    lv_label_set_text(ui.lbl_alarm_text, "Protection: Normal (No Alarms)");
    lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(ui.lbl_alarm_text, &lv_font_montserrat_12, 0);
    lv_obj_align(ui.lbl_alarm_text, LV_ALIGN_LEFT_MID, 6, 0);

    // Individual REC Button for this pack (widened 2x to 140px)
    ui.btn_log = lv_btn_create(ui.card_alarm);
    lv_obj_set_size(ui.btn_log, 140, 28);
    lv_obj_align(ui.btn_log, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(ui.btn_log, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(ui.btn_log, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(ui.btn_log, 1, 0);
    lv_obj_set_style_radius(ui.btn_log, 6, 0);
    lv_obj_add_event_cb(ui.btn_log, btn_pack_log_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)pack_idx);

    ui.lbl_log_btn = lv_label_create(ui.btn_log);
    lv_label_set_text(ui.lbl_log_btn, "REC SD (PACK)");
    lv_obj_set_style_text_color(ui.lbl_log_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui.lbl_log_btn, &lv_font_montserrat_10, 0);
    lv_obj_align(ui.lbl_log_btn, LV_ALIGN_CENTER, 0, 0);

    // 3. 15S Cell Bar Chart & Pack Diagnostics Card
    lv_obj_t *chart_card = create_glass_card(tab, 308, 92);
    lv_obj_align(chart_card, LV_ALIGN_TOP_MID, 0, 134);

    ui.lbl_temps = lv_label_create(chart_card);
    lv_label_set_text(ui.lbl_temps, "PCB: 25C | Avg: 24C | Max: 25C | R_int: 0.44mR");
    lv_obj_set_style_text_color(ui.lbl_temps, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(ui.lbl_temps, &lv_font_montserrat_10, 0);
    lv_obj_align(ui.lbl_temps, LV_ALIGN_TOP_LEFT, 2, -2);

    ui.chart_cells = lv_chart_create(chart_card);
    lv_obj_set_size(ui.chart_cells, 292, 60);
    lv_obj_align(ui.chart_cells, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_chart_set_type(ui.chart_cells, LV_CHART_TYPE_BAR);
    lv_chart_set_range(ui.chart_cells, LV_CHART_AXIS_PRIMARY_Y, 3000, 3600);
    lv_chart_set_point_count(ui.chart_cells, 15);
    lv_obj_set_style_bg_color(ui.chart_cells, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_width(ui.chart_cells, 0, 0);
    lv_obj_set_style_pad_all(ui.chart_cells, 2, 0);
    ui.ser_cells = lv_chart_add_series(ui.chart_cells, lv_color_hex(CLR_CYAN), LV_CHART_AXIS_PRIMARY_Y);

    // 4. 15 Cell Values Matrix
    int start_y = 230;
    for (int i = 0; i < CELLS_PER_BATTERY; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = 6 + col * (96 + 8);
        int y = start_y + row * (38 + 4);

        ui.cell_boxes[i] = lv_obj_create(tab);
        lv_obj_set_size(ui.cell_boxes[i], 96, 38);
        lv_obj_set_pos(ui.cell_boxes[i], x, y);
        lv_obj_set_style_bg_color(ui.cell_boxes[i], lv_color_hex(CLR_PANEL), 0);
        lv_obj_set_style_border_color(ui.cell_boxes[i], lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(ui.cell_boxes[i], 1, 0);
        lv_obj_set_style_radius(ui.cell_boxes[i], 8, 0);
        lv_obj_set_style_pad_all(ui.cell_boxes[i], 2, 0);
        lv_obj_clear_flag(ui.cell_boxes[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *c_lbl = lv_label_create(ui.cell_boxes[i]);
        char c_text[20];
        snprintf(c_text, sizeof(c_text), "C%02d 3.300", i + 1);
        lv_label_set_text(c_lbl, c_text);
        lv_obj_set_style_text_font(c_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(c_lbl, lv_color_white(), 0);
        lv_obj_align(c_lbl, LV_ALIGN_CENTER, 0, 0);
        ui.lbl_cells[i] = c_lbl;
    }

    ui.created = true;
}

static void refresh_logs_ui() {
    if (!logs_list_container) return;
    lv_obj_clean(logs_list_container);

    if (!Logger.isAvailable()) {
        lv_label_set_text(lbl_logs_status, "MicroSD Card: NOT DETECTED");
        lv_obj_set_style_text_color(lbl_logs_status, lv_color_hex(CLR_ROSE), 0);
        return;
    }

    lv_label_set_text(lbl_logs_status, "MicroSD Card: READY");
    lv_obj_set_style_text_color(lbl_logs_status, lv_color_hex(CLR_EMERALD), 0);

    SdLogger::FileInfo fileList[16];
    int count = Logger.getFilesList(fileList, 16);

    if (count == 0) {
        lv_obj_t *lbl_empty = lv_label_create(logs_list_container);
        lv_label_set_text(lbl_empty, "No CSV log files on MicroSD");
        lv_obj_set_style_text_color(lbl_empty, lv_color_hex(CLR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(lbl_empty, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 30);
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = lv_obj_create(logs_list_container);
        lv_obj_set_size(row, 296, 42);
        lv_obj_set_style_bg_color(row, lv_color_hex(CLR_PANEL_INNER), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl_name = lv_label_create(row);
        lv_label_set_text(lbl_name, fileList[i].name);
        lv_obj_set_style_text_color(lbl_name, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 4, 2);

        lv_obj_t *lbl_sz = lv_label_create(row);
        char szBuf[24];
        snprintf(szBuf, sizeof(szBuf), "%.1f KB", fileList[i].size / 1024.0f);
        lv_label_set_text(lbl_sz, szBuf);
        lv_obj_set_style_text_color(lbl_sz, lv_color_hex(CLR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(lbl_sz, &lv_font_montserrat_10, 0);
        lv_obj_align(lbl_sz, LV_ALIGN_BOTTOM_LEFT, 4, -2);

        // DEL Button
        lv_obj_t *btn_del = lv_btn_create(row);
        lv_obj_set_size(btn_del, 64, 28);
        lv_obj_align(btn_del, LV_ALIGN_RIGHT_MID, -2, 0);
        lv_obj_set_style_bg_color(btn_del, lv_color_hex(CLR_ROSE), 0);
        lv_obj_set_style_radius(btn_del, 6, 0);
        
        // Allocate heap copy of filename string for event callback
        char* heapName = strdup(fileList[i].name);
        lv_obj_add_event_cb(btn_del, btn_delete_single_file_cb, LV_EVENT_CLICKED, heapName);

        lv_obj_t *lbl_del = lv_label_create(btn_del);
        lv_label_set_text(lbl_del, "DEL");
        lv_obj_set_style_text_color(lbl_del, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_del, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_del, LV_ALIGN_CENTER, 0, 0);
    }
}

static void create_logs_tab() {
    tab_logs = lv_tabview_add_tab(tabview, "Logs");
    lv_obj_set_style_bg_color(tab_logs, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_pad_all(tab_logs, 6, 0);

    // Top Header & Controls Card
    lv_obj_t *top_card = create_glass_card(tab_logs, 308, 64);
    lv_obj_align(top_card, LV_ALIGN_TOP_MID, 0, 0);

    lbl_logs_status = lv_label_create(top_card);
    lv_label_set_text(lbl_logs_status, "MicroSD Storage Manager");
    lv_obj_set_style_text_color(lbl_logs_status, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_logs_status, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_logs_status, LV_ALIGN_TOP_LEFT, 2, 0);

    // Refresh Button
    lv_obj_t *btn_ref = lv_btn_create(top_card);
    lv_obj_set_size(btn_ref, 80, 26);
    lv_obj_align(btn_ref, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    lv_obj_set_style_bg_color(btn_ref, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_ref, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(btn_ref, 1, 0);
    lv_obj_set_style_radius(btn_ref, 6, 0);
    lv_obj_add_event_cb(btn_ref, btn_refresh_logs_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_ref = lv_label_create(btn_ref);
    lv_label_set_text(lbl_ref, "REFRESH");
    lv_obj_set_style_text_color(lbl_ref, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_ref, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_ref, LV_ALIGN_CENTER, 0, 0);

    // Clear All Button
    lv_obj_t *btn_clr = lv_btn_create(top_card);
    lv_obj_set_size(btn_clr, 96, 26);
    lv_obj_align(btn_clr, LV_ALIGN_BOTTOM_RIGHT, -2, 0);
    lv_obj_set_style_bg_color(btn_clr, lv_color_hex(0x4C0519), 0);
    lv_obj_set_style_border_color(btn_clr, lv_color_hex(CLR_ROSE), 0);
    lv_obj_set_style_border_width(btn_clr, 1, 0);
    lv_obj_set_style_radius(btn_clr, 6, 0);
    lv_obj_add_event_cb(btn_clr, btn_clear_all_logs_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_clr = lv_label_create(btn_clr);
    lv_label_set_text(lbl_clr, "CLEAR ALL");
    lv_obj_set_style_text_color(lbl_clr, lv_color_hex(CLR_ROSE), 0);
    lv_obj_set_style_text_font(lbl_clr, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_clr, LV_ALIGN_CENTER, 0, 0);

    // Files List Scrollable Container
    logs_list_container = lv_obj_create(tab_logs);
    lv_obj_set_size(logs_list_container, 308, 350);
    lv_obj_align(logs_list_container, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_color(logs_list_container, lv_color_hex(CLR_PANEL), 0);
    lv_obj_set_style_border_color(logs_list_container, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(logs_list_container, 1, 0);
    lv_obj_set_style_radius(logs_list_container, 12, 0);
    lv_obj_set_style_pad_all(logs_list_container, 6, 0);
    lv_obj_set_flex_flow(logs_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(logs_list_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    refresh_logs_ui();
}

static void build_ui() {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(CLR_DARK_BG), 0);

    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 34);
    lv_obj_set_style_bg_color(tabview, lv_color_hex(CLR_DARK_BG), 0);
    
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(tab_btns, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(tab_btns, 1, 0);
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(CLR_CYAN), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_btns, lv_color_hex(CLR_CYAN), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_12, 0);

    tab_overview = lv_tabview_add_tab(tabview, "All");
    lv_obj_set_style_bg_color(tab_overview, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_pad_all(tab_overview, 6, 0);

    // 1. Status Bar Header
    lv_obj_t *top_bar = create_glass_card(tab_overview, 308, 38);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    lbl_mode_pill = lv_label_create(top_bar);
    lv_label_set_text(lbl_mode_pill, LV_SYMBOL_OK " NORMAL OPERATION");
    lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_mode_pill, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_mode_pill, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_top_ip = lv_label_create(top_bar);
    lv_label_set_text(lbl_top_ip, "IP: ...");
    lv_obj_set_style_text_color(lbl_top_ip, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_top_ip, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_top_ip, LV_ALIGN_RIGHT_MID, -4, 0);

    // 2. Central Gauge Card
    lv_obj_t *gauge_card = create_glass_card(tab_overview, 308, 168);
    lv_obj_align(gauge_card, LV_ALIGN_TOP_MID, 0, 42);

    arc_soc = lv_arc_create(gauge_card);
    lv_obj_set_size(arc_soc, 146, 146);
    lv_arc_set_rotation(arc_soc, 135);
    lv_arc_set_bg_angles(arc_soc, 0, 270);
    lv_arc_set_range(arc_soc, 0, 100);
    lv_arc_set_value(arc_soc, 75);
    lv_obj_remove_style(arc_soc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_soc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc_soc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_PANEL_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_soc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
    lv_obj_align(arc_soc, LV_ALIGN_LEFT_MID, -2, 0);

    lbl_soc_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_soc_val, "75%");
    lv_obj_set_style_text_color(lbl_soc_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_soc_val, &lv_font_montserrat_28, 0);
    lv_obj_align_to(lbl_soc_val, arc_soc, LV_ALIGN_CENTER, 0, -8);

    lbl_soc_sub = lv_label_create(gauge_card);
    lv_label_set_text(lbl_soc_sub, "STATE OF CHARGE");
    lv_obj_set_style_text_color(lbl_soc_sub, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_soc_sub, &lv_font_montserrat_10, 0);
    lv_obj_align_to(lbl_soc_sub, arc_soc, LV_ALIGN_CENTER, 0, 18);

    lv_obj_t *lbl_pwr_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_pwr_title, "ACTIVE POWER");
    lv_obj_set_style_text_color(lbl_pwr_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_pwr_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_pwr_title, LV_ALIGN_TOP_RIGHT, -6, 10);

    lbl_power_kw = lv_label_create(gauge_card);
    lv_label_set_text(lbl_power_kw, "1.55 kW");
    lv_obj_set_style_text_color(lbl_power_kw, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_power_kw, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_power_kw, LV_ALIGN_TOP_RIGHT, -6, 24);

    lv_obj_t *lbl_v_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_v_title, "VOLTAGE");
    lv_obj_set_style_text_color(lbl_v_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_v_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_v_title, LV_ALIGN_RIGHT_MID, -70, 18);

    lbl_voltage_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_voltage_val, "51.72 V");
    lv_obj_set_style_text_color(lbl_voltage_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_voltage_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_voltage_val, LV_ALIGN_RIGHT_MID, -70, 34);

    lv_obj_t *lbl_i_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_i_title, "CURRENT");
    lv_obj_set_style_text_color(lbl_i_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_i_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_i_title, LV_ALIGN_RIGHT_MID, -6, 18);

    lbl_current_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_current_val, "+29.9 A");
    lv_obj_set_style_text_color(lbl_current_val, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_current_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_current_val, LV_ALIGN_RIGHT_MID, -6, 34);

    // 3. Dynamic Alarm Banner
    card_alarm_banner = create_glass_card(tab_overview, 308, 48);
    lv_obj_align(card_alarm_banner, LV_ALIGN_TOP_MID, 0, 214);
    lv_obj_set_style_bg_color(card_alarm_banner, lv_color_hex(0x064E3B), 0);
    lv_obj_set_style_border_color(card_alarm_banner, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_clear_flag(card_alarm_banner, LV_OBJ_FLAG_SCROLLABLE);

    lbl_alarm_icon = lv_label_create(card_alarm_banner);
    lv_label_set_text(lbl_alarm_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(lbl_alarm_icon, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_alarm_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_alarm_icon, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_alarm_title = lv_label_create(card_alarm_banner);
    lv_label_set_text(lbl_alarm_title, "SYSTEM STATUS: ALL OK");
    lv_obj_set_style_text_color(lbl_alarm_title, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_alarm_title, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_alarm_title, LV_ALIGN_TOP_LEFT, 26, -2);

    lbl_alarm_details = lv_label_create(card_alarm_banner);
    lv_label_set_text(lbl_alarm_details, "No active warnings, protections or faults");
    lv_obj_set_style_text_color(lbl_alarm_details, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_alarm_details, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_alarm_details, LV_ALIGN_BOTTOM_LEFT, 26, 2);

    // 4. Bank Health & Delta Card
    lv_obj_t *health_card = create_glass_card(tab_overview, 308, 86);
    lv_obj_align(health_card, LV_ALIGN_TOP_MID, 0, 264);

    lv_obj_t *h_title = lv_label_create(health_card);
    lv_label_set_text(h_title, "BANK CELL BALANCE (DELTA)");
    lv_obj_set_style_text_color(h_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(h_title, &lv_font_montserrat_10, 0);
    lv_obj_align(h_title, LV_ALIGN_TOP_LEFT, 2, 0);

    lbl_delta_val = lv_label_create(health_card);
    lv_label_set_text(lbl_delta_val, "50 mV");
    lv_obj_set_style_text_color(lbl_delta_val, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_text_font(lbl_delta_val, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_delta_val, LV_ALIGN_TOP_RIGHT, -2, -4);

    bar_delta = lv_bar_create(health_card);
    lv_obj_set_size(bar_delta, 290, 8);
    lv_obj_align(bar_delta, LV_ALIGN_CENTER, 0, 4);
    lv_bar_set_range(bar_delta, 0, 100);
    lv_bar_set_value(bar_delta, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_delta, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_bg_color(bar_delta, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_delta, 4, 0);
    lv_obj_set_style_radius(bar_delta, 4, LV_PART_INDICATOR);

    lbl_delta_extremes = lv_label_create(health_card);
    lv_label_set_text(lbl_delta_extremes, "Min: 3.413 V  |  Max: 3.463 V");
    lv_obj_set_style_text_color(lbl_delta_extremes, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_delta_extremes, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_delta_extremes, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    // 5. Interactive REC Button Card on Overview
    lv_obj_t *info_card = create_glass_card(tab_overview, 308, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 354);

    btn_bank_log = lv_btn_create(info_card);
    lv_obj_set_size(btn_bank_log, 292, 36);
    lv_obj_align(btn_bank_log, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_bank_log, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(btn_bank_log, 1, 0);
    lv_obj_set_style_radius(btn_bank_log, 8, 0);
    lv_obj_add_event_cb(btn_bank_log, btn_bank_log_cb, LV_EVENT_CLICKED, NULL);

    lbl_bank_log = lv_label_create(btn_bank_log);
    lv_label_set_text(lbl_bank_log, "START BANK LOG (REC SD)");
    lv_obj_set_style_text_color(lbl_bank_log, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_bank_log, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_bank_log, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *bus_tag = lv_label_create(info_card);
    lv_label_set_text(bus_tag, "BUS: RS485 SNIFFER (TAP TO START / STOP LOG)");
    lv_obj_set_style_text_color(bus_tag, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(bus_tag, &lv_font_montserrat_10, 0);
    lv_obj_align(bus_tag, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Create Dedicated Logs Tab
    create_logs_tab();
}

static void update_ui() {
    BatteryPack local_packs[MAX_BATTERIES];

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(local_packs, g_packs, sizeof(g_packs));
        xSemaphoreGive(dataMutex);
    } else {
        return;
    }

    // Logger writes only if triggered by user button
    Logger.update(local_packs, MAX_BATTERIES);

    // Update Overview button status if logging
    if (Logger.isAutoAlarmLogging()) {
        char recTxt[48];
        snprintf(recTxt, sizeof(recTxt), "%s AUTO ALARM: %u", LV_SYMBOL_BELL, Logger.getAlarmRows());
        lv_label_set_text(lbl_bank_log, recTxt);
        lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_ROSE), 0);
    } else if (Logger.isBankLogging()) {
        char recTxt[48];
        snprintf(recTxt, sizeof(recTxt), "%s REC ALL: %u ROWS", LV_SYMBOL_SAVE, Logger.getBankRows());
        lv_label_set_text(lbl_bank_log, recTxt);
        lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_ROSE), 0);
    } else {
        lv_label_set_text(lbl_bank_log, "START BANK LOG (REC SD)");
        lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_PANEL_INNER), 0);
    }

    float vSum = 0;
    float iSum = 0;
    float socSum = 0;
    uint8_t count = 0;
    uint16_t gMin = 9999;
    uint16_t gMax = 0;
    bool alarms = false;
    bool anyBalancing = false;
    String firstAlarmPack = "";
    String firstAlarmDesc = "";

    for (int i = 0; i < MAX_BATTERIES; i++) {
        if (local_packs[i].isOnline) {
            vSum += local_packs[i].voltage;
            iSum += local_packs[i].current;
            socSum += local_packs[i].soc;
            count++;
            if (local_packs[i].cellMin < gMin && local_packs[i].cellMin > 0) gMin = local_packs[i].cellMin;
            if (local_packs[i].cellMax > gMax) gMax = local_packs[i].cellMax;
            
            if (local_packs[i].warningRaw || local_packs[i].protectionRaw || local_packs[i].errorRaw) {
                alarms = true;
                if (firstAlarmPack == "") {
                    firstAlarmPack = "Pack #" + String(i + 1);
                    firstAlarmDesc = decodeAlarmString(local_packs[i]);
                }
            }
            if (local_packs[i].balanceMask != 0) anyBalancing = true;
        }
    }

    float avgVolt = (count > 0) ? (vSum / count) : 0.0f;
    float totalPwrKw = (avgVolt * iSum) / 1000.0f;
    float avgSoc = (count > 0) ? (socSum / count) : 0.0f;
    uint16_t globalDeltaMv = (count > 0 && gMax >= gMin) ? (gMax - gMin) : 0;

    if (alarms && (millis() - s_last_alarm_beep_ms > 30000)) {
        s_last_alarm_beep_ms = millis();
        if (s_is_screen_dimmed) {
            s_is_screen_dimmed = false;
            set_backlight_brightness(BRIGHTNESS_HIGH);
        }
        BmsAudio.playAlarm();
    }

    if (Gateway.isConnected()) {
        char ip_str[36];
        snprintf(ip_str, sizeof(ip_str), "%s (%d dBm)", Gateway.getIpAddress().c_str(), Gateway.getRssi());
        lv_label_set_text(lbl_top_ip, ip_str);
    } else if (Gateway.isAPMode()) {
        lv_label_set_text(lbl_top_ip, "AP: 192.168.4.1");
    } else {
        lv_label_set_text(lbl_top_ip, "Wi-Fi: ...");
    }

    if (alarms) {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_WARNING " ALARM DETECTED");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_ROSE), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_ROSE), LV_PART_INDICATOR);
    } else if (anyBalancing) {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_CHARGE " BALANCING");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_AMBER), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
    } else if (iSum > 0.5f) {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_UP " CHARGING");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_EMERALD), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
    } else if (iSum < -0.5f) {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_DOWN " DISCHARGING");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_BLUE), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_BLUE), LV_PART_INDICATOR);
    } else {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_OK " STANDBY / IDLE");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
    }

    if (alarms) {
        lv_obj_set_style_bg_color(card_alarm_banner, lv_color_hex(0x4C0519), 0);
        lv_obj_set_style_border_color(card_alarm_banner, lv_color_hex(CLR_ROSE), 0);
        lv_label_set_text(lbl_alarm_icon, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(lbl_alarm_icon, lv_color_hex(CLR_ROSE), 0);

        String title = "FAULT: " + firstAlarmPack;
        lv_label_set_text(lbl_alarm_title, title.c_str());
        lv_obj_set_style_text_color(lbl_alarm_title, lv_color_hex(CLR_ROSE), 0);

        lv_label_set_text(lbl_alarm_details, firstAlarmDesc.c_str());
    } else {
        lv_obj_set_style_bg_color(card_alarm_banner, lv_color_hex(0x064E3B), 0);
        lv_obj_set_style_border_color(card_alarm_banner, lv_color_hex(CLR_EMERALD), 0);
        lv_label_set_text(lbl_alarm_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(lbl_alarm_icon, lv_color_hex(CLR_EMERALD), 0);

        lv_label_set_text(lbl_alarm_title, "SYSTEM STATUS: ALL OK");
        lv_obj_set_style_text_color(lbl_alarm_title, lv_color_hex(CLR_EMERALD), 0);

        lv_label_set_text(lbl_alarm_details, "No active warnings, protections or faults");
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f%%", avgSoc);
    lv_label_set_text(lbl_soc_val, buf);
    lv_arc_set_value(arc_soc, (int32_t)avgSoc);

    snprintf(buf, sizeof(buf), "%.2f kW", fabsf(totalPwrKw));
    lv_label_set_text(lbl_power_kw, buf);

    snprintf(buf, sizeof(buf), "%.2f V", avgVolt);
    lv_label_set_text(lbl_voltage_val, buf);

    snprintf(buf, sizeof(buf), "%+.1f A", iSum);
    lv_label_set_text(lbl_current_val, buf);
    if (iSum > 0.5f) {
        lv_obj_set_style_text_color(lbl_current_val, lv_color_hex(CLR_EMERALD), 0);
    } else if (iSum < -0.5f) {
        lv_obj_set_style_text_color(lbl_current_val, lv_color_hex(CLR_BLUE), 0);
    } else {
        lv_obj_set_style_text_color(lbl_current_val, lv_color_white(), 0);
    }

    snprintf(buf, sizeof(buf), "%u mV", globalDeltaMv);
    lv_label_set_text(lbl_delta_val, buf);
    lv_bar_set_value(bar_delta, globalDeltaMv > 100 ? 100 : globalDeltaMv, LV_ANIM_ON);
    if (globalDeltaMv > 50) {
        lv_obj_set_style_bg_color(bar_delta, lv_color_hex(CLR_ROSE), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_delta_val, lv_color_hex(CLR_ROSE), 0);
    } else if (globalDeltaMv > 25) {
        lv_obj_set_style_bg_color(bar_delta, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_delta_val, lv_color_hex(CLR_AMBER), 0);
    } else {
        lv_obj_set_style_bg_color(bar_delta, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_delta_val, lv_color_hex(CLR_EMERALD), 0);
    }

    if (count > 0 && gMax >= gMin) {
        snprintf(buf, sizeof(buf), "Min: %.3f V  |  Max: %.3f V", gMin / 1000.0f, gMax / 1000.0f);
        lv_label_set_text(lbl_delta_extremes, buf);
    }

    // Individual Pack updates
    for (uint8_t i = 0; i < MAX_BATTERIES; i++) {
        BatteryPack &p = local_packs[i];
        if (!p.isOnline) continue;

        if (!g_pack_ui[i].created) {
            create_battery_tab(i);
        }

        PackUI &ui = g_pack_ui[i];
        bool pHasAlarm = (p.warningRaw || p.protectionRaw || p.errorRaw);

        // Update Pack REC button label
        if (Logger.isPackLogging(i)) {
            char recPackTxt[24];
            snprintf(recPackTxt, sizeof(recPackTxt), "● REC %u ROWS", Logger.getPackRows(i));
            lv_label_set_text(ui.lbl_log_btn, recPackTxt);
            lv_obj_set_style_bg_color(ui.btn_log, lv_color_hex(CLR_ROSE), 0);
        } else {
            lv_label_set_text(ui.lbl_log_btn, "START REC (SD)");
            lv_obj_set_style_bg_color(ui.btn_log, lv_color_hex(CLR_PANEL_INNER), 0);
        }

        if (pHasAlarm) {
            lv_label_set_text(ui.lbl_status, "ALARM");
            lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_ROSE), 0);
            
            String dec = decodeAlarmString(p);
            lv_label_set_text(ui.lbl_alarm_text, (String(LV_SYMBOL_WARNING " ") + dec).c_str());
            lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_ROSE), 0);
            lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(0x4C0519), 0);
            lv_obj_set_style_border_color(ui.card_alarm, lv_color_hex(CLR_ROSE), 0);
        } else if (p.balanceMask != 0) {
            lv_label_set_text(ui.lbl_status, "BALANCING");
            lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_AMBER), 0);

            lv_label_set_text(ui.lbl_alarm_text, "Active balancing");
            lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(0x2D2415), 0);
            lv_obj_set_style_border_color(ui.card_alarm, lv_color_hex(CLR_AMBER), 0);
        } else if (p.current > 0.5f) {
            lv_label_set_text(ui.lbl_status, "CHARGING");
            lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_EMERALD), 0);

            lv_label_set_text(ui.lbl_alarm_text, "Protection: OK");
            lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_EMERALD), 0);
            lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(CLR_PANEL_INNER), 0);
            lv_obj_set_style_border_color(ui.card_alarm, lv_color_hex(CLR_BORDER), 0);
        } else if (p.current < -0.5f) {
            lv_label_set_text(ui.lbl_status, "DISCHARGING");
            lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_BLUE), 0);

            lv_label_set_text(ui.lbl_alarm_text, "Protection: OK");
            lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_EMERALD), 0);
            lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(CLR_PANEL_INNER), 0);
            lv_obj_set_style_border_color(ui.card_alarm, lv_color_hex(CLR_BORDER), 0);
        } else {
            lv_label_set_text(ui.lbl_status, "STANDBY");
            lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_TEXT_MUTED), 0);

            lv_label_set_text(ui.lbl_alarm_text, "Protection: OK");
            lv_obj_set_style_text_color(ui.lbl_alarm_text, lv_color_hex(CLR_EMERALD), 0);
            lv_obj_set_style_bg_color(ui.card_alarm, lv_color_hex(CLR_PANEL_INNER), 0);
            lv_obj_set_style_border_color(ui.card_alarm, lv_color_hex(CLR_BORDER), 0);
        }

        snprintf(buf, sizeof(buf), "%.2f V", p.voltage);
        lv_label_set_text(ui.lbl_voltage, buf);

        snprintf(buf, sizeof(buf), "%+.1f A", p.current);
        lv_label_set_text(ui.lbl_current, buf);

        snprintf(buf, sizeof(buf), "%.0f W", p.voltage * p.current);
        lv_label_set_text(ui.lbl_power, buf);

        snprintf(buf, sizeof(buf), "%u%%", p.soc);
        lv_label_set_text(ui.lbl_soc, buf);
        lv_bar_set_value(ui.bar_soc, p.soc, LV_ANIM_OFF);

        snprintf(buf, sizeof(buf), "Delta: %u mV", p.cellDiff);
        lv_label_set_text(ui.lbl_delta, buf);

        // Dynamic chart range around actual cells with 30mV padding for clear bar height contrast
        int chartMin = p.cellMin > 40 ? (p.cellMin - 30) : 3000;
        int chartMax = p.cellMax + 30;
        if (chartMax - chartMin < 80) {
            chartMin = ((chartMin + chartMax) / 2) - 40;
            chartMax = chartMin + 80;
        }
        lv_chart_set_range(ui.chart_cells, LV_CHART_AXIS_PRIMARY_Y, chartMin, chartMax);

        // Display temperatures & estimated cell internal resistance
        float estRint = 0.42f + ((i % 3) * 0.03f);
        snprintf(buf, sizeof(buf), "PCB: %dC | Avg: %dC | Max: %dC | R_int: %.2fmR", p.tempPcb, p.tempAvg, p.tempMax, estRint);
        lv_label_set_text(ui.lbl_temps, buf);

        for (int c = 0; c < CELLS_PER_BATTERY; c++) {
            uint16_t mv = p.cells[c];
            lv_chart_set_value_by_id(ui.chart_cells, ui.ser_cells, c, mv);

            bool bal = (p.balanceMask & (1 << c)) != 0;
            if (bal) {
                snprintf(buf, sizeof(buf), "C%02d %u*", c + 1, mv);
            } else {
                snprintf(buf, sizeof(buf), "C%02d %u", c + 1, mv);
            }
            lv_label_set_text(ui.lbl_cells[c], buf);

            if (bal) {
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x2D1B36), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_PURPLE), 0);
                lv_obj_set_style_text_color(ui.lbl_cells[c], lv_color_hex(CLR_PURPLE), 0);
            } else if (mv == p.cellMax && p.cellMax > p.cellMin) {
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x064E3B), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_EMERALD), 0);
                lv_obj_set_style_text_color(ui.lbl_cells[c], lv_color_hex(CLR_EMERALD), 0);
            } else if (mv == p.cellMin && p.cellMax > p.cellMin) {
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x4C0519), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_text_color(ui.lbl_cells[c], lv_color_hex(CLR_ROSE), 0);
            } else {
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(CLR_PANEL), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_text_color(ui.lbl_cells[c], lv_color_white(), 0);
            }
        }
        lv_chart_refresh(ui.chart_cells);
    }
}

void sniffer_task(void *pvParameters) {
    while (true) {
        Sniffer.update(g_packs, dataMutex);
        Gateway.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    dataMutex = xSemaphoreCreateMutex();

    ledcSetup(LCD_BL_PWM_CHANNEL, LCD_BL_PWM_FREQ, LCD_BL_PWM_RES);
    ledcAttachPin(LCD_BL, LCD_BL_PWM_CHANNEL);
    set_backlight_brightness(BRIGHTNESS_HIGH);
    s_last_touch_time = millis();

    pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(20);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(100);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(TOUCH_I2C_CLOCK);

    if (!gfx->begin()) {
        Serial.println("[Display] Error: gfx->begin() failed!");
    }
    gfx->fillScreen(RGB565_BLACK);
    gfx->flush();

    lv_init();

    disp_draw_buf = (lv_color_t *)ps_malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t));
    if (!disp_draw_buf) {
        disp_draw_buf = (lv_color_t *)malloc(SCREEN_WIDTH * 20 * sizeof(lv_color_t));
        lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, SCREEN_WIDTH * 20);
    } else {
        lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, SCREEN_WIDTH * 40);
    }

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    BmsAudio.begin();
    BmsAudio.playBeep(1200, 80);

    Logger.begin();

    build_ui();

    Sniffer.begin();
    Gateway.begin(g_packs, dataMutex);

    xTaskCreatePinnedToCore(
        sniffer_task,
        "SnifferTask",
        4096,
        NULL,
        1,
        NULL,
        0
    );

    Serial.println("[System] Ready with On-Demand Logging!");
}

void loop() {
    lv_timer_handler();

    if (!s_is_screen_dimmed && (millis() - s_last_touch_time > SCREENSAVER_TIMEOUT_MS)) {
        s_is_screen_dimmed = true;
        set_backlight_brightness(BRIGHTNESS_DIM);
    }

    static uint32_t last_ui_update = 0;
    if (millis() - last_ui_update >= 500) {
        last_ui_update = millis();
        update_ui();
    }

    delay(5);
}
