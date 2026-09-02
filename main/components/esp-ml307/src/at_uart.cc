#include "at_uart.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>

#define TAG "AtUart"

AtUart::AtUart(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), ri_pin_(ri_pin), uart_num_(UART_NUM),
      baud_rate_(115200), initialized_(false), dtr_pin_state_(false),
      pm_lock_(nullptr), ri_pm_lock_(nullptr), ri_pm_lock_acquired_(false),
      receive_task_handle_(nullptr), event_task_handle_(nullptr),
      event_queue_handle_(nullptr), event_group_handle_(nullptr) {
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_pm_lock", &pm_lock_);
    if (ri_pin_ != GPIO_NUM_NC) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_ri_pm_lock", &ri_pm_lock_);
    }
}

AtUart::~AtUart() {
    if (receive_task_handle_) {
        vTaskDelete(receive_task_handle_);
        receive_task_handle_ = nullptr;
    }
    if (event_task_handle_) {
        vTaskDelete(event_task_handle_);
        event_task_handle_ = nullptr;
    }
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
        event_group_handle_ = nullptr;
    }
    if (initialized_) {
        if (ri_pin_ != GPIO_NUM_NC) {
            gpio_isr_handler_remove(ri_pin_);
        }
        uart_driver_delete(uart_num_);
        event_queue_handle_ = nullptr;
    }
    if (ri_pm_lock_) {
        if (ri_pm_lock_acquired_) {
            esp_pm_lock_release(ri_pm_lock_);
        }
        esp_pm_lock_delete(ri_pm_lock_);
    }
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void AtUart::Initialize() {
    if (initialized_) {
        return;
    }

    event_group_handle_ = xEventGroupCreate();
    if (!event_group_handle_) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate_;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    // Classic UART path for old Cat.1 modules (UHCI DMA can fail AT detection).
    ESP_ERROR_CHECK(uart_driver_install(uart_num_, AT_UART_RX_BUFFER_SIZE * 2, 0, 100, &event_queue_handle_,
                                        ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode(rx_pin_, GPIO_PULLUP_ONLY);

    if (dtr_pin_ != GPIO_NUM_NC) {
        gpio_config_t config = {};
        config.pin_bit_mask = (1ULL << dtr_pin_);
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&config);
        gpio_set_level(dtr_pin_, 0);
        dtr_pin_state_ = false;
    }

    if (ri_pin_ != GPIO_NUM_NC) {
        gpio_config_t ri_config = {};
        ri_config.pin_bit_mask = (1ULL << ri_pin_);
        ri_config.mode = GPIO_MODE_INPUT;
        ri_config.pull_up_en = GPIO_PULLUP_ENABLE;
        ri_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ri_config.intr_type = GPIO_INTR_LOW_LEVEL;
        gpio_config(&ri_config);
        gpio_wakeup_enable(ri_pin_, GPIO_INTR_LOW_LEVEL);
        gpio_isr_handler_add(ri_pin_, RiPinIsrHandler, this);
    }

    xTaskCreate(
        [](void* arg) {
            auto at_uart = (AtUart*)arg;
            at_uart->EventTask();
            vTaskDelete(NULL);
        },
        "modem_event", 2048 * 3, this, configMAX_PRIORITIES - 3, &event_task_handle_);

    xTaskCreate(
        [](void* arg) {
            auto at_uart = (AtUart*)arg;
            at_uart->ReceiveTask();
            vTaskDelete(NULL);
        },
        "modem_receive", 2048 * 2, this, configMAX_PRIORITIES - 2, &receive_task_handle_);

    initialized_ = true;
    ESP_LOGI(TAG, "Classic UART initialized (UART %d, TX=%d RX=%d)", (int)uart_num_, (int)tx_pin_,
             (int)rx_pin_);
}

void AtUart::EventTask() {
    uart_event_t event;
    while (true) {
        if (xQueueReceive(event_queue_handle_, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case UART_DATA:
                    xEventGroupSetBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE);
                    break;
                case UART_BREAK:
                    ESP_LOGD(TAG, "UART break");
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGE(TAG, "UART buffer full");
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGE(TAG, "UART FIFO overflow");
                    uart_flush_input(uart_num_);
                    xEventGroupSetBits(event_group_handle_, AT_EVENT_FIFO_OVERFLOW);
                    break;
                default:
                    break;
            }
        }
    }
}

