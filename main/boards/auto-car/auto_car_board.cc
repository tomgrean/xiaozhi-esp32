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
			.baud_rate = ECHO_UART_BAUD_RATE,
			.data_bits = UART_DATA_8_BITS,
			.parity = UART_PARITY_DISABLE,
			.stop_bits = UART_STOP_BITS_1,
			.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
			.source_clk = UART_SCLK_DEFAULT,
		};
		int intr_alloc_flags = 0;

		ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, ECHO_BUF_SIZE*2, 0, 0, NULL, intr_alloc_flags));
		ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
		ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, AUTO_CAR_UART_TX, AUTO_CAR_UART_RX, AUTO_CAR_UART_RTS, AUTO_CAR_UART_CTS));

		SendUartMessage("A|8|$A|11|$");
    }

	void SendUartMessage(const char *cmd_str) {
		uint8_t len = strlen(cmd_str);
		uart_write_bytes(ECHO_UART_PORT_NUM, cmd_str, len);
		ESP_LOGI(TAG, "SentUart:%s", cmd_str);
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

		mcp_server.AddTool("self.car.stop", "停止", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|8|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_forward", "前进", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|2|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_back", "后退", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|6|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_left", "左移", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|0|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_right", "右移", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|4|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_forward_left", "左前", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|1|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_forward_right", "右前", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|3|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_back_left", "左后", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|7|$");
			return true;
		});

		mcp_server.AddTool("self.car.go_back_right", "右后", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|5|$");
			return true;
		});

		mcp_server.AddTool("self.car.turn_left", "左转", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|9|$");
			return true;
		});

		mcp_server.AddTool("self.car.turn_right", "右转", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|10|$");
			return true;
		});

		mcp_server.AddTool("self.car.stop_turn", "停止转向", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("A|8|$A|11|$");
			return true;
		});

		mcp_server.AddTool("self.car.set_speed", "设置运动速度", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("C|53|$");//小车回同样内容
			return true;
		});

		mcp_server.AddTool("self.car.free_run", "自由壁障运动", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("F|1|$");
			return true;
		});

		mcp_server.AddTool("self.car.stop_free_run", "停止自由壁障运动", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("F|0|$");
			return true;
		});

		/* logic not working
		mcp_server.AddTool("self.car.get_distance", "获取与前方物体的距离", PropertyList(), [this](const PropertyList &properties) -> ReturnValue {
			SendUartMessage("D|$");
			return true;
			//小车返回:$长度,Voltage$
		});
		*/

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
};

DECLARE_BOARD(AutoCarBoard);

