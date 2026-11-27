/*
 * Конфигурация LovyanGFX под плату JC3636W518C.
 *
 * Дисплей — ST77916 360x360, подключённый по 4-линейному SPI (QSPI).
 * Панель помечена как нечитаемая: запись быстрая, чтение пикселей не используется.
 * Подсветка управляется через PWM, а контроллер тача CST816S входит в тот же
 * экземпляр LGFX, поэтому getTouch() доступен через display_get().
 */
#include "display.h"
#include "config.h"

namespace {

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST77916 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;

 public:
  LGFX() {
    {
      // Шина QSPI: одна линия такта и четыре линии данных из config.h.
      // 40 МГц на запись / 16 МГц на чтение — проверенная комбинация для этой панели.
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000UL;
      cfg.freq_read = 16000000UL;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_io0 = TFT_SDA0;
      cfg.pin_io1 = TFT_SDA1;
      cfg.pin_io2 = TFT_SDA2;
      cfg.pin_io3 = TFT_SDA3;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      // Геометрия ST77916: 360x360, инверсия, порядок RGB, без чтения назад.
      // Значения dummy_read_* и bus_shared=false взяты из демо производителя.
      auto cfg = _panel.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = SCREEN_WIDTH;
      cfg.panel_height = SCREEN_HEIGHT;
      cfg.memory_width = SCREEN_WIDTH;
      cfg.memory_height = SCREEN_HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }

    {
      // Подсветка через PWM. display_set_brightness() переводит проценты в 0..255.
      auto cfg = _light.config();
      cfg.pin_bl = TFT_BLK;
      cfg.invert = false;
      cfg.freq = 5000;
      cfg.pwm_channel = 0;
      _light.config(cfg);
      _panel.setLight(&_light);
    }

    {
      // CST816S на I2C0. Линии INT/RST позволяют корректно будить и сбрасывать контроллер.
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x15;  // адрес CST816S по умолчанию
      cfg.pin_sda = TOUCH_PIN_NUM_I2C_SDA;
      cfg.pin_scl = TOUCH_PIN_NUM_I2C_SCL;
      cfg.pin_int = TOUCH_PIN_NUM_INT;
      cfg.pin_rst = TOUCH_PIN_NUM_RST;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = SCREEN_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = SCREEN_HEIGHT - 1;
      cfg.offset_rotation = 0;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX gfx;

}  // namespace

LGFX_Device& display_get() { return gfx; }

void display_init() {
  gfx.init();              // Применяет все настройки панели/шины/подсветки/тача выше
  gfx.setRotation(0);
  gfx.setBrightness(200);  // ~78%: достаточно ярко на улице, но не максимальный белый
  gfx.fillScreen(TFT_BLACK);
  gfx.setTextColor(TFT_GREEN, TFT_BLACK);
  gfx.setTextDatum(middle_center);
  gfx.drawString("SmartDash", SCREEN_CENTER_X, SCREEN_CENTER_Y - 20, 4);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("JC3636W518C", SCREEN_CENTER_X, SCREEN_CENTER_Y + 16, 2);
  gfx.drawString("Display OK", SCREEN_CENTER_X, SCREEN_CENTER_Y + 40, 2);
}

// Поворот задаётся из UI-задачи. Вместе с MADCTL панели LovyanGFX сам
// пересчитывает координаты тача, поэтому свайпы и тап-зоны остаются на месте.
void display_set_rotation(int degrees) {
  int r = constrain(degrees / 90, 0, 3);
  gfx.setRotation((uint_fast8_t)r);
}

void display_clear(uint16_t color) { gfx.fillScreen(color); }

void display_set_brightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  gfx.setBrightness((uint8_t)(percent * 255 / 100));
}