void AtUart::ReceiveTask() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE | AT_EVENT_FIFO_OVERFLOW |
                                                               AT_EVENT_RI_PIN_INT,
                                        pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & AT_EVENT_FIFO_OVERFLOW) {
            ESP_LOGW(TAG, "UART FIFO overflow, notifying upper layer");
            HandleUrc("FIFO_OVERFLOW", {});
        }

        if (ri_pin_ != GPIO_NUM_NC) {
            if (bits & AT_EVENT_RI_PIN_INT) {
                if (!ri_pm_lock_acquired_) {
                    esp_pm_lock_acquire(ri_pm_lock_);
                    ri_pm_lock_acquired_ = true;
                }
            } else if (bits & AT_EVENT_DATA_AVAILABLE) {
                if (ri_pm_lock_acquired_) {
                    esp_pm_lock_release(ri_pm_lock_);
                    ri_pm_lock_acquired_ = false;
                    gpio_intr_enable(ri_pin_);
                }
            }
        }

        if (bits & AT_EVENT_DATA_AVAILABLE) {
            size_t available = 0;
            uart_get_buffered_data_len(uart_num_, &available);
            if (available == 0) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
                size_t old_size = rx_buffer_.size();
                rx_buffer_.resize(old_size + available);
                int read_len = uart_read_bytes(uart_num_, &rx_buffer_[old_size], available, portMAX_DELAY);
                if (read_len < 0) {
                    rx_buffer_.resize(old_size);
                    ESP_LOGE(TAG, "uart_read_bytes failed");
                    continue;
                }
                if ((size_t)read_len < available) {
                    rx_buffer_.resize(old_size + read_len);
                }
            }
            while (ParseResponse()) {
            }
        }
    }
}

static bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit) && s.length() < 10;
}

