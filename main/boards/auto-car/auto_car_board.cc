#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <string>
#include <vector>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "AutoCarBoard"

class AutoCarBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int car_light = 0;//R|G|B
    int car_speed = 26;//0-100
    bool car_is_moving = false; // 小车是否在运动
    volatile int last_distance_mm = -1; // millimeters
    volatile int last_distance_mv = -1; // millivolts
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 通过UART控制MiniAuto的运动
    void InitializeCarUart() {
        uart_config_t uart_config = {
            .baud_rate = AUTO_CAR_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(uart_driver_install(AUTO_CAR_UART_PORT_NUM, AUTO_CAR_BUF_SIZE*2, AUTO_CAR_BUF_SIZE*2, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(AUTO_CAR_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(AUTO_CAR_UART_PORT_NUM, AUTO_CAR_UART_TX, AUTO_CAR_UART_RX, AUTO_CAR_UART_RTS, AUTO_CAR_UART_CTS));

        SendUartMessage("A|8|$A|11|$");

        // 启动 UART 读取任务 + 距离传感器轮询任务（每 300ms-500ms 发送一次请求）
        xTaskCreate(UartCombinedTask, "car_uart_task", 4096, this, 5, NULL);
    }

    void SendUartMessage(const char *cmd_str) {
        size_t len = strlen(cmd_str);
        int ret = uart_write_bytes(AUTO_CAR_UART_PORT_NUM, cmd_str, len);
        if (ret < len) {
            ESP_LOGE(TAG, "Failed to send All:[%s], written: %d", cmd_str, ret);
            return;
        }
        if ('D' != cmd_str[0]) {
            ESP_LOGI(TAG, "SentUart:%s", cmd_str);
        }
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.car.get_light_mode", "获取车灯颜色", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            int r = 0xff & (car_light >> 16);
            int g = 0xff & (car_light >> 8);
            int b = 0xff & car_light;
            std::string result("{\"r\":");
            result.append(std::to_string(r));
            result.append(",\"g\":");
            result.append(std::to_string(g));
            result.append(",\"b\":");
            result.append(std::to_string(b));
            result.append("}");
            return result;
            //B|255|255|255|$
            //B|R|G|B|$
        });

        mcp_server.AddTool("self.car.set_light_mode", "设置车灯颜色", PropertyList({
            Property("r", kPropertyTypeInteger, 0, 255),
            Property("g", kPropertyTypeInteger, 0, 255),
            Property("b", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList &properties) -> ReturnValue {
            int r = properties["r"].value<int>();
            int g = properties["g"].value<int>();
            int b = properties["b"].value<int>();
            car_light = ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
            char buffer[24];
            snprintf(buffer, sizeof(buffer), "B|%u|%u|%u|$", r, g, b);
            SendUartMessage(buffer);
            return true;
            //B|255|255|255|$
            //B|R|G|B|$
        });

        // 批量执行多条指令：commands 可以是 JSON 数组字符串，或用 ';' / '\n' 分隔的命令列表。
        // 每条指令也可以带后缀 `#<s>` 指定该条指令后等待秒数（例如 `go_forward#2`）。
        // 默认每条指令后等待固定 1 秒（若未在命令中指定）。
        mcp_server.AddTool("self.car.run_sequence", R"=(一次执行多条指令，命令以JSON格式，如：
["go_forward","go_back","go_left","go_right","go_forward_left","go_forward_right","go_back_left","go_back_right","turn_left","turn_right","stop"]
或者直接拼接成以';'分隔的字符串格式，如：
"go_forward;go_back;go_left;go_right;go_forward_left;go_forward_right;go_back_left;go_back_right;turn_left;turn_right;stop"
每个指令可以带后缀"#<s>"指定该条指令后等待秒数，例如：前进4秒，左移2秒的指令序列可以写成：
["go_forward#4","go_left#2"]
)=", PropertyList({
            Property("commands", kPropertyTypeString)
        }), [this](const PropertyList &properties) -> ReturnValue {
            std::string raw = properties["commands"].value<std::string>();
            std::vector<std::string> parts;

            ESP_LOGI(TAG, "Run sequence commands: %s", raw.c_str());
            // Try parse as JSON array first
            cJSON *json = cJSON_Parse(raw.c_str());
            if (json && cJSON_IsArray(json)) {
                int size = cJSON_GetArraySize(json);
                for (int i = 0; i < size; ++i) {
                    cJSON *it = cJSON_GetArrayItem(json, i);
                    if (cJSON_IsString(it)) {
                        parts.emplace_back(it->valuestring);
                    }
                }
                cJSON_Delete(json);
            } else {
                // split by ';' or newlines
                size_t start = 0;
                while (start < raw.size()) {
                    size_t p = raw.find_first_of(";\n", start);
                    std::string token;
                    if (p == std::string::npos) {
                        token = raw.substr(start);
                        start = raw.size();
                    } else {
                        token = raw.substr(start, p - start);
                        start = p + 1;
                    }
                    // trim
                    size_t a = token.find_first_not_of(" \t\r");
                    if (a == std::string::npos) continue;
                    size_t b = token.find_last_not_of(" \t\r");
                    parts.emplace_back(token.substr(a, b - a + 1));
                }
            }

            if (parts.empty()) return false;

            // ensure final stop exists; if none of the commands contains an explicit stop token, append stop
            const auto c = parts.rbegin();
            if (c->find("stop") != std::string::npos) {
                parts.emplace_back("stop");
            }

            // build job and create a task to run it asynchronously
            SequenceJob *job = new SequenceJob();
            job->board = this;
            job->commands = parts;

            BaseType_t ret = xTaskCreate(RunSequenceTask, "run_seq", 4096, job, 5, NULL);
            if (ret != pdPASS) {
                delete job;
                ESP_LOGE(TAG, "Failed to create run_sequence task");
                return false;
            }
            return true;
        });

        mcp_server.AddTool("self.car.stop", "停止", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = false;
            //SendUartMessage("A|8|$");
            SendUartMessage("A|8|$A|11|$");
            return true;
        });

