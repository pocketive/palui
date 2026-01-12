#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

LV_FONT_DECLARE(dmsans_14);
LV_IMAGE_DECLARE(welcome);
LV_IMAGE_DECLARE(wifi);
LV_IMAGE_DECLARE(api);
LV_IMAGE_DECLARE(chat);

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define SERIAL_BAUD 115200
#define MAX_CHAT_HISTORY 50

TFT_eSPI tft = TFT_eSPI();
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
Preferences preferences;

struct ChatMessage {
    String text;
    bool is_user;
};

ChatMessage chat_history[MAX_CHAT_HISTORY];
int chat_count = 0;
String received_response = "";
bool waiting_for_response = false;
bool is_portrait = true;
bool first_message_sent = false;
bool message_sent_at_least_once = false;
bool use_api_mode = false;
bool wifi_connected = false;

lv_obj_t * main_scr;
lv_obj_t * chat_container;
lv_obj_t * input_area;
lv_obj_t * tb;
lv_obj_t * kb;
lv_obj_t * send_btn;
lv_obj_t * status_label;
lv_obj_t * stats_label;
lv_obj_t * orientation_btn;
lv_obj_t * welcome_container;
lv_obj_t * header_container;
lv_obj_t * top_menu;
lv_obj_t * wifi_screen;
lv_obj_t * api_screen;
lv_obj_t * mode_toggle_container;
lv_obj_t * wifi_list;
lv_obj_t * wifi_password_area;
lv_obj_t * wifi_selected_label;
lv_obj_t * wifi_status_label;
lv_obj_t * wifi_password_container;
lv_obj_t * wifi_content_area;
lv_obj_t * wifi_back_btn;
lv_obj_t * wifi_refresh_btn;
lv_obj_t * api_content_area;
lv_obj_t * connect_popup;

int x, y, z;
int dot_animation_state = 0;
unsigned long last_dot_update = 0;
bool menu_expanded = false;
String saved_ssid = "";
String saved_password = "";
String saved_api_key = "";
String selected_wifi_ssid = "";
bool waiting_for_wifi_connection = false;
unsigned long wifi_connection_start = 0;

#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];
lv_display_t * disp;

const char* welcome_messages[] = {
    "ready to help.",
    "how may I help you?",
    "what can I do for you?",
    "here to assist you.",
    "let's get started.",
    "how can I assist?",
    "here to help.",
    "welcome back.",
    "ready when you are.",
    "at your service."
};

int get_screen_width() {
    return is_portrait ? 240 : 320;
}

int get_screen_height() {
    return is_portrait ? 320 : 240;
}

void load_preferences() {
    preferences.begin("chat-app", false);
    saved_ssid = preferences.getString("wifi_ssid", "");
    saved_password = preferences.getString("wifi_pass", "");
    saved_api_key = preferences.getString("api_key", "");
    preferences.end();
}

void save_wifi_credentials(String ssid, String pass) {
    preferences.begin("chat-app", false);
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", pass);
    preferences.end();
    saved_ssid = ssid;
    saved_password = pass;
}

void save_api_key(String key) {
    preferences.begin("chat-app", false);
    preferences.putString("api_key", key);
    preferences.end();
    saved_api_key = key;
}

void clear_wifi_credentials() {
    preferences.begin("chat-app", false);
    preferences.remove("wifi_ssid");
    preferences.remove("wifi_pass");
    preferences.end();
    saved_ssid = "";
    saved_password = "";
}

void clear_api_key() {
    preferences.begin("chat-app", false);
    preferences.remove("api_key");
    preferences.end();
    saved_api_key = "";
}

void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
    if(touchscreen.tirqTouched() && touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
       
        if(is_portrait) {
            x = map(p.x, 200, 3700, 1, 240);
            y = map(p.y, 240, 3800, 1, 320);
        } else {
            x = map(p.y, 240, 3800, 1, 320);
            y = map(p.x, 3700, 200, 1, 240);
        }
       
        z = p.z;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void send_prompt(const char *prompt) {
    Serial.println(prompt);
    Serial.flush();
}

String build_chat_context() {
    String context;
    for (int i = 0; i < chat_count; i++) {
        if (chat_history[i].is_user) {
            context += "User: " + chat_history[i].text + "\n";
        } else {
            context += "Assistant: " + chat_history[i].text + "\n";
        }
    }
    return context;
}

void send_api_request(const String& prompt)
{
    if (!wifi_connected) {
        add_message_bubble("Error: WiFi not connected", false);
        waiting_for_response = false;
        return;
    }

    HTTPClient https;
    String clean = prompt;
    clean.trim();
    if (clean.isEmpty()) { waiting_for_response = false; return; }

    String payload = "{\"contents\":[";
    
    for (int i = 0; i < chat_count; i++) {
        if (i > 0) payload += ",";
        payload += "{\"role\":\"";
        payload += chat_history[i].is_user ? "user" : "model";
        payload += "\",\"parts\":[{\"text\":\"";
        String escaped = chat_history[i].text;
        escaped.replace("\"", "\\\"");
        escaped.replace("\n", "\\n");
        payload += escaped;
        payload += "\"}]}";
    }
    
    payload += "]}";

    String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent";
    if (!https.begin(url)) {
        add_message_bubble("Error: HTTPS begin failed", false);
        waiting_for_response = false;
        return;
    }

    https.addHeader("Content-Type", "application/json");
    https.addHeader("X-goog-api-key", saved_api_key);

    int httpCode = https.POST(payload);
    String responseText;

    if (httpCode == HTTP_CODE_OK) {
        String raw = https.getString();
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, raw);
        if (err) {
            responseText = "JSON parse error";
        } else if (!doc["candidates"].size() ||
                   !doc["candidates"][0]["content"]["parts"].size() ||
                   !doc["candidates"][0]["content"]["parts"][0].containsKey("text")) {
            responseText = "No valid response";
        } else {
            responseText = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
            responseText.trim();
        }
    } else if (httpCode == 503) {
        responseText = "Seems like servers are a bit too busy right now, can you try again?";
    } else if (httpCode == -11) {
        responseText = "Connection timeout. Please check your WiFi and try again.";
    } else if (httpCode < 0) {
        responseText = "Network error. Please check your connection and try again.";
    } else {
        responseText = "HTTP " + String(httpCode) + ": Server error, please try again";
    }

    https.end();

    if (chat_count < MAX_CHAT_HISTORY) {
        chat_history[chat_count].text = responseText;
        chat_history[chat_count].is_user = false;
        chat_count++;
    }
    add_message_bubble(responseText.c_str(), false);
    waiting_for_response = false;
    lv_label_set_text(status_label, "Ready");
}