bool AtUart::ParseResponse() {
    std::string command, values;
    std::string::size_type end_pos;

    {
        std::lock_guard<std::mutex> lock(rx_buffer_mutex_);

        if (rx_buffer_.empty()) {
            return false;
        }

        if (wait_for_response_ && rx_buffer_[0] == '>') {
            rx_buffer_.erase(0, 1);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        }

        end_pos = rx_buffer_.find("\r\n");
        if (end_pos == std::string::npos) {
            if (rx_buffer_.size() >= 16 && memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"ind\"", 16) == 0) {
                auto next_plus = rx_buffer_.find("+", 1);
                if (next_plus != std::string::npos) {
                    rx_buffer_.insert(next_plus, "\r\n");
                } else {
                    rx_buffer_.append("\r\n");
                }
                end_pos = rx_buffer_.find("\r\n");
            } else {
                return false;
            }
        }

        if (end_pos == 0) {
            rx_buffer_.erase(0, 2);
            return true;
        }

        if (debug_) {
            ESP_LOGI(TAG, "<< %.64s (%u bytes) [%02x%02x%02x]", rx_buffer_.substr(0, end_pos).c_str(), (unsigned)end_pos,
                     (unsigned)(uint8_t)rx_buffer_[0], (unsigned)(uint8_t)rx_buffer_[1],
                     (unsigned)(uint8_t)rx_buffer_[2]);
        }

        // Parse "+CME ERROR: 123" 以及 XJ2113 "+MDNSCFG:"host","ip""（冒号后可无空格）
        if (rx_buffer_[0] == '+') {
            auto pos = rx_buffer_.find(':');
            if (pos == std::string::npos || pos > end_pos) {
                command = rx_buffer_.substr(1, end_pos - 1);
            } else {
                command = rx_buffer_.substr(1, pos - 1);
                size_t value_start = pos + 1;
                if (value_start < end_pos && rx_buffer_[value_start] == ' ') {
                    value_start++;
                }
                values = rx_buffer_.substr(value_start, end_pos - value_start);
            }
            rx_buffer_.erase(0, end_pos + 2);
        } else if (rx_buffer_.size() >= 4 && rx_buffer_[0] == 'O' && rx_buffer_[1] == 'K' && rx_buffer_[2] == '\r' &&
                   rx_buffer_[3] == '\n') {
            rx_buffer_.erase(0, 4);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        } else if (rx_buffer_.size() >= 7 && rx_buffer_[0] == 'E' && rx_buffer_[1] == 'R' && rx_buffer_[2] == 'R' &&
                   rx_buffer_[3] == 'O' && rx_buffer_[4] == 'R' && rx_buffer_[5] == '\r' && rx_buffer_[6] == '\n') {
            rx_buffer_.erase(0, 7);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
            return true;
        } else if ((uint8_t)rx_buffer_[0] == 0xE0) {
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        } else {
            std::lock_guard<std::mutex> response_lock(mutex_);
            response_ = rx_buffer_.substr(0, end_pos);
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        }
    }

    if (!command.empty()) {
        std::vector<AtArgumentValue> arguments;
        std::istringstream iss(values);
        std::string item;
        while (std::getline(iss, item, ',')) {
            if (item.empty()) {
                continue;
            }
            AtArgumentValue argument;
            if (item.front() == '"') {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = item.substr(1, item.size() - 2);
            } else if (item.find(".") != std::string::npos) {
                argument.type = AtArgumentValue::Type::Double;
                argument.double_value = std::stod(item);
                argument.string_value = item;
            } else if (is_number(item)) {
                argument.type = AtArgumentValue::Type::Int;
                argument.int_value = std::stoi(item);
                argument.string_value = std::move(item);
            } else {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = std::move(item);
            }
            arguments.push_back(argument);
        }

        HandleUrc(command, arguments);
        return true;
    }

    return false;
}

void AtUart::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "CME ERROR") {
        cme_error_code_ = arguments[0].int_value;
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
        return;
    }

    std::lock_guard<std::mutex> lock(urc_mutex_);
    for (auto& callback : urc_callbacks_) {
        callback(command, arguments);
    }
}