#if 0   // 单独的指令延时太大，合并到 run_sequence 中调用。
        mcp_server.AddTool("self.car.go_forward", "前进", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|2|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_back", "后退", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|6|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_left", "左移", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|0|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_right", "右移", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|4|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_forward_left", "左前", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|1|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_forward_right", "右前", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|3|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_back_left", "左后", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|7|$");
            return true;
        });

        mcp_server.AddTool("self.car.go_back_right", "右后", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|5|$");
            return true;
        });

        mcp_server.AddTool("self.car.turn_left", "左转", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|9|$");
            return true;
        });

        mcp_server.AddTool("self.car.turn_right", "右转", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            car_is_moving = true;
            SendUartMessage("A|10|$");
            return true;
        });

        // “停止转向”和“停止”功能合并，不区分转向和移动状态，任何停止命令都会发送完全停止指令给小车。
        // mcp_server.AddTool("self.car.stop_turn", "停止转向", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
        //     car_is_moving = false;
        //     SendUartMessage("A|8|$A|11|$");
        //     return true;
        // });
#endif

        mcp_server.AddTool("self.car.set_speed", "设置移动速度", PropertyList({
            Property("speed", kPropertyTypeInteger, 0, 100)
        }), [this](const PropertyList &properties) -> ReturnValue {
            int speed = properties["speed"].value<int>();
            if (speed < 0) {
                speed = 0;
            } else if (speed > 100) {
                speed = 100;
            }
            //car_speed = speed;//等小车回传速度后再更新。
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "C|%u|$", speed);
            SendUartMessage(buffer);//小车回同样内容
            return true;
        });

        mcp_server.AddTool("self.car.get_speed", "获取小车当前移动速度", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "{\"speed\":%d}", car_speed);
            return std::string(buffer);
        });

        mcp_server.AddTool("self.car.free_run", "自由避障运动", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            SendUartMessage("F|1|$");
            return true;
        });

        mcp_server.AddTool("self.car.stop_free_run", "停止自由避障运动", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            SendUartMessage("F|0|$");
            return true;
        });

        // 返回最近一次轮询到的距离/电压（轮询任务每 300ms 发送请求并更新缓存）
        mcp_server.AddTool("self.car.get_distance", "获取与前方物体的距离（毫米，毫伏）", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
            char buffer[64];
            if (last_distance_mm == -1 || last_distance_mv == -1) {
                // 未轮询到数据，返回错误。
                snprintf(buffer, sizeof(buffer), "{\"error\":\"Car not connected\"}");
            } else {
                // 返回当前缓存值（整数，距离单位 mm，电压单位 mV）
                snprintf(buffer, sizeof(buffer), "{\"distance_mm\":%d,\"voltage_mv\":%d}", last_distance_mm, last_distance_mv);
            }
            return std::string(buffer);
        });
    }

