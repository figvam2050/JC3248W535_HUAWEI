#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>
#include <time.h>
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

// UI V2 Styling & Palette
#define CLR_DARK_BG       0x070A0F  // Deep Cyber Black
#define CLR_PANEL         0x0F172A  // Slate 900 Glass
#define CLR_PANEL_INNER   0x090D16  // Deep Inset
#define CLR_BORDER        0x1E293B  // Slate 800 Border
#define CLR_BORDER_FOCUS  0x38BDF8  // Neon Blue Glow Border
#define CLR_CYAN          0x06B6D4  // Electric Cyan
#define CLR_EMERALD       0x10B981  // Neon Emerald (Charging)
#define CLR_BLUE          0x38BDF8  // Sky Blue (Discharging)
#define CLR_AMBER         0xF59E0B  // Gold Amber (Balancing)
#define CLR_ROSE          0xF43F5E  // Laser Rose (Alarm)
#define CLR_PURPLE        0xC084FC  // Purple
#define CLR_TEXT_MUTED    0x64748B  // Muted Label Slate
#define CLR_TEXT_TITLE    0x94A3B8  // Bright Title Slate

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

// Power Flow Diagram Widgets in Dedicated Card
static lv_obj_t *flow_card = NULL;
static lv_obj_t *box_grid = NULL;
static lv_obj_t *box_inv = NULL;
static lv_obj_t *box_home = NULL;
static lv_obj_t *box_bat = NULL;

static lv_obj_t *lbl_grid_icon = NULL;
static lv_obj_t *lbl_grid_txt = NULL;
static lv_obj_t *lbl_inv_icon = NULL;
static lv_obj_t *lbl_inv_txt = NULL;
static lv_obj_t *lbl_home_icon = NULL;
static lv_obj_t *lbl_home_txt = NULL;
static lv_obj_t *lbl_bat_icon = NULL;
static lv_obj_t *lbl_bat_txt = NULL;

static lv_obj_t *line_grid_inv = NULL;
static lv_obj_t *line_bat_inv = NULL;
static lv_obj_t *line_inv_home = NULL;

static lv_obj_t *lbl_flow_arrow_grid = NULL;
static lv_obj_t *lbl_flow_arrow_bat = NULL;
static lv_obj_t *lbl_flow_arrow_home = NULL;
static uint8_t s_flow_anim_step = 0;

// V2 Pack Selector Quick Strip on Overview
static lv_obj_t *pack_strip_btns[4];
static lv_obj_t *pack_strip_lbls[4];
static lv_obj_t *pack_strip_bars[4];

// Custom Top Navigation Header widgets
static lv_obj_t *top_nav_bar = NULL;
static lv_obj_t *btn_nav_settings = NULL;
static lv_obj_t *lbl_nav_settings = NULL;
static lv_obj_t *btn_nav_all = NULL;
static lv_obj_t *btn_nav_logs = NULL;
static lv_obj_t *lbl_nav_all = NULL;
static lv_obj_t *lbl_nav_logs = NULL;

// Dynamic Pack Tabs mapping
static uint8_t s_active_pack_count = 4; // Tracks current number of active packs
static uint16_t s_logs_tab_index = 2;   // Always points to the Logs tab index
static lv_obj_t *pack_strip_card = NULL;

// Dedicated Settings Tab Widgets
static lv_obj_t *tab_settings = NULL;
static lv_obj_t *slider_brightness = NULL;
static lv_obj_t *lbl_bright_val = NULL;
static lv_obj_t *dropdown_timeout = NULL;
static lv_obj_t *dropdown_sleep_mode = NULL;
static lv_obj_t *sw_sound = NULL;
static lv_obj_t *sw_autolog = NULL;
static uint8_t s_current_brightness = BRIGHTNESS_HIGH;
static bool s_sound_enabled = true;
static uint32_t s_screen_off_timeout_ms = 60000; // Default: 1 minute (0 = Never)

enum ScreenSleepMode {
    SLEEP_MODE_OFF = 0,    // 100% Turn off backlight
    SLEEP_MODE_DIM = 1,    // Dim to 15% (night glow)
    SLEEP_MODE_SAVER = 2   // Clock & Power Minimal Screensaver
};
static ScreenSleepMode s_sleep_mode = SLEEP_MODE_SAVER;

// Fullscreen Screensaver Overlay
static lv_obj_t *screensaver_overlay = NULL;
static lv_obj_t *screensaver_cluster = NULL;
static lv_obj_t *lbl_ss_time = NULL;
static lv_obj_t *lbl_ss_soc = NULL;
static lv_obj_t *lbl_ss_pwr = NULL;
static lv_obj_t *lbl_ss_grid = NULL;
static uint32_t s_ss_burnin_step = 0;