void add_message_bubble(const char* text, bool is_user) {
    lv_obj_t * bubble_wrapper = lv_obj_create(chat_container);
    lv_obj_set_size(bubble_wrapper, get_screen_width() - 16, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bubble_wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bubble_wrapper, 0, 0);
    lv_obj_set_style_pad_all(bubble_wrapper, 0, 0);
    lv_obj_t * bubble = lv_obj_create(bubble_wrapper);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_max_width(bubble, get_screen_width() - 80, 0);
    if (is_user) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x0A84FF), 0);
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2C2C2E), 0);
        lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    lv_obj_t * label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, &dmsans_14, 0);
    lv_obj_set_style_max_width(label, get_screen_width() - 100, 0);
    lv_obj_update_layout(bubble);
    lv_obj_scroll_to_y(chat_container, LV_COORD_MAX, LV_ANIM_ON);
}

String extract_tag_content(String &text, const char* start_tag, const char* end_tag) {
    int start_pos = text.indexOf(start_tag);
    if (start_pos == -1) return "";
   
    start_pos += strlen(start_tag);
    int end_pos = text.indexOf(end_tag, start_pos);
    if (end_pos == -1) return "";
   
    String content = text.substring(start_pos, end_pos);
    text.remove(start_pos - strlen(start_tag), end_pos - start_pos + strlen(start_tag) + strlen(end_tag));
    return content;
}