public:
    AutoCarBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeCarUart();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    bool IsCarOnline() override {
        return last_distance_mm > 0;
    }

    // Combined UART task (reads and polls distance)
    static void UartCombinedTask(void *arg) {
        static_cast<AutoCarBoard*>(arg)->UartCombinedLoop();
        vTaskDelete(NULL);
    }

    // Task params for running a sequence of commands
    struct SequenceJob {
        AutoCarBoard* board;
        std::vector<std::string> commands;
    };

    // Task to execute a sequence of UART commands asynchronously
    static void RunSequenceTask(void *arg) {
        SequenceJob *job = static_cast<SequenceJob*>(arg);
        AutoCarBoard *self = job->board;
        for (const auto &raw_cmd : job->commands) {
            std::string cmd = raw_cmd;

            // detect per-command trailing interval suffix: either "#<s>" or " #<s>"
            int local_interval_s = 1; // default 1 second
            size_t hash_pos = cmd.find_last_of('#');
            if (hash_pos != std::string::npos && hash_pos + 1 < cmd.size()) {
                // try parse integer after '#'
                const char *num_start = cmd.c_str() + hash_pos + 1;
                int val = atoi(num_start);
                if (val > 0) {
                    local_interval_s = val;
                    // strip the suffix (and any space before '#')
                    size_t strip_pos = hash_pos;
                    if (strip_pos > 0 && cmd[strip_pos - 1] == ' ') --strip_pos;
                    cmd.erase(strip_pos);
                }
            }

            // send UART command if non-empty
            if (!cmd.empty()) {
                if (cmd.compare(0, 9, "self.car.") == 0) {
                    cmd = cmd.substr(9);
                }
                // If command matches a known MCP tool name without the "self.car." prefix,
                // invoke the corresponding action (same as AddTool handlers).
                if (cmd == "stop") {
                    self->car_is_moving = false;
                    self->SendUartMessage("A|8|$A|11|$");
                } else if (cmd == "go_forward") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|2|$");
                } else if (cmd == "go_back") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|6|$");
                } else if (cmd == "go_left") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|0|$");
                } else if (cmd == "go_right") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|4|$");
                } else if (cmd == "go_forward_left") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|1|$");
                } else if (cmd == "go_forward_right") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|3|$");
                } else if (cmd == "go_back_left") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|7|$");
                } else if (cmd == "go_back_right") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|5|$");
                } else if (cmd == "turn_left") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|9|$");
                } else if (cmd == "turn_right") {
                    self->car_is_moving = true;
                    self->SendUartMessage("A|10|$");
                } else {
                    // fallback: treat as raw UART string
                    self->SendUartMessage(cmd.c_str());
                }
            }

            // delay after command (per-command interval if specified, otherwise default)
            if (local_interval_s > 0) vTaskDelay(pdMS_TO_TICKS(local_interval_s * 1000));
        }
        delete job;
        vTaskDelete(NULL);

        // // 上报 MCP 通知：命令序列执行完成（没有 id 字段）
        // {
        //     std::string mcp_msg;
        //     mcp_msg.reserve(128);
        //     mcp_msg.append("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/run_sequence_changed\",\"params\":{\"newState\":true,\"oldState\":false}}");
        //     Application::GetInstance().SendMcpMessage(mcp_msg);
        // }
    }

    void UartCombinedLoop() {
        std::string pending;
        const TickType_t interval = pdMS_TO_TICKS(300);
        int count_uart_not_response = 0;// 未收到uart数据的次数
        uint8_t buf[128];
        while (true) {
            // send distance poll each cycle
            SendUartMessage("D|$");

            // block up to `interval` waiting for incoming UART data
            int len = uart_read_bytes(AUTO_CAR_UART_PORT_NUM, buf, sizeof(buf), interval);
            if (len > 0) {
                count_uart_not_response = 0;
                pending.append(reinterpret_cast<char*>(buf), len);
                size_t pos;
                while ((pos = pending.find('$')) != std::string::npos) {
                    std::string pkt = pending.substr(0, pos); // 不含终止符
                    pending.erase(0, pos + 1);
                    HandleUartMessage(pkt);
                }
                // 可能没有等待就收到消息了，需要在此等待一段时间，避免发消息太快。
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                count_uart_not_response++;
                if (count_uart_not_response > 3 && last_distance_mm >= 0) {
                    //ESP_LOGE(TAG, "UART not response for %d times, reset distance", count_uart_not_response);
                    last_distance_mm = -1;
                    last_distance_mv = -1;
                    count_uart_not_response = 0;
                }
            }
            // loop sends next poll immediately after read timeout or data processed
        }
    }

    void HandleUartMessage(const std::string &msg) {
        if (msg.empty()) return;
        ESP_LOGI(TAG, "UART RX: %s", msg.c_str());
        // 简单解析: 用 '|' 分割，首字段为命令字母（例如 A/B/C/F...）
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t p = msg.find('|', start);
            if (p == std::string::npos) {
                break;
            }
            parts.push_back(msg.substr(start, p - start));
            start = p + 1;
        }

        if (parts.empty()) {
            // maybe a raw response like "123,4" (distance,voltage)
            size_t comma = msg.find(',');
            if (comma != std::string::npos) {
                std::string s_len = msg.substr(0, comma);
                std::string s_vol = msg.substr(comma + 1);
                // 直接按整数解析：传感器直接返回整数毫米和毫伏（无比例）
                int len_mm = atoi(s_len.c_str());
                int vol_mv = atoi(s_vol.c_str());
                last_distance_mm = len_mm;
                last_distance_mv = vol_mv;
                ESP_LOGI(TAG, "Distance response: %d mm, %d mV", len_mm, vol_mv);

                // 距离小于40mm且小车在运动时，自动停止
                if (len_mm < 40 && car_is_moving) {
                    ESP_LOGI(TAG, "Distance %d mm < 40mm, stopping car", len_mm);
                    car_is_moving = false;
                    SendUartMessage("A|8|$A|11|$");
                    // 上报 MCP 通知：距离过近导致已停止（没有 id 字段）
                    {
                        std::string mcp_msg;
                        mcp_msg.reserve(128);
                        mcp_msg.append("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/moving_changed\",\"params\":{\"newState\":false,\"oldState\":true}}");
                        Application::GetInstance().SendMcpMessage(mcp_msg);
                    }
                }
            }
            return;
        }

        const std::string &cmd = parts[0];

        // 处理示例：C|speed|
        if (cmd == "C" && parts.size() >= 2) {
            int speed = atoi(parts[1].c_str());
            car_speed = speed;
            ESP_LOGI(TAG, "Car speed updated from UART: %d", car_speed);
        // } else if (cmd == "B" && parts.size() >= 4) {
        //     int r = atoi(parts[1].c_str());
        //     int g = atoi(parts[2].c_str());
        //     int b = atoi(parts[3].c_str());
        //     car_light = ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
        //     ESP_LOGI(TAG, "Car light updated from UART: R=%d G=%d B=%d", r, g, b);
        } else {
            // 需要时在这里扩展解析逻辑
            ESP_LOGW(TAG, "Unknown UART message format: %s", msg.c_str());
        }
    }
};

DECLARE_BOARD(AutoCarBoard);