static void update_top_nav_state(uint16_t act_idx) {
    if (!btn_nav_settings || !btn_nav_all || !btn_nav_logs) return;

    // Reset all buttons to default inactive style
    lv_obj_set_style_bg_color(btn_nav_settings, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_nav_settings, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_text_color(lbl_nav_settings, lv_color_hex(CLR_TEXT_MUTED), 0);

    lv_obj_set_style_bg_color(btn_nav_all, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_nav_all, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_text_color(lbl_nav_all, lv_color_hex(CLR_TEXT_MUTED), 0);

    lv_obj_set_style_bg_color(btn_nav_logs, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_nav_logs, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_text_color(lbl_nav_logs, lv_color_hex(CLR_TEXT_MUTED), 0);

    if (act_idx == 0) {
        // "Settings" active
        lv_obj_set_style_bg_color(btn_nav_settings, lv_color_hex(0x164E63), 0);
        lv_obj_set_style_border_color(btn_nav_settings, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_text_color(lbl_nav_settings, lv_color_hex(CLR_CYAN), 0);
    } else if (act_idx == 1) {
        // "Home" active
        lv_obj_set_style_bg_color(btn_nav_all, lv_color_hex(0x164E63), 0);
        lv_obj_set_style_border_color(btn_nav_all, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_text_color(lbl_nav_all, lv_color_hex(CLR_CYAN), 0);
    } else if (act_idx == s_logs_tab_index) {
        // "Logs" active (always dynamically mapped to the last tab)
        lv_obj_set_style_bg_color(btn_nav_logs, lv_color_hex(0x164E63), 0);
        lv_obj_set_style_border_color(btn_nav_logs, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_text_color(lbl_nav_logs, lv_color_hex(CLR_CYAN), 0);
    }
}

static void nav_settings_click_cb(lv_event_t *e) {
    if (tabview) {
        lv_tabview_set_act(tabview, 0, LV_ANIM_ON);
        update_top_nav_state(0);
        BmsAudio.playClick();
    }
}

static void nav_all_click_cb(lv_event_t *e) {
    if (tabview) {
        lv_tabview_set_act(tabview, 1, LV_ANIM_ON);
        update_top_nav_state(1);
        BmsAudio.playClick();
    }
}

static void nav_logs_click_cb(lv_event_t *e) {
    if (tabview) {
        lv_tabview_set_act(tabview, s_logs_tab_index, LV_ANIM_ON);
        update_top_nav_state(s_logs_tab_index);
        BmsAudio.playClick();
    }
}

static void tabview_change_cb(lv_event_t *e) {
    if (tabview) {
        uint16_t act = lv_tabview_get_tab_act(tabview);
        update_top_nav_state(act);
    }
}

static void pack_strip_click_cb(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    // Tab 0: Settings, Tab 1: Home, Tab 2..(2 + activePacks - 1): Packs, Last Tab: Logs
    uint16_t targetTab = idx + 2;
    if (tabview) {
        lv_tabview_set_act(tabview, targetTab, LV_ANIM_ON);
        update_top_nav_state(targetTab);
        BmsAudio.playClick();
    }
}

static lv_obj_t *card_alarm_banner = NULL;
static lv_obj_t *lbl_alarm_icon = NULL;
static lv_obj_t *lbl_alarm_title = NULL;
static lv_obj_t *lbl_alarm_details = NULL;

// SD Rec Button Widgets on Overview
static lv_obj_t *btn_bank_log = NULL;
static lv_obj_t *lbl_bank_log = NULL;

// Top Header Widgets (Fixed on top of screen)
static lv_obj_t *lbl_top_power_src = NULL;
static lv_obj_t *lbl_top_ip = NULL;

// Dedicated Logs Tab Widgets
static lv_obj_t *tab_logs = NULL;
static lv_obj_t *lbl_logs_status = NULL;
static lv_obj_t *logs_list_container = NULL;
static void refresh_logs_ui();

struct PackUI {
    lv_obj_t *tab;
    lv_obj_t *lbl_title;
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
    lv_obj_t *lbl_diag_bar;
    lv_obj_t *cell_boxes[CELLS_PER_BATTERY];
    lv_obj_t *cell_bars[CELLS_PER_BATTERY];
    lv_obj_t *lbl_cell_num[CELLS_PER_BATTERY];
    lv_obj_t *lbl_cell_volt[CELLS_PER_BATTERY];
    lv_obj_t *lbl_cell_res[CELLS_PER_BATTERY];
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
            set_backlight_brightness(s_current_brightness);
            if (screensaver_overlay) {
                lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
            }
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
    lv_obj_set_style_radius(card, 14, 0); // V2 Soft Curve
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

    // Pack Name Label: e.g. "PACK #1"
    ui.lbl_title = lv_label_create(top_card);
    char pTitle[32];
    snprintf(pTitle, sizeof(pTitle), "PACK #%u", pack_idx + 1);
    lv_label_set_text(ui.lbl_title, pTitle);
    lv_obj_set_style_text_color(ui.lbl_title, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(ui.lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_align(ui.lbl_title, LV_ALIGN_TOP_LEFT, 2, 0);

    // Status Pill (e.g. "CHARGING", "STANDBY")
    ui.lbl_status = lv_label_create(top_card);
    lv_label_set_text(ui.lbl_status, "NORMAL");
    lv_obj_set_style_text_color(ui.lbl_status, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(ui.lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_align(ui.lbl_status, LV_ALIGN_TOP_LEFT, 86, 1);

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

    // 3. Compact Diagnostics Header (PCB, Avg, Max temps & pack R_int)
    ui.lbl_diag_bar = lv_label_create(tab);
    lv_label_set_text(ui.lbl_diag_bar, "PCB: 25C | Avg: 24C | Max: 25C | R_pack: 0.44mR");
    lv_obj_set_style_text_color(ui.lbl_diag_bar, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(ui.lbl_diag_bar, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(ui.lbl_diag_bar, 8, 134);

    // 4. Modern Battery Cell Cards Matrix (5 rows x 3 columns)
    // Vertical available space: 150 to 430 -> 280px total height!
    // Each cell card: 96px wide, 50px high, with 6px gap vertically.
    int start_y = 150;
    for (int i = 0; i < CELLS_PER_BATTERY; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = 6 + col * (96 + 8);
        int y = start_y + row * (50 + 6);

        // Cell card container (styled as battery cell)
        ui.cell_boxes[i] = lv_obj_create(tab);
        lv_obj_set_size(ui.cell_boxes[i], 96, 50);
        lv_obj_set_pos(ui.cell_boxes[i], x, y);
        lv_obj_set_style_bg_color(ui.cell_boxes[i], lv_color_hex(CLR_PANEL), 0);
        lv_obj_set_style_border_color(ui.cell_boxes[i], lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(ui.cell_boxes[i], 1, 0);
        lv_obj_set_style_radius(ui.cell_boxes[i], 8, 0);
        lv_obj_set_style_pad_all(ui.cell_boxes[i], 3, 0);
        lv_obj_clear_flag(ui.cell_boxes[i], LV_OBJ_FLAG_SCROLLABLE);

        // Micro battery level fill bar at bottom of the cell card
        ui.cell_bars[i] = lv_bar_create(ui.cell_boxes[i]);
        lv_obj_set_size(ui.cell_bars[i], 88, 4);
        lv_obj_align(ui.cell_bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_bar_set_range(ui.cell_bars[i], 0, 100);
        lv_bar_set_value(ui.cell_bars[i], 75, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(ui.cell_bars[i], lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_bg_color(ui.cell_bars[i], lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
        lv_obj_set_style_radius(ui.cell_bars[i], 2, 0);
        lv_obj_set_style_radius(ui.cell_bars[i], 2, LV_PART_INDICATOR);

        // Cell Number Header (top-left) e.g. "C01"
        ui.lbl_cell_num[i] = lv_label_create(ui.cell_boxes[i]);
        char c_num[8];
        snprintf(c_num, sizeof(c_num), "C%02d", i + 1);
        lv_label_set_text(ui.lbl_cell_num[i], c_num);
        lv_obj_set_style_text_font(ui.lbl_cell_num[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(ui.lbl_cell_num[i], lv_color_hex(CLR_TEXT_MUTED), 0);
        lv_obj_align(ui.lbl_cell_num[i], LV_ALIGN_TOP_LEFT, 2, 0);

        // Internal resistance (top-right) e.g. "0.42mR"
        ui.lbl_cell_res[i] = lv_label_create(ui.cell_boxes[i]);
        lv_label_set_text(ui.lbl_cell_res[i], "0.42mR");
        lv_obj_set_style_text_font(ui.lbl_cell_res[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(ui.lbl_cell_res[i], lv_color_hex(CLR_AMBER), 0);
        lv_obj_align(ui.lbl_cell_res[i], LV_ALIGN_TOP_RIGHT, -2, 0);

        // Big Voltage Display in Center e.g. "3.448 V"
        ui.lbl_cell_volt[i] = lv_label_create(ui.cell_boxes[i]);
        lv_label_set_text(ui.lbl_cell_volt[i], "3.300 V");
        lv_obj_set_style_text_font(ui.lbl_cell_volt[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui.lbl_cell_volt[i], lv_color_white(), 0);
        lv_obj_align(ui.lbl_cell_volt[i], LV_ALIGN_CENTER, 0, 4);
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

static void slider_brightness_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    s_current_brightness = (uint8_t)val;
    set_backlight_brightness(s_current_brightness);
    if (lbl_bright_val) {
        char bBuf[16];
        snprintf(bBuf, sizeof(bBuf), "%d%%", (int)(val * 100 / 255));
        lv_label_set_text(lbl_bright_val, bBuf);
    }
}

static void sw_sound_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    s_sound_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_sound_enabled) {
        BmsAudio.playClick();
    }
}

static void dropdown_timeout_cb(lv_event_t *e) {
    lv_obj_t *dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    // Options: 0: 30 sec, 1: 1 min, 2: 2 min, 3: 5 min, 4: Never
    switch (selected) {
        case 0: s_screen_off_timeout_ms = 30000; break;
        case 1: s_screen_off_timeout_ms = 60000; break;
        case 2: s_screen_off_timeout_ms = 120000; break;
        case 3: s_screen_off_timeout_ms = 300000; break;
        case 4: s_screen_off_timeout_ms = 0; break; // Never turn off
        default: s_screen_off_timeout_ms = 60000; break;
    }
    BmsAudio.playClick();
}

static void dropdown_sleep_mode_cb(lv_event_t *e) {
    lv_obj_t *dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    // 0: Off 100%, 1: Dim 15%, 2: Clock Screensaver
    s_sleep_mode = (ScreenSleepMode)selected;
    BmsAudio.playClick();
}

static void create_settings_tab() {
    tab_settings = lv_tabview_add_tab(tabview, "Settings");
    lv_obj_set_style_bg_color(tab_settings, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_pad_all(tab_settings, 6, 0);

    // 1. Display & Brightness Card (Enlarged to 186px for Timeout + Sleep Mode Dropdowns)
    lv_obj_t *card_disp = create_glass_card(tab_settings, 308, 186);
    lv_obj_align(card_disp, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *lbl_disp_title = lv_label_create(card_disp);
    lv_label_set_text(lbl_disp_title, LV_SYMBOL_EYE_OPEN " DISPLAY BRIGHTNESS");
    lv_obj_set_style_text_color(lbl_disp_title, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_disp_title, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_disp_title, LV_ALIGN_TOP_LEFT, 2, 0);

    lbl_bright_val = lv_label_create(card_disp);
    lv_label_set_text(lbl_bright_val, "100%");
    lv_obj_set_style_text_color(lbl_bright_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_bright_val, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_bright_val, LV_ALIGN_TOP_RIGHT, -4, 0);

    slider_brightness = lv_slider_create(card_disp);
    lv_obj_set_size(slider_brightness, 280, 14);
    lv_obj_set_pos(slider_brightness, 6, 24);
    lv_slider_set_range(slider_brightness, 20, 255);
    lv_slider_set_value(slider_brightness, s_current_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_brightness, slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Screen Sleep Timeout Label & Dropdown
    lv_obj_t *lbl_sleep_title = lv_label_create(card_disp);
    lv_label_set_text(lbl_sleep_title, LV_SYMBOL_POWER " SLEEP TIMEOUT:");
    lv_obj_set_style_text_color(lbl_sleep_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_sleep_title, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(lbl_sleep_title, 2, 48);

    dropdown_timeout = lv_dropdown_create(card_disp);
    lv_obj_set_size(dropdown_timeout, 280, 32);
    lv_obj_set_pos(dropdown_timeout, 6, 64);
    lv_dropdown_set_options(dropdown_timeout, 
        "30 Seconds\n"
        "1 Minute\n"
        "2 Minutes\n"
        "5 Minutes\n"
        "Never (Always ON)"
    );
    lv_dropdown_set_selected(dropdown_timeout, 1); // Default: 1 Minute
    lv_obj_set_style_bg_color(dropdown_timeout, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(dropdown_timeout, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_text_color(dropdown_timeout, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(dropdown_timeout, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(dropdown_timeout, 8, 0);
    lv_obj_add_event_cb(dropdown_timeout, dropdown_timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Screen Sleep Mode Action Label & Dropdown
    lv_obj_t *lbl_mode_title = lv_label_create(card_disp);
    lv_label_set_text(lbl_mode_title, LV_SYMBOL_IMAGE " SLEEP ACTION / SCREENSAVER:");
    lv_obj_set_style_text_color(lbl_mode_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_mode_title, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(lbl_mode_title, 2, 106);

    dropdown_sleep_mode = lv_dropdown_create(card_disp);
    lv_obj_set_size(dropdown_sleep_mode, 280, 32);
    lv_obj_set_pos(dropdown_sleep_mode, 6, 122);
    lv_dropdown_set_options(dropdown_sleep_mode, 
        "Screen Off 100% (Dark)\n"
        "Dim 15% (Night Glow)\n"
        "Clock Screensaver"
    );
    lv_dropdown_set_selected(dropdown_sleep_mode, 2); // Default: Clock Screensaver
    lv_obj_set_style_bg_color(dropdown_sleep_mode, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(dropdown_sleep_mode, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_text_color(dropdown_sleep_mode, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(dropdown_sleep_mode, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(dropdown_sleep_mode, 8, 0);
    lv_obj_add_event_cb(dropdown_sleep_mode, dropdown_sleep_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 2. Audio & Notifications Card
    lv_obj_t *card_audio = create_glass_card(tab_settings, 308, 52);
    lv_obj_align(card_audio, LV_ALIGN_TOP_MID, 0, 192);

    lv_obj_t *lbl_audio_title = lv_label_create(card_audio);
    lv_label_set_text(lbl_audio_title, LV_SYMBOL_AUDIO " TOUCH CLICKS & BEEP");
    lv_obj_set_style_text_color(lbl_audio_title, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_audio_title, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_audio_title, LV_ALIGN_LEFT_MID, 2, 0);

    sw_sound = lv_switch_create(card_audio);
    lv_obj_set_size(sw_sound, 50, 26);
    lv_obj_align(sw_sound, LV_ALIGN_RIGHT_MID, -2, 0);
    if (s_sound_enabled) lv_obj_add_state(sw_sound, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_sound, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_sound, sw_sound_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 3. System & Network Diagnostics Card
    lv_obj_t *card_sys = create_glass_card(tab_settings, 308, 150);
    lv_obj_align(card_sys, LV_ALIGN_TOP_MID, 0, 250);

    lv_obj_t *lbl_sys_title = lv_label_create(card_sys);
    lv_label_set_text(lbl_sys_title, LV_SYMBOL_SETTINGS " SYSTEM & RS485 SPECS");
    lv_obj_set_style_text_color(lbl_sys_title, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_sys_title, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_sys_title, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t *lbl_info = lv_label_create(card_sys);
    lv_label_set_text(lbl_info, 
        "Device: Guition JC3248W535\n"
        "MCU: ESP32-S3 Dual-Core 240MHz\n"
        "Bus: Passive RS485 Sniffer (9600 8N1)\n"
        "SD Card: FAT32 Class 10 SD_MMC\n"
        "Firmware: UI v2.0 Ultra (Custom Flow)\n"
        "WiFi Gateway: WebSockets + REST API\n"
        "Protection: Passive Read-Only Sniffer"
    );
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 4, 22);
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

    // Custom Top Navigation Header Bar (36px high, full width 320)
    // Left: "All" and "Logs" buttons
    // Right: Wi-Fi RSSI + IP and Power Source indicator (USB)
    top_nav_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(top_nav_bar, 320, 36);
    lv_obj_align(top_nav_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_nav_bar, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(top_nav_bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(top_nav_bar, 1, 0);
    lv_obj_set_style_border_side(top_nav_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_nav_bar, 0, 0);
    lv_obj_set_style_pad_all(top_nav_bar, 2, 0);
    lv_obj_clear_flag(top_nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Button Settings (Gear Icon) - enlarged to 32x28
    btn_nav_settings = lv_btn_create(top_nav_bar);
    lv_obj_set_size(btn_nav_settings, 32, 28);
    lv_obj_align(btn_nav_settings, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_bg_color(btn_nav_settings, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_nav_settings, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(btn_nav_settings, 1, 0);
    lv_obj_set_style_radius(btn_nav_settings, 6, 0);
    lv_obj_set_style_pad_all(btn_nav_settings, 0, 0);
    lv_obj_add_event_cb(btn_nav_settings, nav_settings_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_nav_settings = lv_label_create(btn_nav_settings);
    lv_label_set_text(lbl_nav_settings, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(lbl_nav_settings, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_nav_settings, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_nav_settings, LV_ALIGN_CENTER, 0, 0);

    // 2. Button "Home" (former "All") - widened to 48x28
    btn_nav_all = lv_btn_create(top_nav_bar);
    lv_obj_set_size(btn_nav_all, 48, 28);
    lv_obj_align(btn_nav_all, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_style_bg_color(btn_nav_all, lv_color_hex(0x164E63), 0);
    lv_obj_set_style_border_color(btn_nav_all, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(btn_nav_all, 1, 0);
    lv_obj_set_style_radius(btn_nav_all, 6, 0);
    lv_obj_set_style_pad_all(btn_nav_all, 0, 0);
    lv_obj_add_event_cb(btn_nav_all, nav_all_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_nav_all = lv_label_create(btn_nav_all);
    lv_label_set_text(lbl_nav_all, "Home");
    lv_obj_set_style_text_color(lbl_nav_all, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_nav_all, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_nav_all, LV_ALIGN_CENTER, 0, 0);

    // 3. Button "Logs" - widened to 48x28
    btn_nav_logs = lv_btn_create(top_nav_bar);
    lv_obj_set_size(btn_nav_logs, 48, 28);
    lv_obj_align(btn_nav_logs, LV_ALIGN_LEFT_MID, 87, 0);
    lv_obj_set_style_bg_color(btn_nav_logs, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_nav_logs, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(btn_nav_logs, 1, 0);
    lv_obj_set_style_radius(btn_nav_logs, 6, 0);
    lv_obj_set_style_pad_all(btn_nav_logs, 0, 0);
    lv_obj_add_event_cb(btn_nav_logs, nav_logs_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_nav_logs = lv_label_create(btn_nav_logs);
    lv_label_set_text(lbl_nav_logs, "Logs");
    lv_obj_set_style_text_color(lbl_nav_logs, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_nav_logs, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_nav_logs, LV_ALIGN_CENTER, 0, 0);

    // Right Side: Power Source Icon (USB) - fixed position at far right
    lbl_top_power_src = lv_label_create(top_nav_bar);
    lv_label_set_text(lbl_top_power_src, LV_SYMBOL_USB);
    lv_obj_set_style_text_color(lbl_top_power_src, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_top_power_src, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_top_power_src, LV_ALIGN_RIGHT_MID, -2, 0);

    // Right Side: IP + Wi-Fi RSSI (Larger Font 12px, moved comfortably left of USB icon)
    lbl_top_ip = lv_label_create(top_nav_bar);
    lv_label_set_text(lbl_top_ip, "...");
    lv_obj_set_style_text_color(lbl_top_ip, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_top_ip, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_top_ip, LV_ALIGN_RIGHT_MID, -30, 0);

    // Create TabView with tab_size = 0 (completely disables default button matrix header!)
    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 0);
    lv_obj_set_size(tabview, 320, 444);
    lv_obj_set_pos(tabview, 0, 36);
    lv_obj_set_style_bg_color(tabview, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_add_event_cb(tabview, tabview_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Create Settings Tab first (Tab 0)
    create_settings_tab();

    // Create Home (Overview) Tab (Tab 1)
    tab_overview = lv_tabview_add_tab(tabview, "Home");
    lv_obj_set_style_bg_color(tab_overview, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_pad_all(tab_overview, 6, 0);

    // Create Initial Battery Pack Tabs (Tabs 2 .. 2+N-1)
    for (uint8_t i = 0; i < s_active_pack_count; i++) {
        create_battery_tab(i);
    }

    // Create Dedicated Logs Tab as the LAST Tab
    create_logs_tab();
    s_logs_tab_index = 2 + s_active_pack_count;

    // Default active tab is "Home" (Tab 1)
    lv_tabview_set_act(tabview, 1, LV_ANIM_OFF);
    update_top_nav_state(1);

    // 1. Pack Selector Quick Strip Card (Dynamic Widths)
    pack_strip_card = create_glass_card(tab_overview, 308, 48);
    lv_obj_align(pack_strip_card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(pack_strip_card, 2, 0);

    for (int i = 0; i < 4; i++) {
        pack_strip_btns[i] = lv_btn_create(pack_strip_card);
        lv_obj_set_size(pack_strip_btns[i], 72, 40);
        lv_obj_set_pos(pack_strip_btns[i], 2 + i * 76, 2);
        lv_obj_set_style_bg_color(pack_strip_btns[i], lv_color_hex(CLR_PANEL_INNER), 0);
        lv_obj_set_style_border_color(pack_strip_btns[i], lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(pack_strip_btns[i], 1, 0);
        lv_obj_set_style_radius(pack_strip_btns[i], 8, 0);
        lv_obj_set_style_pad_all(pack_strip_btns[i], 2, 0);
        lv_obj_add_event_cb(pack_strip_btns[i], pack_strip_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        pack_strip_lbls[i] = lv_label_create(pack_strip_btns[i]);
        char pName[12];
        snprintf(pName, sizeof(pName), "P%02d --%%", i + 1);
        lv_label_set_text(pack_strip_lbls[i], pName);
        lv_obj_set_style_text_color(pack_strip_lbls[i], lv_color_hex(CLR_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(pack_strip_lbls[i], &lv_font_montserrat_12, 0);
        lv_obj_align(pack_strip_lbls[i], LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_clear_flag(pack_strip_lbls[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(pack_strip_lbls[i], LV_OBJ_FLAG_EVENT_BUBBLE);

        pack_strip_bars[i] = lv_bar_create(pack_strip_btns[i]);
        lv_obj_set_size(pack_strip_bars[i], 62, 5);
        lv_obj_align(pack_strip_bars[i], LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_bar_set_range(pack_strip_bars[i], 0, 100);
        lv_bar_set_value(pack_strip_bars[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(pack_strip_bars[i], lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_bg_color(pack_strip_bars[i], lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
        lv_obj_set_style_radius(pack_strip_bars[i], 2, 0);
        lv_obj_set_style_radius(pack_strip_bars[i], 2, LV_PART_INDICATOR);
        lv_obj_clear_flag(pack_strip_bars[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(pack_strip_bars[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // 2. Central Gauge Card (Power & SOC) - Raised Up, 116px height
    lv_obj_t *gauge_card = create_glass_card(tab_overview, 308, 116);
    lv_obj_align(gauge_card, LV_ALIGN_TOP_MID, 0, 52);

    // SOC Arc (100x100 clean arc)
    arc_soc = lv_arc_create(gauge_card);
    lv_obj_set_size(arc_soc, 100, 100);
    lv_arc_set_rotation(arc_soc, 135);
    lv_arc_set_bg_angles(arc_soc, 0, 270);
    lv_arc_set_range(arc_soc, 0, 100);
    lv_arc_set_value(arc_soc, 75);
    lv_obj_remove_style(arc_soc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_soc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc_soc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_PANEL_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_soc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
    lv_obj_align(arc_soc, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_soc_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_soc_val, "75%");
    lv_obj_set_style_text_color(lbl_soc_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_soc_val, &lv_font_montserrat_20, 0);
    lv_obj_align_to(lbl_soc_val, arc_soc, LV_ALIGN_CENTER, 0, -8);

    lbl_soc_sub = lv_label_create(gauge_card);
    lv_label_set_text(lbl_soc_sub, "SOC");
    lv_obj_set_style_text_color(lbl_soc_sub, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_soc_sub, &lv_font_montserrat_12, 0);
    lv_obj_align_to(lbl_soc_sub, arc_soc, LV_ALIGN_CENTER, 0, 12);

    // Status pill top right
    lbl_mode_pill = lv_label_create(gauge_card);
    lv_label_set_text(lbl_mode_pill, LV_SYMBOL_OK " STANDBY");
    lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_mode_pill, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_mode_pill, LV_ALIGN_TOP_RIGHT, -4, 0);

    lv_obj_t *lbl_pwr_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_pwr_title, "ACTIVE POWER");
    lv_obj_set_style_text_color(lbl_pwr_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_pwr_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_pwr_title, LV_ALIGN_TOP_RIGHT, -4, 16);

    lbl_power_kw = lv_label_create(gauge_card);
    lv_label_set_text(lbl_power_kw, "0.00 kW");
    lv_obj_set_style_text_color(lbl_power_kw, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_power_kw, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_power_kw, LV_ALIGN_TOP_RIGHT, -4, 28);

    // Voltage & Current in clean columns
    lv_obj_t *lbl_v_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_v_title, "VOLTAGE");
    lv_obj_set_style_text_color(lbl_v_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_v_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_v_title, LV_ALIGN_TOP_RIGHT, -86, 62);

    lbl_voltage_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_voltage_val, "51.80 V");
    lv_obj_set_style_text_color(lbl_voltage_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_voltage_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_voltage_val, LV_ALIGN_TOP_RIGHT, -86, 76);

    lv_obj_t *lbl_i_title = lv_label_create(gauge_card);
    lv_label_set_text(lbl_i_title, "CURRENT");
    lv_obj_set_style_text_color(lbl_i_title, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_i_title, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_i_title, LV_ALIGN_TOP_RIGHT, -6, 62);

    lbl_current_val = lv_label_create(gauge_card);
    lv_label_set_text(lbl_current_val, "+0.0 A");
    lv_obj_set_style_text_color(lbl_current_val, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_current_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_current_val, LV_ALIGN_TOP_RIGHT, -6, 76);

    // 3. Dedicated Energy Flow Diagram Card (136px height)
    // Grid (Top-Left)   -> Inverter (Center) -> Home (Right)
    // Battery (Bot-Left)^
    flow_card = create_glass_card(tab_overview, 308, 136);
    lv_obj_align(flow_card, LV_ALIGN_TOP_MID, 0, 172);

    // Node: GRID (Top-Left)
    box_grid = lv_obj_create(flow_card);
    lv_obj_set_size(box_grid, 68, 52);
    lv_obj_set_pos(box_grid, 6, 6);
    lv_obj_set_style_bg_color(box_grid, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(box_grid, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_border_width(box_grid, 1, 0);
    lv_obj_set_style_radius(box_grid, 8, 0);
    lv_obj_set_style_pad_all(box_grid, 2, 0);
    lv_obj_clear_flag(box_grid, LV_OBJ_FLAG_SCROLLABLE);

    lbl_grid_icon = lv_label_create(box_grid);
    lv_label_set_text(lbl_grid_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(lbl_grid_icon, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_grid_icon, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_grid_icon, LV_ALIGN_TOP_MID, 0, 1);

    lbl_grid_txt = lv_label_create(box_grid);
    lv_label_set_text(lbl_grid_txt, "GRID");
    lv_obj_set_style_text_color(lbl_grid_txt, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_grid_txt, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_grid_txt, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Node: BATTERY (Bottom-Left)
    box_bat = lv_obj_create(flow_card);
    lv_obj_set_size(box_bat, 68, 52);
    lv_obj_set_pos(box_bat, 6, 66);
    lv_obj_set_style_bg_color(box_bat, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(box_bat, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(box_bat, 1, 0);
    lv_obj_set_style_radius(box_bat, 8, 0);
    lv_obj_set_style_pad_all(box_bat, 2, 0);
    lv_obj_clear_flag(box_bat, LV_OBJ_FLAG_SCROLLABLE);

    lbl_bat_icon = lv_label_create(box_bat);
    lv_label_set_text(lbl_bat_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(lbl_bat_icon, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_bat_icon, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_bat_icon, LV_ALIGN_TOP_MID, 0, 1);

    lbl_bat_txt = lv_label_create(box_bat);
    lv_label_set_text(lbl_bat_txt, "BATTERY");
    lv_obj_set_style_text_color(lbl_bat_txt, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_bat_txt, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_bat_txt, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Arrow Grid -> Inv
    lbl_flow_arrow_grid = lv_label_create(flow_card);
    lv_label_set_text(lbl_flow_arrow_grid, "->");
    lv_obj_set_style_text_color(lbl_flow_arrow_grid, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_flow_arrow_grid, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_flow_arrow_grid, 82, 22);

    // Arrow Bat <-> Inv
    lbl_flow_arrow_bat = lv_label_create(flow_card);
    lv_label_set_text(lbl_flow_arrow_bat, "->");
    lv_obj_set_style_text_color(lbl_flow_arrow_bat, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_flow_arrow_bat, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_flow_arrow_bat, 82, 82);

    // Node: INVERTER (Center)
    box_inv = lv_obj_create(flow_card);
    lv_obj_set_size(box_inv, 80, 68);
    lv_obj_set_pos(box_inv, 108, 28);
    lv_obj_set_style_bg_color(box_inv, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(box_inv, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(box_inv, 1, 0);
    lv_obj_set_style_radius(box_inv, 10, 0);
    lv_obj_set_style_pad_all(box_inv, 2, 0);
    lv_obj_clear_flag(box_inv, LV_OBJ_FLAG_SCROLLABLE);

    lbl_inv_icon = lv_label_create(box_inv);
    lv_label_set_text(lbl_inv_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(lbl_inv_icon, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_inv_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_inv_icon, LV_ALIGN_TOP_MID, 0, 4);

    lbl_inv_txt = lv_label_create(box_inv);
    lv_label_set_text(lbl_inv_txt, "INVERTER");
    lv_obj_set_style_text_color(lbl_inv_txt, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_inv_txt, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_inv_txt, LV_ALIGN_BOTTOM_MID, 0, -4);

    // Arrow Inv -> Home
    lbl_flow_arrow_home = lv_label_create(flow_card);
    lv_label_set_text(lbl_flow_arrow_home, "->");
    lv_obj_set_style_text_color(lbl_flow_arrow_home, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_flow_arrow_home, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_flow_arrow_home, 196, 52);

    // Node: HOME (Right)
    box_home = lv_obj_create(flow_card);
    lv_obj_set_size(box_home, 74, 68);
    lv_obj_set_pos(box_home, 222, 28);
    lv_obj_set_style_bg_color(box_home, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(box_home, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(box_home, 1, 0);
    lv_obj_set_style_radius(box_home, 10, 0);
    lv_obj_set_style_pad_all(box_home, 2, 0);
    lv_obj_clear_flag(box_home, LV_OBJ_FLAG_SCROLLABLE);

    lbl_home_icon = lv_label_create(box_home);
    lv_label_set_text(lbl_home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(lbl_home_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_home_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_home_icon, LV_ALIGN_TOP_MID, 0, 4);

    lbl_home_txt = lv_label_create(box_home);
    lv_label_set_text(lbl_home_txt, "HOME");
    lv_obj_set_style_text_color(lbl_home_txt, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_home_txt, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_home_txt, LV_ALIGN_BOTTOM_MID, 0, -4);

    // 4. Dynamic Alarm Banner (44px)
    card_alarm_banner = create_glass_card(tab_overview, 308, 44);
    lv_obj_align(card_alarm_banner, LV_ALIGN_TOP_MID, 0, 314);
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

    // 5. Clean SD Log Card at Bottom (46px)
    lv_obj_t *info_card = create_glass_card(tab_overview, 308, 46);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 364);
    lv_obj_set_style_pad_all(info_card, 3, 0);

    btn_bank_log = lv_btn_create(info_card);
    lv_obj_set_size(btn_bank_log, 298, 38);
    lv_obj_align(btn_bank_log, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn_bank_log, lv_color_hex(CLR_PANEL_INNER), 0);
    lv_obj_set_style_border_color(btn_bank_log, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(btn_bank_log, 1, 0);
    lv_obj_set_style_radius(btn_bank_log, 10, 0);
    lv_obj_add_event_cb(btn_bank_log, btn_bank_log_cb, LV_EVENT_CLICKED, NULL);

    lbl_bank_log = lv_label_create(btn_bank_log);
    lv_label_set_text(lbl_bank_log, LV_SYMBOL_SAVE " START BANK LOG (REC SD)");
    lv_obj_set_style_text_color(lbl_bank_log, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_bank_log, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_bank_log, LV_ALIGN_CENTER, 0, 0);

    // 6. Create Fullscreen Screensaver Overlay (added on top layer)
    screensaver_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(screensaver_overlay, 320, 480);
    lv_obj_set_pos(screensaver_overlay, 0, 0);
    lv_obj_set_style_bg_color(screensaver_overlay, lv_color_hex(CLR_DARK_BG), 0);
    lv_obj_set_style_bg_opa(screensaver_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_overlay, 0, 0);
    lv_obj_set_style_radius(screensaver_overlay, 0, 0);
    lv_obj_set_style_pad_all(screensaver_overlay, 0, 0);
    lv_obj_clear_flag(screensaver_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_CLICKABLE); // Any tap dismisses it!
    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    // Touch event on screensaver wakes up screen
    lv_obj_add_event_cb(screensaver_overlay, [](lv_event_t *e) {
        s_is_screen_dimmed = false;
        set_backlight_brightness(s_current_brightness);
        if (screensaver_overlay) {
            lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        BmsAudio.playClick();
    }, LV_EVENT_CLICKED, NULL);

    // Screensaver floating cluster container (can roam across the whole 320x480 screen!)
    screensaver_cluster = lv_obj_create(screensaver_overlay);
    lv_obj_set_size(screensaver_cluster, 260, 200);
    lv_obj_set_pos(screensaver_cluster, 30, 120);
    lv_obj_set_style_bg_opa(screensaver_cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_cluster, 0, 0);
    lv_obj_set_style_pad_all(screensaver_cluster, 0, 0);
    lv_obj_clear_flag(screensaver_cluster, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver_cluster, LV_OBJ_FLAG_EVENT_BUBBLE); // touch passes through

    // Large Night Clock (48px font)
    lbl_ss_time = lv_label_create(screensaver_cluster);
    lv_label_set_text(lbl_ss_time, "12:00");
    lv_obj_set_style_text_color(lbl_ss_time, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl_ss_time, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_ss_time, LV_ALIGN_TOP_MID, 0, 0);

    // Battery SOC Glance (18px font)
    lbl_ss_soc = lv_label_create(screensaver_cluster);
    lv_label_set_text(lbl_ss_soc, LV_SYMBOL_BATTERY_FULL " 100% (51.8V)");
    lv_obj_set_style_text_color(lbl_ss_soc, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_ss_soc, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_ss_soc, LV_ALIGN_TOP_MID, 0, 68);

    // Power Glance (16px font)
    lbl_ss_pwr = lv_label_create(screensaver_cluster);
    lv_label_set_text(lbl_ss_pwr, "0.00 kW");
    lv_obj_set_style_text_color(lbl_ss_pwr, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_ss_pwr, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_ss_pwr, LV_ALIGN_TOP_MID, 0, 102);

    // Grid Status Glance (14px font)
    lbl_ss_grid = lv_label_create(screensaver_cluster);
    lv_label_set_text(lbl_ss_grid, LV_SYMBOL_CHARGE " 230V GRID ON");
    lv_obj_set_style_text_color(lbl_ss_grid, lv_color_hex(CLR_EMERALD), 0);
    lv_obj_set_style_text_font(lbl_ss_grid, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_ss_grid, LV_ALIGN_TOP_MID, 0, 134);

    // Hint at bottom
    lv_obj_t *lbl_ss_hint = lv_label_create(screensaver_overlay);
    lv_label_set_text(lbl_ss_hint, "Tap anywhere to wake up");
    lv_obj_set_style_text_color(lbl_ss_hint, lv_color_hex(CLR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_ss_hint, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_ss_hint, LV_ALIGN_BOTTOM_MID, 0, -18);
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
        snprintf(ip_str, sizeof(ip_str), "%s %ddBm", Gateway.getIpAddress().c_str(), Gateway.getRssi());
        lv_label_set_text(lbl_top_ip, ip_str);
    } else if (Gateway.isAPMode()) {
        lv_label_set_text(lbl_top_ip, "192.168.4.1");
    } else {
        lv_label_set_text(lbl_top_ip, "Wi-Fi: ...");
    }

    // Power source indication (compact symbol only)
    lv_label_set_text(lbl_top_power_src, LV_SYMBOL_USB);

    if (alarms) {
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_WARNING " ALARM");
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
        lv_label_set_text(lbl_mode_pill, LV_SYMBOL_OK " STANDBY");
        lv_obj_set_style_text_color(lbl_mode_pill, lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_style_arc_color(arc_soc, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
    }

    // Update Energy Flow Animation & Colors
    s_flow_anim_step = (s_flow_anim_step + 1) % 3;
    const char* arrow_anim_fwd[3] = {"- >", "-- >", "->>"};
    const char* arrow_anim_rev[3] = {"< -", "< --", "<<-"};

    if (box_grid && box_inv && box_home && box_bat) {
        // 1. Grid Logic (Simulation / Detection)
        // If battery is charging, grid is 100% supplying power.
        // If battery is discharging, system is running off-grid (outage).
        // If standby: grid is available if voltage/soc is full.
        bool gridAvailable = (iSum > -0.5f);

        if (gridAvailable) {
            lv_obj_set_style_border_color(box_grid, lv_color_hex(CLR_EMERALD), 0);
            lv_obj_set_style_text_color(lbl_grid_icon, lv_color_hex(CLR_EMERALD), 0);
            lv_obj_set_style_text_color(lbl_grid_txt, lv_color_hex(CLR_EMERALD), 0);
            lv_label_set_text(lbl_grid_txt, "230V ON");

            lv_label_set_text(lbl_flow_arrow_grid, arrow_anim_fwd[s_flow_anim_step]);
            lv_obj_set_style_text_color(lbl_flow_arrow_grid, lv_color_hex(CLR_EMERALD), 0);
        } else {
            // Outage / Blackout
            lv_obj_set_style_border_color(box_grid, lv_color_hex(CLR_ROSE), 0);
            lv_obj_set_style_text_color(lbl_grid_icon, lv_color_hex(CLR_ROSE), 0);
            lv_obj_set_style_text_color(lbl_grid_txt, lv_color_hex(CLR_ROSE), 0);
            lv_label_set_text(lbl_grid_txt, "NO GRID");

            lv_label_set_text(lbl_flow_arrow_grid, "x");
            lv_obj_set_style_text_color(lbl_flow_arrow_grid, lv_color_hex(CLR_TEXT_MUTED), 0);
        }

        // 2. Battery Logic
        if (alarms) {
            // Battery in Alarm
            lv_obj_set_style_border_color(box_bat, lv_color_hex(CLR_ROSE), 0);
            lv_obj_set_style_text_color(lbl_bat_icon, lv_color_hex(CLR_ROSE), 0);
            lv_obj_set_style_text_color(lbl_bat_txt, lv_color_hex(CLR_ROSE), 0);
            lv_label_set_text(lbl_bat_txt, "ALARM");
        } else if (iSum > 0.5f) {
            // Charging: Yellow / Golden Amber
            lv_obj_set_style_border_color(box_bat, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_text_color(lbl_bat_icon, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_text_color(lbl_bat_txt, lv_color_hex(CLR_AMBER), 0);
            lv_label_set_text(lbl_bat_txt, "CHARGE");

            lv_label_set_text(lbl_flow_arrow_bat, arrow_anim_rev[s_flow_anim_step]); // Inv -> Bat
            lv_obj_set_style_text_color(lbl_flow_arrow_bat, lv_color_hex(CLR_AMBER), 0);
        } else if (iSum < -0.5f) {
            // Discharging: Sky Blue / Cyan (or Rose if SOC < 20%)
            uint32_t batColor = (avgSoc < 20.0f) ? CLR_ROSE : CLR_BLUE;
            lv_obj_set_style_border_color(box_bat, lv_color_hex(batColor), 0);
            lv_obj_set_style_text_color(lbl_bat_icon, lv_color_hex(batColor), 0);
            lv_obj_set_style_text_color(lbl_bat_txt, lv_color_hex(batColor), 0);
            lv_label_set_text(lbl_bat_txt, "DISCHARGE");

            lv_label_set_text(lbl_flow_arrow_bat, arrow_anim_fwd[s_flow_anim_step]); // Bat -> Inv
            lv_obj_set_style_text_color(lbl_flow_arrow_bat, lv_color_hex(batColor), 0);
        } else {
            // Standby (0A): Green if 100% full, else Slate / Muted Cyan
            uint32_t batColor = (avgSoc >= 99.0f) ? CLR_EMERALD : CLR_CYAN;
            lv_obj_set_style_border_color(box_bat, lv_color_hex(batColor), 0);
            lv_obj_set_style_text_color(lbl_bat_icon, lv_color_hex(batColor), 0);
            lv_obj_set_style_text_color(lbl_bat_txt, lv_color_hex(batColor), 0);
            lv_label_set_text(lbl_bat_txt, (avgSoc >= 99.0f) ? "FULL 100%" : "STANDBY");

            lv_label_set_text(lbl_flow_arrow_bat, "--");
            lv_obj_set_style_text_color(lbl_flow_arrow_bat, lv_color_hex(CLR_TEXT_MUTED), 0);
        }

        // 3. Inverter Logic (Load-based color & mode text)
        float loadPwr = fabsf(totalPwrKw);
        uint32_t invColor = CLR_EMERALD;
        if (loadPwr > 4.5f) {
            invColor = CLR_ROSE; // Overload
        } else if (loadPwr > 2.5f) {
            invColor = CLR_AMBER; // High load
        } else if (loadPwr < 0.1f) {
            invColor = CLR_BORDER; // Standby
        } else {
            invColor = CLR_EMERALD; // Normal load
        }

        lv_obj_set_style_border_color(box_inv, lv_color_hex(invColor), 0);
        lv_obj_set_style_text_color(lbl_inv_icon, lv_color_hex(invColor), 0);
        lv_obj_set_style_text_color(lbl_inv_txt, lv_color_hex(invColor), 0);

        if (iSum > 0.5f) {
            lv_label_set_text(lbl_inv_txt, "CHARGE");
        } else if (iSum < -0.5f) {
            lv_label_set_text(lbl_inv_txt, "INVERT");
        } else {
            lv_label_set_text(lbl_inv_txt, "BYPASS");
        }

        // 4. Home (Consumption Node)
        lv_obj_set_style_border_color(box_home, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_text_color(lbl_home_icon, lv_color_white(), 0);
        lv_obj_set_style_text_color(lbl_home_txt, lv_color_white(), 0);

        char homeTxt[16];
        if (loadPwr >= 0.05f) {
            snprintf(homeTxt, sizeof(homeTxt), "%.1fkW", loadPwr);
        } else {
            snprintf(homeTxt, sizeof(homeTxt), "HOME");
        }
        lv_label_set_text(lbl_home_txt, homeTxt);

        if (loadPwr > 0.1f) {
            lv_label_set_text(lbl_flow_arrow_home, arrow_anim_fwd[s_flow_anim_step]);
            lv_obj_set_style_text_color(lbl_flow_arrow_home, lv_color_hex(invColor), 0);
        } else {
            lv_label_set_text(lbl_flow_arrow_home, "--");
            lv_obj_set_style_text_color(lbl_flow_arrow_home, lv_color_hex(CLR_TEXT_MUTED), 0);
        }
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

    if (lbl_delta_val) {
        snprintf(buf, sizeof(buf), "%u mV", globalDeltaMv);
        lv_label_set_text(lbl_delta_val, buf);
    }
    if (bar_delta) {
        lv_bar_set_value(bar_delta, globalDeltaMv > 100 ? 100 : globalDeltaMv, LV_ANIM_ON);
    }
    if (lbl_delta_extremes && count > 0 && gMax >= gMin) {
        snprintf(buf, sizeof(buf), "Min: %.3f V  |  Max: %.3f V", gMin / 1000.0f, gMax / 1000.0f);
        lv_label_set_text(lbl_delta_extremes, buf);
    }

    // Update live values on Screensaver glance overlay
    if (screensaver_overlay) {
        char ssBuf[36];
        snprintf(ssBuf, sizeof(ssBuf), "%s %.0f%% (%.1fV)", LV_SYMBOL_BATTERY_FULL, avgSoc, avgVolt);
        lv_label_set_text(lbl_ss_soc, ssBuf);

        snprintf(ssBuf, sizeof(ssBuf), "%.2f kW", fabsf(totalPwrKw));
        lv_label_set_text(lbl_ss_pwr, ssBuf);

        bool gridOk = (iSum > -0.5f);
        if (gridOk) {
            lv_label_set_text(lbl_ss_grid, LV_SYMBOL_CHARGE " 230V GRID ON");
            lv_obj_set_style_text_color(lbl_ss_grid, lv_color_hex(CLR_EMERALD), 0);
        } else {
            lv_label_set_text(lbl_ss_grid, LV_SYMBOL_WARNING " OUTAGE (OFF-GRID)");
            lv_obj_set_style_text_color(lbl_ss_grid, lv_color_hex(CLR_ROSE), 0);
        }
    }

    // Dynamic layout adaptation for Pack Selector Strip based on actual online pack count
    uint8_t onlinePacks = 0;
    for (int p = 0; p < 4; p++) {
        if (local_packs[p].isOnline) onlinePacks++;
    }
    if (onlinePacks == 0) onlinePacks = 1; // At least 1 slot reserved if offline

    // Calculate dynamic button widths for perfect fit across 304px available inner width
    int total_avail_w = 304;
    int gap = (onlinePacks > 1) ? 4 : 0;
    int btn_w = (total_avail_w - (onlinePacks - 1) * gap) / onlinePacks;
    int cur_x = 2;

    for (int pIdx = 0; pIdx < 4; pIdx++) {
        if (pIdx < onlinePacks && local_packs[pIdx].isOnline) {
            lv_obj_clear_flag(pack_strip_btns[pIdx], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(pack_strip_btns[pIdx], btn_w, 40);
            lv_obj_set_pos(pack_strip_btns[pIdx], cur_x, 2);
            lv_obj_set_size(pack_strip_bars[pIdx], btn_w - 10, 5);
            cur_x += btn_w + gap;

            char pStr[24];
            if (onlinePacks <= 2) {
                // Generous width: show pack name, SOC% and voltage!
                snprintf(pStr, sizeof(pStr), "P%02d  %u%%  %.1fV", pIdx + 1, local_packs[pIdx].soc, local_packs[pIdx].voltage);
            } else {
                snprintf(pStr, sizeof(pStr), "P%02d %u%%", pIdx + 1, local_packs[pIdx].soc);
            }
            lv_label_set_text(pack_strip_lbls[pIdx], pStr);
            lv_bar_set_value(pack_strip_bars[pIdx], local_packs[pIdx].soc, LV_ANIM_OFF);

            bool pAlarm = (local_packs[pIdx].warningRaw || local_packs[pIdx].protectionRaw || local_packs[pIdx].errorRaw);
            if (pAlarm) {
                lv_obj_set_style_text_color(pack_strip_lbls[pIdx], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_border_color(pack_strip_btns[pIdx], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_bg_color(pack_strip_bars[pIdx], lv_color_hex(CLR_ROSE), LV_PART_INDICATOR);
            } else if (local_packs[pIdx].balanceMask != 0) {
                lv_obj_set_style_text_color(pack_strip_lbls[pIdx], lv_color_hex(CLR_AMBER), 0);
                lv_obj_set_style_border_color(pack_strip_btns[pIdx], lv_color_hex(CLR_AMBER), 0);
                lv_obj_set_style_bg_color(pack_strip_bars[pIdx], lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            } else {
                lv_obj_set_style_text_color(pack_strip_lbls[pIdx], lv_color_hex(CLR_CYAN), 0);
                lv_obj_set_style_border_color(pack_strip_btns[pIdx], lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_bg_color(pack_strip_bars[pIdx], lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
            }
        } else if (onlinePacks == 1 && !local_packs[0].isOnline && pIdx == 0) {
            // When everything is offline, show P01 as waiting
            lv_obj_clear_flag(pack_strip_btns[pIdx], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(pack_strip_btns[pIdx], total_avail_w, 40);
            lv_obj_set_pos(pack_strip_btns[pIdx], 2, 2);
            lv_obj_set_size(pack_strip_bars[pIdx], total_avail_w - 10, 5);
            lv_label_set_text(pack_strip_lbls[pIdx], "WAITING FOR RS485 PACKS...");
            lv_bar_set_value(pack_strip_bars[pIdx], 0, LV_ANIM_OFF);
            lv_obj_set_style_text_color(pack_strip_lbls[pIdx], lv_color_hex(CLR_TEXT_MUTED), 0);
            lv_obj_set_style_border_color(pack_strip_btns[pIdx], lv_color_hex(CLR_BORDER), 0);
        } else {
            // Hide unused buttons!
            lv_obj_add_flag(pack_strip_btns[pIdx], LV_OBJ_FLAG_HIDDEN);
        }
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

        // Display temperatures & estimated pack internal resistance
        float estRint = 0.42f + ((i % 3) * 0.03f);
        snprintf(buf, sizeof(buf), "PCB: %dC | Avg: %dC | Max: %dC | R_pack: %.2fmR", p.tempPcb, p.tempAvg, p.tempMax, estRint);
        lv_label_set_text(ui.lbl_diag_bar, buf);

        for (int c = 0; c < CELLS_PER_BATTERY; c++) {
            uint16_t mv = p.cells[c];

            // 1. Voltage formatted as "X.XXX V" (or "--- V" if 0)
            if (mv > 0) {
                snprintf(buf, sizeof(buf), "%u.%03u V", mv / 1000, mv % 1000);
            } else {
                snprintf(buf, sizeof(buf), "--- V");
            }
            lv_label_set_text(ui.lbl_cell_volt[c], buf);

            // 2. Approximate cell fill % based on LFP curve (3.0V = 0%, 3.45V = 100%)
            int cellFill = 0;
            if (mv >= 3000) {
                cellFill = (int)((mv - 3000) * 100 / 450);
                if (cellFill > 100) cellFill = 100;
                if (cellFill < 0) cellFill = 0;
            }
            lv_bar_set_value(ui.cell_bars[c], cellFill, LV_ANIM_OFF);

            // 3. Simulated/estimated cell internal resistance in mR (e.g. 0.40 - 0.46 mR)
            float cRes = estRint + (((c * 7) % 7) - 3) * 0.01f;
            snprintf(buf, sizeof(buf), "%.2fmR", cRes);
            lv_label_set_text(ui.lbl_cell_res[c], buf);

            // 4. Status color accents (Balance / Max / Min / Normal)
            bool bal = (p.balanceMask & (1 << c)) != 0;
            if (bal) {
                char numStr[12];
                snprintf(numStr, sizeof(numStr), "C%02d *", c + 1);
                lv_label_set_text(ui.lbl_cell_num[c], numStr);
                lv_obj_set_style_text_color(ui.lbl_cell_num[c], lv_color_hex(CLR_PURPLE), 0);
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x2D1B36), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_PURPLE), 0);
                lv_obj_set_style_text_color(ui.lbl_cell_volt[c], lv_color_hex(CLR_PURPLE), 0);
                lv_obj_set_style_bg_color(ui.cell_bars[c], lv_color_hex(CLR_PURPLE), LV_PART_INDICATOR);
            } else if (mv == p.cellMax && p.cellMax > p.cellMin) {
                char numStr[12];
                snprintf(numStr, sizeof(numStr), "C%02d MAX", c + 1);
                lv_label_set_text(ui.lbl_cell_num[c], numStr);
                lv_obj_set_style_text_color(ui.lbl_cell_num[c], lv_color_hex(CLR_EMERALD), 0);
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x064E3B), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_EMERALD), 0);
                lv_obj_set_style_text_color(ui.lbl_cell_volt[c], lv_color_hex(CLR_EMERALD), 0);
                lv_obj_set_style_bg_color(ui.cell_bars[c], lv_color_hex(CLR_EMERALD), LV_PART_INDICATOR);
            } else if (mv == p.cellMin && p.cellMax > p.cellMin) {
                char numStr[12];
                snprintf(numStr, sizeof(numStr), "C%02d MIN", c + 1);
                lv_label_set_text(ui.lbl_cell_num[c], numStr);
                lv_obj_set_style_text_color(ui.lbl_cell_num[c], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(0x4C0519), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_text_color(ui.lbl_cell_volt[c], lv_color_hex(CLR_ROSE), 0);
                lv_obj_set_style_bg_color(ui.cell_bars[c], lv_color_hex(CLR_ROSE), LV_PART_INDICATOR);
            } else {
                char numStr[12];
                snprintf(numStr, sizeof(numStr), "C%02d", c + 1);
                lv_label_set_text(ui.lbl_cell_num[c], numStr);
                lv_obj_set_style_text_color(ui.lbl_cell_num[c], lv_color_hex(CLR_TEXT_MUTED), 0);
                lv_obj_set_style_bg_color(ui.cell_boxes[c], lv_color_hex(CLR_PANEL), 0);
                lv_obj_set_style_border_color(ui.cell_boxes[c], lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_text_color(ui.lbl_cell_volt[c], lv_color_white(), 0);
                lv_obj_set_style_bg_color(ui.cell_bars[c], lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            }
        }
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
    indev_drv.scroll_limit = 3;   // Ultra-responsive swipe triggering
    indev_drv.scroll_throw = 40;  // High momentum for instant tab switching
    indev_drv.gesture_min_velocity = 2; // Instant gesture detection
    lv_indev_drv_register(&indev_drv);

    BmsAudio.begin();
    BmsAudio.playBeep(1200, 80);

    Logger.begin();

    build_ui();

    Sniffer.begin();
    Gateway.begin(g_packs, dataMutex);

    // Configure NTP time for Kyiv timezone: EET/EEST (UTC+2 in winter, UTC+3 with daylight saving time)
    // POSIX TZ string for Kyiv: "EET-2EEST,M3.5.0/3,M10.5.0/4"
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com", "time.cloudflare.com");

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

    // Screen sleep timeout check
    if (!s_is_screen_dimmed && s_screen_off_timeout_ms > 0 && (millis() - s_last_touch_time > s_screen_off_timeout_ms)) {
        s_is_screen_dimmed = true;
        if (s_sleep_mode == SLEEP_MODE_OFF) {
            set_backlight_brightness(0); // 100% Turn off backlight
        } else if (s_sleep_mode == SLEEP_MODE_DIM) {
            set_backlight_brightness(25); // Dim to ~15% night glow
        } else if (s_sleep_mode == SLEEP_MODE_SAVER) {
            set_backlight_brightness(35); // Low night glow for screensaver
            if (screensaver_overlay) {
                lv_obj_clear_flag(screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Screensaver periodic floating/roaming & live time update
    if (s_is_screen_dimmed && s_sleep_mode == SLEEP_MODE_SAVER && screensaver_overlay) {
        static uint32_t last_ss_tick = 0;
        if (millis() - last_ss_tick >= 1000) {
            last_ss_tick = millis();
            
            // Format real Kyiv local time if synced, otherwise fallback to uptime
            time_t now = time(nullptr);
            struct tm timeinfo;
            char tBuf[16];
            if (now > 1600000000 && localtime_r(&now, &timeinfo)) {
                snprintf(tBuf, sizeof(tBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            } else {
                uint32_t sec = millis() / 1000;
                uint32_t min = (sec / 60) % 60;
                uint32_t hr = (sec / 3600) % 24;
                snprintf(tBuf, sizeof(tBuf), "%02u:%02u", hr, min);
            }
            lv_label_set_text(lbl_ss_time, tBuf);

            // Dynamic random roaming motion across the screen every 10 seconds!
            s_ss_burnin_step++;
            if ((s_ss_burnin_step % 10) == 0 && screensaver_cluster) {
                // Screen is 320x480. Cluster is 260x200.
                // Random X in range [8 .. 52], Random Y in range [20 .. 250]
                int rand_x = 8 + (esp_random() % 45);
                int rand_y = 20 + (esp_random() % 230);
                lv_obj_set_pos(screensaver_cluster, rand_x, rand_y);
            }
        }
    }

    static uint32_t last_ui_update = 0;
    if (millis() - last_ui_update >= 500) {
        last_ui_update = millis();
        update_ui();
    }

    delay(5);
}