bool AtUart::DetectBaudRate(int timeout_ms) {
    int baud_rates[] = {115200, 921600, 460800, 230400, 57600, 38400, 19200, 9600};
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    while (true) {
        ESP_LOGI(TAG, "Detecting baud rate...");
        for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++) {
            int rate = baud_rates[i];
            uart_set_baudrate(uart_num_, rate);
            if (SendCommand("AT", 100)) {
                ESP_LOGI(TAG, "Detected baud rate: %d", rate);
                baud_rate_ = rate;
                return true;
            }
        }

        if (timeout_ms != -1) {
            TickType_t elapsed = xTaskGetTickCount() - start_time;
            if (elapsed >= timeout_ticks) {
                ESP_LOGE(TAG, "Baud rate detection timeout");
                return false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool AtUart::SetBaudRate(int new_baud_rate, int timeout_ms) {
    if (!DetectBaudRate(timeout_ms)) {
        ESP_LOGE(TAG, "Failed to detect baud rate");
        return false;
    }
    if (new_baud_rate == baud_rate_) {
        return true;
    }
    if (!SendCommand(std::string("AT+IPR=") + std::to_string(new_baud_rate))) {
        ESP_LOGI(TAG, "Failed to set baud rate to %d", new_baud_rate);
        return false;
    }
    uart_set_baudrate(uart_num_, new_baud_rate);
    baud_rate_ = new_baud_rate;
    ESP_LOGI(TAG, "Set baud rate to %d", new_baud_rate);
    return true;
}

bool AtUart::SendData(const char* data, size_t length) {
    if (!initialized_) {
        ESP_LOGE(TAG, "UART未初始化");
        return false;
    }

    int ret = uart_write_bytes(uart_num_, data, length);
    if (ret < 0 || (size_t)ret != length) {
        ESP_LOGE(TAG, "uart_write_bytes failed: %d", ret);
        return false;
    }
    return true;
}

bool AtUart::SendCommandWithData(const std::string& command, size_t timeout_ms, bool add_crlf, const char* data,
                                 size_t data_length) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (debug_) {
        ESP_LOGI(TAG, ">> %.64s (%u bytes)", command.data(), (unsigned)command.length());
    }

    xEventGroupClearBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR);
    wait_for_response_ = true;
    cme_error_code_ = 0;
    {
        std::lock_guard<std::mutex> response_lock(mutex_);
        response_.clear();
    }

    if (add_crlf) {
        if (!SendData((command + "\r\n").data(), command.length() + 2)) {
            return false;
        }
    } else {
        if (!SendData(command.data(), command.length())) {
            return false;
        }
    }
    if (timeout_ms > 0) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE,
                                        pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    } else {
        wait_for_response_ = false;
    }

    if (data && data_length > 0) {
        wait_for_response_ = true;
        // XJ2113/Meig: 业务数据（如 MQTTPUB payload）同样需要以 \r\n 结束，否则模组不认完整帧
        if (add_crlf) {
            std::string payload;
            payload.reserve(data_length + 2);
            payload.assign(data, data_length);
            payload.append("\r\n");
            if (!SendData(payload.data(), payload.size())) {
                return false;
            }
        } else if (!SendData(data, data_length)) {
            return false;
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE,
                                        pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    }
    return true;
}

bool AtUart::SendCommand(const std::string& command, size_t timeout_ms, bool add_crlf) {
    return SendCommandWithData(command, timeout_ms, add_crlf, nullptr, 0);
}

std::string AtUart::GetResponse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_;
}

std::list<UrcCallback>::iterator AtUart::RegisterUrcCallback(UrcCallback callback) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    return urc_callbacks_.insert(urc_callbacks_.end(), callback);
}

void AtUart::UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    urc_callbacks_.erase(iterator);
}

void AtUart::SetDtrPin(bool high) {
    if (dtr_pin_ != GPIO_NUM_NC) {
        if (debug_) {
            ESP_LOGI(TAG, "Set DTR pin %d to %d", dtr_pin_, high ? 1 : 0);
        }
        gpio_set_level(dtr_pin_, high ? 1 : 0);
        dtr_pin_state_ = high;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static const char hex_chars[] = "0123456789ABCDEF";

inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 0;
}

void AtUart::EncodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length * 2 + 4);
    for (size_t i = 0; i < length; i++) {
        dest.push_back(hex_chars[(data[i] & 0xF0) >> 4]);
        dest.push_back(hex_chars[data[i] & 0x0F]);
    }
}

void AtUart::DecodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length / 2 + 4);
    for (size_t i = 0; i < length; i += 2) {
        char byte = (CharToHex(data[i]) << 4) | CharToHex(data[i + 1]);
        dest.push_back(byte);
    }
}

std::string AtUart::EncodeHex(const std::string& data) {
    std::string encoded;
    EncodeHexAppend(encoded, data.c_str(), data.size());
    return encoded;
}

std::string AtUart::DecodeHex(const std::string& data) {
    std::string decoded;
    DecodeHexAppend(decoded, data.c_str(), data.size());
    return decoded;
}

void AtUart::SetDebug(bool enable) {
    debug_ = enable;
}

void IRAM_ATTR AtUart::RiPinIsrHandler(void* arg) {
    AtUart* at_uart = static_cast<AtUart*>(arg);
    gpio_intr_disable(at_uart->ri_pin_);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(at_uart->event_group_handle_, AT_EVENT_RI_PIN_INT, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