static void toggle_keyboard(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * tb = lv_event_get_target_obj(e);
    lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
    if(code == LV_EVENT_FOCUSED) {
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, tb);
        lv_obj_move_foreground(kb);
        lv_obj_align(input_area, LV_ALIGN_BOTTOM_MID, 0, -112);
    }
    else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_align(input_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

static void connect_to_wifi_popup() {
    connect_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(connect_popup, 200, 150);
    lv_obj_align(connect_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(connect_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(connect_popup, 0, 0);
    lv_obj_set_style_radius(connect_popup, 12, 0);
    lv_obj_move_foreground(connect_popup);

    lv_obj_t * label = lv_label_create(connect_popup);
    lv_label_set_text(label, ("Connecting to:\n" + saved_ssid).c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, &dmsans_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
}

static void close_popup(lv_event_t * e) {
    lv_obj_t * popup = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_delete(popup);
    connect_popup = NULL;
}

static void show_success_popup() {
    lv_obj_t * success_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(success_popup, 200, 150);
    lv_obj_align(success_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(success_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(success_popup, 0, 0);
    lv_obj_set_style_radius(success_popup, 12, 0);
    lv_obj_move_foreground(success_popup);

    lv_obj_t * label = lv_label_create(success_popup);
    lv_label_set_text(label, "Connected successfully!");
    lv_obj_set_style_text_color(label, lv_color_hex(0x30D158), 0);
    lv_obj_set_style_text_font(label, &dmsans_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t * ok_btn = lv_button_create(success_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_add_event_cb(ok_btn, close_popup, LV_EVENT_CLICKED, success_popup);

    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ok_label, LV_ALIGN_CENTER, 0, 0);
}

static void show_error_popup(String error) {
    lv_obj_t * error_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(error_popup, 220, 160);
    lv_obj_align(error_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(error_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(error_popup, 0, 0);
    lv_obj_set_style_radius(error_popup, 12, 0);
    lv_obj_move_foreground(error_popup);

    lv_obj_t * label = lv_label_create(error_popup);
    lv_label_set_text(label, ("Connection failed:\n" + error).c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_text_font(label, &dmsans_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    lv_obj_t * ok_btn = lv_button_create(error_popup);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_add_event_cb(ok_btn, close_popup, LV_EVENT_CLICKED, error_popup);

    lv_obj_t * ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ok_label, LV_ALIGN_CENTER, 0, 0);
}

static bool connect_to_saved_wifi() {
    if (saved_ssid.length() == 0) {
        return false;
    }

    connect_to_wifi_popup();
    
    WiFi.begin(saved_ssid.c_str(), saved_password.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        delay(500);
        lv_task_handler();
    }
    
    if (connect_popup) {
        lv_obj_delete(connect_popup);
        connect_popup = NULL;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        show_success_popup();
        return true;
    } else {
        wifi_connected = false;
        String error = "Could not connect to network";
        if (WiFi.status() == WL_CONNECT_FAILED) {
            error = "Wrong password";
        } else if (WiFi.status() == WL_NO_SSID_AVAIL) {
            error = "Network not available";
        }
        show_error_popup(error);
        return false;
    }
}

static void send_message(lv_event_t * e) {
    if (waiting_for_response) {
        return;
    }
    
    if (use_api_mode && !wifi_connected) {
        if (!connect_to_saved_wifi()) {
            return;
        }
    }
    
    lv_obj_t * tb = (lv_obj_t *)lv_event_get_user_data(e);
    const char* message = lv_textarea_get_text(tb);
   
    if (strlen(message) == 0) {
        return;
    }
    
    if (!first_message_sent) {
        first_message_sent = true;
        if (welcome_container != NULL) {
            lv_obj_delete(welcome_container);
            welcome_container = NULL;
        }
        if (mode_toggle_container != NULL) {
            lv_obj_delete(mode_toggle_container);
            mode_toggle_container = NULL;
        }
    }
    
    if (chat_count < MAX_CHAT_HISTORY) {
        chat_history[chat_count].text = String(message);
        chat_history[chat_count].is_user = true;
        chat_count++;
    }
    
    add_message_bubble(message, true);
   
    while(Serial.available()) {
        Serial.read();
    }
   
    message_sent_at_least_once = true;
    if (use_api_mode) {
        send_api_request(String(message));
    } else {
        send_prompt(message);
    }
    
    lv_textarea_set_text(tb, "");
    received_response = "";
    waiting_for_response = true;
    dot_animation_state = 0;
    last_dot_update = millis();
    lv_label_set_text(status_label, "Thinking.");
    lv_label_set_text(stats_label, "");
}

void rebuild_gui();

static void toggle_orientation(lv_event_t * e) {
    is_portrait = !is_portrait;
   
    if(is_portrait) {
        tft.setRotation(0);
        lv_display_set_resolution(disp, 240, 320);
    } else {
        tft.setRotation(1);
        lv_display_set_resolution(disp, 320, 240);
    }
   
    bool wifi_visible = !lv_obj_has_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
    bool api_visible = !lv_obj_has_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
   
    rebuild_gui();
   
    if (wifi_visible) {
        lv_obj_add_flag(main_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(header_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(header_container);
    } else if (api_visible) {
        lv_obj_add_flag(main_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(header_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(header_container);
    }
}

static void toggle_menu(lv_event_t * e) {
    menu_expanded = !menu_expanded;
   
    if (top_menu == NULL) return;
   
    if (menu_expanded) {
        lv_obj_remove_flag(top_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_menu);
        lv_obj_move_foreground(header_container);
    } else {
        lv_obj_add_flag(top_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mode_toggle_changed(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target_obj(e);
    use_api_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    
    if (use_api_mode && saved_ssid.length() > 0 && !wifi_connected) {
        connect_to_saved_wifi();
    }
}

static void show_wifi_screen(lv_event_t * e);
static void show_api_screen(lv_event_t * e);
static void show_chat_screen(lv_event_t * e);
static void wifi_network_selected(lv_event_t * e);
static void wifi_connect_clicked(lv_event_t * e);
static void wifi_disconnect_clicked(lv_event_t * e);
static void wifi_delete_clicked(lv_event_t * e);
static void api_save_clicked(lv_event_t * e);
static void api_delete_clicked(lv_event_t * e);
void create_wifi_screen();
void create_api_screen();

static void show_wifi_screen(lv_event_t * e) {
    lv_obj_add_flag(main_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(header_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(header_container);
    menu_expanded = false;
    lv_obj_add_flag(top_menu, LV_OBJ_FLAG_HIDDEN);
    wifi_refresh_clicked(NULL);
}

static void show_api_screen(lv_event_t * e) {
    lv_obj_add_flag(main_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(header_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(header_container);
    menu_expanded = false;
    lv_obj_add_flag(top_menu, LV_OBJ_FLAG_HIDDEN);
}

static void show_chat_screen(lv_event_t * e) {
    lv_obj_remove_flag(main_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(header_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(header_container);
    menu_expanded = false;
    lv_obj_add_flag(top_menu, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_network_selected(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target_obj(e);
    lv_obj_t * label = lv_obj_get_child(btn, 1);
    const char* text = lv_label_get_text(label);
    selected_wifi_ssid = String(text);
   
    if (wifi_selected_label != NULL) {
        lv_label_set_text(wifi_selected_label, ("Selected: " + selected_wifi_ssid).c_str());
    }
   
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_password_container, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(wifi_password_area, "");
    lv_obj_align(wifi_password_container, LV_ALIGN_CENTER, 0, 0);
}

static void wifi_back_clicked(lv_event_t * e) {
    lv_obj_remove_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_password_container, LV_OBJ_FLAG_HIDDEN);
    selected_wifi_ssid = "";
    if (wifi_selected_label != NULL) {
        lv_label_set_text(wifi_selected_label, "Select a network");
    }
}

static void wifi_connect_clicked(lv_event_t * e) {
    String ssid = selected_wifi_ssid;
    String pass = lv_textarea_get_text(wifi_password_area);
    if (ssid.length() == 0) {
        return;
    }
    
    lv_obj_t * conn_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(conn_popup, 200, 150);
    lv_obj_align(conn_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(conn_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(conn_popup, 0, 0);
    lv_obj_set_style_radius(conn_popup, 12, 0);
    lv_obj_move_foreground(conn_popup);

    lv_obj_t * conn_label = lv_label_create(conn_popup);
    lv_label_set_text(conn_label, ("Connecting to:\n" + ssid).c_str());
    lv_obj_set_style_text_color(conn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(conn_label, &dmsans_14, 0);
    lv_obj_align(conn_label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * conn_spinner = lv_spinner_create(conn_popup);
    lv_obj_set_size(conn_spinner, 40, 40);
    lv_obj_align(conn_spinner, LV_ALIGN_CENTER, 0, -10);
    lv_spinner_set_anim_params(conn_spinner, 1000, 60);
    
    lv_task_handler();
    
    WiFi.begin(ssid.c_str(), pass.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        delay(500);
        lv_task_handler();
    }
    
    lv_obj_delete(conn_popup);
    
    if (WiFi.status() == WL_CONNECTED) {
        save_wifi_credentials(ssid, pass);
        wifi_connected = true;
        lv_obj_add_flag(wifi_password_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
        selected_wifi_ssid = "";
        show_success_popup();
        rebuild_gui();
    } else {
        wifi_connected = false;
        String error = "Maybe wrong password?";
        show_error_popup(error);
    }
}

static void wifi_disconnect_clicked(lv_event_t * e) {
    lv_obj_t * disc_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(disc_popup, 200, 150);
    lv_obj_align(disc_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(disc_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(disc_popup, 0, 0);
    lv_obj_set_style_radius(disc_popup, 12, 0);
    lv_obj_move_foreground(disc_popup);

    lv_obj_t * disc_label = lv_label_create(disc_popup);
    lv_label_set_text(disc_label, "Disconnecting...");
    lv_obj_set_style_text_color(disc_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(disc_label, &dmsans_14, 0);
    lv_obj_align(disc_label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * disc_spinner = lv_spinner_create(disc_popup);
    lv_obj_set_size(disc_spinner, 40, 40);
    lv_obj_align(disc_spinner, LV_ALIGN_CENTER, 0, -10);
    lv_spinner_set_anim_params(disc_spinner, 1000, 60);
    
    lv_task_handler();
    delay(500);
    
    WiFi.disconnect();
    wifi_connected = false;
    
    lv_obj_delete(disc_popup);
    rebuild_gui();
}

static void wifi_delete_clicked(lv_event_t * e) {
    lv_obj_t * del_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(del_popup, 200, 150);
    lv_obj_align(del_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(del_popup, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(del_popup, 0, 0);
    lv_obj_set_style_radius(del_popup, 12, 0);
    lv_obj_move_foreground(del_popup);

    lv_obj_t * del_label = lv_label_create(del_popup);
    lv_label_set_text(del_label, "Deleting credentials...");
    lv_obj_set_style_text_color(del_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(del_label, &dmsans_14, 0);
    lv_obj_align(del_label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * del_spinner = lv_spinner_create(del_popup);
    lv_obj_set_size(del_spinner, 40, 40);
    lv_obj_align(del_spinner, LV_ALIGN_CENTER, 0, -10);
    lv_spinner_set_anim_params(del_spinner, 1000, 60);
    
    lv_task_handler();
    delay(500);
    
    WiFi.disconnect();
    wifi_connected = false;
    clear_wifi_credentials();
    
    lv_obj_delete(del_popup);
    rebuild_gui();
}

static void wifi_refresh_clicked(lv_event_t * e) {
    wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (wifi_status_label != NULL) {
        if (wifi_connected) {
            lv_label_set_text(wifi_status_label, ("Connected: " + saved_ssid).c_str());
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x30D158), 0);
        } else {
            lv_label_set_text(wifi_status_label, "Not connected");
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFF9500), 0);
        }
    }
}

static void api_save_clicked(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    String key = lv_textarea_get_text(ta);
   
    if (key.length() > 0) {
        save_api_key(key);
        rebuild_gui();
    }
}

static void api_delete_clicked(lv_event_t * e) {
    clear_api_key();
    rebuild_gui();
}

void create_wifi_screen() {
    wifi_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_screen, get_screen_width(), get_screen_height());
    lv_obj_set_style_bg_color(wifi_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(wifi_screen, 0, 0);
    lv_obj_add_flag(wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);

    wifi_content_area = lv_obj_create(wifi_screen);
    lv_obj_set_size(wifi_content_area, get_screen_width(), get_screen_height() - 28);
    lv_obj_align(wifi_content_area, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(wifi_content_area, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(wifi_content_area, 0, 0);
    lv_obj_set_style_pad_all(wifi_content_area, 10, 0);
    lv_obj_remove_flag(wifi_content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wifi_content_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_content_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * title_row = lv_obj_create(wifi_content_area);
    lv_obj_set_size(title_row, get_screen_width() - 20, 30);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(title_row);
    lv_label_set_text(title, "WiFi Networks");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &dmsans_14, 0);

    wifi_refresh_btn = lv_button_create(title_row);
    lv_obj_set_size(wifi_refresh_btn, 60, 25);
    lv_obj_set_style_bg_color(wifi_refresh_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(wifi_refresh_btn, 0, 0);
    lv_obj_set_style_radius(wifi_refresh_btn, 6, 0);
    lv_obj_add_event_cb(wifi_refresh_btn, wifi_refresh_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t * refresh_label = lv_label_create(wifi_refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(0x0A84FF), 0);
    lv_obj_align(refresh_label, LV_ALIGN_CENTER, 0, 0);

    wifi_status_label = lv_label_create(wifi_content_area);
    if (wifi_connected) {
        lv_label_set_text(wifi_status_label, ("Connected: " + saved_ssid).c_str());
        lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x30D158), 0);
    } else {
        lv_label_set_text(wifi_status_label, "Not connected");
        lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFF9500), 0);
    }
    lv_obj_set_style_text_font(wifi_status_label, &dmsans_14, 0);
    lv_obj_set_width(wifi_status_label, get_screen_width());

    wifi_selected_label = lv_label_create(wifi_content_area);
    lv_label_set_text(wifi_selected_label, "Select a network");
    lv_obj_set_style_text_color(wifi_selected_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(wifi_selected_label, &dmsans_14, 0);
    lv_obj_set_width(wifi_selected_label, get_screen_width() - 40);
    lv_label_set_long_mode(wifi_selected_label, LV_LABEL_LONG_WRAP);

    wifi_list = lv_list_create(wifi_content_area);
    lv_obj_set_size(wifi_list, get_screen_width() - 20, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(wifi_list, get_screen_height() > 240 ? 160 : 100, 0);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 10; i++) {
        String network_name = WiFi.SSID(i);
        lv_obj_t * btn = lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, network_name.c_str());
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_text_font(btn, &dmsans_14, 0);
        lv_obj_add_event_cb(btn, wifi_network_selected, LV_EVENT_CLICKED, NULL);
    }

    wifi_password_container = lv_obj_create(wifi_content_area);
    lv_obj_set_size(wifi_password_container, get_screen_width() - 20, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wifi_password_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_password_container, 0, 0);
    lv_obj_set_style_pad_all(wifi_password_container, 0, 0);
    lv_obj_add_flag(wifi_password_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_password_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wifi_password_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_password_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_width(wifi_password_container, get_screen_width() - 20);

    wifi_password_area = lv_textarea_create(wifi_password_container);
    lv_obj_set_width(wifi_password_area, get_screen_width() - 20);
    lv_textarea_set_placeholder_text(wifi_password_area, "Password (leave empty if none)");
    lv_textarea_set_one_line(wifi_password_area, true);
    lv_textarea_set_password_mode(wifi_password_area, true);
    lv_obj_set_style_bg_color(wifi_password_area, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(wifi_password_area, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(wifi_password_area, &dmsans_14, 0);
    lv_obj_set_style_border_color(wifi_password_area, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(wifi_password_area, 1, 0);
    lv_obj_add_event_cb(wifi_password_area, toggle_keyboard, LV_EVENT_ALL, kb);

    wifi_back_btn = lv_button_create(wifi_password_container);
    lv_obj_set_size(wifi_back_btn, get_screen_width() - 20, 35);
    lv_obj_set_style_bg_color(wifi_back_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(wifi_back_btn, 0, 0);
    lv_obj_add_event_cb(wifi_back_btn, wifi_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t * back_label = lv_label_create(wifi_back_btn);
    lv_label_set_text(back_label, "Back to Network List");
    lv_obj_set_style_text_font(back_label, &dmsans_14, 0);
    lv_obj_align(back_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * connect_btn = lv_button_create(wifi_password_container);
    lv_obj_set_size(connect_btn, get_screen_width() - 20, 40);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_border_width(connect_btn, 0, 0);
    lv_obj_add_event_cb(connect_btn, wifi_connect_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t * connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect & Save");
    lv_obj_set_style_text_font(connect_label, &dmsans_14, 0);
    lv_obj_align(connect_label, LV_ALIGN_CENTER, 0, 0);

    if (saved_ssid.length() > 0) {
        lv_obj_t * button_row = lv_obj_create(wifi_content_area);
        lv_obj_set_size(button_row, get_screen_width() - 20, 35);
        lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button_row, 0, 0);
        lv_obj_set_style_pad_all(button_row, 0, 0);
        lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(button_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * disconnect_btn = lv_button_create(button_row);
        lv_obj_set_size(disconnect_btn, (get_screen_width() - 30) / 2, 35);
        lv_obj_set_style_bg_color(disconnect_btn, lv_color_hex(0xFF9500), 0);
        lv_obj_set_style_border_width(disconnect_btn, 0, 0);
        lv_obj_add_event_cb(disconnect_btn, wifi_disconnect_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t * disc_label = lv_label_create(disconnect_btn);
        lv_label_set_text(disc_label, "Disconnect");
        lv_obj_set_style_text_font(disc_label, &dmsans_14, 0);
        lv_obj_align(disc_label, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t * delete_btn = lv_button_create(button_row);
        lv_obj_set_size(delete_btn, (get_screen_width() - 30) / 2, 35);
        lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xFF3B30), 0);
        lv_obj_set_style_border_width(delete_btn, 0, 0);
        lv_obj_add_event_cb(delete_btn, wifi_delete_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t * del_label = lv_label_create(delete_btn);
        lv_label_set_text(del_label, "Delete");
        lv_obj_set_style_text_font(del_label, &dmsans_14, 0);
        lv_obj_align(del_label, LV_ALIGN_CENTER, 0, 0);
    }
}

void create_api_screen() {
    api_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(api_screen, get_screen_width(), get_screen_height());
    lv_obj_set_style_bg_color(api_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(api_screen, 0, 0);
    lv_obj_add_flag(api_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(api_screen, LV_OBJ_FLAG_SCROLLABLE);

    api_content_area = lv_obj_create(api_screen);
    lv_obj_set_size(api_content_area, get_screen_width(), get_screen_height() - 28);
    lv_obj_align(api_content_area, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(api_content_area, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(api_content_area, 0, 0);
    lv_obj_set_style_pad_all(api_content_area, 10, 0);
    lv_obj_remove_flag(api_content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(api_content_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(api_content_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * title = lv_label_create(api_content_area);
    lv_label_set_text(title, "Gemini API Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &dmsans_14, 0);
    lv_obj_set_width(title, get_screen_width());

    lv_obj_t * status_label = lv_label_create(api_content_area);
    if (saved_api_key.length() > 0) {
        lv_label_set_text(status_label, "API Key: Saved");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x30D158), 0);
    } else {
        lv_label_set_text(status_label, "No API Key");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF9500), 0);
    }
    lv_obj_set_style_text_font(status_label, &dmsans_14, 0);
    lv_obj_set_width(status_label, get_screen_width());

    lv_obj_t * api_area = lv_textarea_create(api_content_area);
    lv_obj_set_width(api_area, get_screen_width() - 20);
    lv_textarea_set_placeholder_text(api_area, "Enter Gemini API Key");
    lv_obj_set_style_bg_color(api_area, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(api_area, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(api_area, &dmsans_14, 0);
    lv_obj_set_style_border_color(api_area, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(api_area, 1, 0);
    lv_obj_add_event_cb(api_area, toggle_keyboard, LV_EVENT_ALL, kb);

    if (saved_api_key.length() > 0) {
        lv_textarea_set_text(api_area, saved_api_key.c_str());
    }

    lv_obj_t * save_btn = lv_button_create(api_content_area);
    lv_obj_set_size(save_btn, get_screen_width() - 20, 40);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, api_save_clicked, LV_EVENT_CLICKED, api_area);

    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save API Key");
    lv_obj_set_style_text_font(save_label, &dmsans_14, 0);
    lv_obj_align(save_label, LV_ALIGN_CENTER, 0, 0);

    if (saved_api_key.length() > 0) {
        lv_obj_t * delete_btn = lv_button_create(api_content_area);
        lv_obj_set_size(delete_btn, get_screen_width() - 20, 35);
        lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xFF3B30), 0);
        lv_obj_set_style_border_width(delete_btn, 0, 0);
        lv_obj_add_event_cb(delete_btn, api_delete_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t * del_label = lv_label_create(delete_btn);
        lv_label_set_text(del_label, "Delete API Key");
        lv_obj_set_style_text_font(del_label, &dmsans_14, 0);
        lv_obj_align(del_label, LV_ALIGN_CENTER, 0, 0);
    }
}

void main_gui(void) {
    int width = get_screen_width();
    int height = get_screen_height();

    main_scr = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_scr, width, height);
    lv_obj_set_style_pad_all(main_scr, 0, 0);
    lv_obj_set_style_bg_color(main_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(main_scr, 0, 0);
    lv_obj_set_scrollbar_mode(main_scr, LV_SCROLLBAR_MODE_AUTO);

    chat_container = lv_obj_create(main_scr);
    lv_obj_set_size(chat_container, width, height - 98);
    lv_obj_align(chat_container, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_flex_flow(chat_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(chat_container, 8, 0);
    lv_obj_set_style_bg_color(chat_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(chat_container, 0, 0);
    lv_obj_set_scrollbar_mode(chat_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_y(chat_container, LV_SCROLL_SNAP_NONE);

    if (!first_message_sent) {
        welcome_container = lv_obj_create(chat_container);
        lv_obj_set_size(welcome_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(welcome_container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(welcome_container, 0, 0);
        lv_obj_set_style_pad_all(welcome_container, 0, 0);
        lv_obj_set_flex_flow(welcome_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(welcome_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(welcome_container, 8, 0);
        lv_obj_align(welcome_container, LV_ALIGN_TOP_LEFT, 20, (height - 98 - 60) / 2 + 40);

        lv_obj_t * logo = lv_image_create(welcome_container);
        lv_image_set_src(logo, &welcome);

        lv_obj_t * welcome_text = lv_label_create(welcome_container);
        int random_index = random(0, sizeof(welcome_messages) / sizeof(welcome_messages[0]));
        lv_label_set_text(welcome_text, welcome_messages[random_index]);
        lv_obj_set_style_text_color(welcome_text, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(welcome_text, &dmsans_14, 0);

        mode_toggle_container = lv_obj_create(chat_container);
        lv_obj_set_size(mode_toggle_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(mode_toggle_container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mode_toggle_container, 0, 0);
        lv_obj_set_style_pad_all(mode_toggle_container, 10, 0);
        lv_obj_set_flex_flow(mode_toggle_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(mode_toggle_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(mode_toggle_container, 10, 0);
        lv_obj_align(mode_toggle_container, LV_ALIGN_TOP_MID, 0, 60);

        lv_obj_t * local_label = lv_label_create(mode_toggle_container);
        lv_label_set_text(local_label, "Local");
        lv_obj_set_style_text_color(local_label, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(local_label, &dmsans_14, 0);

        lv_obj_t * mode_switch = lv_switch_create(mode_toggle_container);
        lv_obj_set_size(mode_switch, 50, 25);
        lv_obj_set_style_bg_color(mode_switch, lv_color_hex(0x2C2C2E), 0);
        lv_obj_add_event_cb(mode_switch, mode_toggle_changed, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t * api_label = lv_label_create(mode_toggle_container);
        lv_label_set_text(api_label, "API");
        lv_obj_set_style_text_color(api_label, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(api_label, &dmsans_14, 0);
    } else {
        welcome_container = NULL;
        mode_toggle_container = NULL;
        for (int i = 0; i < chat_count; i++) {
            add_message_bubble(chat_history[i].text.c_str(), chat_history[i].is_user);
        }
    }

    kb = lv_keyboard_create(lv_scr_act());
    lv_obj_set_size(kb, width, 112);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_text_font(kb, &dmsans_14, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);

    input_area = lv_obj_create(main_scr);
    lv_obj_set_size(input_area, width, 70);
    lv_obj_align(input_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(input_area, 8, 0);
    lv_obj_set_style_bg_color(input_area, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(input_area, 0, 0);

    tb = lv_textarea_create(input_area);
    lv_obj_set_size(tb, width - 76, 50);
    lv_obj_align(tb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_placeholder_text(tb, "Message...");
    lv_textarea_set_one_line(tb, false);
    lv_textarea_set_max_length(tb, 512);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(tb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(tb, &dmsans_14, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_radius(tb, 10, 0);

    send_btn = lv_button_create(input_area);
    lv_obj_set_size(send_btn, 52, 50);
    lv_obj_align(send_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_border_width(send_btn, 0, 0);
    lv_obj_set_style_radius(send_btn, 10, 0);
    lv_obj_add_event_cb(send_btn, send_message, LV_EVENT_CLICKED, tb);

    lv_obj_t * send_label = lv_label_create(send_btn);
    lv_label_set_text(send_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(send_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(send_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_event_cb(tb, toggle_keyboard, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(kb, send_message, LV_EVENT_READY, tb);

    header_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(header_container, width, 28);
    lv_obj_align(header_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(header_container, 4, 0);
    lv_obj_set_style_border_width(header_container, 0, 0);
    lv_obj_set_style_bg_color(header_container, lv_color_hex(0x1C1C1E), 0);
    lv_obj_move_foreground(header_container);

    lv_obj_t * menu_btn = lv_button_create(header_container);
    lv_obj_set_size(menu_btn, 24, 20);
    lv_obj_align(menu_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(menu_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_radius(menu_btn, 4, 0);
    lv_obj_add_event_cb(menu_btn, toggle_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t * menu_icon = lv_label_create(menu_btn);
    lv_label_set_text(menu_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(menu_icon, lv_color_hex(0x0A84FF), 0);
    lv_obj_align(menu_icon, LV_ALIGN_CENTER, 0, 0);

    status_label = lv_label_create(header_container);
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(status_label, &dmsans_14, 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 56, 0);

    orientation_btn = lv_button_create(header_container);
    lv_obj_set_size(orientation_btn, 24, 20);
    lv_obj_align(orientation_btn, LV_ALIGN_LEFT_MID, 28, 0);
    lv_obj_set_style_bg_color(orientation_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(orientation_btn, 0, 0);
    lv_obj_set_style_radius(orientation_btn, 4, 0);
    lv_obj_add_event_cb(orientation_btn, toggle_orientation, LV_EVENT_CLICKED, NULL);

    lv_obj_t * orient_label = lv_label_create(orientation_btn);
    lv_label_set_text(orient_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(orient_label, lv_color_hex(0x0A84FF), 0);
    lv_obj_align(orient_label, LV_ALIGN_CENTER, 0, 0);

    stats_label = lv_label_create(header_container);
    lv_label_set_text(stats_label, "");
    lv_obj_set_style_text_color(stats_label, lv_color_hex(0x30D158), 0);
    lv_obj_set_style_text_font(stats_label, &dmsans_14, 0);
    lv_obj_align(stats_label, LV_ALIGN_RIGHT_MID, -5, 0);

    top_menu = lv_obj_create(lv_scr_act());
    lv_obj_set_size(top_menu, width, 60);
    lv_obj_align(top_menu, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(top_menu, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(top_menu, 0, 0);
    lv_obj_set_style_pad_all(top_menu, 10, 0);
    lv_obj_add_flag(top_menu, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * wifi_btn = lv_button_create(top_menu);
    lv_obj_set_size(wifi_btn, 60, 40);
    lv_obj_align(wifi_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(wifi_btn, 0, 0);
    lv_obj_set_style_radius(wifi_btn, 8, 0);
    lv_obj_add_event_cb(wifi_btn, show_wifi_screen, LV_EVENT_CLICKED, NULL);

    lv_obj_t * wifi_img = lv_image_create(wifi_btn);
    lv_image_set_src(wifi_img, &wifi);
    lv_obj_align(wifi_img, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * api_btn = lv_button_create(top_menu);
    lv_obj_set_size(api_btn, 60, 40);
    lv_obj_align(api_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(api_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(api_btn, 0, 0);
    lv_obj_set_style_radius(api_btn, 8, 0);
    lv_obj_add_event_cb(api_btn, show_api_screen, LV_EVENT_CLICKED, NULL);

    lv_obj_t * api_img = lv_image_create(api_btn);
    lv_image_set_src(api_img, &api);
    lv_obj_align(api_img, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * chat_btn = lv_button_create(top_menu);
    lv_obj_set_size(chat_btn, 60, 40);
    lv_obj_align(chat_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(chat_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(chat_btn, 0, 0);
    lv_obj_set_style_radius(chat_btn, 8, 0);
    lv_obj_add_event_cb(chat_btn, show_chat_screen, LV_EVENT_CLICKED, NULL);

    lv_obj_t * chat_img = lv_image_create(chat_btn);
    lv_image_set_src(chat_img, &chat);
    lv_obj_align(chat_img, LV_ALIGN_CENTER, 0, 0);

    create_wifi_screen();
    create_api_screen();
}

void rebuild_gui() {
    lv_obj_clean(lv_scr_act());
    menu_expanded = false;
    selected_wifi_ssid = "";
    waiting_for_wifi_connection = false;
    main_gui();
}

void show_splash_screen() {
    lv_obj_t * splash = lv_obj_create(lv_scr_act());
    lv_obj_set_size(splash, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(splash, 0, 0);

    lv_obj_t * logo = lv_image_create(splash);
    lv_image_set_src(logo, &welcome);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * pocketive_label = lv_label_create(splash);
    lv_label_set_text(pocketive_label, "Pocketive");
    lv_obj_set_style_text_color(pocketive_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(pocketive_label, &dmsans_14, 0);
    lv_obj_align(pocketive_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * palui_label = lv_label_create(splash);
    lv_label_set_text(palui_label, "PalUI for P1");
    lv_obj_set_style_text_color(palui_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(palui_label, &dmsans_14, 0);
    lv_obj_align(palui_label, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t * booting_label = lv_label_create(splash);
    lv_label_set_text(booting_label, "Booting...");
    lv_obj_set_style_text_color(booting_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(booting_label, &dmsans_14, 0);
    lv_obj_align(booting_label, LV_ALIGN_CENTER, 0, 80);

    lv_obj_t * version_label = lv_label_create(splash);
    lv_label_set_text(version_label, "v0.8.0");
    lv_obj_set_style_text_color(version_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(version_label, &dmsans_14, 0);
    lv_obj_align(version_label, LV_ALIGN_CENTER, 0, 120);
}

void setup() {
    Serial.begin(SERIAL_BAUD);
   
    lv_init();
    tft.begin();
    tft.fillScreen(TFT_BLACK);
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    touchscreen.setRotation(0);
    disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchscreen_read);
    tft.setRotation(0);
    lv_display_set_resolution(disp, 240, 320);
    
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

    show_splash_screen();
    lv_task_handler();

    load_preferences();
    
    while(Serial.available()) {
        Serial.read();
    }
    
    lv_obj_clean(lv_scr_act());
    main_gui();
}

void loop() {
    lv_task_handler();
    lv_tick_inc(5);
    
    if (waiting_for_response && (millis() - last_dot_update > 500)) {
        last_dot_update = millis();
        dot_animation_state = (dot_animation_state + 1) % 4;
       
        String status_text = received_response.length() > 0 ? "Receiving" : "Thinking";
        for (int i = 0; i < dot_animation_state; i++) {
            status_text += ".";
        }
        lv_label_set_text(status_label, status_text.c_str());
    }
    
    if (use_api_mode) {
        if (waiting_for_response) {
            waiting_for_response = false;
            lv_label_set_text(status_label, "Ready");
        }
    } else {
        if (message_sent_at_least_once) {
            while (Serial.available()) {
                char c = Serial.read();
            
                if (c != '\r') {
                    received_response += c;
                
                    if (received_response.endsWith("<END>")) {
                        String stats = extract_tag_content(received_response, "<STATS>", "</STATS>");
                        received_response.replace("<END>", "");
                        received_response.trim();
                    
                        if (received_response.length() > 0) {
                            if (chat_count < MAX_CHAT_HISTORY) {
                                chat_history[chat_count].text = received_response;
                                chat_history[chat_count].is_user = false;
                                chat_count++;
                            }
                        
                            add_message_bubble(received_response.c_str(), false);
                        }
                    
                        if (stats.length() > 0) {
                            String stats_text = stats + " tok/s";
                            lv_label_set_text(stats_label, stats_text.c_str());
                        }
                    
                        received_response = "";
                        waiting_for_response = false;
                        lv_label_set_text(status_label, "Ready");
                    }
                }
            }
        } else {
            while (Serial.available()) {
                Serial.read();
            }
        }
    }
    
    delay(5);
}